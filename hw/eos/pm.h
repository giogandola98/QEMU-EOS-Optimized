#ifndef EOS_PM_H
#define EOS_PM_H

/*
 * Power-management models (Canon PowerMgr idle loop and selfRefresh deep sleep), one per camera.
 * A model is selected by eos_model_desc.pm_handler (model_list.c) and called by eos_handler()
 * for every MMIO access before the regular handlers. QEMU_EOS_PM_MODEL=0 disables it (A/B runs).
 */

#include <stdbool.h>

/* 1100D 1.0.5 (eos.c) */
bool eos_pm_1100d(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret);

/* 4000D 1.1.0 (cams/pm_4000d.c) */
bool eos_pm_4000d(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret);

/* Clock gate shared by the models: while set (selfRefresh wrote CLOCK_ENABLE = 0), the DIGIC timers,
 * the DryOS tick and the HPTimers stand still; other interrupts (MPU, keys) still arrive and wake the
 * CPU. Always clear on models without a PM model. */
void eos_pm_set_clocks_gated(bool gated);
bool eos_pm_clocks_gated(void);

#endif
