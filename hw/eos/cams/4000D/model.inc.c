/*
 * Canon EOS 4000D / Rebel T100 / 3000D, fw 1.1.0, model ID 0x422: entry of eos_model_list[].
 * Included by model_list.c inside the table, in the DIGIC 4 group: zero fields take the DIGIC 4
 * defaults. Profile layout: docs/models/PROFILES.md.
 */
        /* DIGIC 4+ like the 1300D, whose values are reused unless noted (starting point: the 2000D
         * sketch in qemu-eos branch 2000d_test by stephen-e). See docs/models/4000D.md. */
        .name                   = MODEL_NAME_4000D,
        .pm_handler             = eos_pm_4000d, /* PowerMgr idle loop + selfRefresh (pm.c) */
        .capture                = &capture_4000d, /* shutter release (capture.c) */
        .mpu_spells             = MODEL_NAME_600D, /* as the 1300D: 600D spells, 1200D buttons */
        .button_codes           = MODEL_NAME_1200D,
        .flash_id               = 0x003825C2,   /* C2 25 38: 16MB part (FE0C1BC0, assert Startup.c:220) */
        .quirks                 = EOS_QUIRK_JPCORE_READY | EOS_QUIRK_JPCORE_IRQ_DONE |
                                  EOS_QUIRK_GPIO_F48C_HDMI,   /* as the 1300D */
        .digic_version          = 4,
        .ram_size               = 0x10000000,   /* 256MB */
        .rom0_size              = 0,            /* unpopulated: the dump is a 4-byte bus pattern */
        .rom1_size              = 0x01000000,   /* 16MB; the 32MB dump holds two identical copies */
        .firmware_start         = 0xFF0C0000,   /* ROMBASEADDR; the bootloader jumps to the FE0C0000 mirror */
        .rom1_hivecs_offset     = 0x00010000,   /* bootloader vectors point to 0xFFFF0040... */
        .dryos_timer_id         = 1,
        .dryos_timer_interrupt  = 0x09,
        .mpu_request_register   = 0xC022D0C4,
        .mpu_request_bitmask    = 0x00100000,
        .mpu_status_register    = 0xC022F484,
        .current_task_addr      = 0x30978,      /* -d tasks names all tasks (1300D: 0x31170) */
        .sd_driver_interrupt    = 0x4B,
        .sd_dma_interrupt       = 0x32,
        .uart_rx_interrupt      = 0x38,
        .rtc_time_correct       = 0x31,         /* PROP_RTC in this ROM (log: "0xfd ---> 0x31" with 0xFD) */
        .rtc_cs_register        = 0xC022D0B8,
        .dedicated_movie_mode   = 1,
