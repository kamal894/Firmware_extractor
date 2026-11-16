#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Boot caiman-probe on a connected Pixel 9 Pro and save its log.
#
#   caiman/tools/run_probe.sh                 boot prebuilt/probe-boot.img, save probe-log.txt
#   caiman/tools/run_probe.sh IMAGE [LOG]     boot another image (e.g. probe-boot-lz4.img)
#   caiman/tools/run_probe.sh --fetch [LOG]   only read the log (after a manual force-restart)
#
#   UART=1 caiman/tools/run_probe.sh          also run `fastboot oem uart enable` first
#
# Picks the device whose `adb devices -l` line says device:caiman, so other
# phones or emulators can stay connected. Set ANDROID_SERIAL to override.
# Needs root on the phone (su) to read /sys/fs/pstore.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
RAW=https://raw.githubusercontent.com/kamal894/Firmware_extractor/claude/new-session-yom1ka
fetch_only=0
if [ "${1:-}" = "--fetch" ]; then
	fetch_only=1
	shift
	log=${1:-probe-log.txt}
else
	img=${1:-}
	if [ -z "$img" ]; then
		for c in "$here/../prebuilt/probe-boot.img" ./probe-boot.img; do
			[ -f "$c" ] && { img=$c; break; }
		done
	fi
	log=${2:-probe-log.txt}
	if [ ! -f "${img:-}" ]; then
		echo "image not found: ${img:-probe-boot.img}. Download it with:" >&2
		echo "  curl -LO $RAW/caiman/prebuilt/${1:-probe-boot.img}" >&2
		exit 1
	fi
fi

for t in adb fastboot; do
	command -v "$t" >/dev/null || { echo "$t not found: install Android platform-tools" >&2; exit 1; }
done

if [ -z "${ANDROID_SERIAL:-}" ]; then
	ANDROID_SERIAL=$(adb devices -l | awk '$2 == "device" && /device:caiman/ { print $1; exit }')
	if [ -z "$ANDROID_SERIAL" ]; then
		echo "no Pixel 9 Pro (device:caiman) found. adb sees:" >&2
		adb devices -l >&2
		echo "set ANDROID_SERIAL=<serial> to pick one by hand" >&2
		exit 1
	fi
fi
export ANDROID_SERIAL	# adb and fastboot both honour it
echo "device: $ANDROID_SERIAL"

wait_android() { # seconds
	local deadline=$((SECONDS + $1))
	until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
		if [ $SECONDS -ge $deadline ]; then
			return 1
		fi
		sleep 2
	done
}

if [ $fetch_only = 0 ]; then
	echo "rebooting to the bootloader..."
	adb reboot bootloader
	for _ in $(seq 60); do
		fastboot devices 2>/dev/null | grep -q "^$ANDROID_SERIAL" && break
		sleep 1
	done
	if ! fastboot devices 2>/dev/null | grep -q "^$ANDROID_SERIAL"; then
		echo "the phone did not show up in fastboot within 60 s." >&2
		echo "if this computer is a VM, USB passthrough needs a rule for the bootloader-mode USB ID too." >&2
		exit 1
	fi
	if [ "${UART:-0}" = "1" ]; then
		fastboot oem uart enable || echo "(oem uart enable failed; continuing)"
	fi
	echo "booting $(basename "$img") from RAM (nothing is flashed)..."
	fastboot boot "$img"
	echo "probe started; it reboots into Android by itself. Waiting up to 4 minutes..."
	if ! wait_android 240; then
		echo "Android did not come back, so the probe is running (or stuck)." >&2
		echo "Force-restart the phone (hold Power until it restarts), let Android boot, then run:" >&2
		echo "  bash $0 --fetch $log" >&2
		exit 2
	fi
else
	wait_android 30 || { echo "phone is not booted into Android" >&2; exit 1; }
fi

echo "reading pstore..."
adb shell su -c "'ls -l /sys/fs/pstore'" || true
adb shell su -c "'cat /sys/fs/pstore/console-ramoops*'" | tr -d '\r' > "$log" || true

if grep -q '=== caiman-probe' "$log"; then
	echo "saved $(wc -c < "$log") bytes to $log"
	if grep -q '=== SUMMARY ===' "$log"; then
		sed -n '/=== SUMMARY ===/,$p' "$log"
	else
		echo "no SUMMARY: the probe stopped early. Last lines:"
		tail -n 15 "$log"
	fi
else
	echo "no caiman-probe text in pstore (saved what was there to $log)."
	echo "--- diagnostics (paste all of this) ---"
	echo "pstore log: $(wc -c < "$log" | tr -d ' ') bytes, last lines:"
	tail -n 8 "$log"
	for p in ro.boot.bootreason sys.boot.reason sys.boot.reason.last persist.sys.boot.reason.history; do
		echo "$p = $(adb shell getprop "$p" 2>/dev/null | tr -d '\r' | tr '\n' ' ')"
	done
	echo "--- next test ---"
	echo "download the 'hang' probe, which never reboots by itself:"
	echo "  curl -LO $RAW/caiman/prebuilt/probe-boot-hang.img"
	echo "  bash $0 probe-boot-hang.img"
	exit 3
fi
