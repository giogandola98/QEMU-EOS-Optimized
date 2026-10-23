#!/bin/bash
# 1100D power-management model A/B (workflow pm.yml): Canon firmware idle with the model off
# (QEMU_EOS_PM_MODEL=0, previous behaviour) and on, DryosDebugMsg logged to see [PM] messages.
# Needs ROM_TOKEN and SANDBOX; REPS (default 3). Results: emu-out/pm/summary.md
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPS=${REPS:-3}
rm -rf "$ROOT/emu-out/pm" && mkdir -p "$ROOT/emu-out/pm"
IMG=$("$ROOT/ci/build-image.sh" opt)
export IMG ROOT REPS
"$ROOT/ci/with-roms.sh" 1100D-105 bash -c '
set -euo pipefail
run() {
    local name=$1; shift
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMG" \
        python3 -I bench/bench.py --cam 1100D --rom-dir /roms --expect bench/expected.json --idle 20 \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --debugmsg --out "emu-out/pm/$name" --label "$name" \
            --count pmSelfRefresh --count "EnablePowerSave (Counter = 0)" --count "[PM]" --count "[PM1100D]" \
            "$@" || echo "run $name FAILED"
}
for r in $(seq 1 "$REPS"); do
    run "a-pm-off.r$r" --env QEMU_EOS_PM_MODEL=0
    run "b-pm-on.r$r"
done
run "c-pm-on-trace" --env QEMU_EOS_PM_TRACE=1
'
podman run $SANDBOX -v "$ROOT:/work" -w /work "$IMG" python3 -I bench/report.py emu-out/pm
podman run $SANDBOX -v "$ROOT:/work" -w /work "$IMG" python3 -I -c '
import json, glob
for p in sorted(glob.glob("emu-out/pm/*/result.json")):
    r = json.load(open(p)); i = r.get("idle") or {}
    print(r["label"], "wfi/s", i.get("wfi_per_s"), "busy", i.get("busy_ratio"), "mips", i.get("mips"),
          "irq/s", i.get("irq_per_s"), "cpu_s", r.get("host_cpu_user_s"), r.get("log_counts"), r["errors"])'
grep -h "PM1100D\|pmSelfRefresh" "$ROOT"/emu-out/pm/c-pm-on-trace/qemu.log | sed 's/\x1b\[[0-9;]*m//g' | head -120 || true
