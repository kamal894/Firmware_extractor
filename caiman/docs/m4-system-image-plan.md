# M4/M5 plan: iOS's own user space on caiman

Goal: launchd, then backboardd and SpringBoard, from iOS 16.5's own files. Today's root disk is
HackiOS's Apple-free `hello` (run 5).

## Getting the system image to the kernel

HackiOS's in-memory system image (`sysimage/select-system.py`: the parts of the 7.9 GB root that a
phone with 4 GB for the kernel can hold) is about 2.5 GB. `fastboot boot` cannot carry that.

Measured on the phone (`lpdump`, 2026-11-16):

- `super` is 8,531,214,336 bytes (16,662,528 sectors of 512 B). Only slot `_a` is in it
  (Virtual A/B: no `_b` copies).
- The last used sector is 12,877,208 (`vendor_dlkm_a`), so **≈ 1.94 GB (3,785,320 sectors) is free**
  at its end.

**Chosen (by the owner): the free space of `super`.**

1. An image of at most ~1.9 GB: HackiOS's selection trimmed further (fewer applications, no
   wallpapers or sounds), built on the owner's Mac from their own IPSW
   (`iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`, 6,165,698,198 bytes). It is never shared or
   committed.
2. A logical partition `xnusys` in that free space, made with fastbootd's standard commands
   (`fastboot reboot fastboot`, `fastboot create-logical-partition xnusys <bytes>`,
   `fastboot flash xnusys system.img`). Android and its data are not touched. Before an Android OTA,
   remove it (`fastboot delete-logical-partition xnusys`) so the update has its room back.
3. The loader reads `xnusys` from UFS into XNU's window and lists it as `RAMDisk` (md0), as
   mtkloader does from eMMC. This needs:
   - the UFS host controller (0x13200000, Samsung/Exynos UFSHCI), either as the bootloader left it
     or brought up again;
   - the GPT on the right LUN, to find `super`;
   - the LP metadata in `super` (geometry at +4096, metadata after), to find `xnusys`'s extents.

## Step 1: what state is UFS in when we start? (`probe-boot-ufs.img`)

Read-only: the HCI registers, and the bootloader's own transfer request descriptors if a list is
still programmed. Those show the exact UTRD/UCD/PRDT layout this controller uses (Exynos's PRDT has
quirks). The verdict line says whether the link is still up (then a loader only issues SCSI
READs), or off (then the loader has to start the controller and the UniPro link itself, which is
much more work).
