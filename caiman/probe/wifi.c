// SPDX-License-Identifier: MIT
/*
 * PROBE_WIFI=1: the Wi-Fi's PCIe link as the bootloader leaves it, read only (docs/wifi-plan.md). The BCM4390 is
 * on HSI2's PCIe root complex 1 (DT pcie@13120000): ELBI 0x13120000, SoC 0x13130000, UDBG 0x13150000, PHY
 * 0x13160000, SYSREG 0x13020000, DBI 0x13400000, PCS 0x13140000; its pins (PERST# gph3-0, CLKREQ# gph3-1,
 * WL_REG_ON gph3-4, DEV_WAKE gph3-5) in HSI2's pinctrl 0x13040000, whose bank layout is not known here, so the
 * first 0x200 bytes of it are shown. Every first read of a block is one that survives a block whose clock is off.
 */
#include "probe.h"

#if PROBE_WIFI >= 3
#define ELBI	((void *)0x13120000ULL)
#define SOC	((void *)0x13130000ULL)
#define PCS	((void *)0x13140000ULL)
#define UDBG	((void *)0x13150000ULL)
#define PHY	((void *)0x13160000ULL)
#define DBI	0x13400000ULL
#define PIN	0x13040000ULL		/* HSI2 pinctrl bank +0x00 = gph3 (run wifi1): CON +0, DAT +4 */
#define EP_TYPE	1			/* DT ep-device-type 1 = EP_BCM_WIFI (cmp #1 in the CAL module's phy_config) */
#define EP_BCM_WIFI 1
#define EP_QC_WIFI 4
#define EP_SAMSUNG_WIFI 5
#define PCIE_SLV_PEND_SEL_NAK 0x03D8

static inline u32 readl(void *a) { return *(volatile u32 *)a; }
static inline void writel(u32 v, void *a) { *(volatile u32 *)a = v; }

/* Linux's check_exynos_pcie_reg_status: up to 1000 x 10 us for the bit */
static u32 reg_wait(void *base, int off, int bit, int *cnt)
{
	u32 st = 0;
	int i;

	for (i = 0; i < 1000; i++) {
		delay_us(10);
		st = readl((char *)base + off) & (1u << bit);
		if (st)
			break;
	}
	*cnt = i;
	return st;
}

#include "wifi_cal.inc"

static void pin(u32 n, u32 func, int out)
{
	u32 con = rd32(PIN), dat = rd32(PIN + 4);

	if (out >= 0)
		wr32(PIN + 4, out ? dat | 1u << n : dat & ~(1u << n));
	wr32(PIN, (con & ~(0xfu << (4 * n))) | func << (4 * n));
}

#if PROBE_WIFI >= 8
extern const u8 wifi_blob_start[] __attribute__((visibility("hidden")));	/* (hidden: no GOT entry to relocate) */

/* dongle RAM through BAR2 (DHD's BAR1): its window at config 0x84 picks the 4 MiB the address is in */
static u64 bar2_at(u64 cfg, u64 bar2, u32 a)
{
	static u32 win = 0xffffffff;

	if ((a & ~0x3fffffu) != win) {
		win = a & ~0x3fffffu;
		wr32(cfg + 0x84, win);
		(void)rd32(cfg + 0x84);
	}
	return bar2 + (a & 0x3fffff);
}

static void dram_w(u64 cfg, u64 bar2, u32 a, u32 v) { wr32(bar2_at(cfg, bar2, a), v); }
static u32 dram_r(u64 cfg, u64 bar2, u32 a) { return rd32(bar2_at(cfg, bar2, a)); }

/*
 * PROBE_WIFI=8: the firmware in and the CA7 let go, as Google's bcmdhd does for this chip (CA7 already up and
 * halted, wifi7: DMP 0x21): RAM 0x2a0000 + 0x320000; last word 0 (dhdpcie_init_shared_addr); firmware at the base;
 * vars just below the last word and the length token in it (dhdpcie_bus_write_vars); 128 random bytes as the RNG
 * rTLV below the vars (dhdpcie_wrt_rnd, type 0xfeedc0de, no length checksum); the firmware's first word at address 0
 * (dhdpcie_bus_write_flops); CA7 DMP control bit 5 (CPUHALT) cleared; then the last word is polled for the
 * shared-area pointer. Bus master stays off: the firmware cannot write to the phone's RAM.
 */
static void wifi_boot(u64 cfg, u64 bar0, u64 bar2)
{
	const u32 *hdr = (const u32 *)wifi_blob_start;
	const u32 base = 0x2a0000, size = 0x320000, last = base + size - 4;
	u32 fwlen, varlen, v, i, token, addr0, dmp, p = 0;
	const u8 *fw, *vars;
	u64 t0;

	if (hdr[0] != 0x31424657) {	/* 'WFB1' */
		pr("wifi: no firmware blob in the image\n");
		return;
	}
	fwlen = hdr[1];
	varlen = hdr[2];
	fw = wifi_blob_start + 16;
	vars = fw + ((fwlen + 3) & ~3u);
	pr("wifi: firmware %u bytes, vars %u bytes\n", fwlen, varlen);

	dram_w(cfg, bar2, last, 0);
	t0 = timer_us();
	for (i = 0; i < fwlen; i += 4) {
		u32 w = fw[i] | (i + 1 < fwlen ? fw[i + 1] << 8 : 0) | (i + 2 < fwlen ? fw[i + 2] << 16 : 0) |
			(i + 3 < fwlen ? (u32)fw[i + 3] << 24 : 0);
		dram_w(cfg, bar2, base + i, w);
	}
	pr("wifi: firmware written in %llu ms; first words %08x %08x, read back %08x %08x\n", (timer_us() - t0) / 1000,
	   *(const u32 *)fw, *(const u32 *)(fw + 4), dram_r(cfg, bar2, base), dram_r(cfg, bar2, base + 4));
	v = last - varlen;
	for (i = 0; i < varlen; i += 4)
		dram_w(cfg, bar2, v + i, *(const u32 *)(vars + i));
	token = ((~(varlen / 4) & 0xffff) << 16) | ((varlen / 4) & 0xffff);
	dram_w(cfg, bar2, last, token);
	/* RNG rTLV: 128 bytes, then length 0x80, then type, ending at the vars */
	for (i = 0; i < 128; i += 4) {
		u64 r = timer_ticks();

		r ^= r >> 13;
		r *= 0x9e3779b97f4a7c15ULL;
		dram_w(cfg, bar2, v - 8 - 128 + i, (u32)(r >> 17) ^ (i * 0x01010101u));
	}
	dram_w(cfg, bar2, v - 8, 128);
	dram_w(cfg, bar2, v - 4, 0xfeedc0de);
	pr("wifi: vars at 0x%x, token 0x%08x at 0x%x, RNG rTLV at 0x%x\n", v, token, last, v - 8 - 128);

	addr0 = dram_r(cfg, bar2, 0);
	pr("wifi: address 0 reads 0x%08x%s\n", addr0, addr0 == 0xffffffff ? " (secured: the firmware cannot be started so)" : "");
	if (addr0 == 0xffffffff)
		return;
	dram_w(cfg, bar2, 0, *(const u32 *)fw);

	wr32(cfg + 0x80, 0x18006000);
	(void)rd32(cfg + 0x80);
	dmp = rd32(bar0 + 0x248);
	wr32(bar0 + 0x248, dmp & ~0x20u);
	pr("wifi: CA7 DMP control 0x%x -> 0x%x: the firmware runs\n", dmp, rd32(bar0 + 0x248));
	wr32(cfg + 0x80, 0x18000000);
	(void)rd32(cfg + 0x80);

	t0 = timer_us();
	while (timer_us() - t0 < 5000000) {
		p = dram_r(cfg, bar2, last);
		if (p != token && p != 0 && p != 0xffffffff)
			break;
		delay_us(1000);
	}
	pr("wifi: after %llu ms the last word is 0x%08x%s\n", (timer_us() - t0) / 1000, p,
	   p == token ? " (still the token: the firmware did not come up)" : "");
	if (p >= base && p < last) {
		u32 w[8];

		for (i = 0; i < 8; i++)
			w[i] = dram_r(cfg, bar2, p + 4 * i);
		pr("wifi: shared area at 0x%x: flags 0x%08x (version %u), trap 0x%x, assert 0x%x 0x%x line %u, console 0x%x, "
		   "msgtrace 0x%x\n", p, w[0], w[0] & 0xff, w[1], w[2], w[3], w[4], w[5], w[6]);
	}
	/* the ARM halted again before the chip goes off */
	wr32(cfg + 0x80, 0x18006000);
	(void)rd32(cfg + 0x80);
	wr32(bar0 + 0x248, rd32(bar0 + 0x248) | 0x20u);
	wr32(cfg + 0x80, 0x18000000);
	(void)rd32(cfg + 0x80);
	wr32(cfg + 0x84, 0);
}
#endif

static u32 elbi(u32 off) { return rd32(0x13120000ULL + off); }
static void elbi_w(u32 off, u32 v) { wr32(0x13120000ULL + off, v); }

/*
 * PROBE_WIFI=3: the link, as Linux's exynos_pcie_rc_poweron + establish_link do for channel 1 (pcie-exynos-rc.c),
 * up to LTSSM in L0. Not yet: dw_pcie_setup_rc, iATU, the endpoint's config space.
 */
static void wifi_link(void)
{
	u32 st, last = 0xff, seen[16], ns = 0, i;

	/* the chip on (DHD's wlan power-on: WL_REG_ON high), PERST# held low, CLKREQ# to its function */
	pin(0, 1, 0);		/* PERST#: output, low */
	pin(1, 2, -1);		/* CLKREQ# */
	pin(4, 1, 1);		/* WL_REG_ON: output, high */
	pr("wifi: WL_REG_ON high, PERST# low: gph3 CON 0x%08x DAT 0x%08x\n", rd32(PIN), rd32(PIN + 4));
	delay_us(200000);

	wr32(0x13130000ULL + 0x4000, 0x15);			/* exynos_soc_write(0x15, 0x4000) */
	cal_pwrdn_clear();
	cal_phy_config();
	psci_call(0x82000504u, 0x15460000ULL + (0x3ec8 | 0xc000), 1, 10);	/* rmw_priv_reg(.., 0x400, 0x400) */
	wr32(0x13150000ULL + 0xc800, 0x421);
	wr32(0x13160000ULL + 0x32c, 0);
	wr32(DBI + 0x890, 0x12000);				/* EQ off */
	pin(0, 1, 1);						/* PERST# high */
	delay_us(15000);					/* DT perst-delay-us 0x3a98 */
	elbi_w(0x3bc, elbi(0x3bc) | 0x1 | 0x10);		/* APP_REQ_EXIT_L1_MODE, L1_REQ_NAK_CONTROL_MASTER */
	elbi_w(0x3a0, 0x2);					/* PCIE_LINKDOWN_RST_MANUAL */
	elbi_w(0x74, 0x1);					/* APP_XFER_PENDING */
	elbi_w(0x3a8, elbi(0x3a8) & ~0xfffu);			/* Q-channel: no gating (ip_ver 0x987500) */
	elbi_w(0x474, 0x1);					/* NACK on AXI pending */
	elbi_w(0x1078, 0x1);					/* DBI L1 exit disable */
	pr("wifi: before LTSSM: state 0x%x, DBI link status 0x%08x\n", elbi(0x2c8) & 0x3f, rd32(DBI + 0x80));
	elbi_w(0x54, 0x1);					/* LTSSM enable */
	for (i = 0; i < 4800; i++) {
		st = elbi(0x2c8) & 0x3f;
		if (st != last && ns < 16)
			seen[ns++] = st;
		last = st;
		if (st == 0x11)
			break;
		delay_us(10);
	}
	pr("wifi: LTSSM %s after %u us; states seen:", st == 0x11 ? "L0 (link up)" : "NOT in L0", i * 10);
	for (i = 0; i < ns; i++)
		pr(" 0x%x", seen[i]);
	pr("\n");
	pr("wifi: DBI link control/status 0x%08x (speed %u, width x%u)\n", rd32(DBI + 0x80),
	   (rd32(DBI + 0x80) >> 16) & 0xf, (rd32(DBI + 0x80) >> 20) & 0x3f);
#if PROBE_WIFI >= 4
	/*
	 * PROBE_WIFI=4: the endpoint's config space through outbound iATU region 0 as CFG0 to bus 1 device 0
	 * (exynos_pcie_rc_prog_viewport_cfg0; unrolled iATU at DBI+0x300000, pcie-exynos-rc.h), bridge buses 0/1/1.
	 */
	if (st == 0x11) {
		int ok;
		u32 k, w[16];

		wr32(DBI + 0x18, 0x00010100);			/* primary 0, secondary 1, subordinate 1 */
		wr32(DBI + 0x4, rd32(DBI + 0x4) | 0x6);		/* memory + bus master on the bridge */
		wr32(DBI + 0x300008, 0x60ffe000);
		wr32(DBI + 0x30000c, 0);
		wr32(DBI + 0x300010, 0x60ffe000 + 0x2000 - 1);
		wr32(DBI + 0x300014, 1u << 24);			/* bus 1, device 0, function 0 */
		wr32(DBI + 0x300018, 0);
		wr32(DBI + 0x300000, 0x4);			/* CFG0 */
		wr32(DBI + 0x300004, 0x80000000u);		/* enable */
		(void)rd32(DBI + 0x300004);
		for (k = 0; k < 16; k++) {
			ok = 1;
			w[k] = safe_rd32(0x60ffe000ULL + 4 * k, &ok);
			if (!ok)
				w[k] = 0xdeadbeef;
		}
		pr("wifi: endpoint 01:00.0: vendor 0x%04x device 0x%04x, class 0x%06x rev %u, header %u\n", w[0] & 0xffff,
		   w[0] >> 16, w[2] >> 8, w[2] & 0xff, (w[3] >> 16) & 0x7f);
		for (k = 0; k < 16; k += 4)
			pr("wifi: endpoint +0x%02x: %08x %08x %08x %08x\n", 4 * k, w[k], w[k + 1], w[k + 2], w[k + 3]);
#if PROBE_WIFI >= 5
		/*
		 * PROBE_WIFI=5: the BARs sized and placed in the RC's memory window (DT ranges: 0x60000000, 0xff0000),
		 * outbound iATU region 1 as MEM there, memory decoding on (no bus master: the chip cannot write to RAM),
		 * then the chip's own registers through BAR0 (Broadcom: the window at config 0x80 picks the backplane
		 * address; ChipCommon at 0x18000000 holds the chip ID at +0).
		 */
		{
			const u64 cfg = 0x60ffe000ULL;
			u64 at = 0x60000000ULL, bar[3] = { 0 }, size[3] = { 0 };
			u32 b;

			for (b = 0; b < 3; b++) {
				u32 off = 0x10 + 8 * b, lo, hi;

				wr32(cfg + off, 0xffffffffu);
				wr32(cfg + off + 4, 0xffffffffu);
				lo = rd32(cfg + off) & ~0xfu;
				hi = rd32(cfg + off + 4);
				size[b] = ~(((u64)hi << 32) | lo) + 1;
				if (size[b] == 0 || size[b] > 0x800000) {
					pr("wifi: BAR%u: size 0x%llx: not placed\n", 2 * b, size[b]);
					wr32(cfg + off, 0);
					wr32(cfg + off + 4, 0);
					continue;
				}
				at = (at + size[b] - 1) & ~(size[b] - 1);
				bar[b] = at;
				wr32(cfg + off, (u32)at);
				wr32(cfg + off + 4, 0);
				at += size[b];
				pr("wifi: BAR%u: 0x%llx bytes at 0x%llx\n", 2 * b, size[b], bar[b]);
			}
			wr32(DBI + 0x20, (0x60fu << 20) | (0x600u << 4));	/* bridge memory window 0x60000000-0x60ffffff */
			wr32(DBI + 0x300200, 0x0);				/* region 1: MEM */
			wr32(DBI + 0x300208, 0x60000000);
			wr32(DBI + 0x30020c, 0);
			wr32(DBI + 0x300210, 0x60feffff);
			wr32(DBI + 0x300214, 0x60000000);
			wr32(DBI + 0x300218, 0);
			wr32(DBI + 0x300204, 0x80000000u);
			(void)rd32(DBI + 0x300204);
			wr32(cfg + 0x4, (rd32(cfg + 0x4) & ~0x7u) | 0x2);	/* memory space only */
			pr("wifi: endpoint command 0x%08x, BAR0 window (config 0x80) 0x%08x\n", rd32(cfg + 0x4), rd32(cfg + 0x80));
			if (bar[0]) {
				u32 win = rd32(cfg + 0x80);

				for (k = 0; k < 8; k++) {
					ok = 1;
					w[k] = safe_rd32(bar[0] + 4 * k, &ok);
				}
				pr("wifi: BAR0 at the chip's window 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n", win, w[0], w[1],
				   w[2], w[3], w[4], w[5], w[6], w[7]);
				wr32(cfg + 0x80, 0x18000000);		/* the window to ChipCommon */
				(void)rd32(cfg + 0x80);
				for (k = 0; k < 8; k++) {
					ok = 1;
					w[k] = safe_rd32(bar[0] + 4 * k, &ok);
					if (!ok)
						w[k] = 0xdeadbeef;
				}
				pr("wifi: BAR0 +0x00: %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5],
				   w[6], w[7]);
				pr("wifi: ChipCommon chip ID 0x%04x, rev %u, package %u, %u cores\n", w[0] & 0xffff, (w[0] >> 16) & 0xf,
				   (w[0] >> 20) & 0xf, (w[0] >> 24) & 0xf);
#if PROBE_WIFI >= 6
				/*
				 * PROBE_WIFI=6: the chip's core table (EROM: ChipCommon +0xfc, EromPtrOffset) read through the BAR0
				 * window; then a pattern written into dongle RAM near its top and read back through BAR2 (DHD's
				 * "BAR1": window at config 0x84, 4 MiB; RAM 0x2a0000..0x5c0000), the last word left alone.
				 */
				{
					u32 erom = rd32(bar[0] + 0xfc), e, pat[4] = { 0x12345678, 0x9abcdef0, 0x0f1e2d3c, 0xa5a55a5a };

					pr("wifi: EROM pointer (ChipCommon +0xfc) 0x%08x; BAR1 window (config 0x84) 0x%08x\n", erom,
					   rd32(cfg + 0x84));
					if (erom >= 0x18000000 && erom < 0x19000000) {
						/* the OOBR page first (EROM2 base & ~0x1fff: core info of the OOBR cores), then EROM1 */
						wr32(cfg + 0x80, erom & ~0x1fffu);
						(void)rd32(cfg + 0x80);
						for (e = 0; e < 0x1000; e += 32) {
							for (k = 0; k < 8; k++) {
								ok = 1;
								w[k] = safe_rd32(bar[0] + e + 4 * k, &ok);
							}
							pr("wifi: OOBR +0x%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", e, w[0], w[1], w[2],
							   w[3], w[4], w[5], w[6], w[7]);
						}
						wr32(cfg + 0x80, erom & ~0xfffu);
						(void)rd32(cfg + 0x80);
						for (e = 0; e < 0x1000; e += 32) {	/* the whole 4 KiB page from 0x18007000 */
							for (k = 0; k < 8; k++) {
								ok = 1;
								w[k] = safe_rd32(bar[0] + e + 4 * k, &ok);
							}
							pr("wifi: EROM +0x%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", e, w[0], w[1], w[2],
							   w[3], w[4], w[5], w[6], w[7]);
						}
						wr32(cfg + 0x80, 0x18000000);
					}
#if PROBE_WIFI >= 8
					wifi_boot(cfg, bar[0], bar[1]);
#endif
#if PROBE_WIFI >= 7
					/*
					 * PROBE_WIFI=7, read only: CA7's and SYSMEM's DMP control (OOBR +0x240/+0x4c0, +8:
					 * nciutils.c DMP_DMPCTRL_REG_OFFSET; bit 5 CPUHALT, bit 0 clock enable, bit 1 force gated
					 * clock) and the next words, CA7's own registers, and their nodes' IDM reset words at +0x140,
					 * +0x144 (offsets inferred, docs/bcm4390-fw-download.md).
					 */
					{
						/* (addresses only: a table of string pointers would need relocations in this image) */
						static const u32 look[] = { 0x18006248, 0x180064c8, 0x18080000, 0x18088000, 0x18504140,
									    0x18516140, 0x18505140 };
						u32 l;

						for (l = 0; l < sizeof(look) / sizeof(look[0]); l++) {
							wr32(cfg + 0x80, look[l] & ~0xfffu);
							(void)rd32(cfg + 0x80);
							for (k = 0; k < 4; k++) {
								ok = 1;
								w[k] = safe_rd32(bar[0] + (look[l] & 0xfff) + 4 * k, &ok);
								if (!ok)
									w[k] = 0xdeadbeef;
							}
							/* (no table of names: the compiler makes one of pointers, which this image cannot have) */
							pr("wifi: core look 0x%08x: %08x %08x %08x %08x\n", look[l], w[0], w[1], w[2], w[3]);
						}
						wr32(cfg + 0x80, 0x18000000);
					}
#endif
					if (bar[1]) {
						u64 a = 0x5b0000, base;

						wr32(cfg + 0x84, 0x400000);
						(void)rd32(cfg + 0x84);
						base = bar[1] + (a - 0x400000);
						for (k = 0; k < 4; k++)
							w[k] = rd32(base + 4 * k);
						pr("wifi: dongle RAM 0x%llx before: %08x %08x %08x %08x; last word 0x5bfffc: %08x\n", a, w[0], w[1],
						   w[2], w[3], rd32(bar[1] + 0x1bfffc));
						for (k = 0; k < 4; k++)
							wr32(base + 4 * k, pat[k]);
						for (k = 0; k < 4; k++)
							w[k] = rd32(base + 4 * k);
						pr("wifi: dongle RAM 0x%llx after writing: %08x %08x %08x %08x (%s)\n", a, w[0], w[1], w[2], w[3],
						   w[0] == pat[0] && w[3] == pat[3] ? "the RAM takes writes" : "NOT as written");
						wr32(cfg + 0x84, 0);
					}
				}
#endif
			}
		}
#endif
	}
#endif
	/* leave the chip off again and the link down */
	elbi_w(0x54, 0);
	pin(0, 1, 0);
	pin(4, 1, 0);
}
#endif

static void dump(const char *name, u64 base, u32 n)
{
	int ok = 1;
	u32 i;

	safe_rd32_async(base, &ok);
	if (!ok) {
		pr("wifi: %s 0x%llx: does not answer (clock or power off)\n", name, base);
		return;
	}
	for (i = 0; i < n; i += 16) {
		u32 v[4], k;

		for (k = 0; k < 4; k++) {
			ok = 1;
			v[k] = safe_rd32(base + i + 4 * k, &ok);
			if (!ok)
				v[k] = 0xdeadbeef;
		}
		pr("wifi: %s +0x%03x: %08x %08x %08x %08x\n", name, i, v[0], v[1], v[2], v[3]);
	}
}

void wifi_probe(void)
{
	int ok = 1;
	u32 v;

	pr("wifi: PCIe RC1 (BCM4390) as the bootloader left it, read only\n");
	dump("pinctrl-hsi2", 0x13040000ULL, 0x200);
	v = safe_rd32(0x15460000ULL + 0x3ec8, &ok);
	pr("wifi: PMU +0x3ec8 (PCIe 1 PHY isolation): 0x%08x%s\n", v, ok ? "" : " (fault)");
	dump("sysreg-hsi2", 0x13020000ULL, 0x40);
	dump("elbi", 0x13120000ULL, 0x40);
	dump("soc", 0x13130000ULL, 0x20);
	dump("phy", 0x13160000ULL, 0x20);
	dump("dbi", 0x13400000ULL, 0x40);
#if PROBE_WIFI >= 2
	/*
	 * PROBE_WIFI=2: the PHY out of isolation, as Linux's exynos_pcie_phy_isolation(PCIE_PHY_BYPASS) at power-on:
	 * bit 0 of PMU+0x3ec8 (pcie-exynos-rc.h PCIE_PHY_CONTROL_MASK). EL3 owns PMU writes: the set-bit alias
	 * (offset | 0xc000) with the bit number, as ufs.c does for the UFS PHY.
	 */
	if (!(v & 1)) {
		s64 r = psci_call(0x82000504u, 0x15460000ULL + (0x3ec8 | 0xc000), 1, 0);

		ok = 1;
		pr("wifi: PCIe 1 PHY isolation bypass via SMC: %lld, PMU +0x3ec8 now 0x%08x\n", r,
		   safe_rd32(0x15460000ULL + 0x3ec8, &ok));
		delay_us(100);
	}
	dump("phy", 0x13160000ULL, 0x20);
	dump("udbg+c700", 0x13150000ULL + 0xc700, 0x10);
	dump("pcs", 0x13140000ULL, 0x20);
#endif
#if PROBE_WIFI >= 3
	wifi_link();
#endif
}
