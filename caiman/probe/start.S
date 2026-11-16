/* SPDX-License-Identifier: MIT */
/*
 * Entry point. The file starts with an arm64 Linux "Image" header so the
 * stock bootloader loads and starts us exactly like a kernel:
 *   MMU off, D-cache off, x0 = physical address of the device tree,
 *   x1..x3 = 0, running at EL2 (pKVM-capable Pixels) or EL1.
 * All code is position independent: the load address is not known.
 */

	.section .text.head, "ax"
	.globl	_head
_head:
	b	entry			/* code0 */
	.long	0			/* code1 */
	.quad	0			/* text_offset */
	.quad	_image_size		/* image_size: file + bss + stack */
	.quad	0xa			/* flags: little endian, 4K pages, place anywhere */
	.quad	0			/* res2 */
	.quad	0			/* res3 */
	.quad	0			/* res4 */
	.ascii	"ARM\x64"		/* magic */
	.long	0			/* res5 */

entry:
	msr	daifset, #0xf		/* keep all exceptions masked */
	mov	x19, x0			/* device tree */
	adr	x20, _head		/* our load address */
	mrs	x21, CurrentEL

	msr	spsel, #1		/* exceptions use SP_ELx: make that our stack */
	adrp	x1, __stack_top
	add	x1, x1, :lo12:__stack_top
	mov	sp, x1

	adrp	x1, __bss_start		/* zero .bss (and the stack inside it) */
	add	x1, x1, :lo12:__bss_start
	adrp	x2, __bss_end
	add	x2, x2, :lo12:__bss_end
1:	cmp	x1, x2
	b.hs	2f
	stp	xzr, xzr, [x1], #16
	b	1b
2:
	adrp	x1, vectors		/* our own exception vectors */
	add	x1, x1, :lo12:vectors
	ubfx	x2, x21, #2, #2
	cmp	x2, #2
	b.ne	3f
	msr	vbar_el2, x1
	b	4f
3:	msr	vbar_el1, x1
4:	isb

	mov	x0, x19
	mov	x1, x20
	mov	x2, x21
	bl	probe_main
5:	wfe
	b	5b

/* ---------------------------------------------------------------------- */
/* Exception vectors: save everything, hand ESR/FAR/ELR/SPSR to C.        */
/* exc_handler() returns only to resume (guarded MMIO read); it may have  */
/* changed the saved ELR (frame slot 31).                                  */

#define FRAME_SIZE	(34 * 8)
#define FRAME_ELR	(31 * 8)
#define FRAME_SPSR	(32 * 8)

.macro	VENTRY idx
	.balign	0x80
	sub	sp, sp, #FRAME_SIZE
	stp	x0, x1, [sp, #0]
	mov	x0, #\idx
	b	exc_common
.endm

	.section .text.vectors, "ax"
	.balign	0x800
	.globl	vectors
vectors:
	VENTRY	0	/* current EL, SP0: sync */
	VENTRY	1	/*                  irq */
	VENTRY	2	/*                  fiq */
	VENTRY	3	/*                  serror */
	VENTRY	4	/* current EL, SPx: sync */
	VENTRY	5	/*                  irq */
	VENTRY	6	/*                  fiq */
	VENTRY	7	/*                  serror */
	VENTRY	8	/* lower EL, A64:   sync */
	VENTRY	9
	VENTRY	10
	VENTRY	11
	VENTRY	12	/* lower EL, A32 */
	VENTRY	13
	VENTRY	14
	VENTRY	15

exc_common:
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	str	x30, [sp, #240]

	mrs	x9, CurrentEL
	cmp	x9, #(2 << 2)
	b.eq	1f
	mrs	x1, esr_el1
	mrs	x2, far_el1
	mrs	x3, elr_el1
	mrs	x4, spsr_el1
	b	2f
1:	mrs	x1, esr_el2
	mrs	x2, far_el2
	mrs	x3, elr_el2
	mrs	x4, spsr_el2
2:	str	x3, [sp, #FRAME_ELR]
	str	x4, [sp, #FRAME_SPSR]
	mov	x5, sp
	bl	exc_handler

	ldr	x3, [sp, #FRAME_ELR]
	mrs	x9, CurrentEL
	cmp	x9, #(2 << 2)
	b.eq	3f
	msr	elr_el1, x3
	b	4f
3:	msr	elr_el2, x3
4:
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	ldp	x0, x1, [sp, #0]
	add	sp, sp, #FRAME_SIZE
	eret
