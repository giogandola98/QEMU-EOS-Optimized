# Canon EOS M, firmware 2.0.2

Status: **boots Canon firmware to its GUI headless; MENU, PLAY, Q, INFO, arrows, SET and the half-shutter
work. The LiveView screen shows Canon's overlays on grey (no sensor image).** CI smoke test:
`ci/smoke.sh IMAGE EOSM` (step "EOSM smoke test" in `.forgejo/workflows/ci.yml`), skipped cleanly when the
ROMs are not available. Camera profile: `hw/eos/cams/EOSM/` (`docs/models/PROFILES.md`).

| Item | Value |
|---|---|
| Model ID | 0x331 (`K331 READY` on the UART) |
| SoC / OS | DIGIC 5, ARM946E-S (the bootloader programs 946 protection regions), DryOS, ICU firmware 2.0.2 (inner 9.9.8) |
| Base model | upstream qemu-eos `EOSM` entry (own MPU spell table and button codes), unchanged except the quirk below |
| ROMs | `ROM1.BIN` 16 MiB = the firmware; `ROM0.BIN` is a bus pattern (no chip, not loaded, as upstream); `SFDATA.BIN` 8 MiB serial flash, see below |
| ML platform | `platform/EOSM.202` in Penumbra (`stubs.S`: DryosDebugMsg `0x40D4`, current_task `0x3DE78`) |

## What had to be fixed

1. **Serial flash (`SFDATA.BIN`).** The FIX and TUNE property packages (camera settings, model name,
   factory calibration) live in the 8 MiB SPI flash, not in ROM0/ROM1. Without them start-up stops:
   `PROPAD_CreateFROMPropertyHandle : FIX (0x1d)` / `TUNE (0x1d)`, then `ASSERT : RscMgr.c line 239`
   (property 0x2, the model name, not found). The emulator used to exit when `SFDATA.BIN` was missing; it
   now starts with an erased flash and says so (`hw/eos/serial_flash.c`), which reproduces exactly that
   assert.

   No sf_dump of this camera exists, but a 256 MiB RAM dump (`ML/LOGS/RAM4.BIN`) does, and Canon keeps both
   packages in RAM after loading them. `hw/eos/cams/EOSM/test/sfdata_from_ram.py` writes them back into a
   flash image; the result (`a9523d5b...`) is stored with the ROMs in the private package `canon-rom/EOSM-202`
   and listed in `ci/rom-md5.txt`. How the firmware reads the flash, as found in the ROM:

   | What | Where | Behaviour |
   |---|---|---|
   | FIX handle | `0xFF0C3570`: flash `0x340000`, 0x1000-byte sectors x 0xC0, one 0xC0000 slot | loaded into a heap buffer |
   | TUNE handle | `0xFF0C35D0`: flash `0x10000`, 0x330 sectors, one 0x330000 slot | loaded to DRAM `0x4FE00000` (`DRAMAddr` in the log) |
   | Package scan | `0xFF1207B0` | reads a 36-byte header per slot; word 0 `0x0000FFFF` = valid package (`0` = invalid), word 1 = size <= slot |
   | Load | `0xFF120D98` | copies `size` bytes to RAM and writes `0x00FFFFFF` into word 0 of the copy |
   | Small reads (< 512 bytes) | `0xFF139BCC` | READ (0x03) of 512 bytes, then each byte rebuilt as `in[i] << 4 \| in[i+1] >> 4`: the data arrives 4 bits late |
   | Large reads | `0xFF139030` (`readSerialFlashWithQuad`) | QOFR (0x6B) + DMA; Canon sizes the transfer as 0x7F0 data bytes per 0x800, which `serial_flash.c` already replays (block of 0x7F0 from every 0x800 of the file, same nibble shift) |

   ML's `sf_dump` for this camera calls the raw read (`0xFF138554`), so a real dump has the same raw layout
   the emulator replays; `sfdata_from_ram.py` produces that layout (nibble-delayed data, each 0x7F0-byte
   piece on a 0x800 boundary). Only the two packages are filled in, the rest of the flash is erased; Canon
   finds no other package there (`SerialFlash Packages!! 0x7` once per handle is the scan for the next free
   slot) and boots normally.

2. **Audio IC on I2C (`EOS_QUIRK_I2C_TX_READY`).** `InitializeAudioIC` writes to the audio codec (I2C slave
   0x13) through `localI2C_Write` (`0xFF3460C4`): it starts a transmit (config `0xC0090024` = 0x620, then
   0x2E20) and polls status `0xC0090008` bit 4 ("TX data ready") before the first byte. The generic handler
   only set that bit after a byte was written, so every write timed out (`localI2C_Write : 316`,
   `[AUDIO] ERROR I2C abort`) and the start-up task retried for ever with a grey screen. The quirk sets bit 4
   when a transmit starts (config bits 0x20 and 0x200). Only the EOS M sets it.

Nothing else was needed: display, SD card, RTC, MPU spells and button codes from the upstream entry work.

## What works / what does not

| Works | Evidence (frames in `hw/eos/cams/EOSM/test/bench.json`) |
|---|---|
| Bootloader -> main firmware -> `startupInitializeComplete` -> GUI | UART `K331 READY`, `[STARTUP] startupInitializeComplete`, `[MC] guimode comp. 0` |
| MENU: opens on the last tab used (setup 4, "Vers. firmware: 2.0.2", Italian UI), right arrow -> My Menu, SET enters it, MENU back/close | `menu`, `menu_right`, `menu_close` |
| PLAY ("Nessuna immagine": the test card has no pictures) and back | `play`, `play_close` |
| Q (quick control: AF method, drive, quality, WB, ...) and back | `quick`, `quick_close` |
| INFO: shooting-settings screen (Av, F4.0, ISO 800, RAW, 16 shots), then LiveView with the info bars | `info`, `info_lv` |
| Half-shutter: `IDLEHandler PRESS_SW1_BUTTON` / `UNPRESS_SW1_BUTTON`, metering timer | smoke test `--min-count` |
| SD card mounted, file numbering from the camera's settings (100_6561) | log |
| `-d debugmsg` with DryosDebugMsg `0x40D4` (RAM copy, as in ML's stubs) | `bench.py --cam EOSM --debugmsg` |

Not working / not checked:
- **LiveView image.** The EOS M boots into LiveView. Canon starts the LiveView pipeline (`evfActive`, sensor
  power-on, ADTG/CMOS writes) but no frames come back, so the display stays muted until another screen
  (menu, play) turns it on; the LiveView screen then shows Canon's overlays (grid, AF frame) over grey. The
  boot screen is therefore not a GUI sign: `bench.json` sets `gui_uart` and has no `gui` hash. Needs the
  LiveView work of `docs/PLAN.md` (frame injection into the YUV buffers `0x46000080` / `0x463CC080`
  reported by `Evf`, plus the VD/EDMAC events Canon waits for).
- Overlay bars in LiveView with INFO are drawn magenta: palette entries with partial opacity are shown
  opaque. Cosmetic, same as other DIGIC 5 models upstream.
- Full shutter, capture and pictures on the card: not looked at (see "Capture" below).
- MPU spells that the table does not answer (logged as `unknown`, nothing visible breaks): `PROP_LV_LENS`,
  `PROP_LV_AF_RESULT`, `PROP_Q_POSITION`, `PROP_TFT_STATUS 0`, `PROP_BATTERY_REPORT`, `COM_FA_CHECK_FROM`,
  `PROP 80020012`, `09 24`/`09 25`/`0e 00` groups.
- `[SD] ERROR SDINTREP=0x00000000` once (card mounts anyway, as on the 4000D), `[TCH]ERROR TouchPanelIC Ack
  Error` (touch panel not emulated), `[PRP] NOT FOUND PROPERTY ID = 0x02050038` (one property absent from the
  rebuilt packages).
- Properties Canon writes back to the serial flash stay in memory: `SFDATA.BIN` is never written.

## Power management

PowerMgr task: `0xFF341390`, state at RAM `0x4D750` (+0x0C = mode set by `PM_Execute`, `0xFF3415C0`), the
power-save counter at RAM `0x3EE5C` (`EnablePowerSave` `0xFF137E48` calls `PM_Execute(2)` when it drops to 0,
`DisablePowerSave` `0xFF137DDC` calls `PM_Execute(1)`). The loop is the same as the 4000D's:

| Mode | When | What Canon does |
|---|---|---|
| 1 | counter > 0 | IRQ/FIQ masked WFI (`0xFF341648`, ARM946 `mcr p15,0,r0,c15,c8,2`), back on the next interrupt |
| 2, LCLK (`0xC0400008` bit 1) set, `0xC0F01004` == 0x400 | counter 0 | low-clock WFI bracketed by `0xC0400030/44/48/134` writes |
| 2, more than 500 ticks since the last stamp | counter 0 | `[PM] pmSelfRefresh : In`, pre hook, selfRefresh (`+0x14` = `0xFF0CEDCC`, logs `PM : IN`/`PM : OUT`, clears `0xC0400288/290/294` and `0xC04000B4` bit 2, then works on `0xC0201300`), post hook, `Out` |

**Deep sleep is not reached in normal use**, so there is no `pm.c` yet: the counter stays at 1 in LiveView
(one holder remains after start-up), at 2 in PLAY and the menus (DisplayMgr holds one more while the LCD is
on), and the EOS M has no display-off state short of auto power off. Measured: after 75 s in PLAY the mode
is still 1 and the counter 2; idle in LiveView already sleeps in the mode 1 WFI (smoke test: 0.76 MIPS,
106 IRQ/s, 103 WFI/s, busy ratio 0.05). A `pm.c` like the 4000D's (clock gate from `CLOCK_ENABLE` = 0 to
its next write, log of each sleep) becomes useful once something drives the counter to 0, e.g. ML turning
LiveView and the display off; the selfRefresh routine above (and whether it polls a wake flag near
`0xC0201300` like the 1100D's `0xC0201308`) is where to start.

## Capture (not done)

What a `capture.c` would need, as for the 1100D/4000D (`docs/models/capture.md`): the EOS M takes pictures
from LiveView, so the release path differs from the DSLRs' mirror sequence. Needed: the MPU release events
and their payload for this model, the sensor readout (LiveView must stop and the still-capture path run:
`ShootCapture`, `FrontShtDevelop`/`RearShtDevelop` and the "Diana" variants seen at start-up), the JPEG core
status values for DIGIC 5, and the card write. The LiveView frame source should come first.

## Debug addresses

| Symbol | Address | Evidence |
|---|---|---|
| `DryosDebugMsg` | `0x40D4` | ML stubs; `-d debugmsg` prints the start-up log |
| current_task | `0x3DE78` | ML stubs, upstream entry |
| power-save counter | `0x3EE5C` | `EnablePowerSave`/`DisablePowerSave` |
| PowerMgr state | `0x4D750` | see above |

## Timing (local build, host load 10-12)

Boot to `startupInitializeComplete`: 2.3-6 s wall (about 25 M guest instructions; reading the 2.4 MiB of
property packages byte by byte over SPI takes about 1 s guest time). Scenarios 0.1-0.7 s each. Peak RSS
about 130 MiB.

## Open questions and useful captures from the real camera

- **`sf_dump`** of this camera (Penumbra has the module in `modules/dev_tools/sf_dump`, with the EOS M 2.0.2
  addresses): a real `SFDATA.BIN` would replace the rebuilt one, add whatever else lives in the flash, and
  confirm the raw layout (nibble offset, 0x7F0-per-0x800 DMA blocks) assumed here.
- **MPU spell log at boot** (`mpu_send`/`mpu_recv` to a file): answers for the `unknown` spells above.
- **LiveView buffers** already captured (`PH-LV/LCD/LV-000.422`, 720x480 UYVY; `HD-000.422`): the frame
  source for the LiveView injection.
