#!/bin/bash
# Smoke test (CI host side): boot Canon firmware to the GUI in IMAGE, run the scripted scenarios and
# check frames + markers against bench/expected.json. On the 1100D also the deep sleep: INFO turns the
# display off, Canon's PowerMgr must enter and leave selfRefresh (DryosDebugMsg), and the INFO key that
# wakes it must bring the shooting screen back. SKIPPED (exit 0) without ROMs.
# Usage: ci/smoke.sh IMAGE [CAM]     CAM: 1100D (default) or 4000D    (needs ROM_TOKEN, SANDBOX)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=${1:?usage: smoke.sh IMAGE [CAM]}
CAM=${2:-1100D}
case "$CAM" in
    1100D) FW=105; OUT=emu-out/smoke
           # deep sleep (PM model): display off -> selfRefresh In/Out -> INFO wakes the shooting screen
           EXTRA='--debugmsg --min-count "pmSelfRefresh : In=1" --min-count "pmSelfRefresh : Out=1"
                  --step key:i --step wait:3 --step shot:display_off --step wait:15 --step key:i --step wait:4
                  --step shot:woken' ;;
    4000D) FW=110; OUT=emu-out/smoke-4000D; EXTRA= ;;
    *) echo "unknown CAM $CAM" >&2; exit 1 ;;
esac
export IMAGE ROOT CAM OUT EXTRA
"$ROOT/ci/with-roms.sh" "$CAM-$FW" bash -c '
    eval "set -- $EXTRA"   # EXTRA holds shell-quoted arguments ("pmSelfRefresh : In=1" stays one word)
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMAGE" \
        python3 -I bench/bench.py --cam "$CAM" --rom-dir /roms --out "$OUT" --label smoke \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --expect bench/expected.json --idle 5 "$@"'
