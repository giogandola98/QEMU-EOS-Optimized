# Smoke-test settings for the 4000D (sourced by ci/smoke.sh). EXTRA is eval-ed: keep it on ONE line.
# Deep sleep (hw/eos/cams/pm_4000d.c): INFO turns the display off -> selfRefresh In/Out -> INFO wakes the
# shooting screen. "#1 out after 1" = the first sleep lasted 10-19 s, i.e. the timers stopped and only the
# wake key ended it (without the clock gate the DryOS tick ends every sleep within 10 ms).
FW=110; OUT=emu-out/smoke-4000D
# Then a full shutter press (left Ctrl, hw/eos/capture.c): Canon's capture must finish and a picture with
# EXIF must be on the card (bench/card.py reads it after QEMU exits).
EXTRA='--debugmsg --min-count "pmSelfRefresh : In=1" --min-count "pmSelfRefresh : Out=1" --min-count "[PM4000D] self-refresh #1 out after 1=1" --count "pmSelfRefresh : In" --step key:i --step wait:3 --step shot:display_off --step wait:15 --step key:i --step wait:4 --step shot:woken --step key:ctrl --step "until:write done" --step wait:2 --card --min-pictures 1 --min-count "[CAPTURE] shot=1" --min-count "scsCapReady=1" --min-count "write done=1"'
