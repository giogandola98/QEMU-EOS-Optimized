#!/usr/bin/env python3
"""Rebuild the EOS M (fw 2.0.2) serial flash image SFDATA.BIN from a full RAM dump of the same camera.

The serial flash holds Canon's FIX and TUNE property packages (camera settings, factory calibration);
without them the firmware stops at start-up (RscMgr / PropertyAdmin asserts). The usual way to get the
flash is ML's sf_dump module. When only a RAM dump is at hand (ML "Dump RAM", 256 MiB, e.g. RAM4.BIN),
the packages can be taken from there, because Canon loads both into RAM at start-up and keeps them:

  package  flash address  flash region  RAM copy in a 2.0.2 dump   (handles created at ROM 0xFF0C35AC / 0xFF0C360C)
  FIX      0x340000       0xC0000       0x00480000 (heap; found by its header, see below)
  TUNE     0x010000       0x330000      0x0FE00000 (fixed DRAM address, "DRAMAddr 0x4fe00000" in the log)

Package header (as loaded in RAM): word 0 = 0x00FFFFFF, word 1 = total size, then the package records.
On the flash, word 0 of a valid package is 0x0000FFFF (Canon's loader looks for that and writes
0x00FFFFFF into its RAM copy).

The image is written in the raw layout the emulator replays (hw/eos/serial_flash.c), which is also what
sf_dump saves:
  - the data comes 4 bits late: Canon's readers rebuild each byte as in[i] << 4 | in[i+1] >> 4;
  - the quad-read DMA delivers 0x7F0 data bytes per 0x800 bytes of flash (Canon sizes its transfers for
    that), so each 0x7F0-byte piece of a package starts on a 0x800 boundary.
The rest of the flash is left erased (0xFF).

Usage: sfdata_from_ram.py RAM.BIN SFDATA.BIN
The output is camera data like the ROM dumps: keep it out of git (private package canon-rom/EOSM-202).
"""
import struct
import sys

SF_SIZE = 0x800000
CHUNK, STRIDE = 0x7F0, 0x800
RAM_SIZE = 0x10000000

# name, flash address, flash region size, where to look for the RAM copy
PACKAGES = [
    ("FIX", 0x340000, 0xC0000, None),           # heap allocation: searched
    ("TUNE", 0x010000, 0x330000, 0x0FE00000),
]


def late4(data):
    """data delayed by one nibble, erased flash (0xF) before and after"""
    n = len(data) + 1
    v = int.from_bytes(b"\xff" + data + b"\xff", "big") >> 4
    return (v & ((1 << (8 * n)) - 1)).to_bytes(n, "big")


def find_fix(ram):
    """The FIX package has id 0 (word 2) and its first record is property group 0 (word 4 = 0)."""
    for off in range(0, len(ram) - 0x20, 0x100):     # the loader's buffers are 256-byte aligned
        w = struct.unpack_from("<6I", ram, off)
        if w[0] == 0x00FFFFFF and w[2] == 0 and w[3] == w[1] - 12 and w[4] == 0 and 0x1000 < w[1] <= 0xC0000:
            return off
    sys.exit("FIX package not found in the RAM dump")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    ram = open(sys.argv[1], "rb").read()
    if len(ram) != RAM_SIZE:
        sys.exit("expected a 256 MiB RAM dump, got %d bytes" % len(ram))
    sf = bytearray(b"\xff" * SF_SIZE)
    for name, sfaddr, region, ramaddr in PACKAGES:
        if ramaddr is None:
            ramaddr = find_fix(ram)
        w0, size = struct.unpack_from("<2I", ram, ramaddr)
        if w0 != 0x00FFFFFF or not 8 < size:
            sys.exit("%s: no package header at RAM 0x%X" % (name, ramaddr))
        pkg = bytearray(ram[ramaddr:ramaddr + size])
        struct.pack_into("<I", pkg, 0, 0xFFFF)          # valid-package marker as stored on the flash
        nchunks = (size + CHUNK - 1) // CHUNK
        if nchunks * STRIDE > region:
            sys.exit("%s: 0x%X bytes do not fit the flash region" % (name, size))
        for i in range(nchunks):
            piece = late4(bytes(pkg[i * CHUNK:(i + 1) * CHUNK]))
            sf[sfaddr + i * STRIDE:sfaddr + i * STRIDE + len(piece)] = piece
        print("%-4s RAM 0x%08X -> flash 0x%06X, 0x%X bytes" % (name, ramaddr, sfaddr, size))
    with open(sys.argv[2], "wb") as f:
        f.write(sf)


if __name__ == "__main__":
    main()
