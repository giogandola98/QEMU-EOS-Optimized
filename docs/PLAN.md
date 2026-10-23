# QEMU-EOS-Optimized — Plan

Fork of qemu-eos (v4.2.1) used to verify ML-Optimized without hardware. Validation camera: 1100D (fw 1.0.5);
target camera: EOS M (fw 2.0.2).

## Phases
| # | Phase | Output | Exit criterion |
|---|---|---|---|
| 0 | **Infra** | Forgejo CI builds qemu in a pinned container; image published to the private Forgejo registry (a private org) and consumed by ML-Optimized CI | Image built + 1100D boots to Canon GUI in CI |
| 1 | **Baseline** | Benchmarks on 1100D: boot-to-GUI time, time per scripted test, emulated MIPS, host CPU/RAM | `bench/baseline.json` committed |
| 2 | **Performance** | Post-boot snapshots (savevm/loadvm); quiet device logging; no display/VNC unless asked; -O2/LTO arm-softmmu-only build; TCG tuning (tb-size, no icount); parallel runs | Each change measured vs baseline; screenshots/logs identical |
| 3 | **Instrumentation** | Low-overhead wakeup/IRQ/idle plugin (translation-time, inline counters); DryOS task-switch trace from stubs | Self-test on bare-metal program + 1100D numbers stable across runs |
| 4 | **MCP server** | `mcp/` Python server exposing the emulated camera to AI agents (see below) | Agent can boot, press keys, screenshot, read logs/memory, measure wakeups end-to-end |
| 5 | **Emulation fixes** | Fill gaps that block EOS M verification: buttons/MPU events, timers, auto power-off, display on/off, then LiveView-related stubs where feasible | Each fix has a regression test on 1100D and/or EOSM |

## Progress
| # | Status | Notes |
|---|---|---|
| 0 | **done** | `.forgejo/workflows/ci.yml`: image from this repo's source (`ci/qemu/Containerfile` + `Containerfile.tools`), plugin self-test, 1100D smoke test (boot to GUI + menu scenarios, pixel hashes in `bench/expected.json`), push to `<registry>/<private org>/qemu-eos:<sha>` (`:main` on main). ML-Optimized pulls it by digest (`ci/qemu-image.txt` there). Secrets: `ROM_TOKEN` (read:package), `REGISTRY_TOKEN` (write:package) |
| 1 | **done** | `bench/baseline.json` (bench run 7): boot→GUI ~1.5-2 s / ~29 M guest insns, menu scenarios 0.1-0.3 s each, idle ~42 MIPS, ~87 MiB RSS |
| 2 | **done** | `docs/PERFORMANCE.md`: LTO `opt` build (+7 % MIPS, build −57 %), MMIO handler cache (+5 % idle MIPS), plugin v2 (inline counters; overhead below noise, as was v1), parallel runs (−48 % wall, used by ML's run_cam.sh). Snapshots blocked: no VMState in `hw/eos` (documented). Frames identical in all 52 runs |
| 5 | started (branches `emu/1100d-pm`, `emu/deep-sleep`) | 1100D power-management registers (`eos_pm_1100d` in `hw/eos/eos.c`). Canon-only idle, 20 s window, 3 runs each (pm run 11): model off → WFI 0.5/s, busy ratio 0.998, 42.7 MIPS spinning at 0xFF1EA818, 20.4 s host CPU; model on → **WFI 104.5/s (= IRQ rate), busy ratio 0.012, 0.23 MIPS, 1.5 s host CPU**; same frames, boot unchanged. Idle takes the low-clock WFI path (clock select 0x403→0x417→WFI). `[PM] pmSelfRefresh` not reached: LCLK (0xC0400008 bit1) stays on at idle; deep sleep needs the image engine off (display off), not yet scripted. Canon writes 0xC0F01004 itself (0x400 / 0x8400 / 0); the model reads 0x400. **Deep sleep (branch `emu/deep-sleep`):** `selfRefresh` writes CLOCK_ENABLE = 0, and from then until Canon restores it the DIGIC timers, DryOS tick and HPTimers stand still (`eos_clocks_gated`), so only a key/MPU interrupt wakes it; each sleep is logged (`[PM1100D] self-refresh #n in/out after X s, wake irq`). Root cause of LCLK staying on: the JPEG core at 0xC0E00000 never reported ready (bit 24) after reset, Canon's JpCore init failed and leaked its ClockSaver client (slot 3, found with `QEMU_EOS_PM_TRACE=1`: client counts at RAM 0x2BF18, return addresses of the 0xC0F01004 writes); reading 0x1010000 there (as for 1300D/A1100) fixes it. Result (pm run 26, smoke test since run 29): INFO turns the display off, ClockSaver turns LCLK off, `[PM] pmSelfRefresh : In` ~1 s later, the vCPU sleeps for the whole 30 s (IRQ ~5/s instead of ~105/s), INFO wakes it via the MPU interrupt 0x50 (`Out`), the shooting screen and Canon's menu work after it. ML-Optimized CI run 53 (`power/deep-sleep-verify`): canon 1 sleep 29.0 s, upstream-equivalent ML never (counter pinned 1), ML with counted holds 1 sleep 29.1 s and wakes correctly (ML menu, config written, fio holds paired), with an active intervalometer hold no sleep and ML tasks at full rate. Regression: `ci/smoke.sh` requires `pmSelfRefresh : In/Out` and the display-off/woken frames (`bench/expected.json`). Note: the flag first lived in `EOSState` after `clock_enable_6` and was found set by another writer between CLOCK_ENABLE writes (cause not found; possible stray write into that struct area), hence the file-local. Not modelled: RTC/real time across the sleep, LCD auto-off/APO path, card I/O during the sleep, EOSM |
| 5 | 4000D model (branch `emu/4000d`) | 4000D fw 1.1.0 boots Canon firmware to the shooting screen; MENU/arrow scenarios pass; CI smoke test `ci/smoke.sh IMAGE 4000D`. Fixes vs the 1300D: 16 MiB ROM1, no ROM0, boot block overlaid at hivecs, flash ID `C2 25 38`, RTC adjust `0x31`. See `docs/models/4000D.md` |
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

Safety: ROMs fetched from the private package registry at boot, kept in a temp dir, never returned through the API (memory reads are
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

