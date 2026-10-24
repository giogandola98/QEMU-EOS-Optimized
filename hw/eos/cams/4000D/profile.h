#ifndef EOS_CAM_4000D_PROFILE_H
#define EOS_CAM_4000D_PROFILE_H

/* Canon EOS 4000D: the hooks its model entry (model.inc.c) points to. docs/models/PROFILES.md */

#include <stdbool.h>

/* power management: PowerMgr idle loop and selfRefresh (pm.c, hw/eos/pm.h) */
bool eos_pm_4000d(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret);

/* still-photo capture data (capture.c, hw/eos/capture.h) */
struct capture_profile;
extern const struct capture_profile capture_4000d;

#endif
