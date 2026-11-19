/* SPDX-License-Identifier: MIT */
/*
 * The watcher: a second core that stays ours while XNU runs on the first.
 * (After hakios-mt6768's watcher_main, MIT, the HakiOS developers.)
 *
 * The phone was fastboot-booted, so after any reset the stock bootloader
 * starts Android, not us. Everything XNU says must already be in pstore by then.
 * Every second the watcher:
 *   - feeds the bootloader's watchdog (period ~60 s on caiman),
 *   - copies XNU's loader_log into the pstore console zone after our own log
 *     (re-written each time, so the newest snapshot is what survives).
 * After PROBE_XNU_SECONDS it takes a last snapshot and resets the phone.
 *
 * It runs with its own MMU on and DRAM mapped write-back inner-shareable, so its
 * reads see what XNU's core still holds in its caches.
 */
#include "probe.h"

#ifndef PROBE_XNU_SECONDS
#define PROBE_XNU_SECONDS 45
#endif

#define PSCI_CPU_ON_64		0xc4000003u
#define KVA_BASE		0xfffffff004000000ULL

u8 watcher_stack[0x4000] __attribute__((aligned(16)));	/* handoff.S: sp = its top */

static u64 l1[512] __attribute__((aligned(4096)));
static volatile u64 alive __attribute__((aligned(64)));
static volatile u64 go __attribute__((aligned(64)));	/* watcher_go(): XNU is about to start */

static void civac(const volatile void *p, u64 n)
{
	u64 a = (u64)(uptr)p & ~63ULL, e = (u64)(uptr)p + n;

	for (; a < e; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	dsb_sy();
}

static int is_dram(u64 base, u64 size)
{
	int i;

	for (i = 0; i < g.nmem; i++)
		if (overlaps(base, size, g.mem[i].base, g.mem[i].size))
			return 1;
	return 0;
}

static void build_map(u32 el)
{
	int i;

	for (i = 0; i < 512; i++) {
		u64 pa = (u64)i << 30;
		u64 d = pa | (1ULL << 10) | 1;			/* AF, block */

		if (el == 2)
			d |= 1ULL << 6;				/* AP[1]: RES1 in the EL2 regime */
		if (is_dram(pa, 1ULL << 30))
			d |= (3ULL << 8) | (1ULL << 2);		/* inner shareable, attr 1: normal WB */
		/* else attr 0: device, for MMIO and holes alike */
		l1[i] = d;
	}
	civac(l1, sizeof(l1));
}

static u64 rd64(u64 pa)
{
	return *(volatile u64 *)(uptr)pa;
}

static u32 rd32pa(u64 pa)
{
	return *(volatile u32 *)(uptr)pa;
}

/*
 * What is on the panel, as text: a 64 x 48 picture of the frame buffer (each cell the brightest of
 * 4 x 8 samples in it) and the DECON's frame counter. With the TE trigger unmasked the panel shows
 * whatever is in the buffer, so this is what a person looking at the phone would see.
 */
static void screen_report(void)
{
	static const char shade[5] = { ' ', '.', ':', '+', '#' };
	u32 cols = 64, rows = 48, r, c, i, j, lit = 0;

	if (!g.fb_addr || g.fb_bpp != 4 || !g.fb_width || !g.fb_height)
		return;
	if (g.decon_main)
		pr("screen: DECON FRAME_COUNT %u\n", rd32(g.decon_main + 0x4));
	pr("screen: %ux%u at 0x%llx, 64x48 cells (' ' dark .. '#' bright):\n", g.fb_width, g.fb_height,
	   g.fb_addr);
	for (r = 0; r < rows; r++) {
		char line[66];

		for (c = 0; c < cols; c++) {
			u32 best = 0;

			for (j = 0; j < 8; j++) {
				for (i = 0; i < 4; i++) {
					u32 x = (c * 4 + i) * g.fb_width / (cols * 4);
					u32 y = (r * 8 + j) * g.fb_height / (rows * 8);
					u32 px = *(volatile u32 *)(uptr)(g.fb_addr + (u64)y * g.fb_stride + x * 4ULL);
					/* B, G, R (BGRA8888); not byte 3: Darwin's window server sets alpha to
					 * 0xff everywhere, and every cell then looked bright */
					u32 b = px & 0xff, b2 = (px >> 8) & 0xff, b3 = (px >> 16) & 0xff;

					if (b2 > b)
						b = b2;
					if (b3 > b)
						b = b3;
					if (b > best)
						best = b;
				}
			}
			if (best > 0x20)
				lit++;
			line[c] = shade[best * 5 / 256];
		}
		line[cols] = '|';
		line[cols + 1] = 0;
		pr("|%s\n", line);
	}
	pr("screen: %u of %u cells lit\n", lit, cols * rows);
	/* a few pixels as they are, to tell the colour bytes from alpha */
	for (r = 0; r < 5; r++) {
		static const u32 fx[5] = { 1, 2, 1, 1, 3 }, fy[5] = { 1, 1, 2, 3, 3 };	/* quarters */
		u32 x = g.fb_width * fx[r] / 4, y = g.fb_height * fy[r] / 4;

		pr("screen: pixel (%u,%u) = 0x%08x\n", x, y,
		   *(volatile u32 *)(uptr)(g.fb_addr + (u64)y * g.fb_stride + x * 4ULL));
	}
}

#define DUMP_W		256
#define DUMP_H		571			/* 2856 * 256 / 1280 */
#define SCREEN_DUMP_ROOM	(256 * 1024)

/*
 * The panel's picture at 256 x 571, one byte a pixel (RGB 3-3-2), in base64 lines between
 * "=== screen dump" and "=== end of screen dump": tools/screen_dump.py makes a PNG of it.
 */
static void screen_dump(void)
{
	static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	u8 in[3];
	char line[80];
	u32 x, y, n = 0, k = 0;

	if (!g.fb_addr || g.fb_bpp != 4 || !g.fb_width || !g.fb_height || log_space_left() < SCREEN_DUMP_ROOM - 8192)
		return;
	pr("=== screen dump %ux%u rgb332 base64 ===\n", DUMP_W, DUMP_H);
	for (y = 0; y < DUMP_H; y++) {
		u32 sy = y * g.fb_height / DUMP_H;

		for (x = 0; x < DUMP_W; x++) {
			u32 sx = x * g.fb_width / DUMP_W;
			u32 px = *(volatile u32 *)(uptr)(g.fb_addr + (u64)sy * g.fb_stride + sx * 4ULL);
			u32 b = px & 0xff, gr = (px >> 8) & 0xff, r = (px >> 16) & 0xff;

			in[n++] = (u8)((r & 0xe0) | ((gr >> 3) & 0x1c) | (b >> 6));
			if (n == 3) {
				line[k++] = b64[in[0] >> 2];
				line[k++] = b64[((in[0] & 3) << 4) | (in[1] >> 4)];
				line[k++] = b64[((in[1] & 15) << 2) | (in[2] >> 6)];
				line[k++] = b64[in[2] & 63];
				n = 0;
				if (k == 76) {
					line[k] = 0;
					pr("%s\n", line);
					k = 0;
				}
			}
		}
		if (!(y % 64))
			wdt_kick();
	}
	if (n) {			/* 256 * 571 = 146176 = 3 * 48725 + 1: one byte left */
		in[n] = 0;
		line[k++] = b64[in[0] >> 2];
		line[k++] = b64[(in[0] & 3) << 4];
		line[k++] = '=';
		line[k++] = '=';
	}
	line[k] = 0;
	if (k)
		pr("%s\n", line);
	pr("=== end of screen dump ===\n");
}

static void snapshot(u32 base, u32 seconds, int last)
{
	u32 len = xnu.log_len_pa ? rd32pa(xnu.log_len_pa) : 0;
	u64 space;

	log_rewind(base);
	pr("\n=== XNU after %u s%s: loader_log %u bytes ===\n", seconds, last ? " (last look)" : "",
	   len);
	space = log_space_left();
	/* the last look keeps room for the screen picture (screen_dump(): ~200 KB of base64) */
	if (space > (last ? SCREEN_DUMP_ROOM : 4096) + 4096 && xnu.log_pa) {
		u64 keep = last ? SCREEN_DUMP_ROOM : 4096;

		if (len > space - keep)
			len = (u32)(space - keep);
		log_write_raw((const volatile u8 *)(uptr)xnu.log_pa, len);
	}
	if (xnu.panic_str_va) {
		u64 s = rd64(xnu.panic_str_va - KVA_BASE + PLAT_XNU_PHYS);

		if (s >= KVA_BASE && s < KVA_BASE + PLAT_XNU_SIZE) {
			const volatile char *c = (const volatile char *)(uptr)(s - KVA_BASE + PLAT_XNU_PHYS);
			int i;

			pr("\n=== panic: ");
			for (i = 0; i < 512 && c[i]; i++)
				log_putc(c[i]);
			pr(" ===\n");
		}
	}
	screen_report();
	if (last)
		screen_dump();
	pr("\n=== end of XNU snapshot ===\n");
	log_flush();
}

#ifdef PROBE_SHOTS
/*
 * PROBE_SHOTS=N: from XNU's N-th second, a picture of the panel every second, 50 of them, into pstore's pmsg zone
 * (after the console zone; Android shows it as /sys/fs/pstore/pmsg-ramoops-0), which a snapshot's rewrite of the
 * console does not touch: what the screen does while it moves (the kernel's touch_demo swipes), not only at the end.
 * Each: "SHOT", the second (u32), then 128 x 286 bytes of RGB 3-3-2. tools/shots.py makes PNGs of them. (Not with
 * XNU's own pmsg writes, boot-arg pmsg=1.)
 */
#define SHOT_W		128
#define SHOT_H		286
#define SHOT_COUNT	50

static void shot(u32 second)
{
	static u32 taken;
	volatile u32 *hdr = (volatile u32 *)(uptr)(PLAT_RAMOOPS_BASE + PLAT_RAMOOPS_CONSOLE);
	volatile u8 *data = (volatile u8 *)(hdr + 3);
	u32 at = taken * (8 + SHOT_W * SHOT_H), x, y;

	if (!g.fb_addr || g.fb_bpp != 4 || !g.fb_width || !g.fb_height || taken >= SHOT_COUNT ||
	    12 + at + 8 + SHOT_W * SHOT_H > PLAT_RAMOOPS_PMSG)
		return;
	if (taken == 0) {
		hdr[0] = 0x43474244u;		/* "DBGC" */
		hdr[1] = hdr[2] = 0;
	}
	data[at] = 'S', data[at + 1] = 'H', data[at + 2] = 'O', data[at + 3] = 'T';
	*(volatile u32 *)(data + at + 4) = second;
	for (y = 0; y < SHOT_H; y++) {
		u32 sy = y * g.fb_height / SHOT_H;

		/* (this core's cached copy of the row from a picture before, out first: the panel's memory, not it) */
		civac((const volatile void *)(uptr)(g.fb_addr + (u64)sy * g.fb_stride), g.fb_width * 4ULL);
		for (x = 0; x < SHOT_W; x++) {
			u32 px = *(volatile u32 *)(uptr)(g.fb_addr + (u64)sy * g.fb_stride + (x * g.fb_width / SHOT_W) * 4ULL);
			u32 b = px & 0xff, gr = (px >> 8) & 0xff, r = (px >> 16) & 0xff;

			data[at + 8 + y * SHOT_W + x] = (u8)((r & 0xe0) | ((gr >> 3) & 0x1c) | (b >> 6));
		}
	}
	taken++;
	hdr[1] = hdr[2] = taken * (8 + SHOT_W * SHOT_H);
	civac(hdr, 12 + hdr[2]);
}
#endif

/* The loader's core, right before it jumps: the watcher's clock starts now. */
void watcher_go(void)
{
	go = 1;
	civac(&go, 8);
}

void watcher_main(void)
{
	u32 el = (u32)(read_sysreg(CurrentEL) >> 2) & 3, base, s;

	alive = 1;			/* stages, for watcher_start() to report where a core stopped */
	civac(&alive, 8);
	build_map(el);
	alive = 2;
	civac(&alive, 8);
	mmu_on((u64)(uptr)l1);
	alive = 3;
	civac(&alive, 8);

	/*
	 * Started before the loader reads xnusys (~80 s): until XNU starts, only keep the watchdog fed and
	 * leave the log to the loader's core. go is written there with the MMU off: invalidate before reading.
	 */
	for (;;) {
		civac(&go, 8);
		if (go)
			break;
		wdt_kick();
		delay_us(100000);
	}

	log_uart_enable(0);	/* the UART is the kernel's now */
	if (PROBE_XNU_SECONDS)
		pr("watcher: running on MPIDR 0x%llx at EL%u; XNU gets %u s\n", read_sysreg(mpidr_el1), el,
		   PROBE_XNU_SECONDS);
	else
		pr("watcher: running on MPIDR 0x%llx at EL%u; XNU runs with no end set\n", read_sysreg(mpidr_el1), el);
	log_flush();
	base = log_pos();

	/* PROBE_XNU_SECONDS=0: no end (the phone on its own: XNU runs until it stops itself or the battery does) */
	u32 held = 0;

	for (s = 0; PROBE_XNU_SECONDS == 0 || s < PROBE_XNU_SECONDS; s++) {
		int q;

		if (!(s % 2))
			snapshot(base, s, 0);
#if defined(PROBE_SHOTS) && defined(PROBE_SHOTS_FAST)
		/* PROBE_SHOTS_FAST=1: two bursts of 25 pictures 40 ms apart, at second N (the touch_demo swipes) and N + 50
		 * (the screen at rest): a flicker shorter than a second shows up here. The "second" field holds
		 * second * 1000 + the burst's milliseconds. */
		if (s == PROBE_SHOTS || s == PROBE_SHOTS + 50) {
			u32 i;

			for (i = 0; i < 25; i++) {
				shot(s * 1000 + i * 40);
				wdt_kick();
				delay_us(40000);
			}
		}
#elif defined(PROBE_SHOTS)
		if (s >= PROBE_SHOTS)
			shot(s);
#endif
#if PROBE_BATTERY
		if (!(s % 5))
			battery_poll(!(s % 60));	/* the kernel's page every 5 s; a line in the log every minute */
#endif
		for (q = 0; q < 4; q++) {
			wdt_kick();
			delay_us(250000);
			/* both volume keys held 3 s: out to fastboot (the way back to Android on a phone that boots this) */
			if (!(rd32(0x154d0000ULL + 0x80 + 4) & 2) && !(rd32(0x154d0000ULL + 0xa0 + 4) & 4)) {
				if (++held == 12) {
					pr("watcher: both volume keys held 3 s: to fastboot\n");
					goto out;
				}
			} else {
				held = 0;
			}
		}
	}
out:
	snapshot(base, s, 1);
#if defined(PROBE_TO_FASTBOOT) || PROBE_XNU_SECONDS == 0
	/*
	 * PROBE_TO_FASTBOOT=1: the reset lands in the bootloader's fastboot, as Android's pixel-reboot.ko asks for it
	 * ("bootloader"): 0xfc in PMU+0x810 (DT pixel-reboot: syscon PMU, reboot-cmd-offset 0x810), written through EL3
	 * as set_priv_reg() in exynos-pd_el3.ko does: SMC 0x82000504 (address, 1 = write, value). So a phone that
	 * boots this from its own boot partition can be taken back to Android from the Mac (fastboot set_active).
	 */
#if !defined(PROBE_TO_FASTBOOT)
	if (held >= 12)	/* (with no end set: only when the keys asked for it) */
#endif
	{
		s64 r = psci_call(0x82000504, 0x15460810, 1, 0xfc);

		pr("watcher: reboot mode bootloader (0xfc) asked of EL3: %lld\n", r);
	}
#endif
	pr("watcher: %u s are up; resetting the phone\n", s);
	log_flush();
	if (g.psci_reset2 >= 0)
		psci_call(PSCI_SYSTEM_RESET2_64, 0, 0, 0);
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);
	for (;;)
		__asm__ volatile("wfe");
}

void watcher_start(void)
{
	s64 r;
	int i;

	alive = 0;
	civac(&alive, 8);
	r = psci_call(PSCI_CPU_ON_64, PLAT_WATCH_MPIDR, (u64)(uptr)watcher_entry, 0);
	pr("watcher: PSCI CPU_ON(0x%llx) = %lld", (u64)PLAT_WATCH_MPIDR, r);
	for (i = 0; i < 100 && alive != 3; i++)
		delay_us(10000);
	if (alive == 3)
		pr(", running\n");
	else
		pr(", NOT running (stage %llu: 0 never started, 1 entered, 2 map built, MMU not on): nothing will"
		   " save XNU's log\n", alive);
}
