#!/bin/bash
# Phase 1/2 benchmark suite (CI host side, workflow bench.yml). Builds the baseline and opt images,
# runs the ROM-free microbenchmark on both and the 1100D variants below, then bench/report.py writes
# emu-out/bench/summary.json and summary.md. Needs ROM_TOKEN and SANDBOX; REPS (default 3).
#
# Variant = image profile + plugin + logging + extra bench.py args. "ml-ci" logging is what
# ML-Optimized's scenarios run with (-d debugmsg,tasks); "plain" has no -d at all.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPS=${REPS:-3}
OUT=$ROOT/emu-out/bench
rm -rf "$OUT" && mkdir -p "$OUT"

BASE_IMG=$("$ROOT/ci/build-image.sh" baseline)
OPT_IMG=$("$ROOT/ci/build-image.sh" opt)
echo "images: baseline=$BASE_IMG opt=$OPT_IMG; runner $(nproc) CPUs"
export BASE_IMG OPT_IMG ROOT OUT REPS

for prof in baseline opt; do
    img=$BASE_IMG; [ "$prof" = opt ] && img=$OPT_IMG
    podman run $SANDBOX --tmpfs /tmp -v "$ROOT:/work" -w /work "$img" \
        python3 -I bench/microbench.py "emu-out/bench/micro-$prof" --reps "$REPS" --label "$prof"
done

"$ROOT/ci/with-roms.sh" 1100D-105 bash -c '
set -euo pipefail
# Snapshot disks hold guest RAM (ROM-derived): RAM-only dir, removed with the ROMs.
SNAP=$(mktemp -d /dev/shm/qemu-eos-snap.XXXXXX); chmod 700 "$SNAP"
trap "rm -rf \"$SNAP\"" EXIT

run() {     # run IMAGE NAME [bench.py args...]: one 1100D run into emu-out/bench/NAME
    local img=$1 name=$2; shift 2
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -v "$SNAP:/snap" \
        -w /work "$img" bash -c "/opt/qemu-eos/src/build-plugin.sh /tmp/p bench/plugins/wakeups_v1.c >/dev/null && \
        exec python3 -I bench/bench.py --cam 1100D --rom-dir /roms --expect bench/expected.json \
            --out emu-out/bench/$name --label $name \"\$@\"" bench "$@" || echo "run $name FAILED"
}
V1=/tmp/p/libwakeups_v1.so
V2=/opt/qemu-eos/plugins/libwakeups.so

for r in $(seq 1 "$REPS"); do       # interleaved, so runner noise spreads over all variants
    run "$BASE_IMG" "baseline-ml-ci-v1.r$r"  --plugin $V1 --debugmsg --tasks
    run "$BASE_IMG" "baseline-plain-v1.r$r"  --plugin $V1
    run "$OPT_IMG"  "opt-plain-v1.r$r"       --plugin $V1
    run "$OPT_IMG"  "opt-plain-v2.r$r"       --plugin $V2
    run "$OPT_IMG"  "opt-ml-ci-v2.r$r"       --plugin $V2 --debugmsg --tasks
    run "$OPT_IMG"  "opt-noplugin.r$r"
done

# Post-boot snapshot: cold boot on qcow2 + savevm, then every later run starts from it.
run "$OPT_IMG" "opt-savevm" --plugin $V2 --disk-dir /snap/disks --savevm gui --no-scenarios --idle 1
for r in $(seq 1 "$REPS"); do
    run "$OPT_IMG" "opt-loadvm-v2.r$r" --plugin $V2 --disk-dir /snap/disks --loadvm gui
done

# Parallel: two cameras at once on the shared runner (4 CPUs): same results, total wall time?
start=$(date +%s.%N)
run "$OPT_IMG" "opt-par-a" --plugin $V2 & run "$OPT_IMG" "opt-par-b" --plugin $V2 & wait
echo "{\"parallel_pair_wall_s\": $(python3 -c "import time;print(round(time.time()-$start,2))")}" > "$OUT/parallel.json"
'
podman run $SANDBOX -v "$ROOT:/work" -w /work "$OPT_IMG" python3 -I bench/report.py emu-out/bench
