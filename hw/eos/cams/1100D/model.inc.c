/*
 * Canon EOS 1100D / Rebel T3 / Kiss X50, fw 1.0.5: entry of eos_model_list[]. Included by
 * model_list.c inside the table, in the DIGIC 4 group: zero fields take the DIGIC 4 defaults.
 * Profile layout: docs/models/PROFILES.md.
 */
        .name                   = MODEL_NAME_1100D,
        .pm_handler             = eos_pm_1100d, /* PowerMgr idle loop + selfRefresh (pm.c) */
        .capture                = &capture_1100d, /* shutter release (capture.c) */
        .mpu_spells             = MODEL_NAME_60D, /* no own set yet; the 60D spells boot the GUI */
        .quirks                 = EOS_QUIRK_DISP_YUV_HALF |
                                  EOS_QUIRK_JPCORE_READY, /* JpCore init waits for it, see engine.c */
        .digic_version          = 4,
        .ram_size               = 0x08000000,   /* 128MB */
        .current_task_addr      = 0x1A2C,
        .rtc_time_correct       = 0x8D,
        .dedicated_movie_mode   = 1,
