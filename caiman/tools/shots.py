#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
The watcher's pictures of the panel (loader PROBE_SHOTS=N, probe/watcher.c) from pstore's pmsg zone: one PNG a
picture (128 x 286, RGB 3-3-2, shown at 2x), and a contact sheet of all of them.

    caiman/tools/shots.py OUT_PREFIX [PMSG_FILE]        (without PMSG_FILE: adb, su, /sys/fs/pstore/pmsg-ramoops-0)
"""
import struct
import subprocess
import sys
import zlib

W, H = 128, 286


def png(path, w, h, rgb):
    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def rgb(b):
    return ((b & 0xe0) * 255 // 0xe0, ((b << 3) & 0xe0) * 255 // 0xe0, ((b << 6) & 0xc0) * 255 // 0xc0)


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    prefix = sys.argv[1]
    if len(sys.argv) > 2:
        data = open(sys.argv[2], 'rb').read()
    else:
        data = subprocess.run(['adb', 'exec-out', 'su', '-c', 'cat /sys/fs/pstore/pmsg-ramoops-0'],
                              capture_output=True, check=False).stdout
    shots = []
    at = data.find(b'SHOT')
    while at >= 0 and at + 8 + W * H <= len(data):
        second = struct.unpack_from('<I', data, at + 4)[0]
        shots.append((second, data[at + 8:at + 8 + W * H]))
        at = data.find(b'SHOT', at + 8 + W * H)
    if not shots:
        print(f'shots: none in pmsg ({len(data)} bytes)')
        return 1
    lut = [rgb(b) for b in range(256)]
    cols = 10
    rows = (len(shots) + cols - 1) // cols
    sw, sh = cols * (W + 2), rows * (H + 2)
    sheet = bytearray(b'\x40' * (sw * sh * 3))
    for i, (second, pix) in enumerate(shots):
        out = bytearray()
        for y in range(H):
            for x in range(W):
                out += bytes(lut[pix[y * W + x]])
        png(f'{prefix}-{second:03d}.png', W, H, out)
        ox, oy = (i % cols) * (W + 2), (i // cols) * (H + 2)
        for y in range(H):
            o = ((oy + y) * sw + ox) * 3
            sheet[o:o + W * 3] = out[y * W * 3:(y + 1) * W * 3]
    png(f'{prefix}-sheet.png', sw, sh, sheet)
    print(f'shots: {len(shots)} pictures, seconds {shots[0][0]}..{shots[-1][0]}, sheet {prefix}-sheet.png')
    return 0


if __name__ == '__main__':
    sys.exit(main())
