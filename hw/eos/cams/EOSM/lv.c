/*
 * Canon EOS M 2.0.2: LiveView buffers for frame injection (hw/eos/lv_inject.h) and the profile's init hook.
 * Selected by eos_model_desc.init_hook (model.inc.c); see docs/models/EOSM.md, "LiveView frames".
 *
 * Addresses as the real camera reports them in LiveView (Penumbra capture kit, LVINFO.TXT; ML's EOS M
 * constants agree): Canon rotates the 720x480 display image through three buffers 0x410000 apart, and
 * the recording-size (HD) YUV buffer through two. The frames are the capture kit's LVn.422 / HDn.422
 * dumps of the same camera, stored with the ROMs (never in this repository) as LV.422 and HD.422.
 */

#include "qemu/osdep.h"
#include "hw/eos/lv_inject.h"
#include "profile.h"

static const struct lv_inject_profile lv_eosm = {
    .fps = 10,          /* virtual time; Canon runs LiveView at 30 fps, the content does not change */
    .bufs = {
        {
            .file = "LV.422", .width = 720, .height = 480, .pitch = 1440,
            .addrs = { 0x4F1D7800, 0x4F5E7800, 0x4F9F7800 },
            .follow_display = 1,
        },
        {
            .file = "HD.422", .width = 960, .height = 639, .pitch = 1920,
            .addrs = { 0x46000080, 0x463CC080 },
        },
    },
};

void eos_init_eosm(void)
{
    eos_lv_inject_start(&lv_eosm);
}
