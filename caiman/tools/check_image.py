#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sanity-check an arm64 Linux 'Image' header (what the Pixel bootloader expects)."""
import struct
import sys


def check(path):
    data = open(path, "rb").read()
    if len(data) < 64:
        sys.exit(f"{path}: too small for an arm64 Image header")
    code0, code1, text_offset, image_size, flags, _r2, _r3, _r4, magic, _r5 = struct.unpack_from(
        "<IIQQQQQQII", data, 0
    )
    errors = []
    if magic != 0x644D5241:
        errors.append(f"bad magic 0x{magic:08x} (want 'ARM\\x64')")
    if (code0 >> 26) != 0b000101:
        errors.append(f"code0 0x{code0:08x} is not a B instruction")
    if image_size < len(data):
        errors.append(f"image_size {image_size} < file size {len(data)}")
    if flags & 1:
        errors.append("flags say big-endian")
    if errors:
        sys.exit(f"{path}: " + "; ".join(errors))
    print(
        f"{path}: arm64 Image ok: file {len(data)} bytes, image_size {image_size} "
        f"(incl. bss+stack), text_offset {text_offset}, flags 0x{flags:x}"
    )


if __name__ == "__main__":
    for p in sys.argv[1:]:
        check(p)
