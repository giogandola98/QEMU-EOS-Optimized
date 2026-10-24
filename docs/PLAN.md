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
| 5 | 4000D deep sleep (branch `emu/4000d-pm`) | Per-camera PM hook: `eos_model_desc.pm_handler` (`hw/eos/pm.h`), used by the 1100D (`eos_pm_1100d`) and the 4000D (`hw/eos/cams/pm_4000d.c`); smoke-test settings per camera in `ci/smoke/<CAM>.sh`. 4000D `selfRefresh` sleeps in a WFI with IRQs masked (no wake-flag poll), so the model only adds the clock gate: one sleep of ~17 s with the display off, woken by INFO (MPU 0x50), shooting screen back. See `docs/models/4000D.md` |
| 5 | photo capture (branch `emu/shutter`) | 1100D and 4000D: a full shutter press (left Ctrl) or a remote release (ML `call("Release")`, `SW1`/`SW2`, PTP) runs Canon's shooting sequence to the end and writes the picture to the card (1100D CR2, 4000D JPEG) with Canon's EXIF; Tv/Av/ISO/EC that ML set are reported per shot (`[CAPTURE] shot n`, Canon's `iso:..,tv:..,av:..`, EXIF). MPU release events with exposure payload, release GPIO line → CAPREADY, fencing completions, JPEG core status/size and a placeholder JPEG whose level follows the exposure, periodic VD for long exposures, bulb end; deterministic dark frame. Per-camera data in `hw/eos/cams/capture_<cam>.c` (`eos_model_desc.capture`). `ci/smoke/<CAM>.sh` presses the shutter on both and checks the card (`bench/card.py`). No image processing, timing not physical. See `docs/models/capture.md` |
| 3 | partly | plugin side done in phase 2 (translation-time inline counters); DryOS task-switch trace still `-d tasks` |

## Camera profiles (modular emulation)
Goal: emulating a new camera means adding one profile directory, not editing shared code and scripts.
- `cams/<CAM>/`: hardware parameters (memory map, ROM layout, interrupts, MPU registers; today in `model_list.c`),
  behaviour hooks (flash ID, JPEG core, power management / deep sleep, capture) as functions registered on the
  model instead of `strcmp(model->name, ...)` checks in shared code, MPU spells and button codes, test data
  (expected screens, scenario keys, readiness markers, ROM package version).
- Shared code (`hw/eos/*.c`, `ci/smoke.sh`, `bench/*.py`) reads the profile; no per-camera `case` statements.
- Penumbra side: `platform/<CAM>/emulation.*` holds the ML-specific values (DryosDebugMsg address, power-save
  counter, scenarios to run); its `ci/qemu/run_cam.sh` reads it.
- Order: after the 4000D deep-sleep and capture work lands (both written as per-camera hooks), migrate 1100D,
  4000D, EOSM, then the other upstream models.
- **Status (branch `emu/profiles`):** `hw/eos/cams/{1100D,4000D,EOSM}/` hold their model entry (`model.inc.c`),
  hooks (`pm.c`, `capture.c`) and test data (`test/bench.json`, `test/smoke.sh`); every former
  `strcmp(model->name, ...)` in `eos.c`, `engine.c` and `mpu.c` is a field or `EOS_QUIRK_*` flag of the model
  entry (the upstream models set theirs in `model_list.c`); `ci/smoke.sh` and `bench/bench.py` read the profile.
  Pure refactor: CI run 53 green with the same smoke results as main (run 52); locally, main and this branch
  give identical frames, log counts and card contents on the 1100D and 4000D, and identical start-up output on
  every model. How to add a camera: [`docs/models/PROFILES.md`](models/PROFILES.md).

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

