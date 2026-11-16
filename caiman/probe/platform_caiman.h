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

/* Panel (s6e3hc3), for sanity-checking what the registers say. */
#define PLAT_PANEL_W		1280
#define PLAT_PANEL_H		2856

#endif
