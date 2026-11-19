# Booting iOS from the phone's own storage (2026-11-18)

- **Backup first.** The whole UFS is on the external SSD (`SANDISK SSD/caiman-android-backup-2026-11-18`):
  - `sda.img.zst` holds LUN 0, 127578144768 bytes. Its sha256 was checked again after decompression.
  - `sdb`, `sdc` and `sdd` are kept raw.
  - `boot_a.img` and `boot_b.img` are kept apart as well.
- **Slot b cannot be used.** The bootloader itself is A/B (bl1, bl2, abl, ldfw ... `has-slot:bootloader:yes`), and
  slot b's chain is not ready: `slot-fastboot-ok:b:no`. After `fastboot set_active b` the phone came back in
  slot a's Android (run ws67). Flashing bootloader_b is too risky, so it was not done.
- **What works: iOS in `boot_a` (run ws68).**
  - The boot image is loader + kernel + root, the same one `fastboot boot` sends: 49 MB, and `boot_a` holds 64 MiB.
  - It is written with `fastboot flash boot_a`. The phone then starts iOS by itself, with nothing sent from the Mac.
  - The images are the ones `tools/mkxnuimg.py` makes, and `boot_a` is flashed from the scratchpad script
    `runslota.sh`.
- **Back to Android.** Run `fastboot boot android-boot_a.img` with the original image from the backup. Android then
  runs from RAM, with Magisk, which lives in `init_boot_a` and was not touched. To put Android back for good:
  `fastboot flash boot_a boot_a.img`.
- **Ways out of iOS.**
  - Loader `PROBE_TO_FASTBOOT=1`: when the watcher's time is up, it sets reboot mode "bootloader" (0xfc in PMU+0x810,
    through EL3 with SMC 0x82000504 (addr, 1, value), as Android's pixel-reboot.ko does) and resets. The phone
    lands in fastboot (runs ws66, ws68).
  - Loader `PROBE_XNU_SECONDS=0`: no end set. Holding both volume keys for 3 s (GPIO_ALIVE gpa4-1 and gpa6-2, read
    by the watcher) does the same.
- **Open.** The clock is the time the image was made (`epoch=`), until the PMIC's RTC is read.
