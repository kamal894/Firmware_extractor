#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Pack the BCM4390's firmware and NVRAM for the loader (PROBE_WIFI=8, WIFI_BLOB=out): header 'WFB1', firmware
length, vars length, 0; the firmware padded to 4; the vars. The vars are made as Google's bcmdhd does
(dhdpcie_download_nvram: last byte of the file zeroed, process_nvram_vars, rounded up to 4, one 0 byte
appended; dhdpcie_bus_write_vars rounds that up to 4 again).

  tools/wifi_blob.py fw_bcmdhd.bin_4390_b1 bcmdhd.cal_USI_G6BB_6327_V27 out.bin
"""
import struct
import sys

fw = open(sys.argv[1], "rb").read()
raw = bytearray(open(sys.argv[2], "rb").read())
raw[-1] = 0
out, col, skip = bytearray(), 0, False
for ch in raw:
    c = chr(ch)
    if c == "\r":
        continue
    if skip and c != "\n":
        continue
    skip = False
    if c == "#":
        skip = True
        continue
    if c == "\n":
        if col:
            out.append(0)
            col = 0
        continue
    out.append(ch)
    col += 1
n = len(out)
if n % 4 and n + (4 - n % 4) <= len(raw):
    n += 4 - n % 4
out = out[:n] + bytes(n - len(out))
if n % 4:
    n += 4 - n % 4
out = out + bytes(n - len(out)) + b"\0"
varsz = (len(out) + 3) & ~3
out = out + bytes(varsz - len(out))
fwp = fw + bytes((-len(fw)) % 4)
open(sys.argv[3], "wb").write(struct.pack("<4sIII", b"WFB1", len(fw), varsz, 0) + fwp + out)
print("firmware %u bytes, vars %u bytes (%u vars), blob %u bytes" % (len(fw), varsz, out.count(0) and
      sum(1 for v in bytes(out).split(b"\0") if v), 16 + len(fwp) + varsz))
