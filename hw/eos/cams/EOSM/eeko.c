/*
 * Canon EOS M 2.0.2: the Eeko core's answers on the mailbox (hw/eos/eeko.h, docs/models/eeko.md).
 *
 * Default (eeko.c): every request is echoed on the reply channel, which is what Eeko's client tasks do
 * for the colour messages the EOS M sends, e.g. DSLR_COLOR_PROC (cmd 4) subclient 3 = LvWbAlgor,
 * messages 0x300..0x304, each answered with the same cmd/param once the job is done; Canon's handler
 * only releases the waiting task's semaphore (LvWbAlgorWrap.c asserts if that takes over 1 s).
 *
 * Still development, SS_PROC (cmd 0) subclient 1 (Eeko's StillTask), is a pipeline instead:
 *   0x100 init, 0x101 parameters, 0x12E end: no answer.
 *   Parameter blocks come in pairs, the first of each pair with a completion callback:
 *   0x102 -> 0x100, 0x105 -> 0x101, 0x108 -> 0x102, 0x10B -> 0x103, 0x10E -> 0x104, 0x111 -> 0x105,
 *   0x114/0x117/0x11A/0x11D/0x120 -> 0x106..0x10A, 0x123 -> 0x10B, 0x126 -> 0x10C;
 *   the second of each pair (0x103, 0x106, ...: what the normal still path sends) gets no answer.
 *   0x102/0x103 carry a box of five parameter structures, which Eeko frees with the box.
 *   0x12A starts development, 0x129 gives the YUV output address (a value). When both have come, Eeko
 *   answers 0x10D twice with a two-word structure in its own PComMem: { 2, 0 } when RAW -> SSRAW is done,
 *   then { 1, YUV address } when SSRAW -> YUV is done. Canon's handler frees the structure.
 * The image itself is not developed: the YUV buffer keeps what was there. This part comes from the firmware
 * and has not run yet: the EOS M's release stops earlier, in LiveView's sensor standby (docs/models/EOSM.md).
 */

#include "qemu/osdep.h"
#include "hw/eos/eos.h"
#include "hw/eos/eeko.h"
#include "profile.h"

#define SS_STILL 0x100      /* SS_PROC subclient 1: still development */

static struct {
    int started;            /* 0x12A seen */
    int have_yuv;           /* 0x129 seen */
    uint32_t yuv;
} ss;

static int reply(struct eeko_msg *r, int cmd, uint32_t param, uint32_t arg)
{
    r->ch = 3;
    r->cmd = cmd;
    r->param = param;
    r->arg = arg;
    return 1;
}

static int ss_message(const struct eeko_msg *req, struct eeko_msg *replies)
{
    uint32_t msg = req->param & 0xFF;

    if ((req->param & 0xFF00) != SS_STILL) {
        return -1;
    }
    switch (msg) {
    case 0x00:              /* init */
        ss.started = ss.have_yuv = 0;
        return 0;
    case 0x01:              /* parameters */
    case 0x2E:              /* end */
        return 0;
    case 0x2A:              /* start development */
        ss.started = 1;
        break;
    case 0x29:              /* YUV output address */
        ss.have_yuv = 1;
        ss.yuv = req->arg;
        break;
    case 0x02:
    case 0x03: {            /* box of five parameter structures */
        uint32_t box[5];
        eos_mem_read(req->arg & ~CACHING_BIT, box, sizeof(box));
        for (int i = 0; i < 5; i++) {
            if (box[i]) {
                eeko_free(box[i]);
            }
        }
        return msg == 0x02 ? reply(&replies[0], EEKO_SS, SS_STILL | 0x00, 0) : 0;
    }
    default:
        /* parameter blocks 0x105..0x127 in pairs: the first (msg % 3 == 2) answers (msg - 2) / 3 */
        if (msg >= 0x05 && msg <= 0x27) {
            return (msg % 3 == 2) ? reply(&replies[0], EEKO_SS, SS_STILL | ((msg - 2) / 3), 0) : 0;
        }
        return -1;
    }

    if (ss.started && ss.have_yuv) {
        const uint32_t raw_done[2] = { 2, 0 };
        const uint32_t yuv_done[2] = { 1, ss.yuv };
        ss.started = ss.have_yuv = 0;
        reply(&replies[0], EEKO_SS, SS_STILL | 0x0D, eeko_box(raw_done, 2));
        reply(&replies[1], EEKO_SS, SS_STILL | 0x0D, eeko_box(yuv_done, 2));
        return 2;
    }
    return 0;
}

static int eosm_message(const struct eeko_msg *req, struct eeko_msg *replies)
{
    if (req->cmd == EEKO_SS) {
        return ss_message(req, replies);
    }
    return -1;
}

const struct eeko_profile eeko_eosm = {
    .message = eosm_message,
};
