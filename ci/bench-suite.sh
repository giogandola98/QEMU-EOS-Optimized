#!/bin/bash
# Phase 1/2 benchmark suite (CI host side, workflow bench.yml). Builds the baseline and opt images,
# runs the ROM-free microbenchmark (plus Debian's stock QEMU as a reference for this runner's speed)
# and the 1100D variants below, then bench/report.py writes emu-out/bench/summary.{json,md}.
# Needs ROM_TOKEN and SANDBOX; REPS (default 5).
#
# Variant = image profile + plugin + logging + environment. "ml-ci" logging is what ML-Optimized's
# scenarios run with (-d debugmsg,tasks); "plain" has no -d at all. Both images are built from the same
# source, so code-level changes are switched by environment: QEMU_EOS_MMIO_CACHE=0 restores the original
# MMIO handler scan, QEMU_EOS_INT_SLEEP_US the 250 us sleep per triggered interrupt.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
REPS=${REPS:-5}
OUT=$ROOT/emu-out/bench
rm -rf "$OUT" && mkdir -p "$OUT"

BASE_IMG=$("$ROOT/ci/build-image.sh" baseline)
OPT_IMG=$("$ROOT/ci/build-image.sh" opt)
# Reference: Debian bookworm's own qemu-system-arm (7.2), to tell "runner is slow" from "our TCG is slow".
REF_IMG=qemu-ref:bookworm-7ff6ba31
podman image exists "$REF_IMG" || podman build --network=host -q -t "$REF_IMG" -f - "$ROOT/bench" >/dev/null <<'EOF'
FROM docker.io/library/debian:bookworm-slim@sha256:7ff6ba31527b9e336162fa061746c366c4b2032f4d2cff2078bea43f78efe3f8
RUN apt-get update && apt-get install -y --no-install-recommends qemu-system-arm binutils-arm-none-eabi \
      gcc libc6-dev python3 && rm -rf /var/lib/apt/lists/*
EOF
echo "images: baseline=$BASE_IMG opt=$OPT_IMG ref=$REF_IMG; runner $(nproc) CPUs, load $(cut -d' ' -f1-3 /proc/loadavg)"
export BASE_IMG OPT_IMG ROOT OUT REPS

for prof in baseline opt; do
    img=$BASE_IMG; [ "$prof" = opt ] && img=$OPT_IMG
    podman run $SANDBOX --tmpfs /tmp -v "$ROOT:/work" -w /work "$img" \
        python3 -I bench/microbench.py "emu-out/bench/micro-$prof" --reps "$REPS" --label "$prof"
done
podman run $SANDBOX --tmpfs /tmp -v "$ROOT:/work" -w /work "$REF_IMG" \
    python3 -I bench/microbench.py emu-out/bench/micro-debian-ref --reps "$REPS" --label debian-ref \
        --qemu /usr/bin/qemu-system-arm --no-plugins

"$ROOT/ci/with-roms.sh" 1100D-105 bash -c '
set -euo pipefail
B="python3 -I bench/bench.py --cam 1100D --rom-dir /roms --expect --idle 8"
V1=/tmp/p/libwakeups_v1.so
V2=/opt/qemu-eos/plugins/libwakeups.so
OLD="--env QEMU_EOS_MMIO_CACHE=0"           # original handler scan

run() {     # run IMAGE NAME [bench.py args...]: one 1100D run into emu-out/bench/NAME
    local img=$1 name=$2; shift 2
    podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$img" \
        bash -c "/opt/qemu-eos/src/build-plugin.sh /tmp/p bench/plugins/wakeups_v1.c >/dev/null && \
                 exec $B --out emu-out/bench/$name --label $name \"\$@\"" bench "$@" || echo "run $name FAILED"
}

for r in $(seq 1 "$REPS"); do       # interleaved, so runner noise spreads over all variants
    run "$BASE_IMG" "a-baseline-ml-ci-v1.r$r"   --plugin $V1 $OLD --debugmsg --tasks   # ML CI before
    run "$BASE_IMG" "b-baseline-plain-v1.r$r"   --plugin $V1 $OLD
    run "$OPT_IMG"  "c-opt-plain-v1.r$r"        --plugin $V1 $OLD                      # (c) build flags
    run "$OPT_IMG"  "d-opt-plain-v2.r$r"        --plugin $V2 $OLD                      # (a) plugin
    run "$OPT_IMG"  "e-opt-plain-v2-cache.r$r"  --plugin $V2                           # MMIO cache
    run "$OPT_IMG"  "f-opt-plain-v2-sleep0.r$r" --plugin $V2 --env QEMU_EOS_INT_SLEEP_US=0
    run "$OPT_IMG"  "g-opt-ml-ci-v2.r$r"        --plugin $V2 --debugmsg --tasks        # ML CI after
    run "$OPT_IMG"  "h-opt-noplugin.r$r"
done

# (e) Parallel vs sequential pair of runs, timed inside one container.
podman run $SANDBOX --tmpfs /tmp:rw,size=1g -v "$ROOT:/work" -v "$ROM_DIR:/roms:ro" -w /work "$OPT_IMG" \
    bench/parallel.sh emu-out/bench --cam 1100D --rom-dir /roms --expect --idle 8 \
        --plugin $V2 || echo "parallel runs FAILED"
'
podman run $SANDBOX -v "$ROOT:/work" -w /work "$OPT_IMG" python3 -I bench/report.py emu-out/bench
