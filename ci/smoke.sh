#!/bin/bash
# Smoke test (CI host side): boot Canon firmware to the GUI in IMAGE, run the scripted scenarios and
# check frames + markers against bench/expected.json. SKIPPED (exit 0) without ROMs.
# Usage: ci/smoke.sh IMAGE [CAM]     CAM: 1100D (default) or 4000D    (needs ROM_TOKEN, SANDBOX)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=${1:?usage: smoke.sh IMAGE [CAM]}
CAM=${2:-1100D}
case "$CAM" in
    1100D) FW=105; OUT=emu-out/smoke ;;
    4000D) FW=110; OUT=emu-out/smoke-4000D ;;
    *) echo "unknown CAM $CAM" >&2; exit 1 ;;
esac
export IMAGE ROOT CAM OUT
"$ROOT/ci/with-roms.sh" "$CAM-$FW" bash -c '
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMAGE" \
        python3 -I bench/bench.py --cam "$CAM" --rom-dir /roms --out "$OUT" --label smoke \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --expect bench/expected.json --idle 5'
