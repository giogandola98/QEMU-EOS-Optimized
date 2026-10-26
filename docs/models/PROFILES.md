# Camera profiles

A camera profile is one directory, `hw/eos/cams/<CAM>/`, with everything that is specific to one camera: its
entry in the model table, its behaviour hooks and its test data. The shared code (`hw/eos/*.c`, `ci/smoke.sh`,
`bench/*.py`) has no per-camera checks: it reads the fields of the model entry and the profile's test files.
Profiles today (the other upstream models still have their entry directly in `hw/eos/model_list.c`, with
the same fields, see "Upstream models" below):

| Profile | Firmware | Canon GUI | Hooks | CI smoke test | Notes |
|---|---|---|---|---|---|
| `1100D` | 1.0.5 | shooting screen, menus | `pm.c` (deep sleep), `capture.c` | boot, MENU, display off/selfRefresh/wake, shutter + card | validation camera |
| `4000D` | 1.1.0 | shooting screen, menus | `pm.c` (deep sleep), `capture.c` | boot, MENU, display off/selfRefresh/wake, shutter + card | [`4000D.md`](4000D.md) |
| `EOSM` | 2.0.2 | MENU, PLAY, Q, INFO screens and LiveView overlays (LiveView buffers filled with the camera's frames, no Canon pipeline) | `lv.c` (LiveView frame injection, init hook), quirk `I2C_TX_READY` | boot, MENU, PLAY, Q, INFO, half-shutter | needs `SFDATA.BIN`; [`EOSM.md`](EOSM.md) |

## Layout

| File | Needed | What |
|---|---|---|
| `model.inc.c` | yes | The camera's `struct eos_model_desc` initializer body (designated fields, no braces). `model_list.c` includes it inside `eos_model_list[]`, in the group of its DIGIC version |
| `profile.h` | yes | Declarations of the hooks and data `model.inc.c` points to (may be empty). Included at the top of `model_list.c`; must build without QEMU headers (plain C, `<stdbool.h>`/`<stdint.h>` only) |
| `pm.c` | no | Power-management model (`.pm_handler`, signature `eos_pm_handler_fn`, `hw/eos/pm.h`) |
| `capture.c` | no | Still-photo capture data (`.capture`, `struct capture_profile`, `hw/eos/capture.h`, `docs/models/capture.md`) |
| any other `*.c` | no | Built automatically: `hw/eos/Makefile.objs` compiles every `cams/*/*.c` except `*.inc.c` |
| `test/bench.json` | for CI | Test data for `bench/bench.py`: see "Test data" |
| `lv.c` | no | LiveView buffers for frame injection (`hw/eos/lv_inject.h`), started from `.init_hook` (EOSM) |
| `test/smoke.sh` | for CI | Smoke-test settings for `ci/smoke.sh`: see "Test data" |

`test/` is not part of the QEMU build: it is excluded from the image hash (`ci/build-image.sh`) and from the build
context (`.containerignore`), so changing test data does not rebuild QEMU.

## Model entry fields

A model entry has two kinds of fields (`hw/eos/model_list.h`):

- **Hardware parameters** (inside the `params[]` union: memory map, ROM sizes, interrupts, MPU/RTC/serial-flash
  registers, `firmware_start`, `rom1_hivecs_offset`, ...). A zero field takes the value of the DIGIC default entry
  (the nameless entry that starts the group in `model_list.c`).
- **Model-specific behaviour** (outside the union, never inherited). Zero / NULL = the generic upstream behaviour:

| Field | Type | Meaning |
|---|---|---|
| `pm_handler` | function | Called for every MMIO access before the regular handlers (PowerMgr idle loop, selfRefresh). `QEMU_EOS_PM_MODEL=0` disables it |
| `capture` | data | Shutter release emulation (`hw/eos/capture.c`) |
| `init_hook` | function | Called at the end of `eos_init_common()`, after the MPU and engine init and before the DIGIC 6+ start address: ROM patches, boot flags, extra RAM images |
| `mpu_spells` | name | MPU init spell set (`hw/eos/mpu_spells/<name>.h`) when the camera has none of its own, e.g. `MODEL_NAME_60D`. NULL = the set named like the model, else the generic set |
| `button_codes` | name | MPU button code table (`mpu_spells/button_codes.h`), same rule |
| `flash_id` | value | Answer to the flash ID command (0x06 written to `0xF8000000`), e.g. `0x003825C2` = `C2 25 38` |
| `adc_values`, `adc_count` | data | DIGIC 6 ADC (`0xD9800000`) readings by channel |
| `adc_ps_values`, `adc_ps_count`, `adc_ps_battery` | data | PowerShot ADC (`0xC0900040`, half-word channels read in pairs) and its battery register `0xC09000B0` |
| `quirks` | flags | `EOS_QUIRK_*` (below); tested in the shared code with `EOS_QUIRK(NAME)` |

Quirks (`enum eos_quirk`): one flag per hardware difference that is a single branch in a shared handler.

| Flag | Effect | Set on |
|---|---|---|
| `FLASH_CTRL_RO` | writes to the flash command registers `0xF8000AAA` / `0xF8000554` are dropped | A1100 |
| `DISP_LINE_DOUBLE` | 720x240 LCD shown line-doubled, bitmap pitch 720 after a palette write, display `0x014` reads 4 | A1100 |
| `DISP_YUV_HALF` | LiveView YUV buffer has half the display height | 1100D |
| `DISP_BMP_YUV8B` | bitmap drawn as 8-bit YUV (no separate YUV overlay) | EOSM3, M50, RP |
| `DISP_BMP_PAD16` | BMP VRAM register: `0x17410` / `0x18010` keep the buffer, pitch = (width + 16) * 2 | EOSM3 |
| `RTC_PS` | PowerShot RTC: chip select = GPIO bit 11, own protocol on SIO2 (`0xC08202xx`), GPIO `0x68` = RTC com, initial registers from a D10 | A1100 |
| `RTC_NIBBLE_SWAP` | RTC command in the high nibble, register in the low one | 5D2, 50D, 40D |
| `RTC_REG0_0C` | RTC register 0 starts at 0xC | 400D |
| `PS_IS` | PowerShot image stabiliser: SIO `0xC08204xx`, init registers `0xC0500040-58` | A1100 |
| `PS_KEYS` | PowerShot keys: GPIO `0x134` start key, `0x138` playback start, `0x200-0x208` key bits | A1100 |
| `SD_ACMD41_REPLAY` | the repeated CMD55 / ACMD41 is answered from the first one | A1100 |
| `SD_IRQ_EN_ONLY` | SDIO `0x14` only stores the IRQ mask (transfers end through SDDMA `0x10`) | A1100 |
| `UART_TIO_KEEP` | UART control writes keep the TX interrupt enable | EOSM3 |
| `I2C_RANDOM` | I2C status `0xD6050000` reads random values | EOSM10 |
| `JPCORE_READY` | JPEG core `0x00` reads `0x1010000` (ready after reset) | 1300D, 4000D, A1100, 1100D |
| `JPCORE_IRQ_DONE` | JPEG core `0x44` reads `0x400` (done) instead of a random value | 1300D, 4000D, A1100 |
| `GPIO_11C_LOW` | GPIO `0x011C` (VIDEO CONNECT) reads 0 | 40D |
| `GPIO_VSW_6000` | GPIO `0xF480` (VSW status) reads `0x6000` | 5D2, 50D |
| `GPIO_F48C_HDMI` | GPIO `0xF48C` reads `0x4000000` (HDMI disconnected), skips Canon's system check program | 1300D, 4000D |
| `I2C_TX_READY` | I2C `0xC0090000`: status bit 4 ("TX data ready") is set when a transmit starts, not only after the first byte | EOSM |

When a new camera needs a different behaviour: if it is one more value or branch in an existing handler, add a
field or a quirk (default 0 = old behaviour, document it here); if it is a model of its own (a register block, a
state machine), add a hook function in the profile, as `pm_handler` does.

## Test data

`test/bench.json` (read by `bench/bench.py --cam <CAM>`; keys starting with `_` are comments):

| Key | What |
|---|---|
| `debugmsg` | Address of Canon's DryosDebugMsg in this ROM (`--debugmsg` logs it through the plugin) |
| `scenarios` | `[[name, [keys...]], ...]`: keys sent after the GUI is up; the settled frame is saved as `name` |
| `gui_uart` | Optional: a UART line that must appear before the GUI frame is looked for, for cameras whose boot screen does not change when the GUI comes up (EOSM: the LCD stays off in LiveView). Without it the first settled frame (or the `gui` hash) is the GUI |
| `uart_markers` | Strings that must appear on the UART once the GUI is up (e.g. `K422 READY`) |
| `log_markers` | Strings expected in the DryosDebugMsg log (reported, not required) |
| `expected` | Frame name -> pixel hash (`gui`, the scenarios, the smoke test's `shot:` steps); checked with `--expect` |

`test/smoke.sh` (sourced by `ci/smoke.sh IMAGE <CAM>`):

| Variable | What |
|---|---|
| `FW` | ROM package version: the ROMs are fetched as `<CAM>-<FW>` from the private package registry and checked against `ci/rom-md5.txt` (ROM dumps are never in git) |
| `OUT` | Results directory under `emu-out/` (uploaded as CI artifacts, without binaries) |
| `EXTRA` | Extra `bench.py` arguments, shell-quoted on one line: the camera's own checks (deep sleep steps, shutter press, minimum log counts, card contents) |

## How to add a camera

1. **Machine.** If the camera is not one of the upstream qemu-eos models: add `MODEL_NAME_<CAM>` to
   `hw/eos/model_list.h` and its machine (`eos_<CAM>_machine_init` + `DEFINE_MACHINE`) to `hw/eos/eos.c`.
2. **Profile directory** `hw/eos/cams/<CAM>/` with `model.inc.c` (start from the closest model on the same DIGIC,
   set only the fields that differ) and `profile.h`. Include both in `hw/eos/model_list.c`: `profile.h` at the
   top, `model.inc.c` as its own entry in the right DIGIC group:

       #include "cams/<CAM>/profile.h"
       ...
           {
       #include "cams/<CAM>/model.inc.c"
           },

   If the camera already has an entry there, replace it (`1100D`, `4000D` and `EOSM` were moved this way, field
   for field).
3. **Behaviour.** Set the fields and quirks the camera shares with others (`mpu_spells`, `button_codes`,
   `flash_id`, `EOS_QUIRK_*`); write hooks (`pm.c`, `capture.c`, ...) for what is its own, and declare them in
   `profile.h`. Comments say *why* (which ROM routine waits for what); never copy Canon code, only addresses and
   observed behaviour.
4. **ROMs.** Upload the dumps as package `<CAM>-<FW>` to the private registry and add their MD5 to
   `ci/rom-md5.txt`. DIGIC 5 cameras with a serial flash (`serial_flash_size`) also need `SFDATA.BIN` (ML's
   `sf_dump`); without it the flash starts erased and Canon's start-up usually stops at its property
   packages (the EOS M one was rebuilt from a RAM dump, `docs/models/EOSM.md`).
5. **Test data.** `test/bench.json` (run `bench.py` once without `--expect` to get the frame hashes, check the
   PNGs) and `test/smoke.sh`; add a step `ci/smoke.sh "$(cat .image)" <CAM>` to `.forgejo/workflows/ci.yml`.
6. **Docs.** `docs/models/<CAM>.md`: memory map, what had to be fixed and why, debug addresses, open questions.

## What is still outside the profiles

- **Upstream models** (5D ... XF605): their entries stay in `model_list.c`, with their former name checks
  converted to the fields above, so a later move into `cams/<CAM>/` is a cut and paste. Their init hooks
  (`eos_init_7D`, `eos_init_200D`, `eos_init_EOSM3`, `eos_init_EOSM10`, `eos_init_EOSM5`, `eos_init_5D3eeko`) and
  the A1100 handlers (`eos_handle_A1100_rtc`, `_IS_com`, `_IS_init`) stay in `eos.c`: they use its static state
  (RTC, SIO, SD), so moving them would mean exporting that state first.
- **Machine registration** (`DEFINE_MACHINE` list in `eos.c`, step 1 above): one QOM type per model, with the
  description and CPU count; kept next to the other machines.
- **MPU spell and button tables** (`hw/eos/mpu_spells/`): shared by several cameras (the 1100D and 1200D use the
  60D spells), so they stay in one place; a profile only names the set it uses.
- **memcheck stubs** (`hw/eos/dbi/memcheck.c`, 500D only) and the per-model paths of `dbi/logging.c`: debugging
  tools that look up firmware addresses by model name, not emulation behaviour.
- **Suites for one camera**: `ci/pm-suite.sh` and `ci/bench-suite.sh` measure the 1100D on purpose (the PM model
  A/B and the performance baseline).
