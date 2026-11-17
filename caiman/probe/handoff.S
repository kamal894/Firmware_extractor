/* SPDX-License-Identifier: MIT */
/*
 * Loader-only assembly: entering XNU, and the watcher core.
 * (After hakios-mt6768/mtkloader/start.S, MIT, the HakiOS developers.)
 */

/*
 * jump_to_xnu(entry_pa, boot_args_pa): XNU wants EL1h, MMU and caches off, x0 = boot_args.
 * The Pixel's bootloader enters us at EL2 with nothing set up for a guest, so do what Linux's
 * EL2 setup does for its own EL1 before dropping down.
 */
/*
 * What Linux's EL2 setup does for its own EL1 before dropping down (uses x2 only).
 */
	.macro	el2_for_el1
	/* HCR_EL2: EL1 is AArch64 (RW); pointer authentication not trapped (API, APK): iOS's
	 * arm64e code signs pointers. No virtualization, no traps. */
	mov	x2, #(1 << 31)
	orr	x2, x2, #(1 << 40)
	orr	x2, x2, #(1 << 41)
	msr	hcr_el2, x2
	/* the physical counter and timer belong to EL1; virtual == physical */
	mov	x2, #3
	msr	cnthctl_el2, x2
	msr	cntvoff_el2, xzr
	/* FP/SIMD not trapped (TFP = 0; RES1 bits set) */
	mov	x2, #0x33ff
	msr	cptr_el2, x2
	msr	hstr_el2, xzr
	/* what EL1 reads as MIDR/MPIDR */
	mrs	x2, midr_el1
	msr	vpidr_el2, x2
	mrs	x2, mpidr_el1
	msr	vmpidr_el2, x2
	msr	vttbr_el2, xzr
	/* debug: no traps, all PMU counters to EL1 */
	mrs	x2, pmcr_el0
	ubfx	x2, x2, #11, #5
	msr	mdcr_el2, x2
	/* GICv3: EL1 may use the system register interface */
	mrs	x2, icc_sre_el2
	orr	x2, x2, #0xf		/* SRE, DFB, DIB, Enable */
	msr	icc_sre_el2, x2
	isb
	msr	ich_hcr_el2, xzr
	/* EL1 starts with MMU and caches off (RES1 bits only) */
	ldr	x2, =0x30d00800
	msr	sctlr_el1, x2
	.endm

	.text
	.globl	jump_to_xnu
jump_to_xnu:
	msr	daifset, #0xf
	mov	x16, x0
	mov	x17, x1
	mrs	x2, CurrentEL
	ubfx	x2, x2, #2, #2
	cmp	x2, #2
	b.ne	1f

	el2_for_el1
	/* ERET to EL1h with interrupts masked */
	mov	x2, #0x3c5
	msr	spsr_el2, x2
	msr	elr_el2, x16
	ic	iallu
	dsb	sy
	isb
	mov	x0, x17
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	eret

1:	/* already at EL1 */
	mrs	x2, sctlr_el1
	bic	x2, x2, #0x1
	bic	x2, x2, #0x4
	bic	x2, x2, #0x1000
	msr	sctlr_el1, x2
	isb
	ic	iallu
	dsb	sy
	isb
	mov	x0, x17
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x16

/*
 * The kernel's other cores (cpus=N): XNU's psci_smp.c asks for CPU_ON with this as the entry and its
 * own reset vector (LowResetVectorBase, physical) as the context, because the Pixel's firmware
 * starts a core at EL2 and the kernel's vector wants EL1, MMU and caches off, as the boot core
 * got it from jump_to_xnu. x0 = where to go.
 */
	.globl	secondary_entry
secondary_entry:
	msr	daifset, #0xf
	mov	x16, x0
	mrs	x2, CurrentEL
	ubfx	x2, x2, #2, #2
	cmp	x2, #2
	b.ne	6f
	el2_for_el1
	mov	x2, #0x3c5
	msr	spsr_el2, x2
	msr	elr_el2, x16
	ic	iallu
	dsb	sy
	isb
	mov	x0, xzr
	eret
6:	mov	x0, xzr
	br	x16

/* The watcher core, started by PSCI CPU_ON: MMU off, at EL2 or EL1. */
	.globl	watcher_entry
watcher_entry:
	msr	daifset, #0xf
	msr	spsel, #1
	adrp	x1, watcher_stack
	add	x1, x1, :lo12:watcher_stack
	add	x1, x1, #0x4000		/* top of its 16 KiB */
	mov	sp, x1
	adrp	x1, vectors
	add	x1, x1, :lo12:vectors
	mrs	x2, CurrentEL
	ubfx	x2, x2, #2, #2
	cmp	x2, #2
	b.ne	2f
	msr	vbar_el2, x1
	b	3f
2:	msr	vbar_el1, x1
3:	isb
	bl	watcher_main
4:	wfe
	b	4b

/*
 * mmu_on(l1_table): identity map from one level-1 table of 1 GB blocks, 4K granule, 39-bit
 * addresses; MAIR attr 0 Device-nGnRnE, attr 1 Normal write-back. Inner shareable walks, so
 * this core's cached reads see what the kernel's core has in its caches.
 */
	.globl	mmu_on
mmu_on:
	ldr	x1, =0xff00
	mrs	x2, CurrentEL
	ubfx	x2, x2, #2, #2
	cmp	x2, #2
	b.ne	5f
	msr	mair_el2, x1
	ldr	x1, =0x80823519		/* RES1 31/23, PS 40 bit, SH0 inner, WB, T0SZ 25 */
	msr	tcr_el2, x1
	msr	ttbr0_el2, x0
	isb
	tlbi	alle2
	dsb	sy
	isb
	mrs	x1, sctlr_el2
	orr	x1, x1, #0x1
	orr	x1, x1, #0x4
	orr	x1, x1, #0x1000
	msr	sctlr_el2, x1
	isb
	ret
5:	msr	mair_el1, x1
	ldr	x1, =0x200803519	/* IPS 40 bit, EPD1, SH0 inner, WB, T0SZ 25 */
	msr	tcr_el1, x1
	msr	ttbr0_el1, x0
	isb
	tlbi	vmalle1
	dsb	sy
	isb
	mrs	x1, sctlr_el1
	orr	x1, x1, #0x1
	orr	x1, x1, #0x4
	orr	x1, x1, #0x1000
	msr	sctlr_el1, x1
	isb
	ret
