/**
 * Still-photo capture emulation (shutter release).
 *
 * What a real camera does, as far as the DIGIC firmware can tell:
 *  - the MPU reports the release button (SW1/SW2) and the exposure it is about to make
 *    (EVENTID_RELEASE_ON / RELEASE_START / RELEASE_DATA_SW2ON, with Tv/Av/ISO in the payload);
 *  - the DIGIC raises a "release" GPIO line while it prepares the sensor, and lowers it when ready;
 *    the MPU then runs the shutter and the sensor raises CAPREADY at the end of the exposure;
 *  - the image engine reads the sensor out (EDMAC), the JPEG core compresses it, and the
 *    file manager writes it to the card.
 *
 * Here the MPU side is played by this file; the sensor data is a deterministic dark frame
 * (engine.c), the JPEG core output is a small valid grayscale JPEG whose level follows the
 * exposure (so bracketed frames differ), and every engine operation completes at once.
 * No image processing happens and timing is not physical.
 *
 * Generic code only; the per-camera data (struct capture_profile, capture.h) is in
 * hw/eos/cams/capture_<cam>.c, selected by eos_model_desc.capture (model_list.c).
 */

#include <math.h>
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "eos.h"
#include "model_list.h"
#include "mpu.h"
#include "capture.h"

#define CAP_LOG(fmt, ...) fprintf(stderr, "[CAPTURE] " fmt, ## __VA_ARGS__)
#define CAP_TRACE(fmt, ...) do { if (cap.trace) CAP_LOG(fmt, ## __VA_ARGS__); } while (0)

static struct
{
    const struct capture_profile *p;
    int trace;
    /* exposure the DIGIC last told the MPU (raw Canon codes, as in the MPU messages) */
    int tv, av, iso, ae, mode;
    int shots;                      /* releases started */
    int release_edges;              /* falling edges of the release line */
    uint32_t release_line;
    uint8_t inflight[0x200];        /* completion interrupt fired, waiting for its re-enable */
    uint8_t head_running[0x200];    /* HEAD timer started: its interrupt repeats until stopped */
    int sw_held;                    /* full-shutter key held */
    int releasing;                  /* between RELEASE_START and RELEASE_END */
    int sw2_off_pending;            /* SW2 went off during the release: the MPU reports it at its end */
    int sw1_off_pending;
    int exposing;                   /* CAPREADY fired, readout not done */
    int bulb_end_pending;
    /* placeholder JPEG for the current JPCORE run */
    uint8_t *jpeg;
    int jpeg_len;
    int jpeg_pos;
    int jpeg_done;
    int8_t shot_level[256];         /* placeholder level of each shot; the JPEG runs come in shot order,
                                     * possibly after the next release has started */
    int main_runs;                  /* full-size JPEG runs so far */
    int last_level;
    uint32_t jp_regs[0x400];        /* JP51 register writes */
} cap = { .tv = 0x68, .av = 0x30, .iso = 0x48 };

static const struct capture_profile *profile(void)
{
    if (!cap.p)
    {
        if (!eos_state->model->capture)
        {
            return NULL;
        }
        cap.p = eos_state->model->capture;
        if (cap.p->init_tv)
        {
            cap.tv = cap.p->init_tv;
            cap.av = cap.p->init_av;
            cap.iso = cap.p->init_iso;
        }
        /* QEMU_EOS_CAPTURE_TRACE=1: capture steps; 2: also every GPIO write after the first release */
        const char *t = getenv("QEMU_EOS_CAPTURE_TRACE");
        cap.trace = t ? atoi(t) : 0;
    }
    return cap.p;
}

/* ---------------------------------------------------------------------------------------- */
/* MPU side                                                                                  */

/* queue one MPU message: class, id and payload; header bytes as in the spell tables */
static void mpu_event(int class, int id, const uint8_t *payload, int len)
{
    uint16_t spell[256];
    int n = 3 + len;                /* [1] = bytes after [0] that carry data */
    int total = (n + 2) & ~1;       /* [0] = message size, padded to an even length */
    assert(total <= COUNT(spell));
    memset(spell, 0, sizeof(spell));
    spell[0] = total;
    spell[1] = n;
    spell[2] = class;
    spell[3] = id;
    for (int i = 0; i < len; i++)
    {
        spell[4 + i] = payload[i];
    }
    mpu_queue_spell(spell);
}

static void mpu_simple_event(int id)
{
    uint16_t spell[6] = { 0x06, 0x04, 0x05, id, 0x00, 0x00 };
    mpu_queue_spell(spell);
}

static void mpu_switch(int sw, int on)
{
    uint16_t spell[6] = { 0x06, 0x05, 0x06, sw, on, 0x00 };
    mpu_queue_spell(spell);
}

/* exposure in stops relative to 1/100 s, f/5.6, ISO 100 (for the placeholder's brightness) */
static double exposure_stops(void)
{
    /* Canon raw codes: Tv = 8 * log2(1/t) + 56, Av = 8 * (Av APEX) + 8, ISO 100 = 0x48, 8 per stop */
    double t = (cap.tv - (8 * log2(100) + 56)) / 8.0;   /* positive: shorter */
    double a = (cap.av - (8 * 2 * log2(5.6) + 8)) / 8.0; /* positive: smaller aperture */
    double s = (cap.iso - 0x48) / 8.0;
    return -t - a + s;
}

static void release_start(void)
{
    const struct capture_profile *p = profile();
    uint8_t data[64];

    cap.shots++;
    {
        int level = 118 + (int) lround(16 * exposure_stops());
        cap.shot_level[cap.shots % COUNT(cap.shot_level)] = MAX(MIN(level, 250), 5) - 128;
    }
    cap.release_edges = 0;
    cap.releasing = 1;
    cap.exposing = 0;
    cap.bulb_end_pending = 0;
    CAP_LOG("shot %d: tv=0x%02X av=0x%02X iso=0x%02X ae=0x%02X mode=%d (%+.2f EV)\n",
            cap.shots, cap.tv, cap.av, cap.iso, cap.ae, cap.mode, exposure_stops());

    mpu_simple_event(0x01);         /* EVENTID_RELEASE_ON */
    mpu_simple_event(0x0E);         /* EVENTID_RELEASE_START */

    /* EVENTID_RELEASE_DATA_SW2ON: what the MPU is going to expose */
    assert(p->release_data_len <= sizeof(data));
    memset(data, 0, sizeof(data));
    data[0] = cap.tv;
    data[1] = cap.av;
    data[2] = cap.tv;               /* (copies seen in other cameras' logs) */
    data[3] = cap.av;
    data[4] = cap.iso ? cap.iso : 0x48;    /* auto ISO: the MPU picks one (here ISO 100) */
    data[23] = 0x92;                /* tp: temperature */
    data[25] = 0x77;                /* po */
    if (p->eshut_offset)
    {
        data[p->eshut_offset] = 1;      /* add_size */
        data[p->eshut_offset + 1] = 1;  /* electronic first curtain capture */
    }
    mpu_event(0x05, 0x02, data, p->release_data_len);
    mpu_kick();
}

static void release_end(void)
{
    const struct capture_profile *p = profile();
    uint8_t data[64];
    memset(data, 0, sizeof(data));
    data[0] = 1;
    data[1] = 1;
    mpu_event(0x05, 0x03, data, p->after_data_len); /* EVENTID_AFTER_RELEASE_DATA */
    mpu_simple_event(0x0F);                         /* EVENTID_RELEASE_END */
    cap.releasing = 0;
    if (cap.sw2_off_pending)
    {
        cap.sw2_off_pending = 0;
        mpu_simple_event(0x04);                     /* EVENTID_RELEASE_OFF_SW2OFF */
        if (cap.sw1_off_pending)
        {
            cap.sw1_off_pending = 0;
            mpu_simple_event(0x0B);                 /* EVENTID_METERING_TIMER_START_SW1OFF */
        }
    }
    mpu_kick();
}

static void shutter_press(void)
{
    mpu_switch(0x26, 1);
    mpu_simple_event(0x00);         /* EVENTID_METERING_START_SW1ON */
    release_start();
}

static void bulb_end(void)
{
    CAP_LOG("bulb end\n");
    cap.bulb_end_pending = 0;
    mpu_simple_event(0x05);         /* EVENTID_BULB_END */
    mpu_kick();
}

/* SW2 (and SW1) released; while a release is running the MPU finishes it first */
static void switch_off(int sw2, int sw1)
{
    if (cap.releasing && sw2 && cap.tv == 0x0C)
    {
        /* BULB: the exposure lasts while SW2 is held; ending it ends the accumulation. The emulated
         * release takes longer than on the camera, so a short bulb can end before the exposure has
         * started: then end it at the first VD period of the exposure */
        if (cap.exposing)
        {
            bulb_end();
        }
        else
        {
            cap.bulb_end_pending = 1;
        }
    }
    if (cap.releasing && (sw2 || cap.sw2_off_pending))
    {
        cap.sw2_off_pending |= sw2;
        cap.sw1_off_pending |= sw1;
        return;
    }
    if (sw2)
    {
        mpu_simple_event(0x04);     /* EVENTID_RELEASE_OFF_SW2OFF */
    }
    if (sw1)
    {
        mpu_simple_event(0x0B);     /* EVENTID_METERING_TIMER_START_SW1OFF */
    }
    mpu_kick();
}

static void shutter_release(void)
{
    switch_off(1, 1);
}

/* Developer aid: F5..F8 queue the MPU spells listed in $QEMU_EOS_SPELLS_DIR/f5.txt .. f8.txt
 * (one spell per line, hex bytes, '#' comments). */
static void inject_spell_file(const char *name)
{
    const char *dir = getenv("QEMU_EOS_SPELLS_DIR");
    if (!dir)
    {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f)
    {
        CAP_LOG("cannot open %s\n", path);
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f))
    {
        uint16_t spell[256];
        int n = 0;
        char *s = line;
        while (*s && *s != '#' && n < COUNT(spell))
        {
            char *end;
            unsigned long v = strtoul(s, &end, 16);
            if (end == s)
            {
                s++;
                continue;
            }
            spell[n++] = v;
            s = end;
        }
        if (n >= 2)
        {
            mpu_queue_spell(spell);
        }
    }
    fclose(f);
    mpu_kick();
    CAP_LOG("injected %s\n", path);
}

int capture_handle_key(int scancode)
{
    if (!profile())
    {
        return 0;
    }

    switch (scancode)
    {
        case 0x1D:                  /* left Ctrl: full shutter (SW1 + SW2) */
            if (!cap.sw_held)
            {
                cap.sw_held = 1;
                shutter_press();
            }
            return 1;
        case 0x9D:
            if (cap.sw_held)
            {
                cap.sw_held = 0;
                shutter_release();
            }
            return 1;
        case 0x3F: inject_spell_file("f5.txt"); return 1;
        case 0x40: inject_spell_file("f6.txt"); return 1;
        case 0x41: inject_spell_file("f7.txt"); return 1;
        case 0x42: inject_spell_file("f8.txt"); return 1;
        case 0xBF: case 0xC0: case 0xC1: case 0xC2: return 1;
    }
    return 0;
}

/* exposure settings, from the property messages either side sends */
static void track_property(const uint16_t *msg, int len)
{
    if (len < 5 || msg[2] != 0x01)
    {
        return;
    }
    switch (msg[3])
    {
        case 0x00: cap.mode = msg[4]; break;    /* PROP_SHOOTING_MODE */
        case 0x05: cap.tv   = msg[4]; break;    /* PROP_SHUTTER */
        case 0x06: cap.av   = msg[4]; break;    /* PROP_APERTURE */
        case 0x07: cap.iso  = msg[4]; break;    /* PROP_ISO */
        case 0x08: cap.ae   = msg[4]; break;    /* PROP_AE (exposure compensation) */
    }
}

/* remote release (PTP, ML's call("Release"), SW1()/SW2()): PROP_REMOTE_SW1 / PROP_REMOTE_SW2 requests;
 * the MPU answers with the same property carrying a result code (0 = done; the PTP layer reads it as
 * the outcome, nonzero = "REMOTE_RELEASE NG") and acts as if the button was pressed */
static void remote_switch(const uint16_t *msg, int len)
{
    if (len < 5 || msg[2] != 0x04 || (msg[3] != 0x16 && msg[3] != 0x17))
    {
        return;
    }
    int sw2 = msg[3] == 0x17, on = msg[4] != 0;
    CAP_LOG("remote SW%d %s\n", sw2 ? 2 : 1, on ? "on" : "off");
    uint16_t reply[16] = { 0 };
    memcpy(reply, msg, MIN(len, COUNT(reply)) * sizeof(reply[0]));
    for (int i = 4; i < MIN(len, COUNT(reply)); i++)
    {
        reply[i] = 0;
    }
    mpu_queue_spell(reply);
    if (sw2 && on)
    {
        release_start();
    }
    else if (sw2)
    {
        switch_off(1, 0);
    }
    else if (on)
    {
        mpu_simple_event(0x00);     /* EVENTID_METERING_START_SW1ON */
        mpu_kick();
    }
    else
    {
        switch_off(0, 1);
    }
}

void capture_mpu_received(const uint16_t *msg, int len)
{
    if (profile())
    {
        track_property(msg, len);
        remote_switch(msg, len);
    }
}

void capture_mpu_sent(const uint16_t *msg)
{
    if (!profile())
    {
        return;
    }
    track_property(msg, msg[0]);
}

/* ---------------------------------------------------------------------------------------- */
/* DIGIC side                                                                                */

void capture_head_start(int irq, unsigned int value)
{
    if (profile() && irq > 0 && irq < COUNT(cap.head_running))
    {
        /* only during a release: outside of it the emulator keeps its one-shot HEAD behaviour */
        cap.head_running[irq] = (value == 0xC) && cap.releasing && irq == cap.p->vd_irq;
    }
}

int capture_handle_gpio(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret)
{
    const struct capture_profile *p = profile();
    if (p && cap.trace >= 2 && cap.shots && address != p->release_gpio && (type & MODE_WRITE))
    {
        CAP_LOG("GPIO %08X <- %08X (pc %08X)\n", address, value, CURRENT_CPU->env.regs[15]);
    }
    if (!p || address != p->release_gpio)
    {
        return 0;
    }

    if (type & MODE_WRITE)
    {
        if ((cap.release_line & p->release_bit) && !(value & p->release_bit))
        {
            /* 1st falling edge: sensor ready, expose; 2nd: readout done */
            if (!(cap.release_edges++ & 1))
            {
                CAP_LOG("exposure (CAPREADY)\n");
                cap.exposing = 1;
                eos_trigger_int(p->capready_irq, 10);
            }
            else
            {
                CAP_LOG("readout done\n");
                cap.exposing = 0;
                memset(cap.head_running, 0, sizeof(cap.head_running));
                release_end();
            }
        }
        cap.release_line = value;
    }
    else
    {
        /* the firmware checks the pin mode bits before toggling bit 1 */
        *ret = cap.release_line;
    }
    return 1;
}

void capture_irq_enabled(int irq)
{
    const struct capture_profile *p = profile();
    if (!p)
    {
        return;
    }

    /* HEAD (VD) timers are periodic: while one runs, each re-enable after its handler is followed by
     * the next period (long exposures count several VD periods before the readout) */
    if (cap.head_running[irq])
    {
        /* period: long enough that Canon's HPTimer-based "last VD" timer wins, as on the camera */
        eos_trigger_int(irq, 1000);
        if (cap.bulb_end_pending && cap.exposing)
        {
            bulb_end();
        }
    }

    /* an engine operation starts with register_interrupt (enable) and its completion interrupt
     * fires once; DryOS re-enables it after the handler, which must not start another one */
    for (int i = 0; i < COUNT(p->completion_irqs) && p->completion_irqs[i]; i++)
    {
        if (p->completion_irqs[i] == irq)
        {
            if (cap.inflight[irq])
            {
                cap.inflight[irq] = 0;
            }
            else
            {
                cap.inflight[irq] = 1;
                CAP_TRACE("irq 0x%02X: operation started, completes\n", irq);
                eos_trigger_int(irq, 4);
            }
        }
    }
}

/* ---------------------------------------------------------------------------------------- */
/* JPEG core output: a valid baseline grayscale JPEG of the camera's size, uniform level     */

struct bitw
{
    uint8_t *buf;
    int len, size;
    uint32_t acc;
    int nbits;
};

static void bw_byte(struct bitw *w, uint8_t b)
{
    if (w->len == w->size)
    {
        w->size = w->size ? w->size * 2 : 65536;
        w->buf = realloc(w->buf, w->size);
        assert(w->buf);
    }
    w->buf[w->len++] = b;
}

static void bw_bits(struct bitw *w, uint32_t bits, int n)
{
    for (int i = n - 1; i >= 0; i--)
    {
        w->acc = (w->acc << 1) | ((bits >> i) & 1);
        if (++w->nbits == 8)
        {
            bw_byte(w, w->acc);
            if ((w->acc & 0xFF) == 0xFF)
            {
                bw_byte(w, 0);      /* byte stuffing */
            }
            w->acc = 0;
            w->nbits = 0;
        }
    }
}

static void bw_segment(struct bitw *w, const uint8_t *seg, int len)
{
    for (int i = 0; i < len; i++)
    {
        bw_byte(w, seg[i]);
    }
}

/* standard luminance DC table (ITU T.81 K.3) */
static const uint8_t dc_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dc_vals[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
/* minimal AC table: only EOB (0x00), code '0' */
static const uint8_t ac_bits[16] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t ac_vals[1]  = { 0x00 };

/* canonical Huffman code of a DC category */
static void dc_code(int cat, uint32_t *code, int *len)
{
    uint32_t c = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++)
    {
        for (int i = 0; i < dc_bits[l - 1]; i++, k++)
        {
            if (dc_vals[k] == cat)
            {
                *code = c;
                *len = l;
                return;
            }
            c++;
        }
        c <<= 1;
    }
    assert(0);
}

static void make_jpeg(int width, int height, int level)
{
    struct bitw w = { 0 };
    static const uint8_t soi[] = { 0xFF, 0xD8 };
    bw_segment(&w, soi, sizeof(soi));

    /* DQT right after SOI, as Canon's JPEG core writes it; unit quantizer */
    uint8_t dqt[5 + 64] = { 0xFF, 0xDB, 0x00, 67, 0x00 };
    memset(dqt + 5, 1, 64);
    bw_segment(&w, dqt, sizeof(dqt));

    uint8_t sof[] = { 0xFF, 0xC0, 0x00, 11, 8, height >> 8, height & 0xFF, width >> 8, width & 0xFF,
                      1, 1, 0x11, 0 };
    bw_segment(&w, sof, sizeof(sof));

    uint8_t dht[4 + 1 + 16 + 12 + 1 + 16 + 1];
    int n = 0;
    dht[n++] = 0xFF; dht[n++] = 0xC4; dht[n++] = 0; dht[n++] = sizeof(dht) - 2;
    dht[n++] = 0x00; memcpy(dht + n, dc_bits, 16); n += 16; memcpy(dht + n, dc_vals, 12); n += 12;
    dht[n++] = 0x10; memcpy(dht + n, ac_bits, 16); n += 16; memcpy(dht + n, ac_vals, 1); n += 1;
    assert(n == sizeof(dht));
    bw_segment(&w, dht, sizeof(dht));

    static const uint8_t sos[] = { 0xFF, 0xDA, 0x00, 8, 1, 1, 0x00, 0, 63, 0 };
    bw_segment(&w, sos, sizeof(sos));

    /* DC coefficient of a flat 8x8 block = 8 * (level - 128) with a unit quantizer */
    int dc = 8 * (level - 128);
    int blocks = ((width + 7) / 8) * ((height + 7) / 8);
    for (int b = 0; b < blocks; b++)
    {
        int diff = b ? 0 : dc;
        int mag = abs(diff), cat = 0;
        while (mag >> cat)
        {
            cat++;
        }
        uint32_t code;
        int len;
        dc_code(cat, &code, &len);
        bw_bits(&w, code, len);
        if (cat)
        {
            bw_bits(&w, diff > 0 ? diff : diff + (1 << cat) - 1, cat);
        }
        bw_bits(&w, 0, 1);          /* EOB */
    }
    if (w.nbits)
    {
        bw_bits(&w, 0x7F, 8 - w.nbits);     /* pad with 1 bits */
    }
    static const uint8_t eoi[] = { 0xFF, 0xD9 };
    bw_segment(&w, eoi, sizeof(eoi));

    free(cap.jpeg);
    cap.jpeg = w.buf;
    cap.jpeg_len = w.len;
    cap.jpeg_pos = 0;
}

void capture_jpcore_write(unsigned int offset, unsigned int value)
{
    if (offset < 0x1000 && profile())
    {
        cap.jp_regs[offset / 4] = value;
    }
}

/* placeholder output for the run the guest configured: size from JP51 0x080 (height << 16 | width),
 * mode from 0x004 (2 = lossless, the CR2 raw path: zeros of a fixed size) */
static void jpcore_new_output(void)
{
    uint32_t mode = cap.jp_regs[0x004 / 4];
    int width = cap.jp_regs[0x080 / 4] & 0xFFFF;
    int height = cap.jp_regs[0x080 / 4] >> 16;

    if ((mode & 0xF) == 2)
    {
        free(cap.jpeg);
        cap.jpeg_len = 256 * 1024;
        cap.jpeg = calloc(1, cap.jpeg_len);
        cap.jpeg_pos = 0;
        CAP_TRACE("JPCORE lossless run %dx%d: %d bytes placeholder\n", width, height, cap.jpeg_len);
        return;
    }

    /* a full-size run belongs to the next shot in order; previews/thumbnails to the last one */
    if (width >= 1000 && cap.main_runs < cap.shots)
    {
        cap.main_runs++;
        cap.last_level = cap.shot_level[cap.main_runs % COUNT(cap.shot_level)] + 128;
    }
    int level = cap.last_level ? cap.last_level : 118 + (int) lround(16 * exposure_stops());
    level = MAX(MIN(level, 250), 5);
    make_jpeg(MAX(width, 8), MAX(height, 8), level);
    CAP_LOG("JPEG %dx%d, level %d (%d bytes)\n", width, height, level, cap.jpeg_len);
}

int capture_fill_conn(int conn, void *buf, int size)
{
    if (!profile() || conn != cap.p->jpcore_conn)
    {
        return 0;
    }
    if (!cap.jpeg || cap.jpeg_done)
    {
        /* the output EDMAC starts before the JPEG core, so the first transfer of a run comes here */
        jpcore_new_output();
        cap.jpeg_done = 0;
    }
    memset(buf, 0, size);
    int n = MIN(size, cap.jpeg_len - cap.jpeg_pos);
    if (n > 0)
    {
        memcpy(buf, cap.jpeg + cap.jpeg_pos, n);
        cap.jpeg_pos += n;
    }
    return 1;
}

unsigned int capture_jpcore_irq_status(unsigned int dflt)
{
    const struct capture_profile *p = profile();
    return p && p->jpcore_irq_status ? p->jpcore_irq_status : dflt;
}

unsigned int capture_jpcore_output_size(unsigned int dflt)
{
    if (!profile() || !cap.jpeg)
    {
        return dflt;
    }
    cap.jpeg_done = 1;              /* read by the completion handler: the run is over */
    return cap.jpeg_len;
}
