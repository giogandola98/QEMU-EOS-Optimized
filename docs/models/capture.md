# Still-photo capture (shutter release): 1100D and 4000D

Status: **a full shutter press runs Canon's whole shooting sequence and the picture is written to the
emulated SD card**, with Canon's own EXIF (exposure time, aperture, ISO, exposure bias, mode). 1100D: a
CR2 (RAW quality on the card image); 4000D: a JPEG. CI: `ci/smoke.sh` presses the shutter on both cameras and
reads the card after QEMU exits (`bench/card.py`).

Code: `hw/eos/capture.c` (generic) with the per-camera data in the camera profiles,
`hw/eos/cams/1100D/capture.c` and `hw/eos/cams/4000D/capture.c` (`struct capture_profile`, `capture.h`),
selected by `eos_model_desc.capture` in the profile's `model.inc.c` (NULL = no capture emulation, the other
cameras are unchanged; `docs/models/PROFILES.md`); small hooks in `mpu.c` (keys,
MPU traffic), `eos.c` (interrupt enable, GPIO) and `engine.c` (JPEG core registers, EDMAC output data, HEAD
timer start, deterministic dark frame).

## What the firmware needs, in order

| Step | Firmware side (log markers) | Emulated by |
|---|---|---|
| SW1 | `emDeliverMulticastEvent : SW1ON`, metering start | MPU: GUI switch `06 05 06 26 01`, `EVENTID_METERING_START_SW1ON` (`05 00`) |
| release | `scsReleaseOn`, `scsReleaseStart`, `StartupCaptureDevice` | MPU: `EVENTID_RELEASE_ON` (`05 01`), `EVENTID_RELEASE_START` (`05 0e`, ShootCapture input 1) |
| exposure data | `scsReleaseData`, `CreateJob`, `iso:..,tv:..,av:..` | MPU: `EVENTID_RELEASE_DATA_SW2ON` (`05 02`), payload 0x2D bytes: Tv `[0]`, Av `[1]`, ISO `[4]`, temperature `[23]`, `[25]`; `[0x2B]` = add_size 1, `[0x2C]` = 1 (electronic first curtain path; without it the 1100D asserts in CaptureState.c) |
| sensor ready | 1100D `EShutCapture` / 4000D `NormalCapture`, `VSizeSetting` | the DIGIC clears a "release" GPIO line (1100D `0xC02200EC` bit 1; 4000D `0xC022D0E8`, DIGIC 4+ values `0x93D800` → `0x83DC00`); the line must read back what was written (the 1100D checks the pin mode bits first) |
| exposure | `scsCapReady`, `Start Loop dwVDCount = n` | `CAPREADY` interrupt 0x51, fired on that falling edge. Exposures longer than a frame period count VD (HEAD1, 0x6A) interrupts: during a release a started HEAD timer repeats (its period long enough that Canon's own "last VD" HPTimer comes first, as on the camera) until the readout is done |
| bulb | `scsBulbEnd`, `Accumulation Stop`, `Last VD Count` | Tv = BULB (0x0C): SW2 off sends `EVENTID_BULB_END` (`05 05`); if SW2 goes off before the exposure has started (the emulated release is slower than the camera's) it is sent at the first VD period |
| readout | EDMAC#2 raw (`[CAPTURE] Generating a WxH dark frame`), `CapEnd`, `scsFinalReadoutDone` | existing EDMAC/PREPRO model; the raw frame is a fixed pseudo-random pattern around black (was Gaussian noise from `rand()`: slow, and different every run) |
| shutter closed | `scsAfterReleaseDataCBR`, `cam event after data` | on the next falling edge of the release line: `EVENTID_AFTER_RELEASE_DATA` (`05 03`), `EVENTID_RELEASE_END` (`05 0f`) |
| fencing / face detection | `[FB] START`, `[FB] FencingCompleteCBR` | Fencing_A/B/C (0xC9/0xD0/0xD1) fire once per operation: on the enable that starts it, not on DryOS' re-enable after the handler |
| compression | `[TTJ] ... JpCoreCompleteCBR( size )`, `JPEG(4272,2848,..)` | JP51 `0x44` = 0x400 (done; was `rand()` on the 1100D), `0x24` = output size. The output (EDMAC connection 5) is a valid baseline grayscale JPEG of the size in JP51 `0x080` (height << 16, width), uniform level: 128 for the first shot, 16 levels per stop of exposure difference from it (Tv, Av, ISO latched at the release), so bracketed frames differ. The firmware checks that it starts with `FF D8 FF DB`. Lossless runs (JP51 `0x004` low bits = 2, the CR2 raw) get 256 KiB of zeros |
| file | `B:/DCIM/100EOS5D/IMG_nnnn.CR2 write done` | existing SD model |
| SW2/SW1 off | `scsReleaseOff`, `cam event release end` | on key release: `05 04`, `05 0b`; while a release runs they are held back until its end (else `ERROR scsEventDispatch ... dwEventID=18`) |
| remote release (ML `call("Release")`, PTP, ML `SW1()`/`SW2()`) | `Release`, `REMOTE_RELEASE_OK` | DIGIC → MPU `PROP_REMOTE_SW1` (`04 16`) / `PROP_REMOTE_SW2` (`04 17`): the MPU answers with the same property carrying 0 (the PTP layer reads it as the result; the value itself gives `REMOTE_RELEASE NG`) and plays SW1 on / the release / SW2 off / SW1 off |

Keys: **left Ctrl** = full shutter (press: SW1 + release; release: SW2 off + SW1 off), Shift = half shutter
(as before). Exposure values come from the property messages either side exchanges (`PROP_SHUTTER` `01 05`,
`PROP_APERTURE` `01 06`, `PROP_ISO` `01 07`, `PROP_AE` `01 08`, `PROP_SHOOTING_MODE` `01 00`), so what ML
sets is what the next shot reports: `[CAPTURE] shot n: tv=.. av=.. iso=.. ae=.. mode=..` (emulator), Canon's
`iso:..,tv:..,av:..` (ShootCapture) and the EXIF of the file.

Start-up exposure: the 4000D's MPU init table sends Tv/Av/ISO; the 1100D's (60D-derived) does not, so its
profile holds the values Canon restores from the settings in the CI ROM image (1/6 s, f/18, ISO 100), until a
property message changes them. To start a test from a given shutter speed, let the MPU report it once the
GUI is up, as if the dial had been turned: F5 with `06 05 01 05 <tv> 00` in `$QEMU_EOS_SPELLS_DIR/f5.txt`
(0x0C = BULB in M mode; Canon's shooting screen keeps the old value until it is redrawn). Penumbra's shooting
scenarios do this.

Debugging: `QEMU_EOS_CAPTURE_TRACE=1` logs the capture steps and the JP51 runs, `=2` also every GPIO
write after the first release. F5..F8 queue the MPU messages listed in `$QEMU_EOS_SPELLS_DIR/f5.txt`..`f8.txt`
(one message per line, hex), which is how the sequence above was found.

## Limits

- No image processing: the sensor data is a fixed pattern, the JPEG a uniform gray frame, the CR2 raw data
  zeros; nothing is developed from the sensor data. Playback of these files shows Canon's decode error (JPEG
  decoding is not emulated).
- Timing is not physical: an exposure takes as many VD periods as Canon programs, at the emulator's period,
  not the shutter speed; a bulb exposure ends when SW2 goes off (or at the first VD period if that was
  earlier), so the EXIF exposure time of a bulb shot is not what was requested.
- Only the paths seen here: single shot, the card's quality setting (RAW on the 1100D image, JPEG on the
  4000D), no LiveView capture, no flash, no continuous drive, no mirror lockup, no dark-frame noise reduction
  beyond what the firmware does with the dummy frame.
- Exposure in auto modes is whatever the MPU last reported (no metering); in M mode it is exactly what was set.
- 4000D: back-to-back releases (ML HDR) are captured with the right exposures, but developing the overlapping
  pictures to files stalls in about one run in three (TwoInTwoOut JPEG path: EDMAC/JPEG-core completions of
  overlapping runs get attributed to the wrong run in the engine model); spaced shots
  (intervalometer, single presses) are reliable. The 1100D develops back-to-back frames reliably.
- Other cameras: `capture_profile = 0`, unchanged behaviour.
