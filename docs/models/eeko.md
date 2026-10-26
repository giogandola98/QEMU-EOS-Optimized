# Eeko (DIGIC 5 image-processing core): mailbox model

Status: **EOS M: the main CPU's Eeko clients get their answers; LiveView white balance starts without the
`LvWbAlgorWrap.c` assert.** Eeko's own code does not run: `hw/eos/eeko.c` answers the inter-processor mailbox
the way Eeko's message loop does. Other cameras: unchanged (the block only logs, as upstream).

## What Eeko is

DIGIC 5 has a second ARM core, Cortex-R4 class (Thumb-2, its own DryOS), called Eeko in Canon's sources. The
main CPU loads Eeko's code at boot but starts the core only when a client needs it: still-image development
(RAW to YUV/JPEG), JPEG/H.264 thumbnails, white balance and colour calculation, Hybrid AF line signals, face and
motion recognition, creative filters. On the EOS M the clients and Eeko's tasks are:

| cmd | client (main CPU) | Eeko task |
|---|---|---|
| 0 | SS_PROC: still RAW -> SSRAW -> YUV | StillTask |
| 1 | IVA_PROC | - |
| 2 | APP1_PROC: HDR, multi-exposure, filters | AppTask1 |
| 3 | JP62_PROC: JPEG / H.264 codec, thumbnails | Jp62Task |
| 4 | DSLR_COLOR_PROC: white balance, ALO, colour | ColorTask |
| 5 | DSLR_SAF_PROC: Hybrid AF | SAFTask |
| 6 | CREATIVEFILTER_PROC | CreativeFilterTask |
| 7 | RECOGNITION_PROC | RecognitionTask |
| 8 | system | systemtask |

## Mailbox (0xD02C2000, both cores)

Eight channels of 0x40 bytes; the main CPU sends on ch0/ch1 and receives on ch2/ch3. A message is two words:
`cmd << 24 | param` (24 bits, `subclient << 8 | message` for the clients above), then `arg` (usually a pointer
to a job structure in uncached RAM, with the uncached bit cleared).

| Offset | Main CPU use |
|---|---|
| +0x00 | words waiting (RX FIFO count on a receive channel; TX FIFO count on a send channel) |
| +0x04 | TX FIFO, two writes per message |
| +0x08 / +0x0C | enable / acknowledge the "TX drained" interrupt of a send channel |
| +0x10 | doorbell, 0 then 1: interrupt on the other core |
| +0x14 | RX FIFO, two reads per message |
| +0x18 | 0xB: RX interrupt configuration |
| +0x20 / +0x24 | enable / acknowledge a doorbell |
| 0xD02C2420 / 0xD02C2424 | global enable / acknowledge of interrupt 0x124 |

Main-CPU interrupts: `0x100 + 8*ch` data available (ch2 0x110, ch3 0x118), `0x101 + 8*ch` doorbell
(ch2 0x111), `0x102 + 8*ch` TX drained (ch0 0x102, ch1 0x10A), 0x124 common. The sender keeps a two-slot
semaphore per channel and gets a slot back on "TX drained"; the receive ISR reads the count and then the
message pairs, and calls the handler registered for (channel, cmd); Canon's PostPostman then dispatches on
`param` to the handler a client registered just before sending.

Start and stop: `0xC0223210 = 0x7E`, `0xC022320C = 6` (clock on, core held), the main CPU copies Eeko's
kernel into its TCM, then `0xC022320C = 7` releases it. Eeko's system task rings the ch2 doorbell when it is
up. A stop is the ch0 doorbell; Eeko resets its engine channels and rings ch2 again, and the main CPU writes
`0xC022320C = 0`. On the real EOS M the register reads 7 in LiveView, the menus and playback: Eeko stays
started once LiveView has woken it.

## Model

- `0xC022320C` / `0xC0223210` read back what was written; writing 7 to `0xC022320C` rings the ch2 doorbell
  (main interrupt 0x111) shortly after, as Eeko's start-up does (upstream did this for every camera, and still
  does for those without an Eeko profile).
- A request (second word written to ch0/ch1) gives the sender its slot back (`0x102 + 8*ch`) and is answered
  on ch3 (system messages on ch2) with the request's own cmd and param and arg 0, a few interrupt-timer steps
  of guest progress later. That is what Eeko's client tasks do: e.g. ColorTask's LvWbAlgor answers message
  `0x300..0x304` with the same value, and Canon's handler only releases the task waiting for it (with a 1 s
  timeout, then the assert).
- The ch0 doorbell (stop request) is answered with the ch2 doorbell.
- Shared command memory (PComMem): request structures come from fixed pools in the main CPU's block (address
  and size in the boot block at 0x01F80000, +0x28/+0x2C; Eeko's own block at +0x30/+0x34). Each has a 4-byte
  header whose byte 0 means "in use", and the receiver frees a structure by clearing it after copying it. Eeko
  frees every request structure it receives; the model does the same for a request's `arg` when it lies in
  the main CPU's pool (so the pools of ten small blocks do not run out), and builds the structures of its own
  replies in Eeko's block (`eeko_box()`), which Canon frees after reading them.
- A camera profile (`hw/eos/cams/<CAM>/eeko.c`, `struct eeko_profile`, `.eeko` in `model.inc.c`) can replace
  the default answer per message (several replies, other params, a result in `arg`).
- What is not done: Eeko's image work. Result buffers keep whatever the main CPU put there (white balance
  gains, colour data); nothing is developed or compressed by Eeko.

Logging: the first message of each kind (channel, cmd, param) as `[EEKO] ch0 -> DSLR_COLOR_PROC 000302 arg ...`
and its answer `[EEKO] ch3 <- ...`; `[EEKO] started`, `[EEKO] stop request`. `QEMU_EOS_EEKO_TRACE=1` (or
`-d io`) logs every message. `QEMU_EOS_EEKO=0` turns the model off (A/B runs).

## Why not run Eeko's code

Running Eeko would need a second CPU (`cortex-r4`, Thumb at 0, PMSA MPU, TCMs at 0 and 0x40000000) in the same
machine, sharing the mailbox and DRAM 0x01E00000..0x01FBFFFF with the ARM946 main core, plus Eeko's own
interrupt controller (0xD02C0200), timer (0xD02C1500) and the image engines it drives (EDMAC, resizers, JPEG
core), which the emulator models only partly. Upstream's `5D3eeko` machine runs Eeko alone from RAM dumps,
with no main CPU. The jobs the EOS M sends in LiveView are completion handshakes whose results the emulator
cannot produce meaningfully anyway (no sensor data), so the answers are modelled at the message level.

## Still development (SS_PROC), EOS M

From the firmware (main CPU `0xFF4149EC..0xFF4156E8`, `0xFF414D88`; Eeko's StillTask), answered by
`hw/eos/cams/EOSM/eeko.c`, **not exercised yet**: the EOS M's release stops before development, in LiveView's
sensor standby (`docs/models/EOSM.md`, "Capture"). Subclient 1 (`param` 0x1nn):

| Request | Answer |
|---|---|
| 0x100 init (table of addresses), 0x101 parameters, 0x12E end | none |
| 0x102 box of five parameter structures, with callback / 0x103 without | 0x100 / none (Eeko frees the five structures and the box) |
| parameter blocks in pairs, with callback / without: 0x105/0x106, 0x108/0x109, 0x10B/0x10C, 0x10E/0x10F, 0x111/0x112, 0x114/0x115 .. 0x120/0x121, 0x123/0x124, 0x126/0x127 | 0x101, 0x102, 0x103, 0x104, 0x105, 0x106 .. 0x10A, 0x10B, 0x10C / none (the still path sends the second of each pair) |
| 0x12A start, 0x129 YUV output address (a value) | when both have come: 0x10D with `{ 2, 0 }` (RAW -> SSRAW done), then 0x10D with `{ 1, YUV address }` (SSRAW -> YUV done); Canon posts SsDevelop events 8 and 9 (`CompleteRawToSsraw`, `CompleteSsrawToYuv`) and frees each structure |

PreDevelop also sends DSLR_COLOR_PROC `0x800`, `0x801`/`0x802`, `0x901`, `0xB01` (answered by echo, each waited
for up to 1 s). JP62_PROC (cmd 3) is used for movies (H.264 encoder, subclient 3), not for stills: `0x303`
encode is answered by `0x306` (status) and `0x307` (two result words); not modelled.

## EOS M messages seen

LiveView start (boot, and after MENU/PLAY): Canon wakes Eeko (`Gmt: Wakeup Eeko`), then DSLR_COLOR_PROC
`0x302` (LvWbAlgor start), `0x304`, `0x800` (ClrAlgor), `0x004` (LvClrAlgor, its result word read back as a
status), `0x900` (chromatic aberration correction), `0xB00` (vignetting correction), `0x003`. The per-frame
LiveView white balance (`0x301`) needs LiveView frames from Canon's pipeline, which does not run here.
