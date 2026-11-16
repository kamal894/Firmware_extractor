#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Android boot image helper for the caiman (Pixel 9 Pro) port.

  pack   wrap a bare-metal arm64 Image as boot.img (header v3/v4) for `fastboot boot`
  info   print the header of a boot.img or vendor_boot.img (any version)
  dtb    pull the DTB blob out of a vendor_boot.img (v3/v4)

Stock images come from Google's factory zip; Firmware_extractor's
extractor.sh unpacks them:  ./extractor.sh caiman-<build>-factory.zip
then look in out/ for boot.img and vendor_boot.img.

Layouts follow AOSP system/tools/mkbootimg (include/bootimg/bootimg.h).
"""
import argparse
import datetime
import gzip
import shutil
import struct
import subprocess
import sys

BOOT_MAGIC = b"ANDROID!"
VENDOR_MAGIC = b"VNDRBOOT"
PAGE = 4096  # boot.img v3/v4 page size is fixed


def align(n, a):
    return (n + a - 1) // a * a


def pad(b, a=PAGE):
    return b + b"\0" * (align(len(b), a) - len(b))


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


# ---- os_version ---------------------------------------------------------


def encode_os_version(version, patch_level):
    a, b, c = (list(map(int, version.split("."))) + [0, 0])[:3]
    y, m = map(int, patch_level.split("-")[:2])
    return ((a << 14 | b << 7 | c) << 11) | ((y - 2000) << 4 | m)


def decode_os_version(v):
    if v == 0:
        return "unset"
    ver, lvl = v >> 11, v & 0x7FF
    return f"{ver >> 14}.{(ver >> 7) & 0x7f}.{ver & 0x7f}, patch {2000 + (lvl >> 4)}-{lvl & 0xf:02d}"


# ---- info ---------------------------------------------------------------


def info_boot(d):
    version = struct.unpack_from("<I", d, 40)[0]
    print(f"boot image, header v{version}")
    if version >= 3:
        ksz, rsz, osv, hsz = struct.unpack_from("<IIII", d, 8)
        print(f"  kernel_size  {ksz}")
        print(f"  ramdisk_size {rsz}")
        print(f"  os_version   0x{osv:08x} ({decode_os_version(osv)})")
        print(f"  header_size  {hsz}")
        print(f"  cmdline      {cstr(d[44:44 + 1536])!r}")
        if version >= 4:
            print(f"  signature_size {struct.unpack_from('<I', d, 1580)[0]}")
        kernel = d[PAGE:PAGE + ksz]
    else:
        (ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, _hv, osv) = struct.unpack_from("<10I", d, 8)
        print(f"  page_size    {page}")
        print(f"  kernel       {ksz} bytes @ 0x{kaddr:08x}")
        print(f"  ramdisk      {rsz} bytes @ 0x{raddr:08x}")
        print(f"  second       {ssz} bytes @ 0x{saddr:08x}")
        print(f"  tags_addr    0x{tags:08x}")
        print(f"  os_version   0x{osv:08x} ({decode_os_version(osv)})")
        print(f"  name         {cstr(d[48:64])!r}")
        print(f"  cmdline      {cstr(d[64:64 + 512]) + cstr(d[608:608 + 1024])!r}")
        kernel = d[page:page + ksz]
    print(f"  kernel format: {kernel_format(kernel)}")


def kernel_format(k):
    if k[:2] == b"\x1f\x8b":
        return "gzip"
    if k[:4] == b"\x02\x21\x4c\x18":
        return "lz4 (legacy)"
    if k[:4] == b"\x04\x22\x4d\x18":
        return "lz4 (frame)"
    if len(k) >= 64 and k[56:60] == b"ARM\x64":
        return "arm64 Image (uncompressed)"
    return "unknown"


def vendor_fields(d):
    (hv, page, kaddr, raddr, vrsz) = struct.unpack_from("<5I", d, 8)
    tags = struct.unpack_from("<I", d, 2076)[0]
    name = cstr(d[2080:2096])
    hsz, dtbsz = struct.unpack_from("<II", d, 2096)
    dtbaddr = struct.unpack_from("<Q", d, 2104)[0]
    return dict(version=hv, page=page, kaddr=kaddr, raddr=raddr, vrsz=vrsz, tags=tags,
                name=name, hsz=hsz, dtbsz=dtbsz, dtbaddr=dtbaddr, cmdline=cstr(d[28:28 + 2048]))


def info_vendor(d):
    f = vendor_fields(d)
    print(f"vendor_boot image, header v{f['version']}")
    print(f"  page_size    {f['page']}")
    print(f"  kernel_addr  0x{f['kaddr']:08x}   <- where the bootloader is told to load the kernel")
    print(f"  ramdisk_addr 0x{f['raddr']:08x}")
    print(f"  dtb_addr     0x{f['dtbaddr']:016x}")
    print(f"  tags_addr    0x{f['tags']:08x}")
    print(f"  vendor_ramdisk_size {f['vrsz']}")
    print(f"  dtb_size     {f['dtbsz']}")
    print(f"  name         {f['name']!r}")
    print(f"  cmdline      {f['cmdline']!r}")
    if f["version"] >= 4:
        tsz, tnum, tesz, bcsz = struct.unpack_from("<4I", d, 2112)
        print(f"  ramdisk table: {tnum} entries, {tsz} bytes; bootconfig {bcsz} bytes")


def cmd_info(args):
    d = open(args.image, "rb").read()
    if d[:8] == BOOT_MAGIC:
        info_boot(d)
    elif d[:8] == VENDOR_MAGIC:
        info_vendor(d)
    else:
        sys.exit(f"{args.image}: not an Android boot or vendor_boot image")


def cmd_dtb(args):
    d = open(args.image, "rb").read()
    if d[:8] != VENDOR_MAGIC:
        sys.exit(f"{args.image}: not a vendor_boot image")
    f = vendor_fields(d)
    off = align(f["hsz"], f["page"]) + align(f["vrsz"], f["page"])
    blob = d[off:off + f["dtbsz"]]
    if blob[:4] != b"\xd0\x0d\xfe\xed":
        print("warning: blob does not start with the FDT magic", file=sys.stderr)
    open(args.out, "wb").write(blob)
    print(f"wrote {len(blob)} bytes of DTB to {args.out} (may hold several concatenated DTBs)")


# ---- pack ---------------------------------------------------------------


def empty_cpio():
    """newc archive containing only the TRAILER!!! entry."""
    name = b"TRAILER!!!\0"
    fields = [0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0]
    hdr = b"070701" + b"".join(b"%08X" % v for v in fields)
    rec = hdr + name
    rec += b"\0" * (align(len(rec), 4) - len(rec))
    return rec


def compress(kernel, how):
    if how == "none":
        return kernel
    if how == "gzip":
        return gzip.compress(kernel, 9, mtime=0)
    if how == "lz4":
        if not shutil.which("lz4"):
            sys.exit("--compress lz4 needs the lz4 command-line tool (apt install lz4)")
        # legacy frame format (-l) is what kernels and bootloaders accept
        return subprocess.run(["lz4", "-l", "-12", "-c"], input=kernel, check=True,
                              stdout=subprocess.PIPE).stdout
    raise ValueError(how)


def cmd_pack(args):
    kernel = open(args.kernel, "rb").read()
    if kernel[56:60] != b"ARM\x64":
        sys.exit(f"{args.kernel}: not an arm64 Image (missing ARM\\x64 magic)")

    header_version = args.header_version
    today = datetime.date.today()
    os_version = encode_os_version(args.os_version or "16.0.0",
                                   args.os_patch_level or f"{today.year}-{today.month:02d}")
    if args.stock_boot:
        s = open(args.stock_boot, "rb").read()
        if s[:8] != BOOT_MAGIC:
            sys.exit(f"{args.stock_boot}: not a boot image")
        sv = struct.unpack_from("<I", s, 40)[0]
        if sv >= 3:
            if args.header_version is None:
                header_version = sv
            stock_os = struct.unpack_from("<I", s, 16)[0]
        else:
            stock_os = struct.unpack_from("<I", s, 44)[0]
        if not (args.os_version or args.os_patch_level):
            os_version = stock_os
        print(f"stock boot.img: header v{sv}, os_version {decode_os_version(stock_os)}, "
              f"kernel {kernel_format(s[PAGE:PAGE + 64] if sv >= 3 else b'')}")
    if header_version is None:
        header_version = 4
    if header_version not in (3, 4):
        sys.exit("only boot header v3/v4 can be packed (Pixel 6 and later use v4)")

    payload = compress(kernel, args.compress)
    ramdisk = b"" if args.no_ramdisk else (open(args.ramdisk, "rb").read() if args.ramdisk
                                           else empty_cpio())
    cmdline = args.cmdline.encode()
    if len(cmdline) >= 1536:
        sys.exit("cmdline too long (max 1535 bytes)")

    if header_version == 4:
        hdr = struct.pack("<8sIIII4II1536sI", BOOT_MAGIC, len(payload), len(ramdisk), os_version,
                          1584, 0, 0, 0, 0, 4, cmdline, 0)
    else:
        hdr = struct.pack("<8sIIII4II1536s", BOOT_MAGIC, len(payload), len(ramdisk), os_version,
                          1580, 0, 0, 0, 0, 3, cmdline)
    img = pad(hdr) + pad(payload) + pad(ramdisk)
    open(args.out, "wb").write(img)
    print(f"wrote {args.out}: boot header v{header_version}, kernel {len(payload)} bytes "
          f"({args.compress}), ramdisk {len(ramdisk)} bytes, os_version "
          f"{decode_os_version(os_version)}, total {len(img)} bytes")
    print(f"run it (RAM only, nothing is flashed):  fastboot boot {args.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("pack", help="wrap an arm64 Image as a boot.img")
    p.add_argument("--kernel", required=True, help="arm64 Image (e.g. probe.Image)")
    p.add_argument("--out", required=True)
    p.add_argument("--header-version", type=int, choices=(3, 4), default=None,
                   help="default: copy from --stock-boot, else 4")
    p.add_argument("--stock-boot", help="stock boot.img to copy header version and os_version from")
    p.add_argument("--os-version", help="e.g. 16.0.0")
    p.add_argument("--os-patch-level", help="e.g. 2026-11")
    p.add_argument("--cmdline", default="", help="kernel command line (the probe ignores it)")
    p.add_argument("--ramdisk", help="ramdisk file (default: an empty cpio archive)")
    p.add_argument("--no-ramdisk", action="store_true", help="pack a zero-length ramdisk")
    p.add_argument("--compress", choices=("none", "gzip", "lz4"), default="none",
                   help="kernel compression; try lz4 if the bootloader rejects the plain Image")
    p.set_defaults(func=cmd_pack)

    p = sub.add_parser("info", help="print a boot.img / vendor_boot.img header")
    p.add_argument("image")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("dtb", help="extract the DTB from a vendor_boot.img")
    p.add_argument("image")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_dtb)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
