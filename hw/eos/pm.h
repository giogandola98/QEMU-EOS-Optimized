#ifndef EOS_PM_H
#define EOS_PM_H

/*
 * Power-management models (Canon PowerMgr idle loop and selfRefresh deep sleep), one per camera:
 * hw/eos/cams/<CAM>/pm.c, declared in that profile's profile.h (signature: eos_pm_handler_fn,
 * model_list.h). A model is selected by eos_model_desc.pm_handler and called by eos_handler()
 * for every MMIO access before the regular handlers. QEMU_EOS_PM_MODEL=0 disables it (A/B runs).
 */

#include <stdbool.h>

/* Clock gate shared by the models: while set (selfRefresh wrote CLOCK_ENABLE = 0), the DIGIC timers,
 * the DryOS tick and the HPTimers stand still; other interrupts (MPU, keys) still arrive and wake the
 * CPU. Always clear on models without a PM model. */
void eos_pm_set_clocks_gated(bool gated);
bool eos_pm_clocks_gated(void);

#endif
