#ifndef EOS_MODEL_LIST_H
#define EOS_MODEL_LIST_H

#include <stdint.h>
#include <stdbool.h>

#define MODEL_NAME_5D "5D"
#define MODEL_NAME_400D "400D"
#define MODEL_NAME_40D "40D"
#define MODEL_NAME_450D "450D"
#define MODEL_NAME_1000D "1000D"
#define MODEL_NAME_50D "50D"
#define MODEL_NAME_5D2 "5D2"
#define MODEL_NAME_500D "500D"
#define MODEL_NAME_550D "550D"
#define MODEL_NAME_7D "7D"
#define MODEL_NAME_60D "60D"
#define MODEL_NAME_600D "600D"
#define MODEL_NAME_1100D "1100D"
#define MODEL_NAME_1200D "1200D"
#define MODEL_NAME_1300D "1300D"
#define MODEL_NAME_4000D "4000D"
#define MODEL_NAME_A1100 "A1100"
#define MODEL_NAME_5D3 "5D3"
#define MODEL_NAME_5D3eeko "5D3eeko"
#define MODEL_NAME_6D "6D"
#define MODEL_NAME_650D "650D"
#define MODEL_NAME_700D "700D"
#define MODEL_NAME_EOSM "EOSM"
#define MODEL_NAME_EOSM2 "EOSM2"
#define MODEL_NAME_100D "100D"
#define MODEL_NAME_70D "70D"
#define MODEL_NAME_80D "80D"
#define MODEL_NAME_750D "750D"
#define MODEL_NAME_760D "760D"
#define MODEL_NAME_7D2 "7D2"
#define MODEL_NAME_7D2S "7D2S"
#define MODEL_NAME_5D4 "5D4"
#define MODEL_NAME_5D4AE "5D4AE"
#define MODEL_NAME_EOSM3 "EOSM3"
#define MODEL_NAME_EOSM10 "EOSM10"
#define MODEL_NAME_200D "200D"
#define MODEL_NAME_6D2 "6D2"
#define MODEL_NAME_77D "77D"
#define MODEL_NAME_800D "800D"
#define MODEL_NAME_850D "850D"
#define MODEL_NAME_90D "90D"
#define MODEL_NAME_EOSM5 "EOSM5"
#define MODEL_NAME_EOSM50 "M50"
#define MODEL_NAME_EOSM6mk2 "M6mk2"
#define MODEL_NAME_EOSRP "RP"
#define MODEL_NAME_EOSR "R"
#define MODEL_NAME_EOSR5 "R5"
#define MODEL_NAME_EOSR6 "R6"
#define MODEL_NAME_SX740 "SX740"
#define MODEL_NAME_SX70 "SX70"
#define MODEL_NAME_XF605 "XF605"

enum { ram_extra_array_len = 2 };

/* Per-camera power-management register model (PowerMgr idle loop, selfRefresh; hw/eos/pm.h).
 * Called by eos_handler() before the regular MMIO handlers: true = access handled (*ret = read
 * value), false = pass it on. NULL (most models): no PM model. */
typedef bool (*eos_pm_handler_fn)(unsigned int address, unsigned char type, unsigned int value,
                                  unsigned int *ret);

struct capture_profile;              /* still-photo capture data, hw/eos/capture.h */

/*
 * Hardware differences that used to be checked by model name in the shared code. Set in .quirks of
 * a model entry; each one replaces exactly one former name check, so a model without it gets the
 * generic (upstream) behaviour. Test with EOS_QUIRK(NAME) (eos.h).
 */
enum eos_quirk {
    EOS_QUIRK_FLASH_CTRL_RO     = 1 << 0,   /* writes to the flash command registers 0xF8000AAA/0x554
                                             * are dropped instead of changing the ROM (A1100) */
    EOS_QUIRK_DISP_LINE_DOUBLE  = 1 << 1,   /* 720x240 LCD shown line-doubled, 720-byte bitmap pitch
                                             * after a palette write, display reg 0x014 reads 4 (A1100;
                                             * DIGIC 2/3 models get the line doubling anyway) */
    EOS_QUIRK_DISP_YUV_HALF     = 1 << 2,   /* LiveView YUV buffer has half the display height (1100D) */
    EOS_QUIRK_DISP_BMP_YUV8B    = 1 << 3,   /* no YUV overlay: the bitmap is drawn as 8-bit YUV
                                             * (EOSM3, M50, RP) */
    EOS_QUIRK_DISP_BMP_PAD16    = 1 << 4,   /* DIGIC 6/7 BMP VRAM register: 0x17410/0x18010 do not move
                                             * the buffer, pitch = (width + 16) * 2 (EOSM3) */
    EOS_QUIRK_RTC_PS            = 1 << 5,   /* PowerShot RTC: CS = GPIO bit 11, own protocol on SIO2
                                             * (0xC08202xx), GPIO 0x68 = RTC com, D10 register values (A1100) */
    EOS_QUIRK_RTC_NIBBLE_SWAP   = 1 << 6,   /* RTC command byte: command in the high nibble, register in
                                             * the low one (5D2, 50D, 40D) */
    EOS_QUIRK_RTC_REG0_0C       = 1 << 7,   /* RTC register 0 reads 0xC (protocol unknown, brings the
                                             * GUI; 400D) */
    EOS_QUIRK_PS_IS             = 1 << 8,   /* PowerShot image stabiliser: SIO 0xC08204xx and init
                                             * registers 0xC0500040-58 (A1100) */
    EOS_QUIRK_PS_KEYS           = 1 << 9,   /* PowerShot keys: GPIO 0x134 start key, 0x138 playback
                                             * start, 0x200-0x208 physw bits (A1100) */
    EOS_QUIRK_SD_ACMD41_REPLAY  = 1 << 10,  /* answer the repeated CMD55/ACMD41 from the first one
                                             * instead of sd.c (A1100) */
    EOS_QUIRK_SD_IRQ_EN_ONLY    = 1 << 11,  /* SDIO 0x14 only stores the IRQ mask, transfers end via
                                             * SDDMA 0x10 (A1100) */
    EOS_QUIRK_UART_TIO_KEEP     = 1 << 12,  /* UART control writes keep the TX interrupt enable (EOSM3) */
    EOS_QUIRK_I2C_RANDOM        = 1 << 13,  /* I2C status 0xD6050000 reads random values (EOSM10) */
    EOS_QUIRK_JPCORE_READY      = 1 << 14,  /* JPEG core 0x00 reads 0x1010000 (ready after reset):
                                             * 1300D, 4000D, A1100, 1100D */
    EOS_QUIRK_JPCORE_IRQ_DONE   = 1 << 15,  /* JPEG core 0x44 reads 0x400 (done) instead of random
                                             * (1300D, 4000D, A1100) */
    EOS_QUIRK_GPIO_11C_LOW      = 1 << 16,  /* GPIO 0x011C (VIDEO CONNECT) reads 0 (40D) */
    EOS_QUIRK_GPIO_VSW_6000     = 1 << 17,  /* GPIO 0xF480 (VSW status) reads 0x6000 (5D2, 50D) */
    EOS_QUIRK_GPIO_F48C_HDMI    = 1 << 18,  /* GPIO 0xF48C reads 0x4000000 (HDMI disconnected): skips
                                             * Canon's system check program (1300D, 4000D) */
    EOS_QUIRK_I2C_TX_READY      = 1 << 19,  /* I2C 0xC0090000: "TX data ready" (status bit 4) is set
                                             * when a transmit starts, before the first byte (EOSM) */
};

struct eos_model_desc {
    const char * name;

    /* Model-specific behaviour. Not inherited from the DIGIC defaults (outside params[]):
     * 0 / NULL = the generic upstream behaviour. docs/models/PROFILES.md */
    eos_pm_handler_fn pm_handler;   /* power management (hw/eos/cams/<CAM>/pm.c) */
    const struct capture_profile *capture;  /* shutter release emulation (hw/eos/cams/<CAM>/capture.c) */
    void (*init_hook)(void);        /* end of eos_init_common, before the start address: ROM patches,
                                     * boot flags, extra images */
    const char *mpu_spells;         /* MPU init spells (mpu_spells/<name>.h), NULL = the model's own */
    const char *button_codes;       /* MPU button codes (button_codes.h), NULL = the model's own */
    uint32_t flash_id;              /* ROM1: answer to the flash ID command (0x06 written to 0xF8000000),
                                     * 0 = none */
    const uint32_t *adc_values;     /* DIGIC 6 ADC (0xD9800000): reading by channel, adc_count */
    int adc_count;                  /* entries; NULL = channels read 0 */
    const uint32_t *adc_ps_values;  /* PowerShot ADC (0xC0900040, half-word channels read in pairs): */
    int adc_ps_count;               /* reading by channel; NULL = read 0 */
    uint32_t adc_ps_battery;        /* PowerShot ADC 0xC09000B0 (battery voltage), 0 = reads 0 */
    uint32_t quirks;                /* enum eos_quirk */

    union
    {
        /* These parameters can be specified either for every single model,
         * or for all models that have the same digic_version.
         * 
         * If a parameter is specified (non-zero) in both ways,
         * the model-specific one is used.
         */
        struct
        {
            uint32_t digic_version;
            uint32_t rom0_addr;
            uint32_t rom0_size;
            uint32_t rom1_addr;
            uint32_t rom1_size;
            uint32_t ram_size;
            uint32_t caching_bit;
            uint32_t atcm_addr;
            uint32_t atcm_size;
            uint32_t btcm_addr;
            uint32_t btcm_size;
            uint32_t ram_extra_addr[ram_extra_array_len];
            uint32_t ram_extra_size[ram_extra_array_len];
            uint32_t mmio_addr;
            uint32_t mmio_size;
            uint32_t firmware_start;
            uint32_t firmware_version;  /* optional, only set if using more than one */
            uint32_t bootflags_addr;
            uint32_t dryos_timer_id;
            uint32_t dryos_timer_interrupt;
            uint32_t hptimer_interrupt;
            uint32_t current_task_addr;
            uint32_t current_task_name_offs;
            uint32_t mpu_request_register;
            uint32_t mpu_request_bitmask;
            uint32_t mpu_status_register;
            uint32_t mpu_control_register;
            uint32_t mpu_mreq_interrupt;
            uint32_t mpu_sio3_interrupt;
            uint32_t serial_flash_size;
            uint32_t serial_flash_cs_register;
            uint32_t serial_flash_cs_bitmask;
            uint32_t serial_flash_sio_ch;
            uint32_t serial_flash_interrupt;
            uint32_t sd_driver_interrupt;
            uint32_t sd_dma_interrupt;
            uint32_t cf_driver_interrupt;
            uint32_t cf_dma_interrupt;
            uint32_t card_led_address;
            uint32_t ram_manufacturer_id;
            uint32_t uart_rx_interrupt;
            uint32_t uart_tx_interrupt;
            uint32_t rtc_cs_register;
            uint32_t rtc_time_correct;
            uint32_t rtc_control_reg_2;
            uint32_t dedicated_movie_mode;
            uint32_t imgpowcfg_register;
            uint32_t imgpowcfg_register_bit;
            uint32_t imgpowdet_register;
            uint32_t imgpowdet_register_bit;
            uint32_t imgpowdet_interrupt;
            uint32_t max_cpus;
            uint32_t rom1_hivecs_offset; /* ROM1 offset of the 64K boot block seen at 0xFFFF0000 */
        };
        
        /* this must match the number of items in the above struct */
        /* note: you get a compile-time error if params[] is smaller than the struct */
        uint32_t params[51 + ram_extra_array_len * 2];
    };
} __attribute__((packed));

extern struct eos_model_desc eos_model_list[];

/* init hooks (.init_hook) of the upstream models without a profile directory, in eos.c */
void eos_init_7D(void);
void eos_init_200D(void);
void eos_init_EOSM3(void);
void eos_init_EOSM10(void);
void eos_init_EOSM5(void);
void eos_init_5D3eeko(void);

/* compile-time error if the size of params[] is smaller than
 * the size of the parameter struct in the above union.
 * (not sure how to get the size of an anonymous struct to check for equality)
 */
static uint8_t __attribute__((unused)) __eos_model_desc_size_check[
    sizeof(eos_model_list[0].params) ==
    sizeof(struct eos_model_desc) - offsetof(struct eos_model_desc, params)
    ? 0 : -1
];

#endif

