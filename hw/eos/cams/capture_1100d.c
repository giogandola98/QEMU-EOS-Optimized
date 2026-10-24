/*
 * Canon EOS 1100D 1.0.5: still-photo capture data for hw/eos/capture.c (docs/models/capture.md).
 * Selected by eos_model_desc.capture (model_list.c).
 */

#include "qemu/osdep.h"
#include "hw/eos/capture.h"

const struct capture_profile capture_1100d = {
    .release_gpio       = 0xC02200EC,       /* DIGIC 4 style GPIO: bits 2-3 = mode, bit 1 = level */
    .release_bit        = 0x2,
    .capready_irq       = 0x51,             /* "CAPREADY" (register_interrupt) */
    .vd_irq             = 0x6A,             /* HEAD1 ("RegisterHead1InterruptHandler") */
    .completion_irqs    = { 0xC9, 0xD0, 0xD1 },     /* Fencing_A/B/C */
    .release_data_len   = 0x2D,             /* the firmware reads [0x2B] = add_size, [0x2C]: EShutCapture */
    .after_data_len     = 0x2D,             /* [0x2C] = add_size */
    .eshut_offset       = 0x2B,
    .jpcore_irq_status  = 0x400,            /* JpCore ISR: bit 10 = complete (else the error callback) */
    .jpcore_conn        = 5,
    .init_tv            = 0x4D,             /* 1/6 s, f/18, ISO 100: the settings Canon restores in the */
    .init_av            = 0x4B,             /* CI ROM image (the 60D-derived MPU init table has no */
    .init_iso           = 0x48,             /* exposure properties) */
};
