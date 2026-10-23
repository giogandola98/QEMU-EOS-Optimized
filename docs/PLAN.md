# QEMU-EOS-Optimized — Plan

Fork of qemu-eos (v4.2.1) used to verify ML-Optimized without hardware. Validation camera: 1100D (fw 1.0.5);
target camera: EOS M (fw 2.0.2).

## Phases
| # | Phase | Output | Exit criterion |
|---|---|---|---|
| 0 | **Infra** | Forgejo CI builds qemu in a pinned container; image published to the private Forgejo registry (`<private-org>`) and consumed by ML-Optimized CI | Image built + 1100D boots to Canon GUI in CI |
| 1 | **Baseline** | Benchmarks on 1100D: boot-to-GUI time, time per scripted test, emulated MIPS, host CPU/RAM | `bench/baseline.json` committed |
| 2 | **Performance** | Post-boot snapshots (savevm/loadvm); quiet device logging; no display/VNC unless asked; -O2/LTO arm-softmmu-only build; TCG tuning (tb-size, no icount); parallel runs | Each change measured vs baseline; screenshots/logs identical |
| 3 | **Instrumentation** | Low-overhead wakeup/IRQ/idle plugin (translation-time, inline counters); DryOS task-switch trace from stubs | Self-test on bare-metal program + 1100D numbers stable across runs |
| 4 | **MCP server** | `mcp/` Python server exposing the emulated camera to AI agents (see below) | Agent can boot, press keys, screenshot, read logs/memory, measure wakeups end-to-end |
| 5 | **Emulation fixes** | Fill gaps that block EOS M verification: buttons/MPU events, timers, auto power-off, display on/off, then LiveView-related stubs where feasible | Each fix has a regression test on 1100D and/or EOSM |

## MCP server (phase 4)
Python (official `mcp` SDK), stdio for local agents, optional streamable HTTP. Talks to QEMU via QMP, the GDB stub and
the monitor; one session = one emulated camera.

Tools:
- `boot(cam, ml_zip?, snapshot?, profile?)` / `shutdown()` / `snapshot_save(name)` / `snapshot_load(name)`
- `press(key, hold_ms?)`, `sequence([keys])` (MPU button events), `wait_for(text|screen_hash, timeout)`
- `screenshot()` → PNG (+ perceptual hash), `read_log(since?, grep?)` (DryosDebugMsg / ML console)
- `read_mem(addr, len)`, `regs()`, `breakpoint(addr|symbol)`, `continue(timeout)`, `symbol(name)` (ML `.sym` + stubs)
- `measure_idle(seconds)` → wakeups/s, IRQ/s, busy ratio (plugin), `tasks()` (DryOS task list)
- `compare(run_a, run_b)` → metric deltas for A/B of ML builds

Safety: ROMs fetched from `<private-org>` at boot, kept in a temp dir, never returned through the API (memory reads are
capped and not allowed to dump whole ROM regions).
