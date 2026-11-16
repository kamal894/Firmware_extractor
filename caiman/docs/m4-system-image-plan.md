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

### Result (run 7, `probe-boot-ufs.img` built with PROBE_UFS=1): **UFS is up**

```
ufs: CAP 0x1383ff1f (32 transfer slots, 4 task slots, 64-bit yes) VER 0x00000300 PID 0x00000106 MID 0x000001ce
ufs: HCE 0x00000001 (enabled)  HCS 0x0000010f: device present, UTRL ready, UTMRL ready, UIC cmd ready, power mode 1
ufs: UTRLBA 0x00000000f8c99000  doorbell 0x00000000  run 0x00000001   UTMRLBA 0xf8c97000 run 0x00000001
ufs: verdict: UP: the bootloader left the link running; a loader can issue reads
```

The bootloader's last requests, decoded from its own descriptors (in its reserved memory, abl@f8800000):

- slot 2: `UTRD 15000000 ... 00080010 00180fff`. CT=UFS storage, DD=device to host, interrupt. The
  response UPIU is at UCD+8 dwords (16 dwords long), and the PRDT at UCD+24 dwords with 4095 entries.
  Command UPIU: LUN 0, expected length 0x00fff000, CDB `28 00 00 02 ff 84 00 0f ff 00` =
  **READ(10)** LBA 0x2ff84, 4095 blocks of 4 KiB.
- PRDT entries: 16 bytes each (`a78fe000 00000000 00000000 00000fff`), 4 KiB per entry. This is
  standard UFSHCI 3.0: no Exynos byte-granularity quirk, no inline-crypto fields.
- slot 1: START STOP UNIT (power condition ACTIVE) to the UFS Device W-LUN (0xD0); slot 0: a
  query request.

So the loader can issue its own READ(10)s through slot 0 of that running list, with no controller or
link bring-up. The next probe (PROBE_UFS=2) does exactly that, read-only: every LUN's GPT, the
`super` partition, and the LP geometry and metadata header inside it.

The IPSW is on Apple's CDN (`updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85130/6AB0D10A-8AF9-4F6C-8CF5-A1B315CABE88/`),
SHA-1 `6ee6e5b447902dbb36a3991ca20d032d6b4cc759` (per ipsw.me's index).

### Run 8 (PROBE_UFS=2): every READ timed out

`ufs: lun 0..3: read failed (-3)`. Run 7's dump already showed why: `UICCMD` (0x90) held 0x17,
DME_HIBER_ENTER. The bootloader put the link in Hibern8 before handing over, so requests wait.

### Run 9: DME_HIBER_EXIT alone is not enough

```
ufs: DME_HIBER_EXIT: result 0 (0 = success), IS 0x00000420 (exit status set), HCS 0x0000050f (power mode status 5, 1 = done)
ufs: lun 0: read failed (-3)
```

Power mode status 5 is PWR_FATAL_ERROR. Google's Tensor UFS driver
(`google-modules/soc/gs` `drivers/ufs/ufs-exynos.c`, copied to `reference/google/ufs/`) shows the
missing part. After every Hibern8 entry it runs the CAL table `post_h8_enter` (PHY common register
0x000 = 0x51, ...) and stops the controller's internal clocks (`HCI_CLKSTOP_CTRL`). Before every exit
it undoes both:

1. `exynos_ufs_gate_clk(false)`: clear `CLK_STOP_ALL` (0x17) in vendor `HCI_CLKSTOP_CTRL` (+0xB0);
2. `ufs_call_cal()`: clear `UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN` in `HCI_FORCE_HCS` (+0xB4),
   apply zuma's `pre_h8_exit` table to the PHY (0x13204000: common 0x000 = 0x11, wait 10 us; per
   lane, stride 0x800: 0x9F4 = 0x00, 0xA00 = 0x30, 0xB64 = 0x32 then 0x22), set `MPHY_APBCLK_STOP_EN`
   again;
3. then DME_HIBER_EXIT, and power mode status must read 1 (PWR_LOCAL).

zuma's DT has `fixed-prdt-req_list-ocs`, so the PRDT, UTRLCLR and OCS behave as the UFSHCI standard
says (no Exynos byte-granularity quirk), which matches the bootloader's own descriptors.

The next `probe-boot-ufs.img` does steps 1-3, printing every register before and after.
