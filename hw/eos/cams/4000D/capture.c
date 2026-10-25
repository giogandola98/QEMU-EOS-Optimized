/*
 * Canon EOS 4000D 1.1.0: still-photo capture data for hw/eos/capture.c (docs/models/capture.md).
 * Selected by eos_model_desc.capture (model.inc.c).
 */

#include "qemu/osdep.h"
#include "hw/eos/capture.h"
#include "profile.h"

const struct capture_profile capture_4000d = {
    .release_gpio       = 0xC022D0E8,       /* DIGIC 4+ style GPIO: 0x93D800 (set) / 0x83DC00 (clear) */
    .release_bit        = 0x100000,
    .capready_irq       = 0x51,             /* "CAPREADY" (register_interrupt) */
    .vd_irq             = 0x6A,             /* HEAD1 ("RegisterHead1InterruptHandler") */
    .completion_irqs    = { 0xC9, 0xD0, 0xD1 },     /* Fencing_A/B/C */
    .release_data_len   = 0x2D,
    .after_data_len     = 0x2D,
    .eshut_offset       = 0x2B,
    .jpcore_irq_status  = 0x400,
    .jpcore_conn        = 5,
    .jpcore_in_conns    = { 16, 3 },        /* YUV of the picture (top and bottom halves), thumbnail */
    /* exposure: the MPU init (600D-derived table) sends PROP_SHUTTER/APERTURE/ISO */
};
