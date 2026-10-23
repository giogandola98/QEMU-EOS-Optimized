#!/usr/bin/env python3
"""TCG speed without ROMs: run bench/busyloop.S on -M versatilepb under each plugin configuration.

Usage (inside the qemu-eos image): python3 -I bench/microbench.py OUT_DIR [--reps N] [--label L]
Writes OUT_DIR/micro.json: per configuration the median wall time and MIPS of the fixed instruction
count, and the overhead relative to running without any plugin.
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import time

QEMU_EOS = os.environ.get("QEMU_EOS", "/opt/qemu-eos")
HERE = os.path.dirname(os.path.abspath(__file__))
INSNS = 60000000 * 10           # busyloop.S: ITERS * instructions per iteration


def sh(*cmd):
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--label", default="")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)

    sh("arm-none-eabi-as", "-mcpu=arm926ej-s", "-o", out + "/busyloop.o", HERE + "/busyloop.S")
    sh("arm-none-eabi-ld", "--no-warn-rwx-segments", "-T", HERE + "/busyloop.ld", "-o", out + "/busyloop.elf",
       out + "/busyloop.o")
    v1 = subprocess.run([QEMU_EOS + "/src/build-plugin.sh", out, HERE + "/plugins/wakeups_v1.c"],
                        check=True, capture_output=True, text=True).stdout.strip()
    v2 = QEMU_EOS + "/plugins/libwakeups.so"

    # ML-Optimized's scenario.py arguments: out, start_ms, interval_ms=10000.
    configs = [
        ("none", None),
        ("v1 (per-insn callbacks)", v1 + ",arg=out=/dev/null,arg=interval_ms=10000"),
        ("v2 (inline)", v2 + ",arg=out=/dev/null,arg=interval_ms=10000"),
        ("v2 insns=off", v2 + ",arg=out=/dev/null,arg=interval_ms=10000,arg=insns=off"),
        ("v2 burst=on", v2 + ",arg=out=/dev/null,arg=interval_ms=10000,arg=burst=on"),
    ]
    results = []
    for rep in range(a.reps):              # interleaved, so host noise spreads over all configs
        for name, plugin in configs:
            cmd = [QEMU_EOS + "/bin/qemu-system-arm", "-M", "versatilepb", "-cpu", "arm926", "-m", "16M",
                   "-nodefaults", "-display", "none", "-monitor", "none", "-serial", "none", "-semihosting",
                   "-kernel", out + "/busyloop.elf"]
            if plugin:
                cmd += ["-plugin", plugin]
            t = time.time()
            r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=300)
            wall = time.time() - t
            if r.returncode != 0:
                sys.exit("%s failed: %s" % (name, r.stderr.decode(errors="replace")[-500:]))
            results.append((name, wall))

    table = []
    base = statistics.median(w for n, w in results if n == "none")
    for name, _ in configs:
        walls = [w for n, w in results if n == name]
        med = statistics.median(walls)
        table.append({"config": name, "wall_s": round(med, 3), "min_s": round(min(walls), 3),
                      "mips": round(INSNS / med / 1e6, 1), "overhead_pct": round((med / base - 1) * 100, 1)})
    res = {"label": a.label, "insns": INSNS, "reps": a.reps, "configs": table,
           "build_profile": open(QEMU_EOS + "/BUILD_PROFILE").read().strip()
           if os.path.exists(QEMU_EOS + "/BUILD_PROFILE") else "?"}
    with open(os.path.join(out, "micro.json"), "w") as f:
        json.dump(res, f, indent=1)
    for row in table:
        print("%-26s %7.3f s  %7.1f MIPS  %+6.1f %%" % (row["config"], row["wall_s"], row["mips"],
                                                        row["overhead_pct"]))
    for p in ("busyloop.o", "busyloop.elf", "libwakeups_v1.so"):
        try:
            os.remove(os.path.join(out, p))
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    main()
