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
