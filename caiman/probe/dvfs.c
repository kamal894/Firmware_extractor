/* SPDX-License-Identifier: MIT */
/*
 * The CPU clusters' clocks through ACPM, as Android's exynos-acme does (PROBE_DVFS):
 *
 *   PROBE_DVFS=1  read each cluster's rate (FREQ_GET): what the boot loader leaves
 *   PROBE_DVFS=2  then ask for the rates below (FREQ_REQ) and read them back
 *
 * google-modules/soc/gs drivers/soc/google/cal-if/acpm_dvfs.c: exynos_acpm_set_rate() and
 * exynos_acpm_get_rate() send {domain, kHz, FREQ_REQ 0 / FREQ_GET 1, ms} on the DVFS manager's channel
 * (exynos-dm acpm-ipc-channel = 1 in this phone's device tree); the domain is the low bits of the
 * cal-id (include/dt-bindings/clock/zuma.h: ACPM_DVFS_CPUCL0..2 = 0x0b040002..4). A get answers in
 * cmd[1], or an error in cmd[3]. Voltages are ACPM's own business: nothing here touches the PMIC.
 * The IPC itself is the one touch.c uses for the PMIC (acpm_ipc.c), on another channel.
 */
#include "probe.h"

#if PROBE_DVFS
#define ACPM_MBOX	0x15110000ULL
#define ACPM_SRAM	0x15700000ULL
#define ACPM_SRAM_SIZE	0x58000u
#define ACPM_INITDATA	0xa000u
#define APM_INTGR	0x40
#define AP_INTCR	0x24
#define DVFS_CHANNEL	1
#define SEQ_SHIFT	16

#define CH_SIZE		72
#define CH_ID		0
#define CH_RX_REAR	16
#define CH_RX_FRONT	20
#define CH_RX_BASE	24
#define CH_TX_REAR	40
#define CH_TX_FRONT	44
#define CH_TX_BASE	48
#define CH_Q_LEN	52
#define CH_Q_ELEM	56

#define FREQ_REQ	0
#define FREQ_GET	1

/* exynos-acme domains of the device tree: min, max and resume frequencies in kHz; want: a step of each
 * cluster's table (Android's scaling_available_frequencies), about four fifths of its maximum */
static const struct {
	char name[12];
	u32 domain, min, max, resume, want;
} cluster[] = {
	{ "cl0 A520", 2, 820000, 1950000, 1425000, 1696000 },
	{ "cl1 A720", 3, 357000, 2600000, 1797000, 2130000 },
	{ "cl2 X4",   4, 500000, 3105000, 1164000, 2499000 },
};

static u32 sram32(u32 off)
{
	return rd32(ACPM_SRAM + off);
}

/*
 * One request of four words on channel ch; waiting up to wait_ms for the reply, its words in cmd.
 * Replies to other requests are taken off the queue too (Linux's interrupt handler does), so that
 * a full queue does not hold ACPM up. Returns 0, or -1 (nothing sent, or no answer in time).
 */
static int acpm_xchg(u32 ch, u32 *cmd, u32 wait_ms)
{
	static u32 seq;
	u32 base = sram32(ACPM_INITDATA + 8) + ch * CH_SIZE;
	u32 id, len, esz, txf_at, txr_at, rxf_at, rxr_at, txb, rxb, txf, txr, i;
	u64 start;

	if (base >= ACPM_SRAM_SIZE || ch >= sram32(ACPM_INITDATA + 24)) {
		pr("dvfs: ACPM channel %u not found\n", ch);
		return -1;
	}
	id = sram32(base + CH_ID);
	len = sram32(base + CH_Q_LEN);
	esz = sram32(base + CH_Q_ELEM);
	txf_at = sram32(base + CH_RX_FRONT);
	txr_at = sram32(base + CH_RX_REAR);
	rxf_at = sram32(base + CH_TX_FRONT);
	rxr_at = sram32(base + CH_TX_REAR);
	txb = sram32(base + CH_RX_BASE);
	rxb = sram32(base + CH_TX_BASE);
	if (!len || len > 64 || esz < 16 || esz > 64 || txb + len * esz > ACPM_SRAM_SIZE ||
	    rxb + len * esz > ACPM_SRAM_SIZE || txf_at >= ACPM_SRAM_SIZE || txr_at >= ACPM_SRAM_SIZE ||
	    rxf_at >= ACPM_SRAM_SIZE || rxr_at >= ACPM_SRAM_SIZE) {
		pr("dvfs: ACPM channel %u layout out of range (queue %u x %u): no request sent\n", ch, len, esz);
		return -1;
	}
	txf = sram32(txf_at);
	txr = sram32(txr_at);
	if (txf >= len || txr >= len || (txf + 1) % len == txr) {
		pr("dvfs: ACPM channel %u counters front %u rear %u: no request sent\n", ch, txf, txr);
		return -1;
	}
	if (++seq >= 64)
		seq = 1;
	cmd[0] |= seq << SEQ_SHIFT;
	for (i = 0; i < 4; i++)
		wr32(ACPM_SRAM + txb + esz * txf + 4 * i, cmd[i]);
	for (i = 16; i < esz; i += 4)
		wr32(ACPM_SRAM + txb + esz * txf + i, 0);
	dsb_sy();
	wr32(ACPM_SRAM + txf_at, (txf + 1) % len);
	dsb_sy();
	wr32(ACPM_MBOX + APM_INTGR, 1u << id);
	if (!wait_ms)
		return 0;
	start = timer_ticks();
	while (timer_ticks() - start < timer_freq() / 1000 * wait_ms) {
		u32 rf = sram32(rxf_at), rr = sram32(rxr_at);

		if (rf >= len || rr >= len)
			break;
		for (; rr != rf; rr = (rr + 1) % len) {
			u32 s = (sram32(rxb + esz * rr) >> SEQ_SHIFT) & 0x3f;

			wr32(ACPM_SRAM + rxr_at, (rr + 1) % len);
			wr32(ACPM_MBOX + AP_INTCR, 1u << id);
			if (s != seq) {
				pr("dvfs: (a reply to request %u taken off the queue)\n", s);
				continue;
			}
			for (i = 0; i < 4; i++)
				cmd[i] = sram32(rxb + esz * rr + 4 * i);
			pr("dvfs: reply %u after %llu us: %08x %08x %08x %08x\n", seq,
			   (timer_ticks() - start) * 1000000 / timer_freq(), cmd[0], cmd[1], cmd[2], cmd[3]);
			return 0;
		}
	}
	pr("dvfs: ACPM channel %u: no answer in %u ms (to ACPM front %u rear %u, from ACPM front %u rear %u)\n", ch,
	   wait_ms, sram32(txf_at), sram32(txr_at), sram32(rxf_at), sram32(rxr_at));
	return -1;
}

/* exynos_acpm_get_rate(): kHz, or -1 */
static s64 cluster_rate(u32 domain)
{
	u32 cmd[4] = { domain, 0, FREQ_GET, 0 };

	if (acpm_xchg(DVFS_CHANNEL, cmd, 50))
		return -1;
	if (cmd[3]) {
		pr("dvfs: domain %u: FREQ_GET error %u\n", domain, cmd[3]);
		return -1;
	}
	return cmd[1];
}

static void show_rates(const char *when)
{
	u32 i;

	for (i = 0; i < sizeof(cluster) / sizeof(cluster[0]); i++) {
		s64 r = cluster_rate(cluster[i].domain);

		pr("dvfs: %s: %-8s (domain %u) %lld kHz (min %u, max %u, resume %u)\n", when, cluster[i].name,
		   cluster[i].domain, r, cluster[i].min, cluster[i].max, cluster[i].resume);
	}
}

void dvfs_probe(void)
{
	pr("\n--- CPU clocks through ACPM (channel %u) ---\n", DVFS_CHANNEL);
	show_rates("now");
#if PROBE_DVFS >= 2
	{
		u32 i;

		for (i = 0; i < sizeof(cluster) / sizeof(cluster[0]); i++) {
			u32 cmd[4] = { cluster[i].domain, cluster[i].want, FREQ_REQ, 0 };

			pr("dvfs: asking for %u kHz on %s\n", cluster[i].want, cluster[i].name);
			/*
			 * Linux does not wait for an answer for the CPU clusters (acpm_dvfs.c config.response); here
			 * one is waited for, up to half a second, to see whether and when ACPM has done it.
			 */
			acpm_xchg(DVFS_CHANNEL, cmd, 500);
			delay_us(20000);
		}
		show_rates("after");
	}
#endif
	log_flush();
}
#endif
