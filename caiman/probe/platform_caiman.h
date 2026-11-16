/* SPDX-License-Identifier: MIT */
/*
 * Pixel 9 Pro (caiman), Google Tensor G4 (zumapro).
 *
 * Every value here is a FALLBACK: the probe reads the device tree the
 * bootloader passes in x0 first and only uses these if that fails.
 *
 * Confirmed on hardware (docs/hardware-results.md, run 2): entry at EL2,
 * image loaded at 0x80000000, DTB at 0x8a000000, ramoops console zone
 * 0xfd3ff000, boot frame buffer 0xfac00000 1280x2856 BGRA8888 stride 5120
 * on DMA L0 / DECON window 5, DECON0 in command mode (hw TE trigger),
 * cluster-0 watchdog left running, PSCI 1.1 over SMC.
 * Run 3: SCTLR_EL2 at entry 0x30c50990 (MMU and caches off); watchdog period
 * ~60 s (WTCON 0x0001af39, WTDAT 0xffaf); unmasking the DECON TE trigger
 * gives 60 Hz refresh and the painted colour bars appear on the panel.
 * Sources (copies in caiman/reference/google/):
 *   zumapro.dtsi        serial_0 uart@10870000, GICv3, /chosen earlycon
 *   zumapro-rmem.dtsi   ramoops_mem@fd3ff000, bldr_log_reserved@fd800000
 *   zuma-drm-dpu.dtsi   drmdecon0 @19470000, drmdpp L0-L6 @19900000 (+0x1000)
 *   regs-dpp-9865.h     RDMA register offsets (cal_9865 = zuma/zumapro)
 *   regs-decon-9865.h   DECON GLOBAL_CON / WINCON layout
 */
#ifndef PLATFORM_CAIMAN_H
#define PLATFORM_CAIMAN_H

#define PLAT_NAME		"caiman (Pixel 9 Pro, Tensor G4 / zumapro)"

/* Debug UART: Samsung USI UART, 32-bit register access only, FIFO 256. */
#define PLAT_UART_SAMSUNG	1
#define PLAT_UART_BASE		0x10870000ULL

/* PSCI conduit when the DT has no /psci node. */
#define PLAT_PSCI_HVC		0

/* Arch timer frequency if CNTFRQ_EL0 was left at 0 (Tensor: 24.576 MHz). */
#define PLAT_CNTFRQ_FALLBACK	24576000ULL

/* ramoops (zumapro-rmem.dtsi): no record-size, console 2M, pmsg 2M. */
#define PLAT_RAMOOPS_BASE	0xfd3ff000ULL
#define PLAT_RAMOOPS_SIZE	0x400000ULL
#define PLAT_RAMOOPS_RECORD	0x0ULL
#define PLAT_RAMOOPS_CONSOLE	0x200000ULL
#define PLAT_RAMOOPS_FTRACE	0x0ULL
#define PLAT_RAMOOPS_PMSG	0x200000ULL

/* Bootloader's own log buffer (this boot's log, written before it jumps to us). */
#define PLAT_BLDR_LOG_BASE	0xfd800000ULL
#define PLAT_BLDR_LOG_SIZE	0x100000ULL

/* Display (DPU). */
#define PLAT_DECON0_MAIN	0x19470000ULL	/* GLOBAL_CON etc. */
#define PLAT_DECON0_WIN		0x19480000ULL	/* per-window position */
#define PLAT_DECON0_WINCON	0x194A0000ULL	/* per-window enable + channel map */
#define PLAT_DECON_NWIN		14		/* WINCON block is 0xE000 / 0x1000 */
#define PLAT_DPUF0_DMA		0x19900000ULL	/* L0-L6, stride 0x1000 */
#define PLAT_DPUF1_DMA		0x19D00000ULL	/* L7-L13 (hw channels 8-14) */

/* Cluster-0 watchdog, left running by the bootloader (zumapro.dtsi watchdog_cl0). */
#define PLAT_WDT_BASE		0x10060000ULL
#define PLAT_WDT_CLK		24576000ULL	/* OSCCLK */

/* XNU (LOADER=1). Its window is the high DRAM bank: 0x880000000-0xc00000000 (14 GiB) has no
 * reserved region in the bootloader's device tree (run 3); everything of ours is in the low bank. */
#define PLAT_XNU_PHYS		0x880000000ULL
#define PLAT_XNU_SIZE		0x100000000ULL		/* 4 GiB, as the MediaTek port */
/* /arm-io: one window over the SoC's registers and, for the RAM "UART", our own image */
#define PLAT_ADT_NAME		"caiman"
#define PLAT_IO_BASE		0x10000000ULL
#define PLAT_IO_SIZE		0x80000000ULL
#define PLAT_GICD		0x10400000ULL
#define PLAT_GICR		0x10440000ULL
#define PLAT_GICR_SIZE		0x200000ULL
#define PLAT_UART_FOR_XNU	0			/* none usable: a page of RAM (xnu.c) */
#define PLAT_WATCH_MPIDR	0x100ULL		/* core 1 (Aff1 = 1), a Cortex-A520 */
#define PLAT_XNU_ARGS		"debug=0x8 serial=3 cpus=1 -v"

/* UFS (zuma-ufs.dtsi, "samsung,exynos-ufs"): UFSHCI standard registers, vendor registers */
#define PLAT_UFS_HCI		0x13200000ULL
#define PLAT_UFS_VS		0x13201100ULL
#define PLAT_UFS_SCRATCH	0x900000000ULL		/* read-test buffer: free high-bank DRAM */

/* Panel (s6e3hc3), for sanity-checking what the registers say. */
#define PLAT_PANEL_W		1280
#define PLAT_PANEL_H		2856

#endif
