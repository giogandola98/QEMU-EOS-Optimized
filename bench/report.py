#!/usr/bin/env python3
"""Aggregate bench.py / microbench.py results of one suite run (ci/bench-suite.sh).

Usage: report.py BENCH_DIR  -> BENCH_DIR/summary.json, BENCH_DIR/summary.md (also printed)
Repetitions NAME.r1, NAME.r2, ... are folded into medians per variant NAME.
"""
import json
import os
import re
import statistics
import sys


def med(vals):
    vals = [v for v in vals if v is not None]
    return round(statistics.median(vals), 3) if vals else None


def main():
    d = sys.argv[1]
    runs = {}
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name, "result.json")
        if os.path.exists(p):
            runs.setdefault(re.sub(r"\.r\d+$", "", name), []).append(json.load(open(p)))

    variants = {}
    for v, rs in runs.items():
        idle = [r.get("idle") or {} for r in rs]
        variants[v] = {
            "runs": len(rs),
            "errors": sorted({e for r in rs for e in r.get("errors", [])}),
            "frames": sorted({json.dumps(r.get("frames"), sort_keys=True) for r in rs}),
            "boot_to_gui_s": med(r.get("boot_to_gui_s") for r in rs),
            "boot_insns": med(r.get("boot_insns") for r in rs),
            "scenario_s": {k: med(r["scenarios"].get(k) for r in rs) for k in rs[0].get("scenarios", {})},
            "scenarios_total_s": med(sum(r["scenarios"].values()) if r.get("scenarios") else None for r in rs),
            "idle_mips": med(i.get("mips") for i in idle),
            "idle_irq_per_s": med(i.get("irq_per_s") for i in idle),
            "idle_wfi_per_s": med(i.get("wfi_per_s") for i in idle),
            "wall_s": med(r.get("wall_s") for r in rs),
            "host_cpu_s": med((r.get("host_cpu_user_s") or 0) + (r.get("host_cpu_sys_s") or 0) for r in rs),
            "peak_rss_mib": med(r.get("peak_rss_mib") for r in rs),
            "screendump_ms": med((r.get("screendump_ms") or {}).get("median") for r in rs),
            "tb_flushes": max((r.get("tb_flushes") or 0) for r in rs),
            "log_bytes": med(r.get("log_bytes") for r in rs),
            "savevm_s": med(r.get("savevm_s") for r in rs),
        }
    micro = {}
    for name in sorted(os.listdir(d)):
        p = os.path.join(d, name, "micro.json")
        if os.path.exists(p):
            micro[name] = json.load(open(p))
    extra = {}
    if os.path.exists(os.path.join(d, "parallel.json")):
        extra = json.load(open(os.path.join(d, "parallel.json")))

    summary = {"variants": variants, "micro": micro, **extra}
    with open(os.path.join(d, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)

    lines = ["# Benchmark summary (1100D, Canon firmware, medians)", "",
             "| variant | runs | boot→GUI s | boot Minsns | scenarios s | idle MIPS | IRQ/s | host CPU s | peak RSS MiB | screendump ms | log KiB | errors |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for v, s in variants.items():
        f = lambda x, n=2: "n/a" if x is None else ("%.*f" % (n, x))
        lines.append("| %s | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            v, s["runs"], f(s["boot_to_gui_s"]), f(s["boot_insns"] / 1e6 if s["boot_insns"] else None, 1),
            f(s["scenarios_total_s"]), f(s["idle_mips"], 1), f(s["idle_irq_per_s"], 1), f(s["host_cpu_s"], 1),
            f(s["peak_rss_mib"], 0), f(s["screendump_ms"], 1), f(s["log_bytes"] / 1024 if s["log_bytes"] else None, 0),
            "; ".join(s["errors"])[:80] or "-"))
    frames = {v: s["frames"] for v, s in variants.items()}
    distinct = {f for fs in frames.values() for f in fs}
    lines += ["", "Distinct frame sets across all runs: %d %s" % (len(distinct), sorted(distinct))]
    for name, m in micro.items():
        lines += ["", "## Microbenchmark %s (%s build, %d insns)" % (name, m["build_profile"], m["insns"]), "",
                  "| plugin | wall s | MIPS | overhead vs none |", "|---|---:|---:|---:|"]
        for c in m["configs"]:
            lines.append("| %s | %.3f | %.1f | %+.1f %% |" % (c["config"], c["wall_s"], c["mips"], c["overhead_pct"]))
    if extra:
        lines += ["", "Parallel: %s" % json.dumps(extra)]
    text = "\n".join(lines) + "\n"
    with open(os.path.join(d, "summary.md"), "w") as f:
        f.write(text)
    print(text)


if __name__ == "__main__":
    main()
