#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Trim HackiOS's system image so that it fits the free space of caiman's super.

usage: trim_sysimage.py SYSIMAGE_DIR TRIM_LIST        move the listed paths out of the image
       trim_sysimage.py --undo SYSIMAGE_DIR           put everything back

SYSIMAGE_DIR is hakios-xnu-kernel/sysimage after unpack.sh: rootfs/ (the extracted files) and
system.manifest (what tools/apfs/mkapfs.py builds the image with). TRIM_LIST has one path per line,
relative to the image's root ('#' starts a comment). For each one this
  - moves the files out of rootfs/ into SYSIMAGE_DIR/trimmed/ (nothing is deleted), and
  - drops the path and everything under it from system.manifest (the original is kept as
    system.manifest.full), since mkapfs would make a directory that is only in the manifest.
An application Darwin stages (private/var/staged_system_apps/X.app in rootfs, Applications/X.app in
the manifest) is found under either name. A line '!emoji PPEM,...' keeps only those bitmap strikes of
the colour emoji font (HackiOS's tools/font_trim.py; the original is kept in trimmed/ too).
Then rebuild: SYSTEM=1 PREBUILT=1 userland/build.sh.
"""
import os
import shutil
import subprocess
import sys

STAGED = 'private/var/staged_system_apps/'
EMOJI = 'System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc'


def load_list(path):
    out = []
    for line in open(path):
        line = line.split('#', 1)[0].strip().strip('/')
        if line:
            out.append(line)
    return out


def under(path, prefixes):
    return any(path == p or path.startswith(p + '/') for p in prefixes)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--undo':
        undo(sys.argv[2])
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__.split('\n\n')[1])
    sysimage, trim_list = sys.argv[1:]
    rootfs = os.path.join(sysimage, 'rootfs')
    trimmed = os.path.join(sysimage, 'trimmed')
    manifest = os.path.join(sysimage, 'system.manifest')
    full = manifest + '.full'
    if not os.path.exists(full):
        shutil.copy2(manifest, full)

    paths = load_list(trim_list)
    for p in [p for p in paths if p.startswith('!emoji ')]:
        paths.remove(p)
        emoji(sysimage, p.split()[1])
    # the manifest names staged applications where they go; rootfs has them where Darwin keeps them
    names = set()
    for p in paths:
        names.add(p)
        if p.startswith('Applications/'):
            names.add(STAGED + p[len('Applications/'):])
        if p.startswith(STAGED):
            names.add('Applications/' + p[len(STAGED):])

    moved = 0
    for p in sorted(names):
        src = os.path.join(rootfs, p)
        if not os.path.lexists(src):
            continue
        dst = os.path.join(trimmed, p)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        size = tree_size(src)
        shutil.move(src, dst)
        moved += size
        print(f'trim_sysimage.py: {p}: {size >> 20} MB moved out')

    kept = dropped = 0
    with open(full, 'rb') as f, open(manifest + '.part', 'wb') as out:
        for raw in f:
            path = raw.rstrip(b'\n').split(b'\t')[3].decode('utf-8', 'surrogateescape')
            if under(path, names):
                dropped += 1
                continue
            out.write(raw)
            kept += 1
    os.replace(manifest + '.part', manifest)
    print(f'trim_sysimage.py: {moved >> 20} MB moved to {trimmed}; manifest {kept} entries ({dropped} dropped)')


def emoji(sysimage, strikes):
    font = os.path.join(sysimage, 'rootfs', EMOJI)
    keep = os.path.join(sysimage, 'trimmed', EMOJI)
    if not os.path.exists(keep):
        os.makedirs(os.path.dirname(keep), exist_ok=True)
        shutil.copy2(font, keep)                # undo moves it back over the smaller one
    before = os.path.getsize(font)
    subprocess.run([sys.executable, os.path.join(sysimage, '..', 'tools', 'font_trim.py'), 'emoji', keep, font,
                    strikes], check=True, stdout=subprocess.DEVNULL)
    print(f'trim_sysimage.py: {EMOJI}: strikes {strikes}: {before >> 20} -> {os.path.getsize(font) >> 20} MB')


def undo(sysimage):
    rootfs = os.path.join(sysimage, 'rootfs')
    trimmed = os.path.join(sysimage, 'trimmed')
    manifest = os.path.join(sysimage, 'system.manifest')
    for top, dirs, files in os.walk(trimmed, topdown=True):
        rel = os.path.relpath(top, trimmed)
        for name in list(dirs) + files:
            src = os.path.join(top, name)
            dst = os.path.normpath(os.path.join(rootfs, rel, name))
            if os.path.lexists(dst) and os.path.isdir(dst) and not os.path.islink(dst):
                continue                # a parent directory that is still in rootfs: go into it
            if os.path.lexists(dst):
                os.remove(dst)          # the emoji font, smaller than the copy kept here
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.move(src, dst)
            if name in dirs:
                dirs.remove(name)
    shutil.rmtree(trimmed, ignore_errors=True)
    if os.path.exists(manifest + '.full'):
        os.replace(manifest + '.full', manifest)
    print('trim_sysimage.py: everything is back; rebuild with SYSTEM=1')


def tree_size(path):
    if os.path.islink(path) or not os.path.isdir(path):
        return os.lstat(path).st_size
    total = 0
    for top, _, files in os.walk(path):
        for name in files:
            total += os.lstat(os.path.join(top, name)).st_size
    return total


if __name__ == '__main__':
    main()
