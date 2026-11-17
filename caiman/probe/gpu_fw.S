/* SPDX-License-Identifier: MIT */
/*
 * The Mali firmware inside the image (MALI_FW=path, with PROBE_GPU >= 3): for the loader, whose
 * load base + 1 MiB holds XNU's payload rather than the GPUBLOB of tools/pack_aoc_payload.py. Not
 * committed: the file is the phone's own /vendor/firmware/mali_csffw-r54p3.bin.
 */
	.section .rodata
	.balign	64
	.globl	mali_fw_start
	.globl	mali_fw_end
mali_fw_start:
	.incbin	MALI_FW_PATH
mali_fw_end:
