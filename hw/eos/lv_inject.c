/*
 * LiveView frame injection: see lv_inject.h.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "hw/eos/eos.h"
#include "hw/eos/lv_inject.h"

static struct {
    const struct lv_inject_profile *p;
    uint8_t *frame[LV_INJECT_MAX_BUFS];
    size_t size[LV_INJECT_MAX_BUFS];
    QEMUTimer *timer;
    int64_t period_ns;
    uint64_t ticks;
    uint32_t last_display;
} lvi;

static int in_ram(uint32_t addr, size_t size)
{
    uint32_t phys = addr & ~CACHING_BIT;
    return phys >= 0x1000 && (uint64_t) phys + size <= eos_state->model->ram_size;
}

static void lvi_write(int i, uint32_t addr)
{
    if (in_ram(addr, lvi.size[i])) {
        cpu_physical_memory_write(addr & ~CACHING_BIT, lvi.frame[i], lvi.size[i]);
    }
}

static void lvi_tick(void *opaque)
{
    const struct lv_inject_profile *p = lvi.p;
    uint32_t disp = eos_state->disp.img_vram;

    for (int i = 0; i < LV_INJECT_MAX_BUFS; i++) {
        if (!lvi.frame[i]) {
            continue;
        }
        const struct lv_inject_buffer *b = &p->bufs[i];
        int disp_known = 0;
        for (int k = 0; k < LV_INJECT_MAX_ADDRS && b->addrs[k]; k++) {
            lvi_write(i, b->addrs[k]);
            disp_known |= ((b->addrs[k] ^ disp) & ~CACHING_BIT) == 0;
        }
        /* The display may scan a buffer the profile does not list (emulated start-up differs from the
         * camera's); the image layer must still show the frame. */
        if (b->follow_display && disp && !disp_known) {
            lvi_write(i, disp);
            if (disp != lvi.last_display) {
                fprintf(stderr, "[LV] display YUV VRAM at 0x%08X (not in the profile): frame written there too\n",
                        disp);
            }
        }
    }
    lvi.last_display = disp;
    lvi.ticks++;
    timer_mod(lvi.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + lvi.period_ns);
}

void eos_lv_inject_start(const struct lv_inject_profile *p)
{
    const char *env = getenv("QEMU_EOS_LV_INJECT");
    if (env && env[0] == '0') {
        fprintf(stderr, "[LV] frame injection disabled (QEMU_EOS_LV_INJECT=0)\n");
        return;
    }

    int loaded = 0;
    lvi.p = p;
    for (int i = 0; i < LV_INJECT_MAX_BUFS; i++) {
        const struct lv_inject_buffer *b = &p->bufs[i];
        if (!b->file) {
            continue;
        }
        const char *path = eos_get_cam_path(b->file);
        size_t want = (size_t) b->pitch * b->height;
        gchar *data = NULL;
        gsize len = 0;
        if (!g_file_get_contents(path, &data, &len, NULL)) {
            fprintf(stderr, "[LV] no frame %s: buffer not injected\n", path);
            continue;
        }
        if (len != want) {
            fprintf(stderr, "[LV] %s: %zu bytes, expected %zu (%dx%d, pitch %d): not injected\n",
                    path, (size_t) len, want, b->width, b->height, b->pitch);
            g_free(data);
            continue;
        }
        lvi.frame[i] = (uint8_t *) data;
        lvi.size[i] = want;
        loaded++;
        fprintf(stderr, "[LV] frame injection: %s %dx%d into", b->file, b->width, b->height);
        for (int k = 0; k < LV_INJECT_MAX_ADDRS && b->addrs[k]; k++) {
            fprintf(stderr, " 0x%08X", b->addrs[k]);
        }
        fprintf(stderr, "%s at %d fps\n", b->follow_display ? " + display YUV VRAM" : "", p->fps);
    }
    if (!loaded) {
        return;
    }

    lvi.period_ns = NANOSECONDS_PER_SECOND / (p->fps > 0 ? p->fps : 1);
    lvi.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lvi_tick, NULL);
    timer_mod(lvi.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + lvi.period_ns);
}
