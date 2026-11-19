# Caiman Wi-Fi bring-up

**Hardware.** Google identifies Pixel 9 Pro Wi-Fi as Broadcom **BCM4390**. Use the Caimito 6.1 `bcmdhd/bcm4390` branch. Expected PCI ID is **14e4:4438** (DHD Kbuild), pending a config read. The DT binds WLAN as `android,bcmdhd_wlan` plus a separate `android,bcmdhd_wlan-wonder` node. Bluetooth has its own `goog,nitrous` node and shutdown/wake GPIOs. Treat BT bring-up as separate. (`caiman-live.dts:8462,15130,15210`.) [Google Security Target, Table 30](https://sec-certs.org/cc/3e8369e752fc2e26/target.pdf) · [DHD Kbuild](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/Kbuild)

- **HSI2 PCIe RC channel 1**, `samsung,exynos-pcie-rc`, Gen3 ×1. ELBI `0x13120000`, SoC `0x13130000`, UDBG `0x13150000`, PHY `0x13160000`, SYSREG `0x13020000`, DBI `0x13400000`, PCS `0x13140000`, CFG `0x60ffe000` (8 KiB), IA `0x13100000` (disabled). PMU syscon `0x15460000` plus `0x3ec8`. Both Linux clock counts are zero and the node has no clock specifiers. HSI2 bus clocks and PHY/endpoint reference clock still need to be available. Their handoff state is unverified. (`caiman-live.dts:8422–8470,15436–15439`; `pcie-exynos-rc.c:exynos_pcie_rc_clock_get`.)
- **Pins and rails.** HSI2 pinctrl `0x13040000`: PERST# GPH3.0 (physical low asserts), CLKREQ# GPH3.1 (mux 2), WL_REG_ON GPH3.4 (physical high enables), DEV_WAKE GPH3.5, HOST_WAKE GPA5.3. Linux RC initializes WLAN_EN low; Google DHD platform glue drives it high. The RC has no DT regulator consumer supply. S2MPG15 LDO3 (`L3S_PCIE1`) is always-on (DT limits 725,000–1,300,000 µV, actual setpoint unknown). S2MPG15 uses ACPM IPC channel 2, Speedy bus 1. `VSYS_PWR_WLAN_BT` is an external metered rail. Downstream combo-chip rail voltages and sequencing are unverified. Keep stock PMIC settings for the first probe. (`caiman-live.dts:4698–4710,4784–4821,5284–5303,15640–15645,15926–15935,16291–16298`; `pcie-exynos-rc.c`, WLAN GPIO parsing.) [Google DHD GPIO power sequence, related variant](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4383/+/96454dff665397c5f08bbb83d1b54f9a027faf81/dhd_custom_google.c)
- **Android blobs.** The Caiman extraction manifest lists `/vendor/firmware/fw_bcmdhd.bin_4390_a0` and `/vendor/firmware/bcmdhd_clm.blob_4390_a0`. DT bootargs select `/vendor/firmware`. DHD’s NVRAM basename is `bcmdhd.cal`, with module-ID/chip/revision selection enabled. Exact Caiman calibration suffix is unverified. Resolve it from the matching stock vendor image and DHD selection code before firmware bring-up. (`caiman-live.dts:15169`; `bcm4390/Kbuild`.) [Caiman extraction manifest](https://gitlab.com/CalyxOS/device_google_caimito/-/blob/2f894d11a773d18a4ac6a5b53e619a4981cf23fb/caiman/proprietary-files-vendor.txt)

**Loader milestone → stable endpoint ID.**

1. Snapshot safe PMU, pin and rail state at cold handoff, then DBI and ELBI once clock access is established. Bootloader configuration is unknown. Linux initializes the RC/PHY itself, so plan for a cold sequence even if some clocks and rails survive handoff.
2. Select active pinmux, hold PERST# low and enable WL_REG_ON. Allow endpoint power to settle. Follow `exynos_pcie_rc_poweron` for conditional PMU isolation bypass via the privileged register path, controller clock availability, SoC setup (SoC + `0x4000` ← `0x15`) and PHY power-down clear. Take the channel-1 CAL writes, reset order and delays from the matching `pcie-exynos-zuma-rc-cal.c:exynos_pcie_rc_phy_all_pwrdn_clear/exynos_pcie_rc_pcie_phy_config`. The local ramdisk CAL module exposes those symbols and EXT_PLL/PLL/CDR/OC diagnostics. Verify the exact table against that module before using it.
3. Follow `exynos_pcie_rc_establish_link`: run PHY reset/CAL, apply channel-1 PMU/UDBG/PHY and DBI setup, release PERST# high, wait **15 ms**, set ELBI Q-channel/reset controls and initialize the DesignWare bridge. Start with Gen1 ×1 as a diagnostic, then validate stock Gen3 ×1. Enable LTSSM and poll ELBI for L0 with a bounded timeout. Program bridge bus numbers 0/1/1 and outbound iATU region 0 as CFG0 for `01:00.0`. Read offset zero at `0x60ffe000`, expecting DWORD `0x443814e4`. Keep endpoint bus mastering disabled. Firmware, BAR windows, MSI and DMA can wait. [RC power, training and CFG0 functions](https://android.googlesource.com/kernel/google-modules/soc/gs/+/204078758737776959ecc85c527a00fd2cabb62f/drivers/pci/controller/dwc/pcie-exynos-rc.c) · [Zuma CAL source listing](https://android.googlesource.com/kernel/google-modules/soc/gs/+/refs/tags/android-16.0.0_r0.0)

**Kernel milestone → station traffic.** Port Google DHD’s BCM4390 PCIe FullMAC path first. It has the chip ID, dongle bootstrap and board-file selection. Upstream brcmfmac’s PCIe table lacks BCM4390 support. That route needs chip recognition, firmware mapping and validation of the boot and message-ring ABI. Reuse generic primitives in `wifi/lx` where applicable. `lx_bus.c` and `tools/mk-ath11k.sh` are Qualcomm-specific. Add PCI config/BARs, MSI, noncoherent DMA/cache handling and scoped S2MPU mappings if required, then firmware loading, DHD ring startup and minimal cfg80211/netdev glue. Test firmware-ready → scan → association → DHCP and traffic, then reset/recovery before suspend or BT coexistence. [Google DHD PCIe](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/dhd_pcie.c) · [Upstream brcmfmac PCIe ID table](https://github.com/torvalds/linux/blob/master/drivers/net/wireless/broadcom/brcm80211/brcmfmac/pcie.c)

**Guardrails.** Use RAM-only boots and matching stock calibration/CLM. Avoid OTP/eFuse programming, persistent partition writes and PMIC voltage changes. Wrong rail programming can damage hardware, and unrestricted DMA can corrupt memory. Keep the first probe confined to RC1 and log the first failing stage before retrying.

## Run wifi1 (2026-11-18): what the bootloader leaves (read only, `PROBE_WIFI=1`)

- **HSI2 pinctrl (0x13040000).** Two banks.
  - +0x00 has 6 pins (PUD 0x00111111): taken to be **gph3**. CON is 0 (all inputs); DAT 0x2 (pin 1, CLKREQ#, high;
    PERST# and WL_REG_ON low: the chip is off).
  - +0x20 has 7 pins: taken to be gph4. CON is 0.
  - Android's settings are not there: PERST# out, CLKREQ# function 2, WL_REG_ON out.
- **PMU +0x3ec8** (PCIe 1 PHY isolation): 0xc0.
- **SYSREG HSI2.** Words +0x0 and +0x4 read (0, 0xffffffff). Reads from +0x14 on fault.
- **ELBI** answers: +0x4 0x100, +0x8 0x8000, +0x20/+0x24 1. **SoC** answers, all 0.
- **DBI** answers: the root complex's own ID is 0xa5a5:144d (Samsung), class 0x060400 (a bridge). The bridge's bus
  numbers are 0.
- **PHY (0x13160000) does not answer.** Its clock or power is off, so the PHY bring-up (CMU HSI2 gates, CAL tables)
  comes first.
- **Probes now end in fastboot** (`PROBE_TO_FASTBOOT=1` in `psci.c` too): `boot_a` holds iOS, and a plain reset
  would boot it and overwrite pstore. Script: scratchpad `runprobe.sh` (probe from RAM, then fastboot, then
  Android from RAM, then the log).

## Run wifi2: the PHY answers once its isolation is lifted

- PMU+0x3ec8 bit 0 is Linux's PCIE_PHY_CONTROL_MASK (1 = bypass), found in Google's `pcie-exynos-rc.c`
  `exynos_pcie_phy_isolation`. The bootloader leaves it 0xc0, bit 0 clear: isolated.
- The set-bit alias was written through EL3, as for UFS: SMC 0x82000504 (0x15460000 + (0x3ec8 | 0xc000), 1, bit 0).
  It returned 0, and the register then read **0x1**. Bits 6-7 went to 0 with it; what they are is not known.
- After that the **PHY answers**: +0x0 8, +0x14 3, +0x1c 0x55. UDBG +0xc700 reads 0x00010140, and PCS answers too.
- Next: gph3 pins (bank +0x00: PERST# out low, CLKREQ# function 2, WL_REG_ON out high), then the channel-1 path of
  `pcie-exynos-zuma-rc-cal.c`: `phy_all_pwrdn_clear`, then `pcie_phy_config`. Sources are in
  `~/caiman-work/ref/pcie` (soc/gs `android-gs-akita-5.15-android15`).

## Run wifi3 (2026-11-18): **the link is up** (PROBE_WIFI=3)

- **Pins (gph3, HSI2 pinctrl bank +0x00).** PERST# out low, CLKREQ# function 2, WL_REG_ON out high, then 200 ms:
  CON 0x00010021, DAT 0x12.
- **Then Linux's channel-1 path:**
  - soc +0x4000 = 0x15;
  - the CAL `phy_all_pwrdn_clear` and `pcie_phy_config`, generated into `probe/wifi_cal.inc` straight from Google's
    source (EP_BCM_WIFI = 1, checked against `cmp #1` in the module);
  - PMU 0x3ec8 bit 10, UDBG 0xc800 = 0x421, PHY 0x32c = 0, DBI 0x890 = 0x12000;
  - PERST# high and 15 ms;
  - the ELBI set-up;
  - LTSSM enable.
- **PHY:** external PLL lock, PLL lock, CDR lock and offset calibration were all set.
- **LTSSM: 0x2, then 0x11 = L0 after 40 us.** DBI link status 0x30110000: Gen1 x1, data link active.
- Afterwards the probe drops the link and turns the chip off again.
- **Next:** iATU CFG0 to bus 1 at 0x60ffe000, then read the endpoint's vendor:device (14e4:4438 expected).

## Run wifi4: the chip answers over PCIe (PROBE_WIFI=4)

- **How it was read:** outbound iATU region 0, CFG0 to bus 1 at 0x60ffe000 (unrolled iATU at DBI+0x300000), with
  the bridge's buses set to 0/1/1.
- **Endpoint 01:00.0:** vendor **0x14e4**, device **0x4438**, class 0x028000 (network controller), rev 5, header 0.
  The subsystem is 14e4:**4390**, which confirms the BCM4390.
- **BARs:**
  - BAR0 = 0x4, 64-bit memory;
  - BAR2 = 0x4, 64-bit memory;
  - BAR4 = 0xc, 64-bit prefetchable memory.
  - Capability pointer 0x48, interrupt pin 1.
- **Next:** size and assign the BARs inside the RC's memory window (DT ranges of pcie@13120000), set up outbound
  iATU region 1 as MEM, and read the chip's own registers through BAR0 (Broadcom ChipCommon: chip ID). After that
  comes the DHD port in the kernel: firmware download and the message rings.

## Run wifi5: inside the chip (PROBE_WIFI=5)

- **BARs** were sized and placed in the RC window:
  - BAR0: 64 KiB at 0x60000000 (register window);
  - BAR2: 4 MiB at 0x60400000 (taken to be the TCM, where the firmware goes);
  - BAR4: 1 MiB at 0x60800000.
- Outbound iATU region 1 is MEM over 0x60000000-0x60feffff, and memory decoding is on. Bus master stays off, so the
  chip cannot write to RAM.
- **BAR0 window.** Config 0x80 was left at 0x18001000. Set to 0x18000000 (ChipCommon), BAR0+0 reads 0x60054390:
  **chip ID 0x4390, rev 5**.
- **Next:** the firmware, `/vendor/firmware/fw_bcmdhd.bin_4390_a0`, with CLM and NVRAM. The download path is DHD's:
  halt the ARM core, write the firmware into the TCM through BAR2, put the NVRAM at the end of RAM, release the
  core, then wait for the shared area.

## Firmware files and Android's DHD start (2026-11-18)

- **Files.** Android picks them by chip revision: rev 5 = "b1" (`dhd_get_fw_nvram_names`).
  - Firmware: `fw_bcmdhd.bin_4390_b1` (2516975 bytes).
  - NVRAM: `bcmdhd.cal_USI_G6BB_6327_V27`.
  - CLM: `bcmdhd_clm.blob_4390_b1`; txcap: `bcmdhd_txcap.blob_4390_b1`.
  - Copies are in `~/caiman-work/wifi-fw` (not in git: Broadcom's files).
- **What Android's DHD does at start** (`docs/android-dhd-dmesg.txt`):
  1. WL_REG_ON goes high.
  2. `dhdpcie_chip_specific_init`: an ASR workaround, BAR0 window (0x80) put back to 0x18000000, ssctrl (0x88) to
     0x80080.
  3. ctoctrl 0x8a058000 (completion timeout).
  4. `si_attach` (the core list).
  5. The sysmem core is already up.
  6. Dongle RAM is **3276800 bytes at 0x2a0000**.
  7. Then `dhdpcie_download_code_file`.
- The PCIe core is buscorerev 133. Android's RC is 144d:eced (ours reads 144d:a5a5 before Linux's set-up).

## Run wifi6: the core table, and dongle RAM takes writes (PROBE_WIFI=6)

- **EROM.** ChipCommon +0xfc (EromPtrOffset) = 0x180070b0. Its first 0x200 bytes are in `docs/probe-log-wifi6.txt`.
  They are in NCI format, not AI: they have to be parsed as `nciutils.c` does to find CA7 (0x847), SYSMEM (0x849)
  and PCIe2 (0x83c).
- **Dongle RAM through BAR2, window 0x400000 (config 0x84).**
  - 0x5b0000 held leftover data. A 16-byte pattern written there read back the same.
  - The last word (0x5bfffc) read 0x3fc2553b before anything was written.
  - So the host can write RAM while the chip is as the bootloader left it.
- **Next:** parse EROM (NCI) for CA7's DMP/OOBR control and wrapper, check whether CA7 is halted (DMP bit 5), then
  the download.

## The BCM4390's cores (run wifi6c, decoded by `tools/nci_erom.py`)

- **What was read:** the OOBR page (0x18006000) and EROM1 (0x18007000), both dumped. EROM2 starts at 0x180070b0.
- **How it was decoded:** the way `nciutils.c` reads it (`nci_scan`, `nci_initial_parse`, `nci_save_iface1_reg`
  with the "one less address region" rule). The result is 29 cores (`docs/bcm4390-cores.txt`).
- **Check:** PCIe2 decodes as rev 133, the same as Android's buscorerev.
- **The cores that matter:**
  - ChipCommon rev 79, at 0x18000000.
  - PCIe2 rev 133, at 0x18001000.
  - **ARM CA7 rev 15**: registers at 0x18080000, master node 0x18504000, DMP control 0x18006248 (OOBR +0x240 +8).
  - **SYSMEM rev 16**: registers at 0x18088000, DMP 0x180064c8.
  - D11 x3, rev 89.
  - PMU rev 48, at 0x18018000.
  - GCI rev 32.

## Run wifi7: CA7 is up and halted at hand-off

The CA7 DMP control (0x18006248) reads **0x21**: clock enable plus CPUHALT. SYSMEM DMP reads 0x1. The words at
node +0x140 read 2. That is the state DHD downloads into without resetting anything.

## Run wifi8 (2026-11-18): **the Wi-Fi firmware runs** (PROBE_WIFI=8, WIFI_BLOB=)

- `tools/wifi_blob.py` packs `fw_bcmdhd.bin_4390_b1` (2516975 bytes) and the vars made from
  `bcmdhd.cal_USI_G6BB_6327_V27` the way DHD makes them (501 vars, 18004 bytes). `probe/wifi_fw.S` puts the blob in
  the image.
- **Download:** all of it through BAR2 with the config 0x84 window, as `wifi_boot()` in `wifi.c` does.
  1. The last word is set to 0.
  2. The firmware goes to 0x2a0000. That takes 5.1 s with 32-bit writes, and it reads back the same.
  3. The vars go to 0x5bb9a8, with token 0xee6a1195 at 0x5bfffc.
  4. The RNG rTLV (128 bytes, then 0x80, then 0xfeedc0de) goes below the vars.
  5. Address 0 read 0xb83ef001 (not secured), and the firmware's first word was written there.
  6. CA7 DMP control went from 0x21 to 0x1.
- **Result:** 40 ms later the last word holds the **shared-area pointer, 0x0048a14c**. The shared area reads flags
  0x109 (**version 9**), no trap and no assert. The firmware came up.
- The bus master stayed off throughout. Afterwards the probe halts the CA7 again and powers the chip off.
- **Next, the kernel side:**
  - DMA buffers and the message rings (DHD msgbuf, shared version 9);
  - bus master on, behind the right S2MPU/IOMMU set-up for PCIe;
  - MSI or polling;
  - then the ioctls, the CLM blob, a scan.
  - The download itself can be made faster with memcpy-sized writes.

## Step 9: the firmware's own console (PROBE_WIFI=9, read only)

The first shared pointer (0x48a14c, flags 0x109) is a boot-time one. At ~852 ms the firmware clears its flags and
the last word moves to the real shared area (0x521000 in wifi9d; 0x55b800 in another run, so it is not fixed):

- flags 0x70250109 (version 9), console_addr 0x586134, rings_info_ptr 0x52108c, h2d/d2h mailbox 0x521600/0x521604,
  device_fatal_logbuf 0x4a5e30
- ring_info: ringmem 0x5210d8, index arrays 0x521400/0x5214c0/0x521580/0x5215c0; max_tx_flowrings 0x28,
  max submission queues 0x2b, max completion rings 4
- console (hnd_cons_t: buffer 0x5861f4, 8 KiB): "RTE (PCIE-MSGBUF) 27.10.1121.57.18 (g205e49b b1 release24) on BCM4390
  r5"; three radio slices attach (wl0 5G, wl1 2G, wl2 5G), coex CPU started, "Firmware boot took 889765 us",
  "Notify host through MSI - FW boot complete". No trap.

So the host side must wait for the last word to settle (not take the first non-token value), as dhdpcie_readshared
does by checking the shared version. Log: docs/probe-logs/wifi9.txt.
Next: host DMA buffers + the five common rings (H2D control/rxpost, D2H control/txcpl/rxcpl), bus master on behind
the PCIe S2MPU, MSI or polling of the D2H indices; then the first ioctl (GET_VERSION / "ver" iovar).
