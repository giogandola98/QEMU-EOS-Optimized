/*
 * Canon EOS M 2.0.2: the Eeko core's answers on the mailbox (hw/eos/eeko.h, docs/models/eeko.md).
 *
 * The generic model echoes every request on the reply channel, which is what Eeko's client tasks do
 * for the messages the EOS M sends in LiveView: e.g. DSLR_COLOR_PROC (cmd 4) subclient 3 = LvWbAlgor,
 * messages 0x300..0x304, each answered with the same cmd/param once the job is done; Canon's handler
 * only releases the waiting task's semaphore (LvWbAlgorWrap.c asserts if that takes over 1 s).
 */

#include "qemu/osdep.h"
#include "hw/eos/eeko.h"
#include "profile.h"

const struct eeko_profile eeko_eosm = {
    .message = NULL,
};
