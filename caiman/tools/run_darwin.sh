#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Boot iOS's user space on a connected Pixel 9 Pro from RAM (nothing is flashed) and fetch the log
# and a picture of the panel.
#
#   caiman/tools/run_darwin.sh LOADER_IMAGE KERNEL ROOT_APFS [LOG]
#
# LOADER_IMAGE: probe/out/.../probe.Image from make LOADER=1 PROBE_UFS=2 (with probe/identity.h);
# KERNEL: HackiOS's prebuilt/hakios-kernel; ROOT_APFS: HackiOS's userland/root.apfs. The system
# image itself is the logical partition xnusys in super (docs/m4-system-image-plan.md).
# The boot image is made here, in fastboot, so that its epoch= is the time of the boot.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
loader=$1 kernel=$2 root=$3 log=${4:-darwin-log.txt}
img=$(mktemp -t darwin-boot).img
trap 'rm -f "$img"' EXIT
for t in adb fastboot python3; do command -v $t >/dev/null || { echo "$t not found" >&2; exit 1; }; done

ANDROID_SERIAL=${ANDROID_SERIAL:-$(adb devices -l | awk '$2 == "device" && /device:caiman/ { print $1; exit }')}
[ -n "$ANDROID_SERIAL" ] || { echo "no Pixel 9 Pro (device:caiman) in adb" >&2; exit 1; }
export ANDROID_SERIAL
adb reboot bootloader
for _ in $(seq 60); do fastboot devices | grep -q "^$ANDROID_SERIAL" && break; sleep 1; done
python3 "$here/mkxnuimg.py" --loader "$loader" --kernel "$kernel" --ramdisk "$root" \
	--cmdline "epoch=$(date +%s)" --out-image "$img.Image" --out-bootimg "$img" >/dev/null
rm -f "$img.Image"
fastboot boot "$img"
echo "booted at $(date '+%H:%M:%S'); the loader reads xnusys (~90 s), then XNU runs until the watcher resets the phone"
for _ in $(seq 300); do
	[ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ] && break
	sleep 2
done
bash "$here/run_probe.sh" --fetch "$log" >/dev/null || true
grep -a -E '^xnu: time|^watcher: (PSCI|[0-9]+ s)|BSD root|^screen: [0-9]+ of' "$log" || true
python3 "$here/screen_dump.py" "$log" "${log%.txt}.png" --scale 2 || true
