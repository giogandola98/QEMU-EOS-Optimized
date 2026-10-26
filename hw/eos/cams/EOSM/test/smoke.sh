# Smoke-test settings for the EOS M (sourced by ci/smoke.sh). EXTRA is eval-ed: keep it on ONE line.
# FW: ROM package version (canon-rom <CAM>-<FW>, with SFDATA.BIN); OUT: results dir; EXTRA: bench.py arguments.
# After the scenarios (MENU, PLAY, Q, INFO; bench.json) a half-shutter press must reach Canon's GUI
# (IDLEHandler PRESS_SW1_BUTTON) and its release must follow. The package's LiveView frames (LV.422, HD.422)
# must be injected (two "[LV] frame injection" lines, hw/eos/cams/EOSM/lv.c). No deep sleep or capture
# model yet (docs/models/EOSM.md).
FW=202; OUT=emu-out/smoke-EOSM
EXTRA='--debugmsg --step key:shift --step "until:IDLEHandler UNPRESS_SW1_BUTTON" --step shot:half_shutter --min-count "IDLEHandler PRESS_SW1_BUTTON=1" --min-count "[LV] frame injection=2"'
