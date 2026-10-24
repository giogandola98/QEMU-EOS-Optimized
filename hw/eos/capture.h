#ifndef EOS_CAPTURE_H
#define EOS_CAPTURE_H

#include <stdint.h>

/* Still-photo capture (shutter release) emulation. Generic code in capture.c; the per-camera data
 * below lives in hw/eos/cams/capture_<cam>.c and is selected by eos_model_desc.capture (model_list.c,
 * NULL = no capture emulation). See docs/models/capture.md. */

struct capture_profile
{
    uint32_t release_gpio;          /* GPIO the DIGIC sets at release start and clears when the sensor
                                     * is set up for the exposure (and again after the readout) */
    uint32_t release_bit;           /* the bit of that register that carries the line */
    int capready_irq;               /* "CAPREADY": the exposure runs */
    int vd_irq;                     /* HEAD timer counting the exposure's VD periods (HEAD1) */
    int completion_irqs[8];         /* engine modules that only report completion (0-terminated) */
    int release_data_len;           /* payload bytes of EVENTID_RELEASE_DATA_SW2ON (after class/id) */
    int after_data_len;             /* payload bytes of EVENTID_AFTER_RELEASE_DATA */
    int eshut_offset;               /* payload offset of the "add_size" byte; the next byte selects
                                     * the capture path the firmware expects */
    unsigned int jpcore_irq_status; /* JP51 0x44 at the end of a run: 0x400 = done */
    int jpcore_conn;                /* EDMAC connection of the JPEG core output */
    int init_tv, init_av, init_iso; /* exposure until a property message says otherwise: what Canon
                                     * restores from its settings when the MPU init does not send it
                                     * (0 = the MPU init sends it) */
};

extern const struct capture_profile capture_1100d;
extern const struct capture_profile capture_4000d;

/* key press from the monitor/keyboard (PC scancode, 0x80 bit = release); 1 if consumed */
int capture_handle_key(int scancode);

/* messages between the guest and the MPU (spell layout: [0] = length) */
void capture_mpu_received(const uint16_t *msg, int len);
void capture_mpu_sent(const uint16_t *msg);

/* the guest enabled an interrupt at the interrupt controller */
void capture_irq_enabled(int irq);

/* HEAD timer start register written (value 0xC = start); irq = its interrupt */
void capture_head_start(int irq, unsigned int value);

/* GPIO access (0xC022xxxx); 1 if handled here, with *ret set for reads */
int capture_handle_gpio(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret);

/* JPEG core (JP51): register writes; output data for an EDMAC connection (1 if filled);
 * interrupt status (0x44) and output size (0x24), or the given default */
void capture_jpcore_write(unsigned int offset, unsigned int value);
int capture_fill_conn(int conn, void *buf, int size);
unsigned int capture_jpcore_irq_status(unsigned int dflt);
unsigned int capture_jpcore_output_size(unsigned int dflt);

#endif
