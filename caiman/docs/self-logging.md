# Self-logging: the phone keeps its own logs (no Android needed to keep them)

The loader and the iOS kernel log into the ramoops console zone (0xfd3ff000, 2 MiB, Linux's
persistent_ram_buffer: "DBGC", start, size, data). DRAM keeps it across a warm reset, but the next boot's
loader reuses the zone, and Android was the only way to read it.

With `PROBE_UFSLOG=1` (and `PROBE_UFS=2`) the loader:

1. copies the previous boot's zone aside, first thing, before attaching its own log (main.c,
   `ufslog_snapshot`, persistent_ram_save_old's order: data[start..size) then data[0..start));
2. once UFS is up and the GPT read, writes it into **boot_b** (LUN 0, lba 260278, 16384 blocks of 4 KiB = 64 MiB;
   slot b is not bootable on this phone; Android's own boot_b is in the SSD backup, boot_b.img and sda.img.zst).

Layout of boot_b:

| block | contents |
|---|---|
| 0 | superblock "HKLOGSB1": version 1, next slot, saves so far, 31 slots |
| 512·(k+1) | slot k header "HKLOGSL1": save number, length, CRC-32, loader version |
| 512·(k+1)+1 .. +511 | the text (up to 2 MiB − 4 KiB) |

Safety: `ufs_write()` (WRITE(10)) refuses anything outside boot_b as the GPT names it on LUN 0; the first save
only starts the area if block 0 is our own old slot-b test image (boot header with a 124-byte ramdisk) or our
superblock; the slot header is read back before the superblock points at it. A cold boot (no "DBGC" in RAM)
saves nothing.

Read it back: copy boot_b off the phone and `tools/ufslog.py DUMP [N]` (lists the saves, checks each CRC, prints
the newest or save N).

Verified 2026-11-19: the same probe run twice from RAM; the second run saved the first one's whole log
(71340 bytes, bootloader lines through "=== caiman-probe end ===") as save 1 in slot 0, CRC ok, read back
through Android with dd.

Verified with iOS, 2026-11-19 (scratchpad runselflog.sh): loader `out/loader-fbl` (loader-fb + PROBE_UFSLOG=1)
and the kernel flashed into boot_a; the phone booted iOS by itself, ran 200 s, went to fastboot (warm reset),
booted iOS by itself again from boot_a, and that boot's loader saved the first boot's whole log as save 2:
1174937 bytes, CRC ok (bootloader lines, loader, XNU, launchd, SpringBoard). No Android was involved in
keeping it; Android only read boot_b back afterwards to check.

Then: the daily loader with PROBE_UFSLOG=1, so every boot keeps the previous boot's iOS log; then a way to see
the saves without Android (the loader showing the last one on the panel, or the kernel exposing them).
