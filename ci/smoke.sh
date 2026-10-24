#!/bin/bash
# Smoke test (CI host side): boot Canon firmware to the GUI in IMAGE, run the scripted scenarios and
# check frames + markers against bench/expected.json. Also the deep sleep (1100D, 4000D): INFO turns the
# display off, Canon's PowerMgr must enter and leave selfRefresh (DryosDebugMsg), and the INFO key that
# wakes it must bring the shooting screen back; then a full shutter press must write a picture to the card
# (docs/models/capture.md). SKIPPED (exit 0) without ROMs.
# Usage: ci/smoke.sh IMAGE [CAM]     CAM: 1100D (default) or 4000D    (needs ROM_TOKEN, SANDBOX)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=${1:?usage: smoke.sh IMAGE [CAM]}
CAM=${2:-1100D}
# per-camera settings (FW, OUT, EXTRA): ci/smoke/<CAM>.sh
[ -f "$ROOT/ci/smoke/$CAM.sh" ] || { echo "unknown CAM $CAM" >&2; exit 1; }
. "$ROOT/ci/smoke/$CAM.sh"
export IMAGE ROOT CAM OUT EXTRA
"$ROOT/ci/with-roms.sh" "$CAM-$FW" bash -c '
    eval "set -- $EXTRA"   # EXTRA holds shell-quoted arguments ("pmSelfRefresh : In=1" stays one word)
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMAGE" \
        python3 -I bench/bench.py --cam "$CAM" --rom-dir /roms --out "$OUT" --label smoke \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --expect bench/expected.json --idle 5 "$@"'
