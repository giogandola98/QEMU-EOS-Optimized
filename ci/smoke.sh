#!/bin/bash
# 1100D smoke test (CI host side): boot Canon firmware to the GUI in IMAGE, run the scripted
# scenarios and check frames + markers against bench/expected.json. SKIPPED (exit 0) without ROMs.
# Usage: ci/smoke.sh IMAGE      (needs ROM_TOKEN, SANDBOX)
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
IMAGE=${1:?usage: smoke.sh IMAGE}
export IMAGE ROOT
"$ROOT/ci/with-roms.sh" 1100D-105 bash -c '
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMAGE" \
        python3 -I bench/bench.py --cam 1100D --rom-dir /roms --out emu-out/smoke --label smoke \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --expect bench/expected.json --idle 5'
