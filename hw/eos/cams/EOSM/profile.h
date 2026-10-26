#ifndef EOS_CAM_EOSM_PROFILE_H
#define EOS_CAM_EOSM_PROFILE_H

/* Canon EOS M: the hooks its model entry (model.inc.c) points to. Power management (pm.c) and capture
 * data (capture.c) go here when they are written. docs/models/PROFILES.md */

/* init hook: LiveView frame injection (lv.c, hw/eos/lv_inject.h) */
void eos_init_eosm(void);

#endif
