#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Decode a Broadcom NCI EROM (BCM4390) from a probe log's "wifi: OOBR +0x..." and "wifi: EROM +0x..." lines
(PROBE_WIFI=6: the pages at EROM2 & ~0x1fff and EROM2 & ~0xfff), the way Google's bcmdhd nciutils.c nci_scan /
nci_initial_parse / nci_save_slaveport_addr do.

  tools/nci_erom.py LOG [EROM2_ADDR]      (EROM2_ADDR: ChipCommon +0xfc, default 0x180070b0)
"""
import re
import sys

log, erom2 = sys.argv[1], int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x180070b0
oobr, erom1 = erom2 & ~0x1fff, erom2 & ~0xfff
mem = {}
for line in open(log, errors="replace"):
    m = re.search(r"wifi: (OOBR|EROM) \+0x([0-9a-f]+): ((?:[0-9a-f]{8} ?){8})", line)
    if m:
        base = oobr if m.group(1) == "OOBR" else erom1
        for i, w in enumerate(m.group(3).split()):
            mem[base + int(m.group(2), 16) + 4 * i] = int(w, 16)


def rd(a):
    if a not in mem:
        raise KeyError("0x%08x not in the dump" % a)
    return mem[a]


NAMES = {0x800: "ChipCommon", 0x812: "D11", 0x83c: "PCIe2", 0x847: "ARM CA7", 0x849: "SYSMEM", 0x840: "GCI",
         0x827: "PMU", 0x135: "NoC", 0x366: "OOBR", 0x880: "SAQM"}
cores, p = [], erom2
while True:
    d0 = rd(p)
    if d0 == 0xffffffff:
        break
    d1 = rd(p + 4)
    coretype, off = (d1 >> 27) & 1, d1 & 0x1fff
    info_at = (erom1 if coretype else oobr) + off
    info, cfg = rd(info_at), rd(info_at + 4)
    cid = info & 0xfff
    cid = cid | 0x800 if cid < 0xff else cid
    unit = sum(1 for c in cores if c["id"] == cid and c["off"] != off)
    core = {"id": cid, "unit": unit, "rev": (info >> 12) & 0xff, "off": off, "type": "EROM1" if coretype else "OOBR",
            "dmp": oobr + off + 8, "ifaces": []}   # DMP: BAR0 window at the OOBR base, + offset + 8
    cores.append(core)
    wordoffset = 0
    for _ in range((cfg >> 12) & 0xf):
        d0, d1 = rd(p), rd(p + 4)
        p += 8
        wordoffset = d1 >> 28
        iface = {"master": (d1 >> 26) & 1, "node": d0 & ~7, "nodetype": d0 & 7, "addrs": []}
        naddr = (d1 >> 24) & 3
        if not iface["master"]:
            # nci_save_iface1_reg: a slave's NumAddressRegion is one less than its ports when WORDOFFSET > 1,
            # and in the last descriptor (WORDOFFSET 0) when the next word is not the end marker
            if wordoffset > 1 or (wordoffset == 0 and rd(p) != 0xffffffff):
                naddr += 1
        for _ in range(naddr) if not iface["master"] else []:
            a = rd(p)
            p += 4
            lo = a & 0xffffff00
            if a & 0x20:
                p += 12
                iface["addrs"].append((lo, None))
            elif a & 0x40:
                hi = rd(p)
                p += 4
                iface["addrs"].append((lo, hi))
            else:
                iface["addrs"].append((lo, lo + (1 << ((a & 0x1f) + 8))))
        core["ifaces"].append(iface)
    if wordoffset == 0:
        break

for i, c in enumerate(cores):
    print("core %2d: 0x%03x %-10s unit %u rev %3u  info in %s+0x%03x, DMP control at 0x%08x" % (
        i, c["id"], NAMES.get(c["id"], ""), c["unit"], c["rev"], c["type"], c["off"], c["dmp"]))
    for f in c["ifaces"]:
        print("         %s node 0x%08x (%s)%s" % ("master" if f["master"] else "slave ", f["node"],
              "NIC-400" if f["nodetype"] else "Booker",
              "".join("  [0x%08x-0x%08x)" % (lo, hi or 0) for lo, hi in f["addrs"])))
