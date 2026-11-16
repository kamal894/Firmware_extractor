/* SPDX-License-Identifier: MIT */
/*
 * The Pixel bootloader leaves the cluster-0 watchdog running when it starts
 * the kernel (first hardware run: "Reboot reason: APC Watchdog Early,
 * RST_STAT CLUSTER0_NONCPU_WDTRESET"); Linux's s3c2410_wdt driver takes it
 * over. Anything else we boot must kick or stop it.
 *
 * Samsung s3c2410-style registers (Linux drivers/watchdog/s3c2410_wdt.c);
 * zumapro.dtsi: watchdog_cl0@10060000, clock OSCCLK, timeout-sec 30.
 */
#include "probe.h"

#define WTCON		0x00
#define WTDAT		0x04
#define WTCNT		0x08
#define WTCON_RSTEN	(1u << 0)
#define WTCON_INTEN	(1u << 2)
#define WTCON_ENABLE	(1u << 5)

static int running;

void wdt_report(void)
{
	u64 base = PLAT_WDT_BASE, freq, div;
	u32 con, dat, cnt;
	int ok1, ok2, ok3;

	pr("\n--- watchdog ---\n");
	pr("wdt: reading cluster-0 watchdog @ 0x%llx\n", base);
	log_flush();
	con = safe_rd32(base + WTCON, &ok1);
	dat = safe_rd32(base + WTDAT, &ok2);
	cnt = safe_rd32(base + WTCNT, &ok3);
	if (!ok1 || !ok2 || !ok3) {
		pr("wdt: registers not readable\n");
		return;
	}
	running = (con & WTCON_ENABLE) && (con & WTCON_RSTEN);
	g.wdt_running = running;
	div = 16ULL << ((con >> 3) & 3);
	freq = PLAT_WDT_CLK / (((con >> 8) & 0xff) + 1) / div;
	pr("wdt: WTCON=0x%08x WTDAT=0x%08x WTCNT=0x%08x: %s, reset on expiry %s, irq %s\n",
	   con, dat, cnt, (con & WTCON_ENABLE) ? "RUNNING" : "stopped",
	   (con & WTCON_RSTEN) ? "ON" : "off", (con & WTCON_INTEN) ? "on" : "off");
	if (freq) {
		g.wdt_period_ms = (u32)((u64)dat * 1000 / freq);
		g.wdt_left_ms = (u32)((u64)cnt * 1000 / freq);
		pr("wdt: counts at %llu Hz (if the input clock is %llu Hz): period %u ms, %u ms left\n",
		   freq, PLAT_WDT_CLK, g.wdt_period_ms, g.wdt_left_ms);
	}
}

/* Reload the counter, as s3c2410wdt_keepalive() does. */
void wdt_kick(void)
{
	if (running)
		wr32(PLAT_WDT_BASE + WTCNT, rd32(PLAT_WDT_BASE + WTDAT));
}
