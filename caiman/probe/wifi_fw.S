/* SPDX-License-Identifier: MIT */
/*
 * The BCM4390's firmware and NVRAM vars inside the image (WIFI_BLOB=path, from tools/wifi_blob.py; PROBE_WIFI >= 8).
 * Not committed: Broadcom's files from the phone's /vendor/firmware.
 */
	.section .rodata
	.balign	64
	.globl	wifi_blob_start
	.globl	wifi_blob_end
wifi_blob_start:
	.incbin	WIFI_BLOB_PATH
wifi_blob_end:
