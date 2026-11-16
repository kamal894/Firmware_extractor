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

/* our own lists and one command descriptor (slot 0), in the image's BSS */
static u8 utrl[32 * 32] __attribute__((aligned(1024)));
static u8 utmrl[32 * 8] __attribute__((aligned(1024)));
static u8 ucd[96 + 16 * UFS_MAX_BLOCKS] __attribute__((aligned(128)));
/* data lands below 4 GiB: Google's driver gives this controller a 32-bit DMA mask */
static u8 iobuf[UFS_MAX_BLOCKS * UFS_BLOCK] __attribute__((aligned(4096)));
static u8 tag;
static u32 mclk_hz, unipro_prd, lanes = 2;

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
	(void)rd32(hci + 0x38);		/* reading UECPA clears it */
	return 0;
}

static int lists_run(void)
{
	const u64 hci = PLAT_UFS_HCI;
	u64 a = (u64)(uptr)utrl, b = (u64)(uptr)utmrl;

	memset(utrl, 0, sizeof(utrl));
	memset(utmrl, 0, sizeof(utmrl));
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
	volatile u32 *utrd = (volatile u32 *)(uptr)utrl;
	u64 ucd_pa = (u64)(uptr)ucd, start;

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
	dsb_sy();
	wr32(hci + IS, IS_UTRCS);
	wr32(hci + UTRLDBR, 1);
	start = timer_ticks();
	while (rd32(hci + UTRLDBR) & 1) {
		if (timer_ticks() - start > timer_freq() / 1000 * ms) {
			wr32(hci + UTRLCLR, ~1u);	/* give slot 0 up */
			return -3;
		}
	}
	return (utrd[2] & 0xff) ? -4 : 0;
}

static void upiu_header(u8 type, u8 flags, u8 lun)
{
	memset(ucd, 0, 96);
	ucd[0] = type;
	ucd[1] = flags;
	ucd[2] = lun;
	ucd[3] = ++tag;
}

static int nop_out(void)
{
	int i, r = -1;

	for (i = 0; i < 5 && r; i++) {
		upiu_header(0x00, 0, 0);	/* NOP OUT */
		r = request(0, 0, 50);
		if (!r && ucd[32] != 0x20)
			r = -5;
	}
	pr("ufs:   NOP OUT: %s after %d tries (answer 0x%02x, OCS 0x%02x)\n", r ? "FAILED" : "NOP IN", i,
	   ucd[32], ((volatile u32 *)(uptr)utrl)[2] & 0xff);
	return r;
}

/* Query: flag fDeviceInit (IDN 1). fn 0x81 write / 0x01 read, op 6 SET FLAG / 5 READ FLAG */
static int query_flag(u8 fn, u8 op, u8 idn, u32 *val)
{
	int r;

	upiu_header(0x16, 0, 0);
	ucd[5] = fn;
	ucd[12] = op;
	ucd[13] = idn;
	r = request(0, 0, 100);
	if (r)
		return r;
	if (ucd[32] != 0x36 || ucd[32 + 6])
		return -6;
	*val = ucd[32 + 23] & 1;
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

	if (!nblocks || nblocks > UFS_MAX_BLOCKS)
		return -1;
	for (tries = 0; tries < 3; tries++) {
		upiu_header(0x01, 0x40, (u8)lun);	/* COMMAND, R */
		put_be32(ucd + 12, len);
		ucd[16] = 0x28;				/* READ(10) */
		put_be32(ucd + 18, lba);
		ucd[23] = (u8)(nblocks >> 8);
		ucd[24] = (u8)nblocks;
		for (i = 0; i < nblocks; i++) {
			u8 *e = ucd + 96 + 16 * i;
			u64 a = dst + (u64)i * UFS_BLOCK;

			put_le32(e, (u32)a);
			put_le32(e + 4, (u32)(a >> 32));
			put_le32(e + 8, 0);
			put_le32(e + 12, UFS_BLOCK - 1);
		}
		r = request(2, nblocks, 2000);
		if (r)
			return r;
		if (ucd[32] == 0x21 && !ucd[32 + 5] && !ucd[32 + 7])
			return 0;
		pr("ufs:   read lun %u lba %u x%u: response UPIU 0x%02x response 0x%02x status 0x%02x%s\n", lun,
		   lba, nblocks, ucd[32], ucd[32 + 5], ucd[32 + 7],
		   ucd[32 + 7] == 0x02 ? " (CHECK CONDITION: retrying)" : "");
		if (ucd[32 + 7] != 0x02)
			return -4;
	}
	return -4;
}

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

/* The GPT of each LUN, and in super the LP (dynamic partitions) metadata header. */
static void ufs_read_test(void)
{
	const u64 buf = (u64)(uptr)iobuf;
	const volatile u8 *b = (const volatile u8 *)(uptr)buf;
	u32 lun, super_lun = 0xff;
	u64 super_lba = 0, super_blocks = 0;

	if (ufs_cold_start())
		return;
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
			if (!strcmp(name, "super")) {
				super_lun = lun;
				super_lba = first;
				super_blocks = last - first + 1;
			}
		}
		log_flush();
	}
	if (super_lun == 0xff) {
		pr("ufs: no partition named super found\n");
		return;
	}
	pr("ufs: super is lun %u lba %llu, %llu blocks (%llu bytes)\n", super_lun, super_lba, super_blocks,
	   super_blocks * UFS_BLOCK);
	/* liblp: 4096 reserved bytes, the geometry (block 1), its backup, then metadata slot 0 (block 3) */
	if (!ufs_read(super_lun, (u32)super_lba + 1, 1, buf))
		pr("ufs: super geometry: magic 0x%08x (want 0x616c4467) struct %u, metadata max %u, slots %u,"
		   " logical block %u\n", le32(b), le32(b + 4), le32(b + 40), le32(b + 44), le32(b + 48));
	if (!ufs_read(super_lun, (u32)super_lba + 3, 1, buf))
		pr("ufs: super metadata slot 0: magic 0x%08x (want 0x414c5030) version %u.%u, header %u bytes,"
		   " tables %u bytes\n", le32(b), le32(b + 4) & 0xffff, le32(b + 4) >> 16, le32(b + 8),
		   le32(b + 44));
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
