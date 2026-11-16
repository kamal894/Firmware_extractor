#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Rebuild the images in caiman/prebuilt/ (see caiman/README.md).
set -euo pipefail
cd "$(dirname "$0")/../probe"
dest=../prebuilt
mkdir -p "$dest"

build() { # name, make args...
	local name=$1; shift
	make -s OUT="out/pb-$name" "$@" >/dev/null
	cp "out/pb-$name/probe-boot.img" "$dest/$name.img"
}

build probe-boot
build probe-boot-lz4 BOOTIMG_ARGS="--compress lz4"
build probe-boot-hang PROBE_FINISH=2
build probe-boot-paint PROBE_PAINT=1 PROBE_FINISH_DELAY_MS=10000
cp out/pb-probe-boot/probe.Image "$dest/probe.Image"

cd "$dest"
sha256sum probe.Image probe-boot*.img > SHA256SUMS
cat SHA256SUMS
