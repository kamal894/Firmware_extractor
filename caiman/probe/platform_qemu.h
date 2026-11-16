/* SPDX-License-Identifier: MIT */
/*
 * QEMU `virt` test target (see caiman/test/qemu_test.sh).
 *
 * The display "registers" and the bootloader log are plain RAM that the
 * test fills in before boot; the test device tree describes them with the
 * same node shapes as caiman's (ramoops, decon memory-region, dpp nodes).
 */
#ifndef PLATFORM_QEMU_H
#define PLATFORM_QEMU_H

#define PLAT_NAME		"qemu-virt (test)"

#define PLAT_UART_PL011		1
#define PLAT_UART_BASE		0x09000000ULL

#define PLAT_PSCI_HVC		0
#define PLAT_CNTFRQ_FALLBACK	62500000ULL

#define PLAT_RAMOOPS_BASE	0x60000000ULL
#define PLAT_RAMOOPS_SIZE	0x400000ULL
#define PLAT_RAMOOPS_RECORD	0x0ULL
#define PLAT_RAMOOPS_CONSOLE	0x200000ULL
#define PLAT_RAMOOPS_FTRACE	0x0ULL
#define PLAT_RAMOOPS_PMSG	0x200000ULL

#define PLAT_BLDR_LOG_BASE	0x50400000ULL
#define PLAT_BLDR_LOG_SIZE	0x10000ULL

#define PLAT_DECON0_MAIN	0x50000000ULL
#define PLAT_DECON0_WIN		0x50010000ULL
#define PLAT_DECON0_WINCON	0x50020000ULL
#define PLAT_DECON_NWIN		14
#define PLAT_DPUF0_DMA		0x50100000ULL
#define PLAT_DPUF1_DMA		0x50200000ULL

#define PLAT_WDT_BASE		0x50280000ULL	/* fake, inside the test's RAM blob */
#define PLAT_WDT_CLK		24576000ULL

/* XNU (LOADER=1): QEMU virt with -m 6G -smp 2 -machine virtualization=on,gic-version=3 */
#define PLAT_XNU_PHYS		0x80000000ULL
#define PLAT_XNU_SIZE		0x100000000ULL
#define PLAT_ADT_NAME		"qemu-virt"
#define PLAT_IO_BASE		0x08000000ULL
#define PLAT_IO_SIZE		0x08000000ULL
#define PLAT_GICD		0x08000000ULL
#define PLAT_GICR		0x080A0000ULL
#define PLAT_GICR_SIZE		0x100000ULL
#define PLAT_UART_FOR_XNU	0x09000000ULL		/* the real PL011 */
#define PLAT_WATCH_MPIDR	0x1ULL			/* the second vCPU */
#define PLAT_XNU_ARGS		"debug=0x8 serial=3 cpus=1"

/* UFS: no such controller on virt; the test points it at the fake register RAM */
#define PLAT_UFS_HCI		0x50300000ULL
#define PLAT_UFS_VS		0x50301100ULL

#define PLAT_PANEL_W		1280
#define PLAT_PANEL_H		2856

/* Unmapped on virt: a guarded read here must fault and be skipped. */
#define PLAT_SELFTEST_ABORT_ADDR 0x0e000000ULL

#endif
