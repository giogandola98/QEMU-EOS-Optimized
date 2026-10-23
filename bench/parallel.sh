#!/bin/bash
# (e) Two 1100D runs one after the other, then the same two at the same time (inside the qemu-eos image;
# no container start-up in the timing). Writes OUT/parallel.json; results in OUT/{p-seq,q-par}-{a,b}/.
# Usage: bench/parallel.sh OUT_DIR BENCH_ARGS...
set -euo pipefail
out=$1; shift
bench() { local n=$1; shift; python3 -I "$(dirname "$0")/bench.py" "$@" --out "$out/$n" --label "$n" > /dev/null; }
t0=$(date +%s.%N)
bench p-seq-a "$@"; bench p-seq-b "$@"
t1=$(date +%s.%N)
bench q-par-a "$@" & bench q-par-b "$@" & wait
t2=$(date +%s.%N)
python3 -c 'import json, sys; t0, t1, t2 = map(float, sys.argv[1:])
print(json.dumps({"pair_sequential_s": round(t1 - t0, 2), "pair_parallel_s": round(t2 - t1, 2)}))' \
    "$t0" "$t1" "$t2" | tee "$out/parallel.json"
