# Smoke-test settings for the 1100D (sourced by ci/smoke.sh). EXTRA is eval-ed: keep it on ONE line.
# FW: ROM package version (canon-rom <CAM>-<FW>); OUT: results dir; EXTRA: bench.py arguments.
# Deep sleep (PM model): INFO turns the display off -> selfRefresh In/Out -> INFO wakes the shooting screen.
FW=105; OUT=emu-out/smoke
# Then a full shutter press (left Ctrl, hw/eos/capture.c): Canon's capture must finish and a picture with
# EXIF must be on the card (bench/card.py reads it after QEMU exits).
EXTRA='--debugmsg --min-count "pmSelfRefresh : In=1" --min-count "pmSelfRefresh : Out=1" --step key:i --step wait:3 --step shot:display_off --step wait:15 --step key:i --step wait:4 --step shot:woken --step key:ctrl --step "until:write done" --step wait:2 --card --min-pictures 1 --min-count "[CAPTURE] shot=1" --min-count "scsCapReady=1" --min-count "write done=1"'
