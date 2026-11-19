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

#ifdef WIFI_CLM_PATH
/* the CLM blob (bcmdhd_clm.blob_4390_b1, WIFI_CLM=path; PROBE_WIFI >= 11): regulatory data for "clmload" */
	.balign	64
	.globl	wifi_clm_start
	.globl	wifi_clm_end
wifi_clm_start:
	.incbin	WIFI_CLM_PATH
wifi_clm_end:
#endif

#ifdef WIFI_PSK_PATH
/* the network to join (WIFI_SSID=file, WIFI_PSK=file; PROBE_WIFI >= 12): the owner's own files, never committed;
 * the passphrase is never printed */
	.balign	8
	.globl	wifi_ssid_start
	.globl	wifi_ssid_end
wifi_ssid_start:
	.incbin	WIFI_SSID_PATH
wifi_ssid_end:
	.balign	8
	.globl	wifi_psk_start
	.globl	wifi_psk_end
wifi_psk_start:
	.incbin	WIFI_PSK_PATH
wifi_psk_end:
#endif
