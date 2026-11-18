#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
The window server's Metal shader cache, brought off the phone through pstore's pmsg zone.

HackiOS's userland/src/cache_export.c (rc.jobs, boot-arg pmsg=1) packs the cache files a run made into the pmsg
zone; after the watcher's reset Android shows it as /sys/fs/pstore/pmsg-ramoops-0. This reads it (adb, su) or a
saved copy and unpacks the files under the RAM disk tree's share/m2v-cache, which HakiosMetalCA.m copies into the
cache directory at the next boot. Each run brings what the last one could not fit (2 MiB a run).

    caiman/tools/m2v_cache.py ROOT_TREE [PMSG_FILE]
"""
import os
import struct
import subprocess
import sys
import zlib


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    tree = sys.argv[1]
    if len(sys.argv) > 2:
        data = open(sys.argv[2], 'rb').read()
    else:
        data = subprocess.run(['adb', 'exec-out', 'su', '-c', 'cat /sys/fs/pstore/pmsg-ramoops-0'],
                              capture_output=True, check=False).stdout
    at = data.find(b'HKC1')
    if at < 0:
        print(f'm2v_cache: no pack in pmsg ({len(data)} bytes)')
        return 1
    at += 4
    out = os.path.join(tree, 'share', 'm2v-cache')
    files = total = 0
    while at + 10 <= len(data):
        pl, size, zsize = struct.unpack_from('<HII', data, at)
        at += 10
        rel = data[at:at + pl].decode()
        at += pl
        body = data[at:at + (zsize or size)]
        at += zsize or size
        if '..' in rel.split('/') or rel.startswith('/'):
            continue
        raw = zlib.decompress(body, -15) if zsize else body
        if len(raw) != size:
            print(f'm2v_cache: {rel}: {len(raw)} bytes, want {size}; stopped')
            break
        path = os.path.join(out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(raw)
        files += 1
        total += size
    print(f'm2v_cache: {files} files, {total} bytes, into {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
