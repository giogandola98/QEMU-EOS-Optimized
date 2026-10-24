#!/bin/bash
# Smoke test (CI host side): boot Canon firmware to the GUI in IMAGE, run the scripted scenarios and
# check frames + markers against the camera profile (hw/eos/cams/<CAM>/test/bench.json). The profile's
# test/smoke.sh adds the camera's own checks (1100D, 4000D: the deep sleep, where INFO turns the display
# off, Canon's PowerMgr must enter and leave selfRefresh (DryosDebugMsg) and the INFO key that wakes it
# must bring the shooting screen back; then a full shutter press must write a picture to the card,
# docs/models/capture.md). SKIPPED (exit 0) without ROMs.
# Usage: ci/smoke.sh IMAGE [CAM]     CAM: a profile with test/smoke.sh (default 1100D)
#                                    (needs ROM_TOKEN, SANDBOX)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=${1:?usage: smoke.sh IMAGE [CAM]}
CAM=${2:-1100D}
# per-camera settings (FW = ROM package version, OUT, EXTRA): hw/eos/cams/<CAM>/test/smoke.sh
PROFILE=$ROOT/hw/eos/cams/$CAM/test/smoke.sh
[ -f "$PROFILE" ] || { echo "no smoke-test profile for CAM $CAM ($PROFILE)" >&2; exit 1; }
. "$PROFILE"
export IMAGE ROOT CAM OUT EXTRA
"$ROOT/ci/with-roms.sh" "$CAM-$FW" bash -c '
    eval "set -- $EXTRA"   # EXTRA holds shell-quoted arguments ("pmSelfRefresh : In=1" stays one word)
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMAGE" \
        python3 -I bench/bench.py --cam "$CAM" --rom-dir /roms --out "$OUT" --label smoke \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --expect --idle 5 "$@"'
