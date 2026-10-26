/*
 * Canon EOS M, fw 2.0.2, model ID 0x331: entry of eos_model_list[]. Included by model_list.c inside
 * the table, in the DIGIC 5 group: zero fields take the DIGIC 5 defaults. Own MPU spells and button
 * codes; needs SFDATA.BIN (serial flash with the property packages); LV.422 / HD.422 (optional) are
 * injected into the LiveView buffers (lv.c).
 * Profile layout: docs/models/PROFILES.md; what was fixed and why: docs/models/EOSM.md.
 */
        .name                   = MODEL_NAME_EOSM,
        .init_hook              = eos_init_eosm,    /* LiveView frame injection (lv.c) */
        .eeko                   = &eeko_eosm,       /* Eeko mailbox answers (eeko.c) */
        .quirks                 = EOS_QUIRK_I2C_TX_READY    /* audio IC init (InitializeAudioIC) */
                                | EOS_QUIRK_RAM_GAP_READ0,  /* display log past its state names */
        .digic_version          = 5,
        .ram_size               = 0x10000000,   /* 256MB */
        .current_task_addr      = 0x3DE78,
        .card_led_address       = 0xC022C188,
        .serial_flash_size      = 0x800000,
        .rtc_cs_register        = 0xC022C0C4,
        .rtc_time_correct       = 0x98,
        .dedicated_movie_mode   = 0,
