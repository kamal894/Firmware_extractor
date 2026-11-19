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
