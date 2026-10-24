/*
 * Canon EOS M: entry of eos_model_list[]. Included by model_list.c inside the table, in the DIGIC 5
 * group: zero fields take the DIGIC 5 defaults. No hooks yet (generic behaviour, own MPU spells and
 * button codes). Profile layout: docs/models/PROFILES.md.
 */
        .name                   = MODEL_NAME_EOSM,
        .digic_version          = 5,
        .ram_size               = 0x10000000,   /* 256MB */
        .current_task_addr      = 0x3DE78,
        .card_led_address       = 0xC022C188,
        .serial_flash_size      = 0x800000,
        .rtc_cs_register        = 0xC022C0C4,
        .rtc_time_correct       = 0x98,
        .dedicated_movie_mode   = 0,
