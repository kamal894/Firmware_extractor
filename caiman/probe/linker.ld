/* SPDX-License-Identifier: MIT */
/*
 * Linked at 0 as a PIE so that any absolute address baked into the image
 * shows up as a dynamic relocation. There is no relocator: the ASSERT
 * below rejects the build instead, keeping the image truly position
 * independent (the bootloader picks the load address).
 */
ENTRY(_head)

SECTIONS
{
	. = 0;
	__image_start = .;

	.text : {
		KEEP(*(.text.head))
		*(.text .text.*)
		. = ALIGN(0x800);
		KEEP(*(.text.vectors))
	}

	.rodata : ALIGN(16) { *(.rodata .rodata.*) }
	.data : ALIGN(16) { *(.data .data.*) }
	.got : ALIGN(8) { *(.got .got.*) }
	.dynamic : { *(.dynamic) }
	.dynsym : { *(.dynsym) }
	.dynstr : { *(.dynstr) }
	.hash : { *(.hash) }
	.gnu.hash : { *(.gnu.hash) }
	.rela.dyn : { *(.rela.dyn) *(.rela.*) }

	. = ALIGN(16);
	__file_end = .;

	.bss (NOLOAD) : ALIGN(16) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(16);
		__stack_bottom = .;
		. += 0x10000;
		__stack_top = .;
		__bss_end = .;
	}

	__image_end = .;
	_image_size = __image_end - __image_start;

	/DISCARD/ : {
		*(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr)
		*(.interp) *(.gnu.version*)
	}
}

ASSERT(SIZEOF(.rela.dyn) == 0, "image needs relocations: an absolute address (pointer table, jump table) slipped in")
