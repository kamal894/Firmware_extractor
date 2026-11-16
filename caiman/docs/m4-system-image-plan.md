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

### Run 10: the device is powered down, not just asleep

With the clocks and the PHY prepared exactly as Google's driver does before an exit, DME_HIBER_EXIT
still ended in power mode status 5. The registers showed why the preparation changed nothing: the
bootloader had not stopped the clocks (`CLKSTOP_CTRL` 0) nor applied `post_h8_enter` (PHY common
0x000 was already 0x11). The answer is in run 7's dump: the bootloader's last request (slot 0) was a
QUERY READ ATTRIBUTE of `bCurrentPowerMode` (IDN 0x02), and the device answered 0x33,
**UFS-PowerDown**. "shutdown complete" means the device was powered down; the link cannot just be
woken.

### Step 2: a cold start (`probe-boot-ufs.img`, PROBE_UFS=2, from commit after 225d5cd)

What Linux's ufshcd core and Google's Tensor driver do at every boot, in `probe/ufs.c`:

1. board: PHY isolation (PMU 0x15460000 + 0x3EC0, bit 0 = bypass; written only through EL3's
   SMC 0x82000504 if it reads isolated), device VCC (`ufs-vcc` = GPIO gpp0-1, 0x10840000 CON/DAT;
   switched on only if the pin is an output and low);
2. host: HCE off, vendor software reset (`HCI_SW_RST`), Google's `exynos_ufs_config_host` values,
   device reset pin (`HCI_GPIO_OUT`) pulsed, HCE on, clock-stop control as in `hce_enable_notify`;
3. link: zuma CAL `init_cfg` table (57 PHY/PCS/UniPro entries, `probe/ufs_cal_zuma.h`, GPL-2.0),
   DME_LINKSTARTUP (3 tries), `post_init_cfg`;
4. our own transfer/task lists (in the image, below 4 GiB: Google gives this controller a 32-bit
   DMA mask), NOP OUT/IN, fDeviceInit;
5. READ(10)s at PWM-G1: the GPTs, `super`, the LP geometry and metadata header.

The timing values (`1US_TO_CNT` = 178, so mclk ≈ 178.5 MHz, and the UniPro period value) are read
from what the bootloader programmed, before the reset clears them.

### Runs 11-14: NOP OUT, and the nexus bit

Run 11 (the cold start): link start-up works (both lanes, 57 + 5 CAL entries), but NOP OUT never
completes: the response stays empty and OCS keeps the 0x0f we put there.

- Run 12 added cache maintenance (`dc civac` around every buffer the controller touches: the
  controller is IO-coherent, sysreg IOCC = 0x13, so its DMA snoops caches our D-cache-off accesses
  bypass) and a register dump. It showed `IS 0x1004` (UTP error) and `HCS 0x0001200f`: UTP error
  code 2 on task tag 1. The controller sent the NOP OUT and rejected the answer.
- Run 13: task tag 0 for slot 0 (as Linux: tag = slot). Same error, now on tag 0.
- Run 14: lists and descriptor moved to where the bootloader kept its own (0xf8c99000...). Same
  error, so it is not where the memory is.

The cause: Google's `exynos_ufs_set_nexus_t_xfer_req()` sets the slot's bit in vendor
`HCI_UTRL_NEXUS_TYPE` (+0x40) before every request, 1 for a SCSI COMMAND and 0 for NOP OUT and
QUERY. `exynos_ufs_config_host()` sets all 32 bits to 1, and the bootloader had left 0xfffffffe
(slot 0, its query slot, at 0). With the bit at 1 the controller expects a RESPONSE UPIU and
rejects a NOP IN or QUERY RESPONSE. `request()` now sets the bit from the UPIU type.

### Run 15: **UFS works**, and super's LP metadata is parsed

```
ufs:   NOP OUT: NOP IN after 1 tries (answer 0x20, OCS 0x00)
ufs:   SET FLAG fDeviceInit: accepted (0)
ufs:   fDeviceInit reads 0 after 0 ms (device ready)
ufs: lun 0: GPT, 34 entries ... super lba 367078 .. 2449893 (8136 MiB)
ufs: lun 1/2: bl1, pbl, bl2, abl, bl31, tzsw, gsa, ldfw ... (_a, _b);  lun 3: devinfo, pinfo, blenv, mfg_data
ufs: super metadata slot 0: version 10.2, header 256 bytes, tables 616 bytes, flags 0x1
ufs:   system_a             attr 0x1, 1 extent(s)
ufs:     linear 2012328 sectors at super sector 2048 = lun 0 lba 367334..618874
...
ufs:   vendor_dlkm_a        attr 0x1, 1 extent(s)
ufs:     linear 49560 sectors at super sector 12827648 = lun 0 lba 1970534..1976728
ufs: super: used up to sector 12877208; free after it: 3785320 sectors (1848 MiB)
ufs: lookup system_a: extent 0 = lun 0 lba 367334, 251541 blocks
ufs: system_a block 0: 00 00 00 00 ... ext4 magic at 0x438: 0xef53 (want 0xef53)
```

All six partitions, their extents and the free space match `lpdump` on the phone exactly. The first
READ(10) on each LUN gets CHECK CONDITION (UNIT ATTENTION after the reset) and the retry succeeds.
`lp_find()` is what the loader will use to find `xnusys`. Still PWM-G1 (slow): HS gear is next,
before reading ~1.9 GB.

### Runs 16-19: HS gear, and the reference clock pin

`ufs_hs_gear()` does what ufshcd's power mode change does with Google's `pre_pmc` CAL around it:
block `PA_ERROR_IND_RECEIVED` in the DL error mask, apply zuma's `calib_of_hs_rate_b` (PA_TXHSADAPTTYPE
and the DL timeouts), DME_SET gear, active lanes, termination and HS series B, then `PA_PWRMODE` =
FAST/FAST (0x11) and wait for `IS.UPMS`.

- Run 16: power mode status 5 (PWR_FATAL_ERROR), no UIC error codes; the link was dead after it.
- Run 17: link start-up twice, as `ufshcd_link_startup()` does for a device that is not active
  yet: the second one fails at once (result 1, device gone). Dropped. The `UECPA 0x80000010` after
  every link start-up is the generic PA error, ufshcd's LINERESET indication: expected.
- Run 18: still status 5, still no UIC errors.
- Run 19: the UFS node's pinctrl (`ufs_refclk_out`, `ufs_rst_n`: GPIO_HSI2UFS at 0x13060000,
  `gph5-0` and `gph5-1`, function 2) had been changed by the bootloader's shutdown:

```
ufs: refclk/RST_n pins (gph5): CON 0x00000021 (functions 1, 2; want 2, 2) DAT 0x00000000 PUD 0x00000001 DRV 0x00000002
ufs: refclk/RST_n pins set to function 2: CON 0x00000022 PUD 0x00000000
ufs: speed (PWM-G1): 2 MiB in 3280 ms = 639 KB/s
ufs: max HS gear: host 4, device 4; connected lanes tx 2 rx 2 -> HS-G4 rate B x2
ufs: power mode change: status 1 (1 = done), IS 0x00000001
ufs: now HS-G4/G4 rate B, 2 lane(s): the link is fast
ufs: speed (system_a): 256 MiB in 11380 ms = 23588 KB/s
```

`gph5-0` was a plain GPIO output driving 0: no reference clock at the device. PWM gears do not
need it (they carry their own clock), HS gears do. With the pin back on function 2, HS-G4 works.

23.6 MB/s is far below what HS-G4 x2 can do (one 256 KiB READ(10) at a time, ~11 ms each), but it
reads ~1.9 GB in about 80 s, which is enough to go on with. Queueing several requests (32 slots)
and longer transfers are the obvious improvements, later.
