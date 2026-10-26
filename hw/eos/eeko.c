/*
 * DIGIC 5 Eeko core: high-level model of the inter-processor mailbox (hw/eos/eeko.h, docs/models/eeko.md).
 *
 * Mailbox block 0xD02C2000, eight channels of 0x40 bytes; the main CPU sends on ch0/ch1 and receives on
 * ch2/ch3 (Eeko the other way round). Registers, as the main CPU uses them:
 *
 *   +0x00  R   words waiting: in the RX FIFO (receive channel) / still in the TX FIFO (send channel)
 *   +0x04  W   TX FIFO; a message is two writes: cmd << 24 | param, then arg
 *   +0x08  W   1 = enable the "TX drained" interrupt (0x102 + 8*ch) of a send channel
 *   +0x0C  W   0 = acknowledge it
 *   +0x10  W   doorbell, 0 then 1: interrupt 0x101 + 8*ch on the other core
 *   +0x14  R   RX FIFO read, two reads per message
 *   +0x18  W   0xB = RX interrupt configuration (0x100 + 8*ch: data available)
 *   +0x20  W   1 = enable the doorbell of a receive channel; +0x24 W 0 = acknowledge it
 *   0xD02C2420 global enable (0x101 << ch per send channel), 0xD02C2424 ack of interrupt 0x124
 *
 * Eeko is held in reset by 0xC022320C until the main CPU writes 7 (6 first, while it copies the kernel
 * into Eeko's TCM). A started Eeko rings the ch2 doorbell (main interrupt 0x111) once its system task
 * is up; a stop request is the ch0 doorbell, answered with the ch2 doorbell again, after which the main
 * CPU writes 0 to 0xC022320C.
 *
 * Model: a request is answered as soon as its second word is written, after a short delay counted in
 * guest progress (eos_trigger_int_after_progress), the way Eeko's client tasks answer: the reply carries
 * the request's cmd and param back on channel 3 (system messages, cmd 8, on channel 2) with arg 0. The
 * camera profile's hook can change that per message.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "eos.h"
#include "model_list.h"
#include "eeko.h"

#define EEKO_CHANNELS   8
#define EEKO_FIFO_WORDS 64      /* software queue; the guest drains the whole count in one ISR */

/* interrupt ids on the main CPU (the same numbering as Eeko's controller) */
#define EEKO_INT_RX(ch)     (0x100 + 8 * (ch))     /* data available, to the receiver */
#define EEKO_INT_BELL(ch)   (0x101 + 8 * (ch))     /* doorbell, to the other core */
#define EEKO_INT_TXDONE(ch) (0x102 + 8 * (ch))     /* TX FIFO drained, to the sender */

#define EEKO_CH_SYSTEM  2       /* Eeko -> main: system messages and the up/stopped doorbell */
#define EEKO_CH_REPLY   3       /* Eeko -> main: client replies */

/* guest-progress steps (DIGIC_TIMER_STEP each) between a request and Eeko's answer */
#define EEKO_REPLY_STEPS 2

static const char *const client_names[] = {
    [EEKO_SS] = "SS_PROC", [EEKO_IVA] = "IVA_PROC", [EEKO_APP1] = "APP1_PROC", [EEKO_JP62] = "JP62_PROC",
    [EEKO_COLOR] = "DSLR_COLOR_PROC", [EEKO_SAF] = "DSLR_SAF_PROC", [EEKO_CREATIVE] = "CREATIVEFILTER_PROC",
    [EEKO_RECOGNITION] = "RECOGNITION_PROC", [EEKO_SYSTEM] = "system", [EEKO_SEQUENCE] = "SEQUENCE_PROC",
};

static struct {
    uint32_t fifo[EEKO_CHANNELS][EEKO_FIFO_WORDS];
    int head[EEKO_CHANNELS], count[EEKO_CHANNELS];
    uint32_t tx_word0[EEKO_CHANNELS];   /* first word of a request being written */
    int tx_half[EEKO_CHANNELS];         /* 1 = waiting for the request's second word */
    uint32_t bell[EEKO_CHANNELS];       /* last doorbell value written (edge detection) */
    uint32_t global_enable;             /* 0xD02C2420 */
    uint32_t clock;                     /* 0xC022320C: 0 off, 6 clock on / in reset, 7 running */
    uint32_t clock_div;                 /* 0xC0223210 */
    unsigned int requests, replies;
    uint32_t seen[256];                 /* messages already logged (ch << 28 | cmd << 24 | param) */
    int nseen;
} ek;

static const char *client_name(int cmd)
{
    return (cmd < (int)ARRAY_SIZE(client_names) && client_names[cmd]) ? client_names[cmd] : "?";
}

/* The first message of each kind (ch, cmd, param), so a run's log shows which clients used Eeko without
 * one line per LiveView frame; every message with -d io or QEMU_EOS_EEKO_TRACE=1. */
static void log_msg(const struct eeko_msg *m, const char *dir, const char *note)
{
    static int trace = -1;
    if (trace < 0) {
        const char *env = getenv("QEMU_EOS_EEKO_TRACE");
        trace = env && atoi(env);
    }
    uint32_t key = (uint32_t)m->ch << 28 | (uint32_t)(m->cmd & 0xF) << 24 | (m->param & 0xFFFFFF);
    if (!trace && !qemu_loglevel_mask(EOS_LOG_IO)) {
        for (int i = 0; i < ek.nseen; i++) {
            if (ek.seen[i] == key) {
                return;
            }
        }
        if (ek.nseen == ARRAY_SIZE(ek.seen)) {
            return;
        }
        ek.seen[ek.nseen++] = key;
    }
    fprintf(stderr, "[EEKO] ch%d %s %s %06X arg %08X%s\n", m->ch, dir, client_name(m->cmd), m->param, m->arg, note);
}

static void fifo_push(int ch, uint32_t word)
{
    if (ek.count[ch] == EEKO_FIFO_WORDS) {
        fprintf(stderr, "[EEKO] ch%d RX FIFO full, word %08X dropped\n", ch, word);
        return;
    }
    ek.fifo[ch][(ek.head[ch] + ek.count[ch]) % EEKO_FIFO_WORDS] = word;
    ek.count[ch]++;
}

static uint32_t fifo_pop(int ch)
{
    if (!ek.count[ch]) {
        return 0;
    }
    uint32_t word = ek.fifo[ch][ek.head[ch]];
    ek.head[ch] = (ek.head[ch] + 1) % EEKO_FIFO_WORDS;
    ek.count[ch]--;
    return word;
}

/* ---- shared command memory (PComMem) ---- */

#define EEKO_BOOT_BLOCK     0x01F80000  /* filled by the main CPU before it starts Eeko */
#define EEKO_BOX_SLOT       0x40        /* reply structures: header + up to 15 words */

static void pcm_region(int eeko_side, uint32_t *base, uint32_t *size)
{
    uint32_t w[2];
    eos_mem_read(EEKO_BOOT_BLOCK + (eeko_side ? 0x30 : 0x28), w, sizeof(w));
    *base = w[0] & ~CACHING_BIT;
    *size = w[1];
}

void eeko_free(uint32_t addr)
{
    uint32_t base, size;
    pcm_region(0, &base, &size);
    addr &= ~CACHING_BIT;
    if (addr < base + 4 || addr >= base + size) {
        return;                 /* not a pool structure (a table, a buffer address, a value) */
    }
    uint8_t in_use;
    eos_mem_read(addr - 4, &in_use, 1);
    if (in_use) {
        in_use = 0;
        eos_mem_write(addr - 4, &in_use, 1);
    }
}

uint32_t eeko_box(const uint32_t *words, int count)
{
    uint32_t base, size;
    pcm_region(1, &base, &size);
    assert(count <= EEKO_BOX_SLOT / 4 - 1);
    for (uint32_t slot = base; size && slot + EEKO_BOX_SLOT <= base + size; slot += EEKO_BOX_SLOT) {
        uint8_t hdr[4];
        eos_mem_read(slot, hdr, 4);
        if (!hdr[0]) {
            hdr[0] = 1;
            hdr[3] = 1;
            eos_mem_write(slot, hdr, 4);
            eos_mem_write(slot + 4, (void *)words, count * 4);
            return slot + 4;
        }
    }
    fprintf(stderr, "[EEKO] no free reply structure in Eeko's PComMem (0x%X, 0x%X bytes)\n", base, size);
    return 0;
}

static void eeko_send_to_main(const struct eeko_msg *m)
{
    fifo_push(m->ch, (uint32_t)m->cmd << 24 | (m->param & 0xFFFFFF));
    fifo_push(m->ch, m->arg);
    ek.replies++;
    log_msg(m, "<-", "");
    eos_trigger_int_after_progress(EEKO_INT_RX(m->ch), EEKO_REPLY_STEPS);
}

static void eeko_request(int ch, uint32_t word0, uint32_t arg)
{
    struct eeko_msg req = { .ch = ch, .cmd = word0 >> 24, .param = word0 & 0xFFFFFF, .arg = arg };
    struct eeko_msg replies[EEKO_MAX_REPLIES];
    int n = -1;

    ek.requests++;
    log_msg(&req, "->", ek.clock == 7 ? "" : " (Eeko not running)");

    /* the request left the TX FIFO: the sender's semaphore slot comes back */
    eos_trigger_int(EEKO_INT_TXDONE(ch), 1);

    const struct eeko_profile *prof = eos_state->model->eeko;
    if (prof->message) {
        n = prof->message(&req, replies);
    }
    if (n < 0) {
        /* Eeko's client tasks answer with the request's own cmd/param (EekoApp ColorTask & co.) */
        replies[0] = req;
        replies[0].ch = (req.cmd == EEKO_SYSTEM) ? EEKO_CH_SYSTEM : EEKO_CH_REPLY;
        replies[0].arg = 0;
        n = 1;
    }
    /* Eeko copies a request's structure and frees it (byte 0 of its PComMem header) */
    if (arg) {
        eeko_free(arg);
    }
    for (int i = 0; i < n && i < EEKO_MAX_REPLIES; i++) {
        eeko_send_to_main(&replies[i]);
    }
}

static void eeko_doorbell(int ch)
{
    /* main -> Eeko doorbells: ch0 = stop request. Eeko resets its engine channels and answers with
     * its ch2 doorbell; the main CPU then gates it with 0xC022320C = 0. */
    if (ch == 0) {
        fprintf(stderr, "[EEKO] stop request (%u requests, %u replies so far)\n", ek.requests, ek.replies);
        eos_trigger_int_after_progress(EEKO_INT_BELL(EEKO_CH_SYSTEM), EEKO_REPLY_STEPS);
    } else {
        fprintf(stderr, "[EEKO] doorbell ch%d (no model)\n", ch);
    }
}

/* upstream logging of the block (models without an Eeko profile) */
static unsigned int eeko_log_only(unsigned int address, unsigned char type, unsigned int value)
{
    const char *msg = "INT%Xh: ???";
    /* upstream numbering: even halves of a 0x40 block are Eeko -> ICU, odd ones ICU -> Eeko */
    static const int interrupt_map[] = {
        0x101, 0x109, 0x111, 0x119, 0x121, 0x129, 0x131, 0x139,
        0x0FF, 0x107, 0x10F, 0x117, 0x11F, 0x127, 0x12F, 0x137,
        0x123,
    };
    int interrupt_index = (address >> 5) & 0x3F;
    assert(interrupt_index / 2 < ARRAY_SIZE(interrupt_map));
    int interrupt_id = interrupt_map[interrupt_index / 2] + interrupt_index % 2;

    switch (address & 0x1F) {
    case 0x04: msg = "INT%Xh: interrupt acknowledged"; break;
    case 0x08: msg = "INT%Xh: setup interrupts? (1)"; break;
    case 0x10: msg = "INT%Xh: trigger interrupt?"; break;
    case 0x18: msg = "INT%Xh: setup interrupts? (B)"; break;
    }
    io_log("EEKO", address, type, value, 0, msg, interrupt_id, 0);
    return 0;
}

/* the model is on for cameras with an Eeko profile, unless QEMU_EOS_EEKO=0 (A/B runs) */
static bool eeko_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("QEMU_EOS_EEKO");
        enabled = eos_state->model->eeko && !(env && !atoi(env));
    }
    return enabled;
}

unsigned int eeko_handle_comm(unsigned int address, unsigned char type, unsigned int value)
{
    if (!eeko_enabled()) {
        return eeko_log_only(address, type, value);
    }

    unsigned int off = address & 0xFFF;
    unsigned int ret = 0;
    const char *msg = NULL;
    int ch = (off >> 6) & 7;

    if (off >= 0x400) {
        switch (off) {
        case 0x420:
            msg = "global enable";
            if (type & MODE_WRITE) {
                ek.global_enable = value;
            } else {
                ret = ek.global_enable;
            }
            break;
        case 0x424:
            msg = "INT 124h ack";
            break;
        }
        io_log("EEKO", address, type, value, ret, msg, 0, 0);
        return ret;
    }

    switch (off & 0x3F) {
    case 0x00:
        msg = "ch%d: words waiting";
        /* send channels: the model consumes requests at once, so their FIFO always reads empty */
        ret = ek.count[ch];
        break;
    case 0x04:
        msg = "ch%d: TX FIFO";
        if (type & MODE_WRITE) {
            if (!ek.tx_half[ch]) {
                ek.tx_word0[ch] = value;
                ek.tx_half[ch] = 1;
            } else {
                ek.tx_half[ch] = 0;
                eeko_request(ch, ek.tx_word0[ch], value);
            }
        }
        break;
    case 0x08:
        msg = "ch%d: TX-drained interrupt enable";
        break;
    case 0x0C:
        msg = "ch%d: TX-drained ack";
        break;
    case 0x10:
        msg = "ch%d: doorbell";
        if (type & MODE_WRITE) {
            if (value && !ek.bell[ch]) {
                eeko_doorbell(ch);
            }
            ek.bell[ch] = value;
        }
        break;
    case 0x14:
        msg = "ch%d: RX FIFO";
        if (!(type & MODE_WRITE)) {
            ret = fifo_pop(ch);
        }
        break;
    case 0x18:
        msg = "ch%d: RX interrupt config";
        break;
    case 0x20:
        msg = "ch%d: doorbell enable";
        break;
    case 0x24:
        msg = "ch%d: doorbell ack";
        break;
    }
    io_log("EEKO", address, type, value, ret, msg, ch, 0);
    return ret;
}

int eeko_handle_gpio(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret)
{
    if (!eeko_enabled()) {
        return 0;
    }

    switch (address & 0xFFFF) {
    case 0x320C:
        /* reads back what was written: the real EOS M reads 7 whenever Eeko runs */
        if (type & MODE_WRITE) {
            if (value == 7 && ek.clock != 7) {
                fprintf(stderr, "[EEKO] started\n");
                /* boot: Eeko's system task rings its ch2 doorbell once it is up */
                eos_trigger_int_after_progress(EEKO_INT_BELL(EEKO_CH_SYSTEM), EEKO_REPLY_STEPS);
            }
            if (value != 7) {
                /* held in reset: whatever was in flight is lost */
                memset(ek.count, 0, sizeof(ek.count));
                memset(ek.tx_half, 0, sizeof(ek.tx_half));
            }
            ek.clock = value;
        }
        *ret = ek.clock;
        io_log("EEKO", address, type, value, *ret,
               ek.clock == 7 ? "Eeko clock/reset: running" : "Eeko clock/reset: held", 0, 0);
        return 1;
    case 0x3210:
        if (type & MODE_WRITE) {
            ek.clock_div = value;
        }
        *ret = ek.clock_div;
        io_log("EEKO", address, type, value, *ret, "Eeko clock setting", 0, 0);
        return 1;
    }
    return 0;
}
