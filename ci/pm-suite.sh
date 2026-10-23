#!/bin/bash
# 1100D power-management model (workflow pm.yml), Canon firmware only, DryosDebugMsg logged to see [PM]:
#   a/b  idle with the model off (QEMU_EOS_PM_MODEL=0, previous behaviour) and on, REPS times each
#   c    model on with QEMU_EOS_PM_TRACE=1 (register accesses, LCLK edges)
#   d    deep sleep: INFO/DISP turns the display off, idle DEEP_S s (expect selfRefresh), then INFO wakes
#        it and MENU must still open Canon's menu
# Needs ROM_TOKEN and SANDBOX; REPS (default 1), DEEP_S (default 30). Results: emu-out/pm/summary.md
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPS=${REPS:-1}
DEEP_S=${DEEP_S:-30}
rm -rf "$ROOT/emu-out/pm" && mkdir -p "$ROOT/emu-out/pm"
IMG=$("$ROOT/ci/build-image.sh" opt)
export IMG ROOT REPS DEEP_S
"$ROOT/ci/with-roms.sh" 1100D-105 bash -c '
set -euo pipefail
run() {
    local name=$1; shift
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$IMG" \
        python3 -I bench/bench.py --cam 1100D --rom-dir /roms --expect bench/expected.json --idle 20 \
            --plugin /opt/qemu-eos/plugins/libwakeups.so --debugmsg --out "emu-out/pm/$name" --label "$name" \
            --count pmSelfRefresh --count "EnablePowerSave (Counter = 0)" --count "[PM]" --count "[PM1100D]" \
            --count "self-refresh #" --count "LCLK o" --count "CLKSAVER" \
            "$@" || echo "run $name FAILED"
}
for r in $(seq 1 "$REPS"); do
    run "a-pm-off.r$r" --env QEMU_EOS_PM_MODEL=0
    run "b-pm-on.r$r"
done
run "c-pm-on-trace" --env QEMU_EOS_PM_TRACE=1
run "d-deep-sleep" --env QEMU_EOS_PM_TRACE=1 --idle 5 \
    --step "mon:xp /30wx 0x2bf18" --step "mon:xp /8wx 0x661c" \
    --step key:i --step wait:3 --step shot:disp_off \
    --step "mon:xp /30wx 0x2bf18" --step "mon:xp /8wx 0x661c" --step "mon:xp /8wx 0x2b60" --step "mon:xp /8wx 0x7504" \
    --step "wait:$DEEP_S" --step shot:asleep --step "mon:xp /8wx 0x7504" --step "mon:info registers" \
    --step key:i --step wait:4 --step shot:woken --step key:m --step wait:4 --step shot:menu_after_wake \
    --step key:m --step wait:3 --step shot:menu_closed
'
podman run $SANDBOX -v "$ROOT:/work" -w /work "$IMG" python3 -I bench/report.py emu-out/pm
podman run $SANDBOX -v "$ROOT:/work" -w /work "$IMG" python3 -I -c '
import json, glob
for p in sorted(glob.glob("emu-out/pm/*/result.json")):
    r = json.load(open(p)); i = r.get("idle") or {}
    print(r["label"], "wfi/s", i.get("wfi_per_s"), "busy", i.get("busy_ratio"), "mips", i.get("mips"),
          "irq/s", i.get("irq_per_s"), "cpu_s", r.get("host_cpu_user_s"), r.get("log_counts"), r["errors"])
    for s in r.get("steps") or []:
        print("   %-18s t=%6.1f %s" % (s["step"], s["t"], {k: v for k, v in s.get("counts", {}).items() if v}))
    if r.get("steps"):
        print("   frames", r["frames"])
    for cmd, outp in r.get("monitor") or []:
        print("   mon %s:\n      %s" % (cmd, outp.replace("\n", "\n      ")[:1500]))'
for d in c-pm-on-trace d-deep-sleep; do
    echo "== $d"
    grep -ah "PM1100D\] \(LCLK\|self-refresh\)\|pmSelfRefresh\|CLKSAVER\|DisablePowerSave\|EnablePowerSave" \
        "$ROOT/emu-out/pm/$d/qemu.log" | sed "s/\x1b\[[0-9;]*m//g" | tail -n 150 || true
done
