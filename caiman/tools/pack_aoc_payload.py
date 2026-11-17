#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Append aoc.bin to probe.Image for PROBE_TOUCH=6 (probe/aoc.c).

The bootloader takes the ramdisk from init_boot, so aoc.bin cannot travel as our ramdisk.
The kernel payload is loaded whole: pad probe.Image to 1 MiB (past bss and stack), then a
0x1000-byte header ("AOCBLOB\\0", u64 size) and aoc.bin itself.

  pack_aoc_payload.py probe.Image aoc.bin out.Image
"""
import struct
import sys

OFFSET = 0x100000


def main():
    image, aoc, out = sys.argv[1:4]
    img = open(image, "rb").read()
    fw = open(aoc, "rb").read()
    # arm64 Image header: image_size at +16 covers the file plus bss
    image_size = struct.unpack_from("<Q", img, 16)[0]
    if max(len(img), image_size) > OFFSET:
        sys.exit(f"probe.Image needs 0x{max(len(img), image_size):x} bytes, over 0x{OFFSET:x}")
    hdr = b"AOCBLOB\0" + struct.pack("<Q", len(fw))
    blob = img.ljust(OFFSET, b"\0") + hdr.ljust(0x1000, b"\0") + fw
    open(out, "wb").write(blob)
    print(f"wrote {out}: probe 0x{len(img):x} (image_size 0x{image_size:x}), aoc.bin 0x{len(fw):x} at 0x{OFFSET + 0x1000:x}")


if __name__ == "__main__":
    main()
