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
			dump_words("vendor (0x13201100)", PLAT_UFS_VS, 16);
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
}

#endif
