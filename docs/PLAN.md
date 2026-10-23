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

## Progress
| # | Status | Notes |
|---|---|---|
| 0 | **done** | `.forgejo/workflows/ci.yml`: image from this repo's source (`ci/qemu/Containerfile` + `Containerfile.tools`), plugin self-test, 1100D smoke test (boot to GUI + menu scenarios, pixel hashes in `bench/expected.json`), push to `<forgejo-server>/<private-org>/qemu-eos:<sha>` (`:main` on main). ML-Optimized pulls it by digest (`ci/qemu-image.txt` there). Secrets: `ROM_TOKEN` (read:package), `REGISTRY_TOKEN` (write:package) |
| 1 | **done** | `bench/baseline.json` (bench run 7): boot→GUI ~1.5-2 s / ~29 M guest insns, menu scenarios 0.1-0.3 s each, idle ~42 MIPS, ~87 MiB RSS |
| 2 | **done** | `docs/PERFORMANCE.md`: LTO `opt` build (+7 % MIPS, build −57 %), MMIO handler cache (+5 % idle MIPS), plugin v2 (inline counters; overhead below noise, as was v1), parallel runs (−48 % wall; opt-in in ML, see PERFORMANCE.md). Snapshots blocked: no VMState in `hw/eos` (documented). Frames identical in all 52 runs |
| 3 | partly | plugin side done in phase 2 (translation-time inline counters); DryOS task-switch trace still `-d tasks` |

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

## LiveView simulation (phase 5)
No camera/video source in CI, so frames come from samples.
- **A — frame injection (first):** a small QEMU device writes a frame into the LV YUV422 buffer at a fixed virtual-time
  rate and raises the vsync/frame event ML listens to. Exercises ML overlays (peaking, zebras, histogram, magic zoom)
  end to end with screenshot comparisons, and lets us measure overlay cost on static vs moving scenes. Bypasses
  Canon's LV state machine.
- **B — Canon pipeline (research):** emulate enough of sensor timers / EDMAC / display path for Canon's own LiveView to
  run on injected frames. Needs ROM RE; effort unknown. Valuable for EOS M power behaviour (always in LV).
- **Frame sources:** (1) deterministic synthetic patterns (focus sweep, exposure ramps with clipping, static vs moving);
  (2) real LV buffers from the user's camera via ML Debug → "Dump image buffers" (private, like ROMs);
  (3) a few permissively licensed test images converted to UYVY.

