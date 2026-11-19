/* SPDX-License-Identifier: MIT */
/*
 * Read-only look at the UFS host controller as the bootloader leaves it (PROBE_UFS=1).
 *
 * caiman: Samsung/Exynos UFS ("samsung,exynos-ufs", zuma-ufs.dtsi): UFSHCI standard registers
 * at 0x13200000, vendor registers at 0x13201100, UniPro at 0x13280000. The bootloader read the
 * boot LUNs over it ("UFS device found ... HS gear 4"); whether the link is still up after its
 * "shutdown complete" decides how much a loader has to do to read the iOS system image.
 *
 * Nothing is written: registers are read (each read guarded, with a breadcrumb first, since a
 * powered-down block can hang the bus), and if a transfer request list is still programmed, the
 * bootloader's own descriptors are dumped from RAM to show the exact layout this controller takes.
 * UFSHCI register and descriptor layouts: JESD223 (UFSHCI 3.0); Linux include/ufs/ufshci.h.
 */
#include "probe.h"

#if PROBE_UFS

#define CAP		0x00	/* NUTRS [4:0] (+1), NUTMRS [18:16] (+1), 64AS bit 24 */
#define VER		0x08
#define HCPID		0x10
#define HCMID		0x14
#define AHIT		0x18
#define IS		0x20
#define IE		0x24
#define HCS		0x30	/* DP 0, UTRLRDY 1, UTMRLRDY 2, UCRDY 3, UPMCRS [10:8] */
#define HCE		0x34
#define UECPA		0x38
#define UECDL		0x3c
#define UTRLBA		0x50
#define UTRLBAU		0x54
#define UTRLDBR		0x58
#define UTRLCLR		0x5c
#define UTRLRSR		0x60
#define UTRLCNR		0x64
#define UTMRLBA		0x70
#define UTMRLBAU	0x74
#define UTMRLRSR	0x80
#define UICCMD		0x90
#define UCMDARG1	0x94
#define UCMDARG2	0x98
#define UCMDARG3	0x9c
#define IS_UPMS		(1u << 4)	/* power mode status */
#define IS_UHXS		(1u << 5)	/* hibern8 exit status */
#define IS_UHES		(1u << 6)	/* hibern8 enter status */
#define IS_UCCS		(1u << 10)	/* UIC command completion */
#define UIC_HIBER_ENTER	0x17
#define UIC_HIBER_EXIT	0x18

/*
 * Tensor (zuma) vendor registers, relative to PLAT_UFS_VS, and the PHY's PMA block: Google's
 * drivers/ufs/ufs-vs-regs.h and zuma/ufs-cal.h (google-modules/soc/gs, reference/google/ufs/).
 */
#define VS_CLKSTOP_CTRL		0xb0	/* clock stop requests */
#define  CLK_STOP_ALL		0x17	/* REFCLKOUT 4, REFCLK 2, UNIPRO_MCLK 1, UNIPRO_PCLK 0 */
#define VS_FORCE_HCS		0xb4	/* which clocks the hardware may stop */
#define  MPHY_APBCLK_STOP_EN	(1u << 10)
#define  UNIPRO_MCLK_STOP_EN	(1u << 5)
#define UNIP_PA_AVAILTXDATALANES	0x3080
#define UNIP_PA_CONNECTEDTXDATALANES	0x3184
#define UNIP_PA_CONNECTEDRXDATALANES	0x3204
#define PMA_LANE		0x800	/* per-lane (TRSV) register stride */

static int rd(u64 a, u32 *v)
{
	int ok;

	*v = safe_rd32(a, &ok);
	return ok;
}

static void dump_words(const char *tag, u64 pa, u32 n)
{
	u32 i;

	pr("ufs:   %s @ 0x%llx:", tag, pa);
	for (i = 0; i < n; i++) {
		if (i && !(i % 8))
			pr("\nufs:     +0x%02x:", i * 4);
		pr(" %08x", *(volatile u32 *)(uptr)(pa + 4ULL * i));
	}
	pr("\n");
}

static void dump_utrl(u64 utrl, u32 nutrs, u32 doorbell)
{
	u32 s;

	pr("ufs: transfer request list (the bootloader's) at 0x%llx, %u slots, doorbell 0x%08x\n", utrl,
	   nutrs, doorbell);
	for (s = 0; s < nutrs && s < 4; s++) {
		volatile u32 *d = (volatile u32 *)(uptr)(utrl + 32ULL * s);
		u64 ucd = ((u64)d[5] << 32) | (d[4] & ~0x7fu);
		u32 rul = d[6] & 0xffff, ruo = d[6] >> 16, prdtl = d[7] & 0xffff, prdto = d[7] >> 16;

		dump_words("UTRD", utrl + 32ULL * s, 8);
		pr("ufs:   slot %u: OCS 0x%02x, UCD 0x%llx, response @+%u len %u, PRDT @+%u x%u (units as the"
		   " controller counts them)\n", s, d[2] & 0xff, ucd, ruo, rul, prdto, prdtl);
		if (!ucd || !addr_in_dram(ucd, 4096))
			continue;
		dump_words("command UPIU", ucd, 8);
		/* the response and PRDT offsets are in dwords by the standard, in bytes with Exynos's quirk */
		if (ruo && ruo * 4 < 4096)
			dump_words("response @dw", ucd + ruo * 4ULL, 8);
		if (prdto && prdto * 4 < 4096)
			dump_words("PRDT @dw", ucd + prdto * 4ULL, 8);
		if (prdto && prdto < 4096 && prdto * 4 >= 4096)
			dump_words("PRDT @byte", ucd + prdto, 8);
	}
}

#if PROBE_UFS >= 2
/*
 * Run 10 settled it: the bootloader does not just park the link in Hibern8. Its last query
 * (slot 0 above: READ ATTRIBUTE bCurrentPowerMode) answered 0x33, UFS-PowerDown: "shutdown
 * complete" powers the device down, and no Hibern8 exit can bring that back (power mode status 5,
 * PWR_FATAL_ERROR, even with the clocks and PHY prepared as Google's driver does).
 *
 * So this is a cold start, the way Linux's ufshcd core plus Google's Tensor driver
 * (google-modules/soc/gs drivers/ufs/ufs-exynos.c, zuma CAL) do it at every boot:
 *   board:  PHY isolation released (PMU), device VCC on (GPIO gpp0-1, the "ufs-vcc" regulator);
 *   host:   HCE off, vendor software reset and set-up, device reset pin pulsed, HCE on;
 *   link:   CAL pre_link table, DME_LINKSTARTUP, CAL post_link table (PWM-G1, both lanes);
 *   lists:  our own transfer and task-management lists, run;
 *   device: NOP OUT/IN, fDeviceInit set and waited for.
 * After that, READ(10)s. The data path stays PWM-G1 (slow, a few MB/s at most): enough for the
 * GPT and the LP metadata. Changing to HS gears is the next step. Everything is printed.
 */
#include "ufs_cal_zuma.h"

#define UTRIACR		0x4c
#define UTMRLDBR	0x78
#define IS_UTRCS	(1u << 0)
#define IS_UE		(1u << 2)
#define IS_ULSS		(1u << 8)
#define UIC_LINK_STARTUP 0x16

#define VS_TXPRDT		0x00
#define VS_RXPRDT		0x04
#define VS_1US_TO_CNT		0x0c
#define VS_UTRL_NEXUS		0x40
#define VS_UTMRL_NEXUS		0x44
#define VS_SW_RST		0x50	/* UNIPRO_SW_RST 1, LINK_SW_RST 0 */
#define VS_DATA_REORDER		0x60
#define VS_AXIDMA_BURST		0x6c
#define VS_GPIO_OUT		0x70	/* bit 0: the device's reset pin (RST_n) */
#define VS_ERR_EN_DL		0x7c
#define VS_ERR_EN_N		0x80
#define VS_ERR_EN_T		0x84
#define VS_V2P1_CTRL		0x8c
#define  IA_TICK_SEL		(1u << 16)
#define  CLK_STOP_CTRL_EN_ALL	0xde0
#define  HCI_CORECLK_STOP_EN	(1u << 4)
#define VS_IOP_ACG_DISABLE	0x100

#define UNIP_AXI_AUX		0x040	/* PCS lane select for the next write */
#define UNIP_DBG_PRD		0x044
#define UNIP_PA_AVAILRXDATALANES 0x3100
#define UNIP_PA_MAXRXHSGEAR	0x321c
#define UNIP_DBG_SUITE1		0x39a8
#define UNIP_DBG_SUITE2		0x39b4

#define UFS_BLOCK	4096u
#define UFS_MAX_BLOCKS	64u
/* one READ(10) of an image (read_extents): 1 MiB, a PRDT entry a block; fewer requests, each one's overhead paid less often */
#ifndef UFS_XFER_BLOCKS
#define UFS_XFER_BLOCKS	256u
#endif

/* our own lists and one command descriptor (slot 0), in the image's BSS */
#define UTRL_BYTES	(32 * 32)
#define UTMRL_BYTES	(32 * 8)
#define UCD_BYTES	(96 + 16 * UFS_XFER_BLOCKS)
static u8 q_utrl[UTRL_BYTES] __attribute__((aligned(1024)));
static u8 q_utmrl[UTMRL_BYTES] __attribute__((aligned(1024)));
static u8 q_ucd[UCD_BYTES] __attribute__((aligned(128)));
/* data lands below 4 GiB: Google's driver gives this controller a 32-bit DMA mask */
static u8 iobuf[UFS_MAX_BLOCKS * UFS_BLOCK] __attribute__((aligned(4096)));
static u32 mclk_hz, unipro_prd, lanes = 2;
static int hs_ok;			/* the link is in HS gear */
static int sw_flushed;			/* dc_all_sw() done with the D-cache off: reads need no maintenance of their buffer */
static u64 busy_ticks;			/* spent waiting for the device (request()), for the load's timing line */
static u32 super_lun_g = 0xff;		/* where ufs_read_test() found super */
static u32 bootb_lun = 0xff;		/* where ufs_read_test() found boot_b: the only place ufs_write() may write */
static u64 bootb_lba, bootb_blocks;
static u64 super_lba_g, super_blocks_g;

static void put_be32(u8 *p, u32 v)
{
	p[0] = (u8)(v >> 24);
	p[1] = (u8)(v >> 16);
	p[2] = (u8)(v >> 8);
	p[3] = (u8)v;
}

static void put_le32(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static u64 le64(const volatile u8 *p)
{
	u64 v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static u32 le32(const volatile u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/*
 * Clean and invalidate [p, p+n) to the point of coherency (VA = PA, MMU off; 64-byte lines,
 * CTR_EL0 DminLine 4). The controller is IO-coherent (sysreg IOCC 0x13): its DMA snoops the CPU
 * caches, while our D-cache-off accesses go straight to RAM. Lines the bootloader left over our
 * buffers would hand the controller stale descriptors and hide its answers from us.
 */
static void dc_civac(const void *p, u64 n)
{
	u64 a = (u64)(uptr)p & ~63ull, end = (u64)(uptr)p + n;

	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	dsb_sy();
}

/*
 * Every level of this processor's data caches (CLIDR: L1, L2 and the cluster's L3) cleaned and invalidated by set
 * and way, as firmware does before it runs with the D-cache off: no line the bootloader left dirty is written back
 * later over what the device writes. With the D-cache off nothing makes a new line, so once is enough.
 */
static void dc_all_sw(void)
{
	u64 clidr = read_sysreg(clidr_el1);
	u32 level;

	for (level = 0; level < 7; level++) {
		u32 ctype = (clidr >> (level * 3)) & 7, ways, sets, way, set, wshift, lshift;
		u64 ccsidr;

		if (ctype < 2)			/* no data cache here (or no cache at all from here on) */
			continue;
		__asm__ volatile("msr csselr_el1, %0; isb" :: "r"((u64)level << 1));
		ccsidr = read_sysreg(ccsidr_el1);
		lshift = (u32)(ccsidr & 7) + 4;
		ways = (u32)((ccsidr >> 3) & 0x3ff) + 1;
		sets = (u32)((ccsidr >> 13) & 0x7fff) + 1;
		wshift = ways > 1 ? (u32)__builtin_clz(ways - 1) : 0;
		for (way = 0; way < ways; way++)
			for (set = 0; set < sets; set++)
				__asm__ volatile("dc cisw, %0" :: "r"(((u64)way << wshift) | ((u64)set << lshift) |
								     (level << 1)) : "memory");
	}
	dsb_sy();
	isb();
}

static int dcache_on(void)
{
	u64 sctlr = (read_sysreg(CurrentEL) >> 2 & 3) == 2 ? read_sysreg(sctlr_el2) : read_sysreg(sctlr_el1);

	return (sctlr & 4) != 0;
}

static u32 rd_or(u64 a, u32 dflt)
{
	u32 v;

	return rd(a, &v) ? v : dflt;
}

static int wait_is(u32 bits, u32 ms)
{
	u64 start = timer_ticks(), limit = timer_freq() / 1000 * ms;

	while (!(rd32(PLAT_UFS_HCI + IS) & bits))
		if (timer_ticks() - start > limit)
			return 0;
	return 1;
}

static int wait_reg(u64 a, u32 mask, u32 want, u32 ms)
{
	u64 start = timer_ticks(), limit = timer_freq() / 1000 * ms;

	while ((rd32(a) & mask) != want)
		if (timer_ticks() - start > limit)
			return 0;
	return 1;
}

/* ---- the CAL table interpreter (zuma/ufs-cal-if.c ufs_cal_config_uic, PWM tables only) ---- */

static void pcs_write(u32 lane_sel, u32 off, u32 v)
{
	const u64 unipro = PLAT_UFS_UNIPRO;

	wr32(unipro + UNIP_AXI_AUX, 0x0f000000u | lane_sel);
	wr32(unipro + off, v);
	wr32(unipro + UNIP_AXI_AUX, 0x0f000000u);
}

static void pcs_write_lr(u32 lane_sel, u32 off, u32 ticks)
{
	pcs_write(lane_sel, off, (ticks >> 16) & 0xff);
	pcs_write(lane_sel, off + 4, (ticks >> 8) & 0xff);
	pcs_write(lane_sel, off + 8, ticks & 0xff);
}

static int cal_apply(const char *name, const struct cal_cfg *c)
{
	const u64 vs = PLAT_UFS_VS, unipro = PLAT_UFS_UNIPRO, pma = PLAT_UFS_PHY;
	u32 rnd = (1000000000u + mclk_hz / 2) / mclk_hz, n = 0, bad = 0, l, v;
	u64 rx_lr = (u64)mclk_hz * 1000 / 1000000, tx_lr = (u64)mclk_hz * 3200 / 1000000;

	/* ufs_call_cal(): PHY APB and UniPro clocks forced on around a table */
	wr32(vs + VS_FORCE_HCS, rd32(vs + VS_FORCE_HCS) & ~(UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN));
	dsb_sy();
	for (; c->lyr != CAL_END; c++, n++) {
		switch (c->lyr) {
		case CAL_PCS_COMN:
		case CAL_UNIPRO:
			wr32(unipro + c->addr, c->val);
			break;
		case CAL_UNIPRO_PRD:
			wr32(unipro + c->addr, unipro_prd);
			break;
		case CAL_UNIPRO_ADAPT:
			v = rd32(unipro + c->addr);
			if (v & 0x80) {
				if ((v & 0x7f) < 2)
					wr32(unipro + c->addr, 0x82);
			} else if ((v + 1) % 4) {
				while ((v + 1) % 4)
					v++;
				wr32(unipro + c->addr, v);
			}
			break;
		case CAL_PMA_COMN:
			wr32(pma + c->addr, c->val);
			break;
		default:
			for (l = 0; l < lanes; l++) {
				switch (c->lyr) {
				case CAL_PCS_RX:
					pcs_write(4 + l, c->addr, c->val);
					break;
				case CAL_PCS_TX:
					pcs_write(l, c->addr, c->val);
					break;
				case CAL_PCS_RX_RND:
					pcs_write(4 + l, c->addr, rnd);
					break;
				case CAL_PCS_TX_RND:
					pcs_write(l, c->addr, rnd);
					break;
				case CAL_PCS_RX_LR:
					pcs_write_lr(4 + l, c->addr, (u32)rx_lr);
					break;
				case CAL_PCS_TX_LR:
					pcs_write_lr(l, c->addr, (u32)tx_lr);
					break;
				case CAL_PMA_TRSV:
					wr32(pma + l * PMA_LANE + c->addr, c->val);
					break;
				case CAL_EMB_CAL_WAIT: {
					u32 i;

					for (i = 0; i < 100; i++) {
						delay_us(40);
						v = rd32(pma + l * PMA_LANE + c->addr);
						if ((v & c->val) == c->val)
							break;
					}
					pr("ufs:   %s: lane %u PHY calibration %s (0x%03x = 0x%02x after %u polls)\n",
					   name, l, i < 100 ? "done" : "NOT done", c->addr, v, i < 100 ? i + 1 : 100);
					bad += i >= 100;
					break;
				}
				}
			}
		}
	}
	wr32(vs + VS_FORCE_HCS, rd32(vs + VS_FORCE_HCS) | MPHY_APBCLK_STOP_EN);
	dsb_sy();
	pr("ufs:   %s: %u entries applied (mclk %u Hz, period %u ns, UniPro period %u, line reset %llu/%llu"
	   " ticks)\n", name, n, mclk_hz, rnd, unipro_prd, rx_lr, tx_lr);
	return bad ? -1 : 0;
}

/* ---- host and link ---- */

static int uic_cmd(u32 cmd, u32 arg1, u32 arg2, u32 arg3, u32 ms, u32 *result)
{
	const u64 hci = PLAT_UFS_HCI;

	if (!wait_reg(hci + HCS, 8, 8, 100)) {
		pr("ufs:   UIC command 0x%02x: interface not ready (HCS 0x%08x)\n", cmd, rd32(hci + HCS));
		return -1;
	}
	wr32(hci + IS, IS_UCCS);
	wr32(hci + UCMDARG1, arg1);
	wr32(hci + UCMDARG2, arg2);
	wr32(hci + UCMDARG3, arg3);
	wr32(hci + UICCMD, cmd);
	if (!wait_is(IS_UCCS, ms)) {
		pr("ufs:   UIC command 0x%02x: no completion in %u ms (IS 0x%08x)\n", cmd, ms, rd32(hci + IS));
		return -2;
	}
	*result = rd32(hci + UCMDARG2) & 0xff;
	wr32(hci + IS, IS_UCCS);
	return 0;
}

/* ufshcd_hba_enable() with exynos_ufs_hce_enable_notify() around it */
static int hba_enable(u32 cnt_1us)
{
	const u64 hci = PLAT_UFS_HCI, vs = PLAT_UFS_VS, unipro = PLAT_UFS_UNIPRO;

	wr32(hci + HCE, 0);
	if (!wait_reg(hci + HCE, 1, 0, 10))
		pr("ufs:   HCE did not read 0 after disabling\n");
	/* PRE_CHANGE: exynos_ufs_init_host() */
	wr32(vs + VS_SW_RST, 3);
	if (!wait_reg(vs + VS_SW_RST, 3, 0, 10))
		pr("ufs:   host software reset did not finish (SW_RST 0x%08x); going on\n", rd32(vs + VS_SW_RST));
	wr32(vs + VS_FORCE_HCS, rd32(vs + VS_FORCE_HCS) & ~HCI_CORECLK_STOP_EN);
	wr32(vs + VS_V2P1_CTRL, rd32(vs + VS_V2P1_CTRL) | IA_TICK_SEL);
	wr32(vs + VS_1US_TO_CNT, cnt_1us & 0x3ff);
	wr32(vs + VS_DATA_REORDER, 0xa);
	wr32(vs + VS_TXPRDT, (1u << 31) | 12);		/* PRDT prefetch, 4 KiB per entry */
	wr32(vs + VS_RXPRDT, 12);
	wr32(vs + VS_UTRL_NEXUS, 0xffffffffu);
	wr32(vs + VS_UTMRL_NEXUS, 0xffffffffu);
	wr32(vs + VS_AXIDMA_BURST, (1u << 31) | (3u << 27) | 3u);
	wr32(vs + VS_IOP_ACG_DISABLE, rd32(vs + VS_IOP_ACG_DISABLE) & ~1u);
	wr32(unipro + UNIP_DBG_SUITE1, 0x90913c1c);
	wr32(unipro + UNIP_DBG_SUITE2, 0xe01c115f);
	/* exynos_ufs_dev_hw_reset() */
	wr32(vs + VS_GPIO_OUT, 0);
	delay_us(5);
	wr32(vs + VS_GPIO_OUT, 1);
	delay_us(2000);
	dsb_sy();
	wr32(hci + HCE, 1);
	if (!wait_reg(hci + HCE, 1, 1, 50)) {
		pr("ufs:   HCE did not read 1 after enabling\n");
		return -1;
	}
	/* POST_CHANGE */
	wr32(vs + VS_FORCE_HCS, rd32(vs + VS_FORCE_HCS) | CLK_STOP_CTRL_EN_ALL);
	wr32(vs + VS_CLKSTOP_CTRL, rd32(vs + VS_CLKSTOP_CTRL) & ~CLK_STOP_ALL);
	dsb_sy();
	pr("ufs:   host enabled: HCE 0x%08x HCS 0x%08x FORCE_HCS 0x%08x CLKSTOP 0x%08x, lanes tx %u rx %u\n",
	   rd32(hci + HCE), rd32(hci + HCS), rd32(vs + VS_FORCE_HCS), rd32(vs + VS_CLKSTOP_CTRL),
	   rd32(unipro + UNIP_PA_AVAILTXDATALANES), rd32(unipro + UNIP_PA_AVAILRXDATALANES));
	return 0;
}

/* The UIC error code registers (reading clears them), and IS.UE. */
static void uic_errors(const char *when)
{
	const u64 hci = PLAT_UFS_HCI;
	u32 is = rd32(hci + IS);

	pr("ufs:   UIC errors %s: IS 0x%08x PA 0x%08x DL 0x%08x N 0x%08x T 0x%08x DME 0x%08x\n", when, is,
	   rd32(hci + 0x38), rd32(hci + 0x3c), rd32(hci + 0x40), rd32(hci + 0x44), rd32(hci + 0x48));
	wr32(hci + IS, is & IS_UE);
}

static int link_startup(void)
{
	const u64 hci = PLAT_UFS_HCI, vs = PLAT_UFS_VS, unipro = PLAT_UFS_UNIPRO;
	u32 res = 0xff;
	int r;

	wr32(vs + VS_ERR_EN_DL, (1u << 31) | 0x2020);
	wr32(vs + VS_ERR_EN_N, (1u << 31) | 0x7);
	wr32(vs + VS_ERR_EN_T, (1u << 31) | 0x17);
	wr32(unipro + UNIP_DBG_SUITE1, 0x90913c1c);
	wr32(unipro + UNIP_DBG_SUITE2, 0xe01c115f);
	cal_apply("pre_link", cal_pre_link);
	log_flush();
	r = uic_cmd(UIC_LINK_STARTUP, 0, 0, 0, 1000, &res);
	pr("ufs:   DME_LINKSTARTUP: %s, result %u (0 = success), IS 0x%08x HCS 0x%08x (device %s)\n",
	   r ? "no answer" : "answered", res, rd32(hci + IS), rd32(hci + HCS),
	   (rd32(hci + HCS) & 1) ? "present" : "NOT present");
	if (r || res || !(rd32(hci + HCS) & 1)) {
		pr("ufs:   UIC errors: PA 0x%08x DL 0x%08x N 0x%08x T 0x%08x DME 0x%08x\n", rd32(hci + 0x38),
		   rd32(hci + 0x3c), rd32(hci + 0x40), rd32(hci + 0x44), rd32(hci + 0x48));
		return -1;
	}
	pr("ufs:   link up: connected lanes tx %u rx %u, max HS gear %u\n",
	   rd32(unipro + UNIP_PA_CONNECTEDTXDATALANES), rd32(unipro + UNIP_PA_CONNECTEDRXDATALANES),
	   rd32(unipro + UNIP_PA_MAXRXHSGEAR));
	cal_apply("post_link", cal_post_link);
	/* PA 0x80000010 here is the generic PA error, ufshcd's LINERESET indication: expected */
	uic_errors("after link start-up");
	return 0;
}

static int lists_run(void)
{
	const u64 hci = PLAT_UFS_HCI;
	u64 a = (u64)(uptr)q_utrl, b = (u64)(uptr)q_utmrl;

	memset(q_utrl, 0, UTRL_BYTES);
	memset(q_utmrl, 0, UTMRL_BYTES);
	wr32(hci + IE, 0);			/* polled */
	wr32(hci + UTRLBA, (u32)a);
	wr32(hci + UTRLBAU, (u32)(a >> 32));
	wr32(hci + UTMRLBA, (u32)b);
	wr32(hci + UTMRLBAU, (u32)(b >> 32));
	dsb_sy();
	if ((rd32(hci + HCS) & 0xe) != 0xe) {
		pr("ufs:   lists not ready (HCS 0x%08x)\n", rd32(hci + HCS));
		return -1;
	}
	wr32(hci + UTMRLRSR, 1);
	wr32(hci + UTRLRSR, 1);
	pr("ufs:   lists at 0x%llx / 0x%llx running (UTRLRSR %u UTMRLRSR %u)\n", a, b, rd32(hci + UTRLRSR),
	   rd32(hci + UTMRLRSR));
	return 0;
}

/*
 * One request through slot 0: ucd[0..31] holds the request UPIU, the response lands at ucd[32..95],
 * data (reads only) through the PRDT at ucd[96..]. dd: 0 none, 2 device to host. 0 on success.
 */
static int request(u32 dd, u32 nblocks, u32 ms)
{
	const u64 hci = PLAT_UFS_HCI;
	volatile u32 *utrd = (volatile u32 *)(uptr)q_utrl;
	u64 ucd_pa = (u64)(uptr)q_ucd, start;

	if (rd32(hci + UTRLDBR) & 1)
		return -2;
	utrd[0] = 0x11000000u | (dd << 25);	/* CT = UFS storage, interrupt, data direction */
	utrd[1] = 0;
	utrd[2] = 0x0f;				/* OCS: invalid until the controller writes it */
	utrd[3] = 0;
	utrd[4] = (u32)ucd_pa;
	utrd[5] = (u32)(ucd_pa >> 32);
	utrd[6] = (8u << 16) | 16u;		/* response UPIU at dword 8, 16 dwords */
	utrd[7] = (24u << 16) | nblocks;	/* PRDT at dword 24 */
	/*
	 * exynos_ufs_set_nexus_t_xfer_req(): the slot's nexus bit says what answer to expect, 1 for a
	 * SCSI COMMAND (RESPONSE UPIU), 0 for NOP OUT and QUERY. With it left at 1 the controller
	 * rejected the NOP IN (IS UTPES, HCS UTPEC 2) and never completed the request.
	 */
	if (q_ucd[0] == 0x01)
		wr32(PLAT_UFS_VS + VS_UTRL_NEXUS, rd32(PLAT_UFS_VS + VS_UTRL_NEXUS) | 1u);
	else
		wr32(PLAT_UFS_VS + VS_UTRL_NEXUS, rd32(PLAT_UFS_VS + VS_UTRL_NEXUS) & ~1u);
	dc_civac(q_utrl, 32);
	dc_civac(q_ucd, UCD_BYTES);
	wr32(hci + IS, IS_UTRCS);
	wr32(hci + UTRLDBR, 1);
	start = timer_ticks();
	while (rd32(hci + UTRLDBR) & 1) {
		if (timer_ticks() - start > timer_freq() / 1000 * ms) {
			wr32(hci + UTRLCLR, ~1u);	/* give slot 0 up */
			dc_civac(q_utrl, 32);
			dc_civac(q_ucd, UCD_BYTES);
			return -3;
		}
	}
	busy_ticks += timer_ticks() - start;
	dc_civac(q_utrl, 32);
	dc_civac(q_ucd, UCD_BYTES);
	return (utrd[2] & 0xff) ? -4 : 0;
}

static void upiu_header(u8 type, u8 flags, u8 lun)
{
	memset(q_ucd, 0, 96);
	q_ucd[0] = type;
	q_ucd[1] = flags;
	q_ucd[2] = lun;
	q_ucd[3] = 0;		/* task tag = slot, as Linux does: the controller rejected tag 1 in slot 0 (UTPES, HCS TTAGUTPE 1) */
}

static int nop_out(void)
{
	int i, r = -1;

	for (i = 0; i < 5 && r; i++) {
		upiu_header(0x00, 0, 0);	/* NOP OUT */
		r = request(0, 0, 50);
		if (!r && q_ucd[32] != 0x20)
			r = -5;
	}
	pr("ufs:   NOP OUT: %s after %d tries (answer 0x%02x, OCS 0x%02x)\n", r ? "FAILED" : "NOP IN", i,
	   q_ucd[32], ((volatile u32 *)(uptr)q_utrl)[2] & 0xff);
	if (r) {
		const u64 hci = PLAT_UFS_HCI;

		/* -3: doorbell never cleared; -4: cleared with OCS != 0; IS bit 16/17: fatal errors */
		pr("ufs:   last try %d (-3 timeout, -4 OCS, -5 wrong answer): IS 0x%08x UTRLDBR 0x%08x HCS 0x%08x\n",
		   r, rd32(hci + IS), rd32(hci + UTRLDBR), rd32(hci + HCS));
		pr("ufs:   UIC errors: PA 0x%08x DL 0x%08x N 0x%08x T 0x%08x DME 0x%08x\n", rd32(hci + 0x38),
		   rd32(hci + 0x3c), rd32(hci + 0x40), rd32(hci + 0x44), rd32(hci + 0x48));
		dump_words("UTRD", (u64)(uptr)q_utrl, 8);
		dump_words("UCD", (u64)(uptr)q_ucd, 24);
	}
	return r;
}

/* Query: flag fDeviceInit (IDN 1). fn 0x81 write / 0x01 read, op 6 SET FLAG / 5 READ FLAG */
static int query_flag(u8 fn, u8 op, u8 idn, u32 *val)
{
	int r;

	upiu_header(0x16, 0, 0);
	q_ucd[5] = fn;
	q_ucd[12] = op;
	q_ucd[13] = idn;
	r = request(0, 0, 100);
	if (r)
		return r;
	if (q_ucd[32] != 0x36 || q_ucd[32 + 6])
		return -6;
	*val = q_ucd[32 + 23] & 1;
	return 0;
}

static int device_init(void)
{
	u64 start;
	u32 v = 1;
	int r;

	r = query_flag(0x81, 0x06, 0x01, &v);
	pr("ufs:   SET FLAG fDeviceInit: %s (%d)\n", r ? "FAILED" : "accepted", r);
	if (r)
		return r;
	start = timer_ticks();
	do {
		r = query_flag(0x01, 0x05, 0x01, &v);
		if (timer_ticks() - start > timer_freq() * 2)
			break;
	} while (!r && v);
	pr("ufs:   fDeviceInit reads %u after %llu ms (%s)\n", v,
	   (timer_ticks() - start) * 1000 / timer_freq(), !r && !v ? "device ready" : "NOT ready");
	return !r && !v ? 0 : -1;
}

/* READ(10) of nblocks (<= 64) 4 KiB blocks into dst. Retries a UNIT ATTENTION (first command after reset). */
static int ufs_read(u32 lun, u32 lba, u32 nblocks, u64 dst)
{
	u32 i, len = nblocks * UFS_BLOCK;
	int r = -1, tries;

	if (!nblocks || nblocks > UFS_XFER_BLOCKS)
		return -1;
	for (tries = 0; tries < 3; tries++) {
		upiu_header(0x01, 0x40, (u8)lun);	/* COMMAND, R */
		put_be32(q_ucd + 12, len);
		q_ucd[16] = 0x28;				/* READ(10) */
		put_be32(q_ucd + 18, lba);
		q_ucd[23] = (u8)(nblocks >> 8);
		q_ucd[24] = (u8)nblocks;
		for (i = 0; i < nblocks; i++) {
			u8 *e = q_ucd + 96 + 16 * i;
			u64 a = dst + (u64)i * UFS_BLOCK;

			put_le32(e, (u32)a);
			put_le32(e + 4, (u32)(a >> 32));
			put_le32(e + 8, 0);
			put_le32(e + 12, UFS_BLOCK - 1);
		}
		/*
		 * Before: no dirty line of the bootloader's may be written back over what the device writes. After: only
		 * when this processor's D-cache is on (it is not in the loader: the reads that follow go to memory).
		 */
		if (!sw_flushed)
			dc_civac((const void *)(uptr)dst, len);
		r = request(2, nblocks, 2000);
		if (dcache_on())
			dc_civac((const void *)(uptr)dst, len);
		if (r)
			return r;
		if (q_ucd[32] == 0x21 && !q_ucd[32 + 5] && !q_ucd[32 + 7])
			return 0;
		pr("ufs:   read lun %u lba %u x%u: response UPIU 0x%02x response 0x%02x status 0x%02x%s\n", lun,
		   lba, nblocks, q_ucd[32], q_ucd[32 + 5], q_ucd[32 + 7],
		   q_ucd[32 + 7] == 0x02 ? " (CHECK CONDITION: retrying)" : "");
		if (q_ucd[32 + 7] != 0x02)
			return -4;
	}
	return -4;
}

#if PROBE_UFS >= 2 && defined(PROBE_UFSLOG)
/*
 * WRITE(10) of nblocks (<= 256) 4 KiB blocks from src. Refused unless the whole range lies inside boot_b as the
 * GPT names it on LUN 0 (64 MiB, slot b: not bootable on this phone, its Android contents backed up on the SSD):
 * the log area, and nothing else, is ever written.
 */
static int ufs_write(u32 lun, u32 lba, u32 nblocks, u64 src)
{
	u32 i, len = nblocks * UFS_BLOCK;
	int r = -1, tries;

	if (!nblocks || nblocks > UFS_XFER_BLOCKS || bootb_lun != 0 || lun != bootb_lun || lba < bootb_lba ||
	    (u64)lba + nblocks > bootb_lba + bootb_blocks) {
		pr("ufs: write lun %u lba %u x%u REFUSED: outside boot_b\n", lun, lba, nblocks);
		return -7;
	}
	dc_civac((const void *)(uptr)src, len);	/* the controller reads memory: our data must be there */
	for (tries = 0; tries < 3; tries++) {
		upiu_header(0x01, 0x20, (u8)lun);	/* COMMAND, W */
		put_be32(q_ucd + 12, len);
		q_ucd[16] = 0x2a;				/* WRITE(10) */
		put_be32(q_ucd + 18, lba);
		q_ucd[23] = (u8)(nblocks >> 8);
		q_ucd[24] = (u8)nblocks;
		for (i = 0; i < nblocks; i++) {
			u8 *e = q_ucd + 96 + 16 * i;
			u64 a = src + (u64)i * UFS_BLOCK;

			put_le32(e, (u32)a);
			put_le32(e + 4, (u32)(a >> 32));
			put_le32(e + 8, 0);
			put_le32(e + 12, UFS_BLOCK - 1);
		}
		r = request(1, nblocks, 2000);
		if (r)
			return r;
		if (q_ucd[32] == 0x21 && !q_ucd[32 + 5] && !q_ucd[32 + 7])
			return 0;
		pr("ufs:   write lun %u lba %u x%u: response UPIU 0x%02x response 0x%02x status 0x%02x\n", lun, lba,
		   nblocks, q_ucd[32], q_ucd[32 + 5], q_ucd[32 + 7]);
		if (q_ucd[32 + 7] != 0x02)
			return -4;
	}
	return -4;
}

/*
 * The previous boot's log (loader + kernel, the ramoops console zone, Linux's persistent_ram_buffer: "DBGC",
 * start, size, data) copied aside before this boot's log reuses the zone, then saved into boot_b:
 *   block 0                superblock "HKLOGSB1": version 1, 31 slots, next slot, saves so far
 *   block 512 * (k + 1)    slot k: header "HKLOGSL1": save number, length, CRC-32, then the text from block +1
 * docs/self-logging.md, tools/ufslog.py reads it back.
 */
#define LOG_SLOTS	31u
#define LOG_SLOT_BLOCKS	512u
#define LOG_MAX		((LOG_SLOT_BLOCKS - 1) * UFS_BLOCK)

static u8 prev_log[LOG_MAX] __attribute__((aligned(4096)));
static u8 log_hdr[UFS_BLOCK] __attribute__((aligned(4096)));
static u32 prev_len;

void ufslog_snapshot(u64 zone_base, u64 zone_size)
{
	const volatile u32 *h = (const volatile u32 *)(uptr)zone_base;
	const volatile u8 *d = (const volatile u8 *)(uptr)(zone_base + 12);
	u64 cap = zone_size - 12;
	u32 start = h[1], size = h[2], n = 0, i;

	prev_len = 0;
	if (h[0] != 0x43474244u || size > cap || start > cap || !size)
		return;
	/* persistent_ram_save_old's order: data[start..size), then data[0..start) */
	for (i = start; i < size && n < LOG_MAX; i++)
		prev_log[n++] = d[i];
	for (i = 0; i < start && n < LOG_MAX; i++)
		prev_log[n++] = d[i];
	prev_len = n;
}

static u32 crc32(const u8 *p, u32 n)
{
	u32 c = 0xffffffffu, k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

static void ufslog_save(void)
{
	const u64 buf = (u64)(uptr)iobuf;
	const volatile u8 *b = (const volatile u8 *)(uptr)buf;
	u32 next = 0, saves = 0, slot, lba, blocks, off, i, crc;
	int r;

	pr("\n--- self-logging: the previous boot's log into boot_b ---\n");
	if (bootb_lun != 0 || bootb_blocks != 16384) {
		pr("ufslog: boot_b not found as 64 MiB on LUN 0 (lun %u, %llu blocks): nothing written\n", bootb_lun,
		   bootb_blocks);
		return;
	}
	if (!prev_len) {
		pr("ufslog: no previous log in RAM (cold boot or a reset that lost it): nothing to save\n");
		return;
	}
	if (ufs_read(0, (u32)bootb_lba, 1, buf)) {
		pr("ufslog: boot_b block 0 not read: nothing written\n");
		return;
	}
	if (!memcmp((const void *)(uptr)buf, "HKLOGSB1", 8)) {
		next = le32(b + 12);
		saves = le32(b + 16);
	} else if (!memcmp((const void *)(uptr)buf, "ANDROID!", 8) && le32(b + 12) == 124) {
		pr("ufslog: boot_b holds our old slot-b test image (ramdisk 124 bytes): starting the log area\n");
	} else {
		pr("ufslog: boot_b holds something that is not ours: NOT writing\n");
		return;
	}
	slot = next % LOG_SLOTS;
	lba = (u32)bootb_lba + LOG_SLOT_BLOCKS * (slot + 1);
	crc = crc32(prev_log, prev_len);
	memset(log_hdr, 0, sizeof(log_hdr));
	memcpy(log_hdr, "HKLOGSL1", 8);
	put_le32(log_hdr + 8, saves + 1);
	put_le32(log_hdr + 12, prev_len);
	put_le32(log_hdr + 16, crc);
	memcpy(log_hdr + 32, PROBE_VERSION, strlen(PROBE_VERSION) < 64 ? strlen(PROBE_VERSION) : 63);
	blocks = (prev_len + UFS_BLOCK - 1) / UFS_BLOCK;
	for (i = prev_len; i < blocks * UFS_BLOCK; i++)
		prev_log[i] = 0;
	r = 0;
	for (off = 0; off < blocks && !r; off += UFS_XFER_BLOCKS) {
		u32 n = blocks - off > UFS_XFER_BLOCKS ? UFS_XFER_BLOCKS : blocks - off;

		r = ufs_write(0, lba + 1 + off, n, (u64)(uptr)(prev_log + (u64)off * UFS_BLOCK));
	}
	if (!r)
		r = ufs_write(0, lba, 1, (u64)(uptr)log_hdr);
	if (r) {
		pr("ufslog: write failed (%d): superblock left as it was\n", r);
		return;
	}
	/* read the header back before the superblock points at it */
	if (ufs_read(0, lba, 1, buf) || memcmp((const void *)(uptr)buf, "HKLOGSL1", 8) || le32(b + 16) != crc) {
		pr("ufslog: read-back of slot %u differs: superblock left as it was\n", slot);
		return;
	}
	memset(log_hdr, 0, sizeof(log_hdr));
	memcpy(log_hdr, "HKLOGSB1", 8);
	put_le32(log_hdr + 8, 1);
	put_le32(log_hdr + 12, next + 1);
	put_le32(log_hdr + 16, saves + 1);
	put_le32(log_hdr + 20, LOG_SLOTS);
	r = ufs_write(0, (u32)bootb_lba, 1, (u64)(uptr)log_hdr);
	pr("ufslog: save %u: %u bytes (CRC-32 0x%08x) into slot %u at lba %u; superblock %s\n", saves + 1, prev_len,
	   crc, slot, lba, r ? "NOT updated" : "updated");
}
#endif

/* PHY isolation (PMU, Google's exynos_ufs_ctrl_phy_pwr) and the device's VCC (gpp0-1) */
static void board_power(void)
{
	u32 iso, con, dat, field;
	int ok;

	iso = safe_rd32(PLAT_UFS_PMU_ISO, &ok);
	pr("ufs: PHY isolation register (PMU) 0x%08x%s\n", iso,
	   !ok ? " (read aborted)" : (iso & 1) ? " (bypassed: PHY powered)" : " (ISOLATED)");
	if (ok && !(iso & 1)) {
		/* EL3 owns PMU writes: the atomic set-bit alias (offset | 0xc000), value = bit number */
		s64 r = psci_call(0x82000504u, PLAT_UFS_PMU_BASE + (PLAT_UFS_PMU_ISO_OFF | 0xc000), 1, 0);

		pr("ufs: PHY isolation bypass via SMC: returned %lld, register now 0x%08x\n", r,
		   rd_or(PLAT_UFS_PMU_ISO, 0xdead));
	}
	/*
	 * gph5 (zumapro-pinctrl.dtsi ufs_refclk_out, ufs_rst_n): both pins function 2, no pull. PWM
	 * gears run without the reference clock, HS gears need it at the device: if "shutdown
	 * complete" left these pins as plain GPIOs, the power mode change to HS fails.
	 */
	con = safe_rd32(PLAT_UFS_PIN_BANK, &ok);
	if (ok) {
		u32 pud = rd_or(PLAT_UFS_PIN_BANK + 8, 0);

		pr("ufs: refclk/RST_n pins (gph5): CON 0x%08x (functions %u, %u; want 2, 2) DAT 0x%08x PUD 0x%08x"
		   " DRV 0x%08x\n", con, con & 0xf, (con >> 4) & 0xf, rd_or(PLAT_UFS_PIN_BANK + 4, 0), pud,
		   rd_or(PLAT_UFS_PIN_BANK + 0xc, 0));
		if ((con & 0xff) != 0x22 || (pud & 0xff)) {
			wr32(PLAT_UFS_PIN_BANK, (con & ~0xffu) | 0x22);
			wr32(PLAT_UFS_PIN_BANK + 8, pud & ~0xffu);
			pr("ufs: refclk/RST_n pins set to function 2: CON 0x%08x PUD 0x%08x\n", rd32(PLAT_UFS_PIN_BANK),
			   rd32(PLAT_UFS_PIN_BANK + 8));
		}
	} else {
		pr("ufs: refclk/RST_n pins (gph5): read aborted\n");
	}
	con = safe_rd32(PLAT_UFS_VCC_BANK, &ok);
	dat = ok ? rd_or(PLAT_UFS_VCC_BANK + 4, 0) : 0;
	field = (con >> (4 * PLAT_UFS_VCC_PIN)) & 0xf;
	pr("ufs: device VCC (gpp0-%u): CON 0x%08x (function %u) DAT 0x%08x: %s\n", PLAT_UFS_VCC_PIN, con, field,
	   dat, !ok ? "not readable" : field != 1 ? "pin is not an output; left alone"
	   : (dat >> PLAT_UFS_VCC_PIN) & 1 ? "ON" : "OFF");
	if (ok && field == 1 && !((dat >> PLAT_UFS_VCC_PIN) & 1)) {
		wr32(PLAT_UFS_VCC_BANK + 4, dat | (1u << PLAT_UFS_VCC_PIN));
		delay_us(10000);
		pr("ufs: device VCC switched on: DAT 0x%08x\n", rd32(PLAT_UFS_VCC_BANK + 4));
	}
}

static int ufs_cold_start(void)
{
	const u64 vs = PLAT_UFS_VS;
	u32 cnt_1us = rd_or(vs + VS_1US_TO_CNT, 0) & 0x3ff;
	int tries;

	pr("\n--- ufs cold start (the bootloader left the device powered down) ---\n");
	unipro_prd = rd_or(PLAT_UFS_UNIPRO + UNIP_DBG_PRD, 0);
	if (cnt_1us < 50 || cnt_1us > 500)
		cnt_1us = 178;			/* the bootloader's value on caiman */
	mclk_hz = cnt_1us * 1000000u + 500000u;
	if (unipro_prd < 30 || unipro_prd > 300)
		unipro_prd = (u32)(16000000000ULL / mclk_hz);
	pr("ufs: bootloader's timing: 1us = %u UniPro clocks, UniPro period value %u; IOCC 0x%08x"
	   " GPIO_OUT 0x%08x\n", cnt_1us, unipro_prd, rd_or(PLAT_UFS_IOCC, 0xdead), rd_or(vs + VS_GPIO_OUT, 0xdead));
	/* before anything writes them: no stale lines may be written back over our descriptors later */
	dc_civac(q_utrl, UTRL_BYTES);
	dc_civac(q_utmrl, UTMRL_BYTES);
	dc_civac(q_ucd, UCD_BYTES);
	dc_civac(iobuf, sizeof(iobuf));
	board_power();
	log_flush();
	for (tries = 1; tries <= 3; tries++) {
		wdt_kick();
		pr("ufs: try %d\n", tries);
		if (hba_enable(cnt_1us))
			continue;
		lanes = rd32(PLAT_UFS_UNIPRO + UNIP_PA_AVAILTXDATALANES);
		if (lanes < 1 || lanes > 2)
			lanes = 2;
		if (!link_startup())
			break;
		log_flush();
	}
	if (tries > 3) {
		pr("ufs: cold start failed at: link start-up\n");
		return -1;
	}
	if (lists_run() || nop_out() || device_init()) {
		pr("ufs: cold start failed after link start-up\n");
		return -1;
	}
	pr("ufs: cold start done: the device answers (PWM-G1)\n");
	return 0;
}

/* ---- HS gear: ufshcd_change_power_mode() with Google's pre_pmc CAL around it ---- */

#define UIC_DME_GET		0x01
#define UIC_DME_SET		0x02
#define UIC_DME_PEER_GET	0x03
#define IS_UPMS			(1u << 4)	/* UIC power mode status */
#define UNIP_DL_ERROR_IRQ_MASK	0x4844
#define  PA_ERROR_IND_RECEIVED	(1u << 15)

#define PA_ACTIVETXDATALANES	0x1560
#define PA_CONNECTEDTXDATALANES	0x1561
#define PA_TXGEAR		0x1568
#define PA_TXTERMINATION	0x1569
#define PA_HSSERIES		0x156a
#define PA_PWRMODE		0x1571
#define PA_ACTIVERXDATALANES	0x1580
#define PA_CONNECTEDRXDATALANES	0x1581
#define PA_RXGEAR		0x1583
#define PA_RXTERMINATION	0x1584
#define PA_MAXRXHSGEAR		0x1587
#define FAST_MODE		1
#define PA_HS_MODE_B		2

/* zuma calib_of_hs_rate_b: PA_TXHSADAPTTYPE, the DL timeouts (std MIB and debug APB copies) */
static const struct { u32 addr, val; } cal_hs_rate_b[] = {
	{0x3350, 0x1},
	{0x4104, 8064}, {0x4108, 28224}, {0x410c, 20160},
	{0x32c0, 12000}, {0x32c4, 32000}, {0x32c8, 16000},
	{0x7888, 8064}, {0x788c, 28224}, {0x7890, 20160},
	{0x78b8, 12000}, {0x78bc, 32000}, {0x78c0, 16000},
};

/* DME_GET / DME_SET / DME_PEER_GET of a MIB attribute (selector 0). 0 on success. */
static int dme(u32 cmd, u32 mib, u32 val, u32 *out)
{
	u32 res = 0xff;
	int r = uic_cmd(cmd, mib << 16, 0, val, 100, &res);

	if (r || res) {
		pr("ufs:   DME 0x%02x of 0x%04x: %s %u\n", cmd, mib, r ? "no answer" : "result", r ? (u32)-r : res);
		return -1;
	}
	if (out)
		*out = rd32(PLAT_UFS_HCI + UCMDARG3);
	return 0;
}

static int ufs_hs_gear(void)
{
	const u64 hci = PLAT_UFS_HCI, unipro = PLAT_UFS_UNIPRO;
	u32 lgear = 0, pgear = 0, tx = 0, rx = 0, gear, lanes_n, i, upmcrs;

	pr("\n--- ufs power mode: HS gear ---\n");
	if (dme(UIC_DME_GET, PA_MAXRXHSGEAR, 0, &lgear) || dme(UIC_DME_PEER_GET, PA_MAXRXHSGEAR, 0, &pgear) ||
	    dme(UIC_DME_GET, PA_CONNECTEDTXDATALANES, 0, &tx) || dme(UIC_DME_GET, PA_CONNECTEDRXDATALANES, 0, &rx))
		return -1;
	gear = lgear < pgear ? lgear : pgear;
	if (gear > 4)
		gear = 4;		/* ufs,pmd-attr-gear */
	lanes_n = tx < rx ? tx : rx;
	if (lanes_n > 2)
		lanes_n = 2;		/* ufs,pmd-attr-lane */
	pr("ufs: max HS gear: host %u, device %u; connected lanes tx %u rx %u -> HS-G%u rate B x%u\n", lgear,
	   pgear, tx, rx, gear, lanes_n);
	if (!gear || !lanes_n)
		return -1;
	/* ufs_cal_pre_pmc() */
	wr32(unipro + UNIP_DL_ERROR_IRQ_MASK, rd32(unipro + UNIP_DL_ERROR_IRQ_MASK) | PA_ERROR_IND_RECEIVED);
	for (i = 0; i < sizeof(cal_hs_rate_b) / sizeof(cal_hs_rate_b[0]); i++)
		wr32(unipro + cal_hs_rate_b[i].addr, cal_hs_rate_b[i].val);
	/* ufshcd_change_power_mode(); PA_PWRMODEUSERDATA comes from the CAL (SKIP_DEF_UNIPRO_TIMEOUT_SETTING) */
	if (dme(UIC_DME_SET, PA_RXGEAR, gear, 0) || dme(UIC_DME_SET, PA_ACTIVERXDATALANES, lanes_n, 0) ||
	    dme(UIC_DME_SET, PA_RXTERMINATION, 1, 0) || dme(UIC_DME_SET, PA_TXGEAR, gear, 0) ||
	    dme(UIC_DME_SET, PA_ACTIVETXDATALANES, lanes_n, 0) || dme(UIC_DME_SET, PA_TXTERMINATION, 1, 0) ||
	    dme(UIC_DME_SET, PA_HSSERIES, PA_HS_MODE_B, 0))
		return -1;
	log_flush();
	wr32(hci + IS, IS_UPMS);
	if (dme(UIC_DME_SET, PA_PWRMODE, FAST_MODE << 4 | FAST_MODE, 0))
		return -1;
	if (!wait_is(IS_UPMS, 1000)) {
		pr("ufs: power mode change: no status in 1 s (IS 0x%08x HCS 0x%08x)\n", rd32(hci + IS), rd32(hci + HCS));
		return -1;
	}
	wr32(hci + IS, IS_UPMS);
	upmcrs = (rd32(hci + HCS) >> 8) & 7;
	pr("ufs: power mode change: status %u (1 = done), IS 0x%08x\n", upmcrs, rd32(hci + IS));
	if (upmcrs != 1) {
		uic_errors("after the power mode change");
		return -1;
	}
	/* post_calib_of_hs_rate_b is empty; post_ah8 only with samsung,support-ah8 (not on caiman) */
	if (!dme(UIC_DME_GET, PA_RXGEAR, 0, &rx) && !dme(UIC_DME_GET, PA_TXGEAR, 0, &tx))
		pr("ufs: now HS-G%u/G%u rate B, %u lane(s): the link is fast\n", rx, tx, lanes_n);
	hs_ok = 1;
	return 0;
}

/* Read mib MiB from (lun, lba) in 256 KiB commands and print the rate. */
static void ufs_speed(const char *what, u32 lun, u32 lba, u32 mib)
{
	const u32 chunk = UFS_MAX_BLOCKS, total = mib * (1024 * 1024 / UFS_BLOCK);
	u64 start = timer_ticks(), us;
	u32 done;

	for (done = 0; done < total; done += chunk) {
		wdt_kick();
		if (ufs_read(lun, lba + done, chunk, (u64)(uptr)iobuf)) {
			pr("ufs: speed (%s): read failed at block %u\n", what, done);
			return;
		}
	}
	us = (timer_ticks() - start) * 1000000 / timer_freq();
	pr("ufs: speed (%s): %u MiB in %llu ms = %llu KB/s\n", what, mib, us / 1000,
	   us ? (u64)mib * 1048576 * 1000 / us : 0);
}

/*
 * liblp (system/core/fs_mgr/liblp/include/liblp/metadata_format.h): super starts with 4096 reserved
 * bytes, the geometry (block 1) and its backup (block 2); metadata slot 0 follows at block 3. Header:
 * magic, version, header_size @8, tables_size @44, then {offset, num_entries, entry_size} for the
 * partitions @80, extents @92, groups @104 and block devices @116; table offsets count from the end
 * of the header. Extents are in 512-byte sectors from the start of super.
 */
#define LP_GEOMETRY_MAGIC	0x616c4467u
#define LP_METADATA_MAGIC	0x414c5030u
#define LP_SECTOR		512u
#define LP_PART_SIZE		52u	/* name[36], attributes, first_extent_index, num_extents, group_index */
#define LP_EXTENT_SIZE		24u	/* num_sectors u64, target_type u32, target_data u64, target_source u32 */
#define LP_BDEV_SIZE		64u	/* first_logical_sector u64, alignment, alignment_offset, size u64 @16, ... */

/* An extent of a logical partition, where it is on the UFS LUN. */
struct lp_extent {
	u64 lba;		/* UFS block (4 KiB) */
	u64 blocks;
};

/*
 * Read metadata slot 0 of super into iobuf and check it. Returns the header size (the tables follow
 * it in iobuf) or 0, and sets *tables_out to the tables' size.
 */
static u32 lp_read_metadata(u32 lun, u64 super_lba, u32 *tables_out)
{
	const volatile u8 *b = (const volatile u8 *)(uptr)iobuf;
	u32 max, hdr, tables, blocks;

	if (ufs_read(lun, (u32)super_lba + 1, 1, (u64)(uptr)iobuf) || le32(b) != LP_GEOMETRY_MAGIC) {
		pr("ufs: super: no LP geometry at block 1 (magic 0x%08x)\n", le32(b));
		return 0;
	}
	max = le32(b + 40);
	pr("ufs: super geometry: struct %u, metadata max %u, slots %u, logical block %u\n", le32(b + 4), max,
	   le32(b + 44), le32(b + 48));
	if (ufs_read(lun, (u32)super_lba + 3, 1, (u64)(uptr)iobuf) || le32(b) != LP_METADATA_MAGIC) {
		pr("ufs: super: no LP metadata at block 3 (magic 0x%08x)\n", le32(b));
		return 0;
	}
	hdr = le32(b + 8);
	tables = le32(b + 44);
	pr("ufs: super metadata slot 0: version %u.%u, header %u bytes, tables %u bytes, flags 0x%x\n",
	   le32(b + 4) & 0xffff, le32(b + 4) >> 16, hdr, tables, hdr >= 132 ? le32(b + 128) : 0);
	if (hdr < 128 || hdr > 4096 || !tables || hdr + tables > max || max > UFS_MAX_BLOCKS * UFS_BLOCK) {
		pr("ufs: super: metadata sizes out of range\n");
		return 0;
	}
	blocks = (hdr + tables + UFS_BLOCK - 1) / UFS_BLOCK;
	if (blocks > 1 && ufs_read(lun, (u32)super_lba + 3, blocks, (u64)(uptr)iobuf)) {
		pr("ufs: super: metadata (%u blocks) not read\n", blocks);
		return 0;
	}
	*tables_out = tables;
	return hdr;
}

/* A table descriptor in the header: the entries' address in iobuf, or 0 if it does not fit. */
static const volatile u8 *lp_table(u32 hdr, u32 tables, u32 desc, u32 want_size, u32 *n)
{
	const volatile u8 *b = (const volatile u8 *)(uptr)iobuf;
	u32 off = le32(b + desc), num = le32(b + desc + 4), esz = le32(b + desc + 8);

	if (esz != want_size || (u64)off + (u64)num * esz > tables)
		return 0;
	*n = num;
	return b + hdr + off;
}

static void lp_name(const volatile u8 *p, char *name)
{
	int k;

	for (k = 0; k < 36; k++) {
		name[k] = (p[k] >= 0x20 && p[k] < 0x7f) ? (char)p[k] : 0;
		if (!p[k])
			break;
	}
	name[36] = 0;
}

/*
 * Find the logical partition `want` in super (lun, super_lba) and return up to max of its extents
 * as UFS blocks. Returns the number of extents, or -1. With want == 0, prints every partition and
 * super's free space instead (and returns 0).
 */
static int lp_find(u32 lun, u64 super_lba, u64 super_blocks, const char *want, struct lp_extent *out,
		   int max)
{
	const u64 limit = super_blocks * (UFS_BLOCK / LP_SECTOR);
	const volatile u8 *parts, *exts, *bdevs;
	u32 hdr, tables = 0, np, ne, nb, i, j;
	u64 end = 0;

	hdr = lp_read_metadata(lun, super_lba, &tables);
	if (!hdr)
		return -1;
	parts = lp_table(hdr, tables, 80, LP_PART_SIZE, &np);
	exts = lp_table(hdr, tables, 92, LP_EXTENT_SIZE, &ne);
	bdevs = lp_table(hdr, tables, 116, LP_BDEV_SIZE, &nb);
	if (!parts || !exts || !bdevs || nb < 1) {
		pr("ufs: super: LP tables do not fit (partitions %p extents %p block devices %p)\n", parts, exts,
		   bdevs);
		return -1;
	}
	if (!want)
		pr("ufs: super: %u logical partitions, %u extents, %u block device(s); super is %llu sectors\n",
		   np, ne, nb, le64(bdevs + 16) / LP_SECTOR);
	for (i = 0; i < np; i++) {
		const volatile u8 *p = parts + i * LP_PART_SIZE;
		u32 first = le32(p + 40), n = le32(p + 44);
		char name[37];
		int found;

		lp_name(p, name);
		found = want && !strcmp(name, want);
		if (want && !found)
			continue;
		if ((u64)first + n > ne) {
			pr("ufs: super: %s: extents %u+%u out of range\n", name, first, n);
			return -1;
		}
		if (!want)
			pr("ufs:   %-20s attr 0x%x, %u extent(s)\n", name, le32(p + 36), n);
		for (j = 0; j < n; j++) {
			const volatile u8 *e = exts + (first + j) * LP_EXTENT_SIZE;
			u64 sectors = le64(e), start = le64(e + 12);
			u32 type = le32(e + 8), src = le32(e + 20);

			if (!want)
				pr("ufs:     %s %llu sectors at super sector %llu = lun %u lba %llu..%llu%s\n",
				   type ? "zero  " : "linear", sectors, start, lun,
				   super_lba + start / 8, super_lba + (start + sectors) / 8 - 1,
				   (start | sectors) % 8 ? " (NOT 4 KiB aligned)" : "");
			if (type == 0 && start + sectors > end)
				end = start + sectors;
			if (!found)
				continue;
			if (type || src || (start | sectors) % 8 || start + sectors > limit || j >= (u32)max) {
				pr("ufs: super: %s: extent %u is not a plain 4 KiB-aligned range of super\n", name, j);
				return -1;
			}
			out[j].lba = super_lba + start / 8;
			out[j].blocks = sectors / 8;
		}
		if (found)
			return (int)n;
	}
	if (want) {
		pr("ufs: super: no logical partition named %s\n", want);
		return -1;
	}
	pr("ufs: super: used up to sector %llu; free after it: %llu sectors (%llu MiB)\n", end,
	   limit > end ? limit - end : 0, limit > end ? ((limit - end) * LP_SECTOR) >> 20 : 0);
	return 0;
}

/* Read extents one after another into dst. Returns the bytes read, 0 on a failure. */
static u64 read_extents(u32 lun, const struct lp_extent *ext, int n, u64 dst)
{
	u64 total = 0;
	int i;

	for (i = 0; i < n; i++) {
		u64 done;

		for (done = 0; done < ext[i].blocks; done += UFS_XFER_BLOCKS) {
			u32 cnt = ext[i].blocks - done < UFS_XFER_BLOCKS ? (u32)(ext[i].blocks - done) : UFS_XFER_BLOCKS;

			wdt_kick();
			if (ufs_read(lun, (u32)(ext[i].lba + done), cnt, dst + total * UFS_BLOCK)) {
				pr("ufs: read failed at extent %d block %llu\n", i, done);
				return 0;
			}
			total += cnt;
		}
	}
	return total * UFS_BLOCK;
}

#if !PROBE_LOADER
/* The same 64 blocks read into iobuf (below 4 GiB) and at pa: equal? */
static int same_as_iobuf(u32 lun, u64 lba, u64 pa)
{
	const volatile u64 *a = (const volatile u64 *)(uptr)iobuf, *b = (const volatile u64 *)(uptr)pa;
	u32 k;

	if (ufs_read(lun, (u32)lba, UFS_MAX_BLOCKS, (u64)(uptr)iobuf))
		return 0;
	for (k = 0; k < UFS_MAX_BLOCKS * UFS_BLOCK / 8; k++)
		if (a[k] != b[k]) {
			pr("ufs:   differs at byte %u: 0x%016llx (below 4 GiB) vs 0x%016llx (at 0x%llx)\n", k * 8, a[k],
			   b[k], pa);
			return 0;
		}
	return 1;
}
#endif

/*
 * Read the logical partition `name` of super to dst (at most max bytes), its extents one after the
 * other: how the loader puts xnusys into XNU's window. Returns its size in bytes, 0 on a failure.
 */
u64 ufs_load_lp(const char *name, u64 dst, u64 max)
{
	struct lp_extent ext[8];
	u64 bytes = 0, start, us;
	int n, i;

	if (super_lun_g == 0xff)
		return 0;
	if (!hs_ok)
		pr("ufs: %s: the link is in PWM-G1, this will take long (well over the watchdog's patience)\n", name);
	n = lp_find(super_lun_g, super_lba_g, super_blocks_g, name, ext, 8);
	if (n <= 0)
		return 0;
	for (i = 0; i < n; i++)
		bytes += ext[i].blocks * UFS_BLOCK;
	if (bytes > max) {
		pr("ufs: %s is %llu bytes, more than the %llu there is room for\n", name, bytes, max);
		return 0;
	}
	start = timer_ticks();
	busy_ticks = 0;
	if (!dcache_on()) {
		dc_all_sw();
		sw_flushed = 1;
	}
	if (!read_extents(super_lun_g, ext, n, dst))
		return 0;
	us = (timer_ticks() - start) * 1000000 / timer_freq();
	pr("ufs: %s: %llu bytes to 0x%llx in %llu ms (%llu KB/s; %llu ms of it waiting for the device, %u KiB a"
	   " request)\n", name, bytes, dst, us / 1000, us ? bytes * 1000 / us : 0,
	   busy_ticks * 1000 / timer_freq(), UFS_XFER_BLOCKS * UFS_BLOCK / 1024);
	return bytes;
}

/*
 * Probe mode: can the controller write above 4 GiB (Google's driver gives it a 32-bit DMA mask)? The
 * first 64 blocks of xnusys at XNU's resident RAM disk address, compared with the same read below
 * 4 GiB; then all of it there, and its last 64 blocks compared too.
 */
#if !PROBE_LOADER
static void xnusys_high(u32 lun, const struct lp_extent *ext, int n)
{
	const u64 hi = PLAT_XNU_PHYS + 0x08000000ULL;	/* HackiOS's RESIDENT_RD_OFFSET in the window */
	u64 last = ext[n - 1].lba + ext[n - 1].blocks - UFS_MAX_BLOCKS, bytes;

	if (ufs_read(lun, (u32)ext[0].lba, UFS_MAX_BLOCKS, hi)) {
		pr("ufs: DMA above 4 GiB (0x%llx): the read failed\n", hi);
		return;
	}
	if (!same_as_iobuf(lun, ext[0].lba, hi)) {
		pr("ufs: DMA above 4 GiB (0x%llx): the data is NOT the same\n", hi);
		return;
	}
	pr("ufs: DMA above 4 GiB (0x%llx): 256 KiB, the same as below 4 GiB\n", hi);
	bytes = ufs_load_lp("xnusys", hi, PLAT_XNU_SIZE - 0x38000000ULL);
	if (!bytes)
		return;
	pr("ufs: xnusys's last 256 KiB at 0x%llx: %s\n", hi + bytes - UFS_MAX_BLOCKS * UFS_BLOCK,
	   same_as_iobuf(lun, last, hi + bytes - UFS_MAX_BLOCKS * UFS_BLOCK) ? "the same as read below 4 GiB" : "DIFFERENT");
}
#endif

static void lp_list(u32 lun, u64 super_lba, u64 super_blocks)
{
	struct lp_extent ext[4];
	int n, i;

	if (lp_find(lun, super_lba, super_blocks, 0, 0, 0))
		return;
	/* the loader's path: look one partition up by name, as it will look up xnusys */
	n = lp_find(lun, super_lba, super_blocks, "system_a", ext, 4);
	for (i = 0; i < n; i++)
		pr("ufs: lookup system_a: extent %d = lun %u lba %llu, %llu blocks\n", i, lun, ext[i].lba,
		   ext[i].blocks);
	if (n > 0 && !ufs_read(lun, (u32)ext[0].lba, 1, (u64)(uptr)iobuf)) {
		const volatile u8 *b = (const volatile u8 *)(uptr)iobuf;

		pr("ufs: system_a block 0: %02x %02x %02x %02x ... ext4 magic at 0x438: 0x%02x%02x (want 0xef53)\n",
		   b[0], b[1], b[2], b[3], b[0x439], b[0x438]);
	}
	/* xnusys, the iOS system image (docs/m4-system-image-plan.md): read all of it, as the loader will */
	n = lp_find(lun, super_lba, super_blocks, "xnusys", ext, 4);
	if (n <= 0) {
		return;
	}
	for (i = 0; i < n; i++)
		pr("ufs: lookup xnusys: extent %d = lun %u lba %llu, %llu blocks\n", i, lun, ext[i].lba, ext[i].blocks);
	if (!ufs_read(lun, (u32)ext[0].lba, 1, (u64)(uptr)iobuf)) {
		const volatile u8 *b = (const volatile u8 *)(uptr)iobuf;

		pr("ufs: xnusys block 0: APFS container magic at 32: %c%c%c%c (want NXSB)\n", b[32], b[33], b[34], b[35]);
	}
#if !PROBE_LOADER
	if (hs_ok)
		xnusys_high(lun, ext, n);	/* the loader reads it itself (xnu.c) */
#endif
}

/* The GPT of each LUN, and in super the LP (dynamic partitions) metadata. */
static void ufs_read_test(void)
{
	const u64 buf = (u64)(uptr)iobuf;
	const volatile u8 *b = (const volatile u8 *)(uptr)buf;
	u32 lun, super_lun = 0xff;
	u64 super_lba = 0, super_blocks = 0;

	if (ufs_cold_start())
		return;
	ufs_speed("PWM-G1", 0, 0, 2);
	if (ufs_hs_gear())
		pr("ufs: staying in PWM-G1\n");
	pr("\n--- ufs read test (READ(10) only) ---\n");
	log_flush();
	for (lun = 0; lun < 4; lun++) {
		u64 entries_lba;
		u32 n, esz, i, blocks;
		int r;

		wdt_kick();
		r = ufs_read(lun, 1, 1, buf);	/* the GPT header: LBA 1 */
		if (r) {
			pr("ufs: lun %u: read failed (%d)\n", lun, r);
			if (r == -3)
				break;		/* a timeout: the others would time out too */
			continue;
		}
		if (memcmp((const void *)(uptr)buf, "EFI PART", 8)) {
			pr("ufs: lun %u: no GPT at LBA 1\n", lun);
			continue;
		}
		entries_lba = le64(b + 72);
		n = le32(b + 80);
		esz = le32(b + 84);
		pr("ufs: lun %u: GPT, %u entries of %u bytes at LBA %llu, last usable LBA %llu\n", lun, n,
		   esz, entries_lba, le64(b + 48));
		if (esz < 128 || esz > 512 || n > 256)
			continue;
		blocks = (n * esz + UFS_BLOCK - 1) / UFS_BLOCK;
		if (blocks > UFS_MAX_BLOCKS || ufs_read(lun, (u32)entries_lba, blocks, buf)) {
			pr("ufs: lun %u: entries not read\n", lun);
			continue;
		}
		for (i = 0; i < n; i++) {
			const volatile u8 *e = b + (u64)i * esz;
			u64 first = le64(e + 32), last = le64(e + 40);
			char name[37];
			int k;

			if (!first)
				continue;
			for (k = 0; k < 36; k++) {
				u8 c = e[56 + 2 * k];

				name[k] = (c >= 0x20 && c < 0x7f) ? (char)c : 0;
				if (!c)
					break;
			}
			name[36] = 0;
			pr("ufs:   %-24s lba %8llu .. %8llu  %6llu MiB\n", name, first, last,
			   ((last - first + 1) * UFS_BLOCK) >> 20);
			if (!strcmp(name, "boot_b") && bootb_lun == 0xff) {
				bootb_lun = lun;
				bootb_lba = first;
				bootb_blocks = last - first + 1;
			}
			if (!strcmp(name, "super")) {
				super_lun = lun;
				super_lba = first;
				super_blocks = last - first + 1;
			}
		}
		log_flush();
	}
#ifdef PROBE_UFSLOG
	ufslog_save();
	log_flush();
#endif
	if (super_lun == 0xff) {
		pr("ufs: no partition named super found\n");
		return;
	}
	pr("ufs: super is lun %u lba %llu, %llu blocks (%llu bytes)\n", super_lun, super_lba, super_blocks,
	   super_blocks * UFS_BLOCK);
	super_lun_g = super_lun;
	super_lba_g = super_lba;
	super_blocks_g = super_blocks;
	lp_list(super_lun, super_lba, super_blocks);
}
#endif

void ufs_probe(void)
{
	const u64 hci = PLAT_UFS_HCI;
	u32 cap, ver, hcs, hce, is, ie, ahit, utrlba, utrlbau, dbr, rsr, utmrlba, utmrlrsr, pid, mid;
	u64 utrl;

	pr("\n--- ufs (read-only) ---\n");
	pr("ufs: reading the host controller @ 0x%llx (a hang right after this line means its power\n"
	   "     domain is off)\n", hci);
	log_flush();
	if (!rd(hci + CAP, &cap)) {
		pr("ufs: CAP read aborted: the controller is not accessible\n");
		return;
	}
	rd(hci + VER, &ver);
	rd(hci + HCPID, &pid);
	rd(hci + HCMID, &mid);
	rd(hci + AHIT, &ahit);
	rd(hci + IS, &is);
	rd(hci + IE, &ie);
	rd(hci + HCS, &hcs);
	rd(hci + HCE, &hce);
	rd(hci + UTRLBA, &utrlba);
	rd(hci + UTRLBAU, &utrlbau);
	rd(hci + UTRLDBR, &dbr);
	rd(hci + UTRLRSR, &rsr);
	rd(hci + UTMRLBA, &utmrlba);
	rd(hci + UTMRLRSR, &utmrlrsr);
	pr("ufs: CAP 0x%08x (%u transfer slots, %u task slots, 64-bit %s) VER 0x%08x PID 0x%08x MID 0x%08x\n",
	   cap, (cap & 0x1f) + 1, ((cap >> 16) & 7) + 1, (cap & (1u << 24)) ? "yes" : "no", ver, pid, mid);
	pr("ufs: HCE 0x%08x (%s)  HCS 0x%08x: device %s, UTRL %s, UTMRL %s, UIC cmd %s, power mode %u\n",
	   hce, (hce & 1) ? "enabled" : "DISABLED", hcs, (hcs & 1) ? "present" : "absent",
	   (hcs & 2) ? "ready" : "not ready", (hcs & 4) ? "ready" : "not ready",
	   (hcs & 8) ? "ready" : "not ready", (hcs >> 8) & 7);
	pr("ufs: IS 0x%08x IE 0x%08x AHIT 0x%08x\n", is, ie, ahit);
	pr("ufs: UTRLBA 0x%08x%08x  doorbell 0x%08x  run 0x%08x   UTMRLBA 0x%08x run 0x%08x\n", utrlbau,
	   utrlba, dbr, rsr, utmrlba, utmrlrsr);
	dump_words("HCI 0x00-0x9f", hci, 40);
	{
		u32 v;

		if (rd(PLAT_UFS_VS, &v))
			dump_words("vendor 0x00-0xcf", PLAT_UFS_VS, 52);
	}

	utrl = ((u64)utrlbau << 32) | (utrlba & ~0x3ffu);
	if ((hce & 1) && utrl && addr_in_dram(utrl, 32ULL * ((cap & 0x1f) + 1)))
		dump_utrl(utrl, (cap & 0x1f) + 1, dbr);
	else
		pr("ufs: no transfer request list to look at (controller %s, UTRL 0x%llx)\n",
		   (hce & 1) ? "enabled" : "off", utrl);
	pr("ufs: verdict: %s\n",
	   (hce & 1) && (hcs & 0xf) == 0xf ? "UP: the bootloader left the link running; a loader can issue reads"
	   : (hce & 1) ? "enabled but not all lists ready: needs (re)starting the lists"
		       : "OFF: a loader must bring the controller and the link up itself");
#if PROBE_UFS >= 2
	ufs_read_test();
#endif
}

#endif
