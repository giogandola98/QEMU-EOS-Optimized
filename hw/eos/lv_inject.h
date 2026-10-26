#ifndef HW_EOS_LV_INJECT_H
#define HW_EOS_LV_INJECT_H

/*
 * LiveView frame injection (docs/PLAN.md "LiveView simulation", option A).
 *
 * Canon's LiveView pipeline does not run in the emulator (no sensor, no VD/EDMAC events for it), so the
 * YUV buffers stay at whatever Canon cleared them to. This writes frames captured on the real camera
 * into those buffers at a fixed virtual-time rate, so the display's image layer and anything that reads
 * the buffers (ML overlays: histogram, zebras, focus peaking) see real data. Nothing of Canon's pipeline
 * is simulated: no frame events are raised.
 *
 * A camera profile describes its buffers (struct lv_inject_profile) and starts injection from its init
 * hook with eos_lv_inject_start(). Frame files are raw YUV 4:2:2 dumps (as ML's / the capture kit's
 * LVn.422, HDn.422), read from the camera's ROM directory (eos_get_cam_path); they are never part of this
 * repository. A missing file disables that buffer (message on stderr), so without frames the camera
 * behaves exactly as before. QEMU_EOS_LV_INJECT=0 disables injection altogether.
 */

#include <stdint.h>

#define LV_INJECT_MAX_ADDRS     4
#define LV_INJECT_MAX_BUFS      3

struct lv_inject_buffer {
    const char *file;                       /* frame file name in the ROM dir, e.g. "LV.422" */
    uint16_t width, height;                 /* pixels */
    uint16_t pitch;                         /* bytes per line, in the file and in RAM */
    uint32_t addrs[LV_INJECT_MAX_ADDRS];    /* every buffer Canon rotates through (0-terminated) */
    int follow_display;                     /* also write it where the display's YUV VRAM points */
};

struct lv_inject_profile {
    int fps;                                /* injection rate in virtual time */
    struct lv_inject_buffer bufs[LV_INJECT_MAX_BUFS];   /* unused entries: file == NULL */
};

void eos_lv_inject_start(const struct lv_inject_profile *profile);

#endif
