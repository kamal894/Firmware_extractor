/* SPDX-License-Identifier: MIT */
#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(PLATFORM_QEMU)
#include "platform_qemu.h"
#else
#include "platform_caiman.h"
#endif

/* Build-time switches (set from the Makefile). */
#ifndef PROBE_UART
#define PROBE_UART 1		/* 0: never touch the UART */
#endif
#ifndef PROBE_PAINT
#define PROBE_PAINT 0		/* 1: paint colour bars into the found frame buffer */
#endif
#ifndef PROBE_FINISH
#define PROBE_FINISH 0		/* 0: reboot (warm if possible), 1: cold reboot, 2: hang */
#endif
#ifndef PROBE_FINISH_DELAY_MS
#define PROBE_FINISH_DELAY_MS 3000
#endif
#ifndef PROBE_WDT_KICK
#define PROBE_WDT_KICK PROBE_PAINT	/* keep the watchdog fed while waiting to reboot */
#endif
#ifndef PROBE_BLDR_LOG_MAX
#define PROBE_BLDR_LOG_MAX (512 * 1024)	/* bytes of bootloader log text to copy */
#endif
#ifndef PROBE_CRASHTEST
#define PROBE_CRASHTEST 0	/* 1: deliberately fault at the end (tests the handler) */
#endif
#ifndef PROBE_VERSION
#define PROBE_VERSION "dev"
#endif

#endif
