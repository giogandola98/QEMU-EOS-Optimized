#ifndef EOS_EEKO_H
#define EOS_EEKO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * High-level model of the DIGIC 5 "Eeko" image-processing core, as the main CPU sees it.
 *
 * Eeko is a second ARM core (Cortex-R4 class, Thumb-2, its own DryOS) that Canon starts on demand for
 * white balance, colour calculation, still-image development and codec jobs. Its code is not run here:
 * this model answers the main CPU on the inter-processor mailbox at 0xD02C2000 the way Eeko's message
 * loop does, so that Canon's clients get their completion messages. The image work itself is not done
 * (result buffers keep what the main CPU put there). See docs/models/eeko.md.
 *
 * The generic part (eeko.c) is the mailbox and the start/stop handshake. A camera profile
 * (hw/eos/cams/<CAM>/eeko.c) selects it with eos_model_desc.eeko and may add a per-message hook for
 * replies that are more than an echo of the request.
 */

/* one mailbox message: two words, word 0 = cmd << 24 | param (param = subclient << 8 | message) */
struct eeko_msg {
    uint8_t ch;         /* mailbox channel */
    uint8_t cmd;        /* client (postman command) */
    uint32_t param;     /* 24 bits */
    uint32_t arg;       /* second word, usually a pointer (uncached bit cleared) */
};

#define EEKO_MAX_REPLIES 8

struct eeko_profile {
    /* Optional: called for every request from the main CPU. Fill replies[] and return their count
     * (0 = no answer), or return -1 for the default answer (the request echoed with arg 0 on the
     * client's reply channel). */
    int (*message)(const struct eeko_msg *req, struct eeko_msg *replies);
};

/* client numbers (postman cmd) and their names, as Canon's PostPostman uses them */
enum {
    EEKO_SS = 0, EEKO_IVA = 1, EEKO_APP1 = 2, EEKO_JP62 = 3, EEKO_COLOR = 4, EEKO_SAF = 5,
    EEKO_CREATIVE = 6, EEKO_RECOGNITION = 7, EEKO_SYSTEM = 8, EEKO_SEQUENCE = 10,
};

/* MMIO 0xD02C2000..0xD02C243F (all models; without an Eeko profile it only logs, as upstream) */
unsigned int eeko_handle_comm(unsigned int address, unsigned char type, unsigned int value);

/* GPIO block hook for 0xC022320C / 0xC0223210 (Eeko clock and reset); 1 if handled */
int eeko_handle_gpio(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret);

#endif
