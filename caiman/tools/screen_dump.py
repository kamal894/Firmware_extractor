#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""PNG of the panel picture the loader's watcher writes into the log (watcher.c screen_dump()).

usage: screen_dump.py LOG OUT.png [--scale N]
"""
import base64
import re
import struct
import sys
import zlib


def main():
    args = sys.argv[1:]
    scale = 1
    if '--scale' in args:
        i = args.index('--scale')
        scale = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__.split('\n\n')[1])
    text = open(args[0], 'rb').read().decode('latin-1')
    m = None
    for m in re.finditer(r'=== screen dump (\d+)x(\d+) rgb332 base64 ===\n(.*?)=== end of screen dump ===', text, re.S):
        pass
    if not m:
        sys.exit('screen_dump.py: no screen dump in ' + args[0])
    w, h = int(m.group(1)), int(m.group(2))
    data = base64.b64decode(''.join(m.group(3).split()))
    if len(data) < w * h:
        sys.exit(f'screen_dump.py: {len(data)} bytes, want {w * h}')
    raw = bytearray()
    for y in range(h):
        row = bytearray()
        for x in range(w):
            v = data[y * w + x]
            px = bytes((((v >> 5) & 7) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3))
            row += px * scale
        for _ in range(scale):
            raw += b'\0' + row
    w, h = w * scale, h * scale
    chunk = lambda k, b: struct.pack('>I', len(b)) + k + b + struct.pack('>I', zlib.crc32(k + b))
    open(args[1], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
    print(f'{args[1]}: {w}x{h}')


if __name__ == '__main__':
    main()
