# Performance (phases 1-2)

How fast and how faithful QEMU-EOS-Optimized is on the 1100D (fw 1.0.5, Canon firmware only), measured in CI on
the shared Forgejo runner (`linux-podman`, 4 vCPUs, 4 GiB). Reference numbers: `bench/baseline.json`.

## Tools

| File | What |
|---|---|
| `bench/bench.py` | One run: boot to the Canon GUI, scripted scenarios, idle window, metrics → `result.json` (also the CI smoke test) |
| `hw/eos/cams/<CAM>/test/bench.json` | Per camera: scenarios, log markers and the pixel hashes of the GUI and scenario frames (`"expected"`, smoke test reference; ML-Optimized's old CI screenshot is pixel-identical). See `docs/models/PROFILES.md` |
| `bench/microbench.py` + `busyloop.S` | ROM-free TCG speed: 600 M guest instructions on `-M versatilepb`, per plugin configuration |
| `bench/parallel.sh` | Same two runs one after the other, then at the same time |
| `ci/bench-suite.sh`, `bench/report.py` | Whole suite (workflow `bench`, manual dispatch) → `summary.md` / `summary.json` artifact |

Metrics: **boot→GUI** = wall time from QEMU start to the first frame of the shooting screen (monitor `screendump`
every 0.1 s, compared by pixel hash); **boot insns** = guest instructions until then (wakeups plugin, interpolated
from 100 ms intervals); **scenarios** = key press → settled screen for `menu` (M), `menu_right` (→), `menu_close` (M);
**idle MIPS** = guest instructions/s over an 8 s idle window: Canon's idle loop spins (0xFF1EA818, see
ML-Optimized `docs/firmware/1100D-powersave.md`), so this is pure emulation speed; **host CPU**, **peak RSS** of the
QEMU process from `/proc`; TB flushes from `info jit`.

Runs are interleaved (5 repetitions per variant) because the runner is shared: during the reference run the load
average was ~8 on 4 CPUs, so single wall-clock numbers swing ±30 %. Compare medians within one suite run only.

## Results (bench run 7, commit 5dda6f5315)

| item | change | metric | before | after | verdict |
|---|---|---|---:|---:|---|
| — | baseline image as ML used it (plugin v1, `-d debugmsg,tasks`) | boot→GUI s / idle MIPS | 1.66 / 39.3 | — | phase-1 reference |
| (a) | wakeups plugin v1 → v2: inline counters at translation time, sampler thread | microbench overhead vs no plugin (opt build) | +0.6 % (v1) | +6.0 % / −1.4 % with `insns=off` | both inside the ±6 % run-to-run noise; the plugin is not a measurable cost |
| (a) | same, on the 1100D | idle MIPS / IRQ/s | 42.2 / 96.8 | 42.3 / 98.2 | identical within noise; self-test counts exact (wfi = irq = 200, 403 830 insns) |
| (b) | post-boot snapshot (`savevm` / `-loadvm`) | time to GUI | 1.5-2 s cold boot | not possible | `-loadvm` fails (`Missing section footer for sd-card`) and `hw/eos` registers **no VMState** for its devices (timers, interrupts, MPU, display, SD/CF): RAM+CPU would be restored over fresh device state. Gain would be ≤ 2 s per run anyway, since boot→GUI is only ~1.5 s |
| (c) | build `baseline` → `opt` (-O2 + LTO, no debug info/debug-tcg, unused block/crypto/accel features off) | microbench MIPS, no plugin | 39.4 | 42.2 | **+7 %** |
| (c) | same | 1100D idle MIPS (plugin v1) | 42.0 | 42.2 | within noise (idle spin is MMIO-bound) |
| (c) | same | QEMU image build time (quiet runner) | 734 s | 299-330 s | **−57 %** (fewer objects, no -g) |
| (d) | `-display none` | — | already | — | screenshots via monitor `screendump` still work |
| (d) | device logging | log size / idle MIPS | 1 019 KiB / 39.3 (ML's `-d debugmsg,tasks`) | 13 KiB / 42.0 (no `-d`) | without `-d` qemu-eos prints only ~13 KiB per boot (MPU/DMA/MMU setup), nothing on hot paths; every per-access log (`io_log`, `-d io/mpu/int`) is already gated. Requested logging costs ~7 % MIPS |
| (d) | TCG `tb-size` | TB flushes / code buffer use | 0 / 13.5 of 32 MiB | — | default is large enough; no change |
| (d) | `-icount` | — | not used | — | stays off (guest time = host time) |
| (d) | MMIO handler lookup cache (new, `hw/eos/eos.c`) | 1100D idle MIPS | 42.3 | 44.5 | **+5 %**; same frames, same IRQ/s. `QEMU_EOS_MMIO_CACHE=0` restores the scan |
| (d) | 250 µs `usleep` per triggered interrupt → 0 (`QEMU_EOS_INT_SLEEP_US`, experiment) | boot→GUI s / boot insns | 1.55 / 29.4 M | 1.84 / 73.5 M | same frames, but the guest executes 2.5× more instructions while booting (timing changes) and is not faster: **default kept at 250 µs** |
| (e) | two 1100D runs in parallel instead of in sequence | wall time of the pair | 21.4 s | 11.0 s | **−48 %**; same frames, idle MIPS −5 %, IRQ/s within noise. Used by ML-Optimized's `run_cam.sh` (`PARALLEL=1`, default; see below) |
| — | all variants | distinct frame sets in 52 runs | — | 1 | behaviour identical across every change |

ML-Optimized `emulation` job: QEMU image now pulled (1-13 s) instead of built (~12 min whenever its Containerfile
changed). Its scenarios now run in parallel (`PARALLEL=1`, default); the first parallel runs showed 18-22 s windows
instead of 30 s. Cause: the plugin times its window from its own load, and a cold start of the freshly pulled
binary on the busy runner took up to ~12 s (sequential runs were hit too, 20-27 s windows). `scenario.py` now
starts its timeline when the monitor socket appears, which QEMU creates right after loading plugins: run 41 has
30.3 / 33.5 s windows, `plugin_clock_lag_s` −0.09 s, `qemu_start_s` 3.8 s, and the same screenshots as before
the migration. Scenario step: 181-213 s sequential (runs 30, 40) vs 188 s parallel (run 41) on this runner,
where load varies a lot between runs.

### Runner speed

Debian bookworm's stock `qemu-system-arm` 7.2 runs the same microbenchmark at 35.9 MIPS vs 42.2 for our `opt`
build, so the low absolute MIPS are this runner (shared, loaded host), not our TCG. The host loop
(`microbench.py`, dependent 64-bit multiply-add) takes ~1.9 ns/iteration there.

### Plugin self-test under load

`ci/qemu/selftest/` checks exact counts (wfi = irq = 200) on a bare-metal guest with a 1 ms host-time timer.
On the loaded shared runner (load ~8-10 on 4 vCPUs) it once saw irq = 201: when the vCPU thread is descheduled
for over 1 ms between the IRQ handler's `IntClr` and the guest's re-mask, the next tick is taken as a second IRQ
in the same cycle. The handler now returns with IRQs masked (I bit set in SPSR), so each unmask admits exactly
one IRQ whatever the host does; the exact counts are unchanged. Under 6 CPU spinners per core: old guest 4/550
runs wrong (irq 201/202), new guest 0/550.

### Snapshots: what would be needed

Each EOS device in `hw/eos` (EOSState: interrupt controller and schedule, DIGIC/UTimer/HPTimer, MPU state machine,
SIO/SPI, display/palette, SD and CF controllers, EDMAC, serial flash) plus the `static` state inside many MMIO
handlers would need a `VMStateDescription`, and the legacy `sd_init()` card would need a reset handler so its
`wp_groups` bitmap has the same size at save and load. Worth it only if a scenario needs a long, state-heavy setup
(e.g. ML menus deep inside a test); for boot (~1.5 s) it is not.

## Reproducing

    # CI: Actions -> bench -> Run workflow (branch), or
    curl -u USER:TOKEN -X POST -H 'Content-Type: application/json' -d '{"ref":"main"}' \
        <forgejo>/api/v1/repos/<owner>/QEMU-EOS-Optimized/actions/workflows/bench.yml/dispatches
    # one run by hand, inside the image, ROMs in ./roms/1100D/
    podman run --rm --tmpfs /tmp -v "$PWD":/work -v "$PWD/roms":/roms:ro -w /work IMAGE \
        python3 -I bench/bench.py --rom-dir /roms --out emu-out/run1 --plugin /opt/qemu-eos/plugins/libwakeups.so \
            --expect
