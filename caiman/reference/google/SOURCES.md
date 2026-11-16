# Reference sources (Google, GPL-2.0)

Unmodified copies of Google's Pixel 9 (caimito) kernel sources, saved so the
port's hardware facts can be checked offline. Fetched 2026-11-16 from
`https://android.googlesource.com`, branch `android-gs-caimito-6.1-android16`.

| File here | Upstream repo / path | Used for |
|---|---|---|
| `zumapro.dtsi` | `kernel/devices/google/zumapro` `dts/zumapro.dtsi` | debug UART `uart@10870000` (reg-io-width 4, FIFO 256), `/chosen` earlycon, GICv3, root `#size-cells = <1>` |
| `zumapro-rmem.dtsi` | same, `dts/zumapro-rmem.dtsi` | `ramoops_mem@fd3ff000` (4 MiB: console 2 MiB + pmsg 2 MiB, no ECC), `bldr_log_reserved@fd800000` |
| `zumapro-debug.dtsi`, `zumapro-usi.dtsi`, `zumapro-common.dtsi`, `zumapro-a0.dtsi`, `zumapro-a1-ipop.dts`, `zumapro-drm-dpu.dtsi` | same, `dts/` | SoC context |
| `zuma-drm-dpu.dtsi` | `kernel/devices/google/zuma` `dts/zuma-drm-dpu.dtsi` | DECON0 @ `0x19470000` (main/win/wincon), DPP DMA channels L0-L6 @ `0x19900000 + n*0x1000`, L7-L13 @ `0x19D00000 + n*0x1000` |
| `caiman-mp.dts`, `caiman-common.dtsi` | `kernel/devices/google/caimito` `dts/zumapro-caiman-*.dts*` | board top level |
| `regs-dpp-9865.h`, `dpp_reg.c` | `kernel/google-modules/display` `samsung/cal_9865/` | RDMA register map: `BASEADDR_P0` +0x40, `SRC_WIDTH/HEIGHT` +0x10/+0x14, `IMG_SIZE` +0x1C, format in `IN_CTRL_0[13:8]`, shadow copy at +0x400 |
| `regs-decon-9865.h`, `decon_reg.c` | same, `samsung/cal_9865/` | `GLOBAL_CON` +0x20 (bit 8 = command mode), `WINCON` per window (+0x1000 each, bit 0 enable, bits 7:4 channel), channel 7 absent on zuma |
| `exynos_drm_decon.c`, `exynos_drm_fb.c`, `exynos_drm_dpp.c`, `exynos_drm_drv.c` | same, `samsung/` | frame-buffer handover: the DECON node's `memory-region` (added by the bootloader at boot) |

`zumapro_defconfig` sets both `CONFIG_SOC_ZUMA=y` and `CONFIG_SOC_ZUMAPRO=y`,
so the display driver is built with the `cal_9865` (zuma) register layout
on the Pixel 9.

To refresh a file:

```bash
curl -s "https://android.googlesource.com/kernel/devices/google/zumapro/+/refs/heads/android-gs-caimito-6.1-android16/dts/zumapro-rmem.dtsi?format=TEXT" | base64 -d
```

## UFS (added for M4)

From `kernel/google-modules/soc/gs`, same branch, `drivers/ufs/`:

| File here | Used for |
|---|---|
| `ufs/ufs-exynos.c` | `exynos_ufs_hibern8_notify()`: clock gating and CAL tables around Hibern8; quirks (`fixed-prdt-req_list-ocs`) |
| `ufs/ufs-vs-regs.h` | vendor registers: `HCI_CLKSTOP_CTRL` 0xB0, `HCI_FORCE_HCS` 0xB4, UniPro `PA_*` offsets |
| `ufs/ufs-exynos-gs.h`, `ufs/ufs-vs-mmio.h`, `ufs/ufs-cal-if.h` | context |
| `ufs/zuma/ufs-cal.h` | zuma PHY tables: `post_h8_enter`, `pre_h8_exit` |
| `ufs/zuma/ufs-cal-if.c`, `ufs/zuma/ufs-cal-if.h`, `ufs/zuma/ufs-vs-mmio.h` | how a table is applied (PMA lane stride 0x800, per-lane loop) |

`zuma-ufs.dtsi` (already here) gives the register map: HCI 0x13200000, vendor 0x13201100, UniPro
0x13280000, PHY 0x13204000.

`../linux/ufs-exynos.c` and `ufs-exynos.h` are upstream Linux's `drivers/ufs/host/` (torvalds/linux
master, GPL-2.0), for comparison.
