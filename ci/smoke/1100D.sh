# Smoke-test settings for the 1100D (sourced by ci/smoke.sh). EXTRA is eval-ed: keep it on ONE line.
# Deep sleep (PM model): INFO turns the display off -> selfRefresh In/Out -> INFO wakes the shooting screen.
FW=105; OUT=emu-out/smoke
EXTRA='--debugmsg --min-count "pmSelfRefresh : In=1" --min-count "pmSelfRefresh : Out=1" --step key:i --step wait:3 --step shot:display_off --step wait:15 --step key:i --step wait:4 --step shot:woken'
