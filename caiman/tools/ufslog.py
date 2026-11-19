#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Read the self-logging area the loader keeps in boot_b (probe/ufs.c ufslog_save, docs/self-logging.md).

  tools/ufslog.py DUMP [N]      DUMP: a copy of boot_b (e.g. adb shell su -c 'dd if=/dev/block/by-name/boot_b');
                                lists the saves, prints save N's text (default: the newest)
"""
import struct
import sys
import zlib

BLOCK, SLOT_BLOCKS = 4096, 512
d = open(sys.argv[1], "rb").read()
if d[:8] != b"HKLOGSB1":
    sys.exit("no HKLOGSB1 superblock: the loader has not saved a log here yet")
ver, nxt, saves, nslots = struct.unpack_from("<4I", d, 8)
print("superblock: version %u, %u slots, next slot %u, %u saves" % (ver, nslots, nxt % nslots, saves))
found = {}
for k in range(nslots):
    o = BLOCK * SLOT_BLOCKS * (k + 1)
    if d[o:o + 8] != b"HKLOGSL1":
        continue
    num, ln, crc = struct.unpack_from("<3I", d, o + 8)
    who = d[o + 32:o + 96].split(b"\0")[0].decode(errors="replace")
    text = d[o + BLOCK:o + BLOCK + ln]
    ok = zlib.crc32(text) & 0xffffffff == crc
    found[num] = text
    print("  save %3u in slot %2u: %7u bytes, CRC %s, written by loader %s" % (num, k, ln, "ok" if ok else "BAD", who))
if found:
    want = int(sys.argv[2]) if len(sys.argv) > 2 else max(found)
    print("\n--- save %u ---" % want)
    sys.stdout.write(found.get(want, b"(no such save)\n").decode(errors="replace"))
