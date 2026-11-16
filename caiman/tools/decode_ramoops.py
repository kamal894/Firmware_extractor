#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Decode the pstore console zone from a raw dump of a ramoops region, the
same way Linux does (fs/pstore/ram.c + ram_core.c persistent_ram_save_old).

On the phone you don't need this: Android shows the same text in
/sys/fs/pstore/console-ramoops-0. It's for QEMU tests and for raw dumps.

  decode_ramoops.py dump.bin [--record-size 0] [--console-size 0x200000]
                             [--ftrace-size 0] [--pmsg-size 0x200000]
"""
import argparse
import struct
import sys

SIG = 0x43474244  # "DBGC"


def rounddown_pow2(v):
    return 1 << (v.bit_length() - 1) if v else 0


def console_zone(mem_size, record, console, ftrace, pmsg):
    record, console, ftrace, pmsg = map(rounddown_pow2, (record, console, ftrace, pmsg))
    if not console or mem_size < console + ftrace + pmsg:
        raise ValueError("layout has no console zone")
    dump = mem_size - console - ftrace - pmsg
    off = 0
    if dump and record:
        cnt = dump // record
        if not cnt:
            raise ValueError("record-size larger than the dump area")
        off = (dump // cnt) * cnt
    return off, console


def decode(region, record=0, console=0x200000, ftrace=0, pmsg=0x200000):
    off, size = console_zone(len(region), record, console, ftrace, pmsg)
    zone = region[off:off + size]
    sig, start, used = struct.unpack_from("<III", zone, 0)
    if sig != SIG:
        raise ValueError(f"no persistent_ram_buffer signature at zone offset 0x{off:x} (got 0x{sig:08x})")
    data = zone[12:]
    if used > len(data) or start > used:
        raise ValueError(f"invalid buffer: start {start} size {used}")
    return (data[start:used] + data[:start]).decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--record-size", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--console-size", type=lambda s: int(s, 0), default=0x200000)
    ap.add_argument("--ftrace-size", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--pmsg-size", type=lambda s: int(s, 0), default=0x200000)
    a = ap.parse_args()
    try:
        sys.stdout.write(decode(open(a.dump, "rb").read(), a.record_size, a.console_size,
                                a.ftrace_size, a.pmsg_size))
    except ValueError as e:
        sys.exit(f"decode_ramoops: {e}")


if __name__ == "__main__":
    main()
