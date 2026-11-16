#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Rebuild the images in caiman/prebuilt/ (see caiman/README.md).
set -euo pipefail
cd "$(dirname "$0")/../probe"
dest=../prebuilt
mkdir -p "$dest"

build() { # name, make args...
	local name=$1; shift
	rm -rf "out/pb-$name"
	make -s OUT="out/pb-$name" "$@" >/dev/null
	cp "out/pb-$name/probe-boot.img" "$dest/$name.img"
}

build probe-boot
build probe-boot-lz4 BOOTIMG_ARGS="--compress lz4"
build probe-boot-hang PROBE_FINISH=2
build probe-boot-paint PROBE_PAINT=1 PROBE_FINISH_DELAY_MS=10000
build probe-boot-ufs PROBE_UFS=1
cp out/pb-probe-boot/probe.Image "$dest/probe.Image"

# M2: the loader with HackiOS's prebuilt XNU (hackios-xnu-kernel/prebuilt/hakios-kernel)
HAKIOS_KERNEL=${HAKIOS_KERNEL:-/home/user/oosexgang/hackios-xnu-kernel/prebuilt/hakios-kernel}
if [ -f "$HAKIOS_KERNEL" ]; then
	rm -rf out/pb-xnu
	make -s OUT=out/pb-xnu LOADER=1 >/dev/null
	python3 ../tools/mkxnuimg.py --loader out/pb-xnu/probe.Image --kernel "$HAKIOS_KERNEL" \
		--kernel-sha256 03e794e8e1d389a6abb9bcd2431eaccf9747271ff8c1234922225718c70441e9 \
		--out-image out/pb-xnu/xnu.Image --out-bootimg "$dest/xnu-boot.img" >/dev/null
	# M4 (first step): the same with HackiOS's raw-syscall hello as the RAM disk (process 1)
	HAKIOS_HELLO=${HAKIOS_HELLO:-$(dirname "$HAKIOS_KERNEL")/userland/hello}
	python3 ../tools/mkxnuimg.py --loader out/pb-xnu/probe.Image --kernel "$HAKIOS_KERNEL" \
		--kernel-sha256 03e794e8e1d389a6abb9bcd2431eaccf9747271ff8c1234922225718c70441e9 \
		--ramdisk "$HAKIOS_HELLO" --out-image out/pb-xnu/hello.Image --out-bootimg "$dest/hello-boot.img" >/dev/null
else
	echo "no HackiOS kernel at $HAKIOS_KERNEL: xnu-boot.img and hello-boot.img not rebuilt" >&2
fi

cd "$dest"
sha256sum probe.Image probe-boot*.img xnu-boot.img hello-boot.img > SHA256SUMS
cat SHA256SUMS
