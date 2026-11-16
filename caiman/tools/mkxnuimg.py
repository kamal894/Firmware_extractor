#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Make the M2 boot image: the caiman loader (make LOADER=1) followed by an XNUP payload.

  mkxnuimg.py --loader probe.Image --kernel hakios-kernel [--ramdisk rd.img] [--cmdline "..."]
              --out-image combined.Image [--out-bootimg boot.img [--stock-boot boot.img] ...]

XNUP (as hackios-xnu-kernel/xnuloader/mkpayload.py): u32 "XNUP", u32 size, then entries
char name[16], u32 size, u32 reserved, data padded to 16, ending with an empty name.
The payload goes after the loader's bss and stack; its offset is written to the arm64
Image header's res3 field (0x30), where the loader looks, and image_size covers it all.
"""
import argparse
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pack_bootimg  # noqa: E402

MAGIC = 0x50554E58


def align(n, a):
    return (n + a - 1) // a * a


def entry(name, data):
    return struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--loader', required=True)
    ap.add_argument('--kernel', required=True, help='XNU Mach-O (e.g. HackiOS prebuilt/hakios-kernel)')
    ap.add_argument('--kernel-sha256', help='refuse a kernel with another hash')
    ap.add_argument('--ramdisk')
    ap.add_argument('--cmdline', default='', help='appended to the loader\'s own boot-args')
    ap.add_argument('--out-image', required=True)
    ap.add_argument('--out-bootimg')
    ap.add_argument('--stock-boot')
    ap.add_argument('--compress', choices=('none', 'gzip', 'lz4'), default='none')
    a = ap.parse_args()

    loader = bytearray(open(a.loader, 'rb').read())
    kernel = open(a.kernel, 'rb').read()
    if loader[0x38:0x3c] != b'ARM\x64':
        sys.exit(f'{a.loader}: not an arm64 Image')
    if kernel[:4] != struct.pack('<I', 0xfeedfacf):
        sys.exit(f'{a.kernel}: not a 64-bit Mach-O')
    digest = hashlib.sha256(kernel).hexdigest()
    if a.kernel_sha256 and digest != a.kernel_sha256.lower():
        sys.exit(f'{a.kernel}: sha256 {digest} is not the expected {a.kernel_sha256}')

    body = entry('kernel', kernel)
    if a.ramdisk:
        body += entry('ramdisk', open(a.ramdisk, 'rb').read())
    if a.cmdline:
        body += entry('cmdline', a.cmdline.encode() + b'\0')
    body += struct.pack('<16sII', b'', 0, 0)
    payload = struct.pack('<II', MAGIC, len(body)) + body

    image_size = struct.unpack_from('<Q', loader, 0x10)[0]
    off = align(max(len(loader), image_size), 0x10000)
    blob = bytearray(loader) + b'\0' * (off - len(loader)) + payload
    struct.pack_into('<Q', blob, 0x10, align(len(blob), 0x10000))   # image_size: all of it
    struct.pack_into('<Q', blob, 0x30, off)                          # res3: the payload
    open(a.out_image, 'wb').write(blob)
    print(f'{a.out_image}: loader {len(loader)} bytes (+bss to 0x{image_size:x}), payload at 0x{off:x}, '
          f'kernel {len(kernel)} bytes sha256 {digest[:16]}..., total {len(blob)} bytes')

    if a.out_bootimg:
        args = argparse.Namespace(kernel=a.out_image, out=a.out_bootimg, header_version=None,
                                  stock_boot=a.stock_boot, os_version=None, os_patch_level=None,
                                  cmdline='', ramdisk=None, no_ramdisk=False, compress=a.compress)
        pack_bootimg.cmd_pack(args)


if __name__ == '__main__':
    main()
