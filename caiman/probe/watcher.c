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
					u32 b = (px >> 8) & 0xff, b2 = (px >> 16) & 0xff, b3 = px >> 24;

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
}

static void snapshot(u32 base, u32 seconds, int last)
{
	u32 len = xnu.log_len_pa ? rd32pa(xnu.log_len_pa) : 0;
	u64 space;

	log_rewind(base);
	pr("\n=== XNU after %u s%s: loader_log %u bytes ===\n", seconds, last ? " (last look)" : "",
	   len);
	space = log_space_left();
	if (space > 4096 && xnu.log_pa) {
		if (len > space - 4096)
			len = (u32)(space - 4096);
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
	pr("\n=== end of XNU snapshot ===\n");
	log_flush();
}

void watcher_main(void)
{
	u32 el = (u32)(read_sysreg(CurrentEL) >> 2) & 3, base, s;

	build_map(el);
	mmu_on((u64)(uptr)l1);
	alive = 1;
	civac(&alive, 8);

	log_uart_enable(0);	/* the UART is the kernel's now */
	pr("watcher: running on MPIDR 0x%llx at EL%u; XNU gets %u s\n", read_sysreg(mpidr_el1), el,
	   PROBE_XNU_SECONDS);
	log_flush();
	base = log_pos();

	for (s = 0; s < PROBE_XNU_SECONDS; s++) {
		int q;

		if (!(s % 2))
			snapshot(base, s, 0);
		for (q = 0; q < 4; q++) {
			wdt_kick();
			delay_us(250000);
		}
	}
	snapshot(base, s, 1);
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
	for (i = 0; i < 100 && !alive; i++)
		delay_us(10000);
	pr(alive ? ", running\n" : ", NOT seen: nothing will save XNU's log\n");
}
