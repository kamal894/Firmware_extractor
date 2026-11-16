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
#define VS_FSM_MONITOR		0xc0
#define UNIP_PA_AVAILTXDATALANES	0x3080
#define UNIP_PA_ACTIVETXDATALANES	0x3180
#define UNIP_PA_CONNECTEDTXDATALANES	0x3184
#define UNIP_PA_ACTIVERXDATALANES	0x3200
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
 * Our own READ(10)s through slot 0 of the request list the bootloader left running, in the layout
 * its own descriptors use (run 7): UTRD CT=1, DD=device-to-host, response UPIU at UCD+8 dwords
 * (16 dwords long), PRDT at UCD+24 dwords, 16-byte PRDT entries of 4 KiB. READ only: the opcode and
 * the data direction below are fixed.
 */
#define UFS_BLOCK	4096u
#define UFS_MAX_BLOCKS	64u

static u8 ucd[96 + 16 * UFS_MAX_BLOCKS] __attribute__((aligned(128)));
static u64 utrl_slot0;
static u8 tag;

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

/* Read nblocks (<= 64) 4 KiB blocks of a LUN into dst. 0 on success. */
static int ufs_read(u32 lun, u32 lba, u32 nblocks, u64 dst)
{
	const u64 hci = PLAT_UFS_HCI;
	volatile u32 *utrd = (volatile u32 *)(uptr)utrl_slot0;
	u64 ucd_pa = (u64)(uptr)ucd, start;
	u32 i, len = nblocks * UFS_BLOCK, ocs, dbr;

	if (!nblocks || nblocks > UFS_MAX_BLOCKS)
		return -1;
	if (rd32(hci + UTRLDBR) & 1)
		return -2;		/* slot 0 busy */
	memset(ucd, 0, sizeof(ucd));
	ucd[0] = 0x01;			/* COMMAND UPIU */
	ucd[1] = 0x40;			/* flags: R (read) */
	ucd[2] = (u8)lun;
	ucd[3] = ++tag;
	put_be32(ucd + 12, len);	/* expected data transfer length */
	ucd[16] = 0x28;			/* CDB: READ(10) */
	put_be32(ucd + 18, lba);
	ucd[23] = (u8)(nblocks >> 8);
	ucd[24] = (u8)nblocks;
	for (i = 0; i < nblocks; i++) {
		u8 *e = ucd + 96 + 16 * i;
		u64 a = dst + (u64)i * UFS_BLOCK;

		put_le32(e, (u32)a);
		put_le32(e + 4, (u32)(a >> 32));
		put_le32(e + 12, UFS_BLOCK - 1);	/* data byte count - 1 */
	}
	utrd[0] = 0x15000000u;		/* CT=UFS storage, DD=device to host, I */
	utrd[1] = 0;
	utrd[2] = 0x0f;			/* OCS: invalid until the controller writes it */
	utrd[3] = 0;
	utrd[4] = (u32)ucd_pa;
	utrd[5] = (u32)(ucd_pa >> 32);
	utrd[6] = (8u << 16) | 16u;		/* response UPIU at dword 8, 16 dwords */
	utrd[7] = (24u << 16) | nblocks;	/* PRDT at dword 24 */
	dsb_sy();
	wr32(hci + IS, 1);		/* clear UTRCS (write 1 to clear) */
	wr32(hci + UTRLDBR, 1);		/* ring slot 0 */
	start = timer_ticks();
	do {
		dbr = rd32(hci + UTRLDBR);
		if (timer_ticks() - start > timer_freq()) {
			wr32(hci + UTRLCLR, ~1u);	/* give slot 0 up */
			return -3;
		}
	} while (dbr & 1);
	ocs = utrd[2] & 0xff;
	if (ocs || ucd[32] != 0x21 || ucd[32 + 5] || ucd[32 + 7]) {
		pr("ufs:   read lun %u lba %u x%u: OCS 0x%02x, response UPIU %02x response %02x status %02x\n",
		   lun, lba, nblocks, ocs, ucd[32], ucd[32 + 5], ucd[32 + 7]);
		return -4;
	}
	return 0;
}

static int wait_is(u32 bits, u32 ms)
{
	u64 start = timer_ticks(), limit = timer_freq() / 1000 * ms;

	while (!(rd32(PLAT_UFS_HCI + IS) & bits))
		if (timer_ticks() - start > limit)
			return 0;
	return 1;
}

static u32 rd_or(u64 a, u32 dflt)
{
	u32 v;

	return rd(a, &v) ? v : dflt;
}

static void pma_show(const char *when, u32 lanes)
{
	const u64 pma = PLAT_UFS_PHY;
	u32 l;

	pr("ufs: PHY %s: common 0x000=0x%02x", when, rd_or(pma, 0xdead));
	for (l = 0; l < lanes; l++)
		pr("  lane %u: 0x9f4=0x%02x 0xa00=0x%02x 0xb64=0x%02x", l, rd_or(pma + l * PMA_LANE + 0x9f4, 0xdead),
		   rd_or(pma + l * PMA_LANE + 0xa00, 0xdead), rd_or(pma + l * PMA_LANE + 0xb64, 0xdead));
	pr("\n");
}

/*
 * Run 9: DME_HIBER_EXIT alone came back with power mode status 5 (PWR_FATAL_ERROR). On Tensor the
 * Hibern8 entry is followed by the vendor steps of Google's exynos_ufs_hibern8_notify(): the CAL
 * table post_h8_enter (PHY common 0x000 = 0x51, ...) and the controller's internal clocks stopped.
 * Before an exit the same driver undoes both: exynos_ufs_gate_clk(false), then, inside
 * ufs_call_cal() (PHY APB and UniPro clocks forced on), the zuma table pre_h8_exit. This is that
 * sequence, with every value printed before and after.
 */
static void hibern8_exit_prepare(void)
{
	const u64 vs = PLAT_UFS_VS, pma = PLAT_UFS_PHY;
	u32 clkstop, force, lanes, l;

	if (!rd(vs + VS_CLKSTOP_CTRL, &clkstop) || !rd(vs + VS_FORCE_HCS, &force)) {
		pr("ufs: vendor clock registers not readable; skipping the Tensor wake steps\n");
		return;
	}
	pr("ufs: before wake: CLKSTOP_CTRL 0x%08x FORCE_HCS 0x%08x FSM 0x%08x\n", clkstop, force,
	   rd_or(vs + VS_FSM_MONITOR, 0xdead));
	wr32(vs + VS_CLKSTOP_CTRL, clkstop & ~CLK_STOP_ALL);	/* internal clocks on */
	wr32(vs + VS_FORCE_HCS, force & ~(UNIPRO_MCLK_STOP_EN | MPHY_APBCLK_STOP_EN));
	dsb_sy();
	delay_us(10);
	pr("ufs: clocks on: CLKSTOP_CTRL 0x%08x FORCE_HCS 0x%08x\n", rd32(vs + VS_CLKSTOP_CTRL),
	   rd32(vs + VS_FORCE_HCS));
	log_flush();
	lanes = rd_or(PLAT_UFS_UNIPRO + UNIP_PA_AVAILTXDATALANES, 0);
	pr("ufs: UniPro: available tx lanes %u, connected tx %u rx %u\n", lanes,
	   rd_or(PLAT_UFS_UNIPRO + UNIP_PA_CONNECTEDTXDATALANES, 0xdead),
	   rd_or(PLAT_UFS_UNIPRO + UNIP_PA_CONNECTEDRXDATALANES, 0xdead));
	if (lanes < 1 || lanes > 2)
		lanes = 2;		/* zuma-ufs.dtsi: ufs,pmd-attr-lane = 2 */
	pma_show("before", lanes);
	log_flush();
	/* zuma/ufs-cal.h pre_h8_exit[], entry by entry, each TRSV entry on every lane */
	wr32(pma + 0x000, 0x11);
	delay_us(10);
	for (l = 0; l < lanes; l++)
		wr32(pma + l * PMA_LANE + 0x9f4, 0x00);
	for (l = 0; l < lanes; l++)
		wr32(pma + l * PMA_LANE + 0xa00, 0x30);
	for (l = 0; l < lanes; l++)
		wr32(pma + l * PMA_LANE + 0xb64, 0x32);
	for (l = 0; l < lanes; l++)
		wr32(pma + l * PMA_LANE + 0xb64, 0x22);
	dsb_sy();
	pma_show("after ", lanes);
	wr32(vs + VS_FORCE_HCS, rd32(vs + VS_FORCE_HCS) | MPHY_APBCLK_STOP_EN);	/* as ufs_call_cal() */
	dsb_sy();
}

/*
 * Run 8: every READ timed out. Run 7's register dump says why: UICCMD (0x90) held 0x17,
 * DME_HIBER_ENTER, the bootloader's last act before "shutdown complete". The link is in Hibern8
 * and requests wait for ever. DME_HIBER_EXIT wakes it: a link power-state change, no data.
 */
static int hibern8_exit(void)
{
	const u64 hci = PLAT_UFS_HCI;
	u32 hcs = rd32(hci + HCS), cmd = rd32(hci + UICCMD) & 0xff, arg2, is;

	pr("ufs: last UIC command 0x%02x%s, HCS 0x%08x\n", cmd,
	   cmd == UIC_HIBER_ENTER ? " (DME_HIBER_ENTER: the link is in Hibern8)" : "", hcs);
	if (cmd != UIC_HIBER_ENTER)
		return 0;
	if (!(hcs & 8)) {
		pr("ufs: UIC command interface not ready; not sending DME_HIBER_EXIT\n");
		return -1;
	}
	hibern8_exit_prepare();
	wr32(hci + IS, IS_UCCS | IS_UHXS | IS_UHES | IS_UPMS);
	wr32(hci + UCMDARG1, 0);
	wr32(hci + UCMDARG2, 0);
	wr32(hci + UCMDARG3, 0);
	pr("ufs: sending DME_HIBER_EXIT\n");
	log_flush();
	wr32(hci + UICCMD, UIC_HIBER_EXIT);
	if (!wait_is(IS_UCCS, 500)) {
		pr("ufs: DME_HIBER_EXIT: no completion in 500 ms (IS 0x%08x)\n", rd32(hci + IS));
		return -2;
	}
	arg2 = rd32(hci + UCMDARG2);
	wait_is(IS_UHXS, 500);
	is = rd32(hci + IS);
	hcs = rd32(hci + HCS);
	pr("ufs: DME_HIBER_EXIT: result %u (0 = success), IS 0x%08x (exit status %s), HCS 0x%08x "
	   "(power mode status %u, 1 = done)\n", arg2 & 0xff, is, (is & IS_UHXS) ? "set" : "NOT set",
	   hcs, (hcs >> 8) & 7);
	pr("ufs: after exit: UniPro active tx %u rx %u lanes, FSM 0x%08x\n",
	   rd_or(PLAT_UFS_UNIPRO + UNIP_PA_ACTIVETXDATALANES, 0xdead),
	   rd_or(PLAT_UFS_UNIPRO + UNIP_PA_ACTIVERXDATALANES, 0xdead), rd_or(PLAT_UFS_VS + VS_FSM_MONITOR, 0xdead));
	wr32(hci + IS, IS_UCCS | IS_UHXS | IS_UPMS);
	return (arg2 & 0xff) == 0 && (is & IS_UHXS) && ((hcs >> 8) & 7) == 1 ? 0 : -3;
}

/* The GPT of each LUN, and in super the LP (dynamic partitions) metadata header. */
static void ufs_read_test(u64 utrl)
{
	const u64 buf = PLAT_UFS_SCRATCH;
	const volatile u8 *b = (const volatile u8 *)(uptr)buf;
	u32 lun, super_lun = 0xff;
	u64 super_lba = 0, super_blocks = 0;

	utrl_slot0 = utrl;
	pr("\n--- ufs read test (READ(10) only, through the bootloader's request list) ---\n");
	log_flush();
	if (hibern8_exit())
		pr("ufs: the link did not leave Hibern8; reads will likely time out\n");
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
	if ((hce & 1) && (hcs & 0xf) == 0xf && (rsr & 1) && utrl)
		ufs_read_test(utrl);
#endif
}

#endif
