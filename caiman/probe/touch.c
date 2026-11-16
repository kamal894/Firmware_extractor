/* SPDX-License-Identifier: MIT */
/*
 * Read-only look at what the touch screen needs (PROBE_TOUCH=1), before any driver for it:
 *
 *   power  the Synaptics TCM chip's supplies are LDO4M (TS_AVDD, 3.3 V) and LDO25M (TSP_DVDD, 1.8 V)
 *          of the main PMIC, S2MPG14, which only ACPM (the power-management core) talks to: an IPC
 *          request through its mailbox and SRAM queues, as Google's acpm_ipc.c / acpm_mfd.c do
 *   pins   reset gpp1-1 (GPIO_PERIC0 0x10840000), interrupt gpn0-0 (0x15060000)
 *   bus    the USI SPI controller at 0x111d0000 (Android's spi20)
 *
 * Everything from the phone's own device tree (/sys/firmware/fdt under Android) and
 * google-modules/soc/gs (drivers/soc/google/acpm, include/linux/mfd/samsung/s2mpg14-register.h).
 * Only reads: PMIC registers are read through ACPM's FUNC_READ, nothing is written but the request.
 */
#include "probe.h"

#if PROBE_TOUCH

#define ACPM_MBOX	0x15110000ULL	/* acpm_ipc reg[0]: the AP <-> APM mailbox */
#define ACPM_SRAM	0x15700000ULL	/* reg[1]: APM SRAM, 0x58000 bytes */
#define ACPM_INITDATA	0xa000u		/* initdata-base: struct acpm_framework */
#define APM_INTGR	0x40		/* INTGR1: interrupt to the APM */
#define AP_INTCR	0x24		/* INTCR0 */
#define AP_INTSR	0x2c		/* INTSR0 */
#define MFD_CHANNEL	2		/* s2mpg14mfd: acpm-ipc-channel */
#define MFD_SPEEDY	0		/* acpm-speedy-channel */
#define SEQ_SHIFT	16		/* ACPM_IPC_PROTOCOL_SEQ_NUM */

#define PMIC_TOP	0x0
#define PMIC_PM		0x1
#define TOP_CHIPID	0x0b		/* S2MPG14_COMMON_CHIPID */
#define PM_L4M_CTRL	0x2e
#define PM_L25M_CTRL	0x43

#define TOUCH_SPI	0x111d0000ULL
#define PIN_RESET_BANK	(0x10840000ULL + 0x20)	/* gpp1 */
#define PIN_RESET	1
#define PIN_IRQ_BANK	0x15060000ULL		/* gpn0 */
#define PIN_IRQ		0

/* struct ipc_channel (fw_header/framework.h): 72 bytes; the AP sends on the ch.rx_* queue */
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
#define CH_AP_POLL	68

static u32 sram32(u32 off)
{
	return rd32(ACPM_SRAM + off);
}

static u32 ch_base, seq;

/*
 * One PMIC register read (exynos_acpm_read_reg). Returns the value, or -1. The channel's *_front and
 * *_rear fields are SRAM offsets of the counters, not the counters (acpm_ipc.c channel_init()).
 */
static int pmic_read(u32 type, u32 reg)
{
	u32 id = sram32(ch_base + CH_ID), len = sram32(ch_base + CH_Q_LEN), esz = sram32(ch_base + CH_Q_ELEM);
	u32 txf_at = sram32(ch_base + CH_RX_FRONT), txr_at = sram32(ch_base + CH_RX_REAR);
	u32 rxf_at = sram32(ch_base + CH_TX_FRONT), rxr_at = sram32(ch_base + CH_TX_REAR);
	u32 txb = sram32(ch_base + CH_RX_BASE), rxb = sram32(ch_base + CH_TX_BASE);
	u32 txf, txr, cmd0, i, s;
	u64 start;

	if (!len || len > 64 || esz < 8 || esz > 64 || txb + len * esz > 0x58000 || rxb + len * esz > 0x58000 ||
	    txf_at >= 0x58000 || txr_at >= 0x58000 || rxf_at >= 0x58000 || rxr_at >= 0x58000) {
		pr("touch: ACPM channel layout out of range; no request sent\n");
		return -1;
	}
	txf = sram32(txf_at);
	txr = sram32(txr_at);
	if (txf >= len || txr >= len || (txf + 1) % len == txr) {
		pr("touch: ACPM channel counters front %u rear %u (queue %u): no request sent\n", txf, txr, len);
		return -1;
	}
	if (++seq >= 64)
		seq = 1;
	cmd0 = (type << 8) | reg | (MFD_SPEEDY << 12) | (seq << SEQ_SHIFT);
	wr32(ACPM_SRAM + txb + esz * txf, cmd0);
	wr32(ACPM_SRAM + txb + esz * txf + 4, 0);	/* FUNC_READ */
	for (i = 8; i < esz; i += 4)
		wr32(ACPM_SRAM + txb + esz * txf + i, 0);
	dsb_sy();
	wr32(ACPM_SRAM + txf_at, (txf + 1) % len);
	dsb_sy();
	wr32(ACPM_MBOX + APM_INTGR, 1u << id);
	start = timer_ticks();
	while (timer_ticks() - start < timer_freq() / 20) {	/* 50 ms, as the Linux driver */
		u32 rf = sram32(rxf_at), rr = sram32(rxr_at);

		if (rf >= len || rr >= len)
			break;
		for (; rr != rf; rr = (rr + 1) % len) {
			u32 c0 = sram32(rxb + esz * rr), c1 = sram32(rxb + esz * rr + 4);

			s = (c0 >> SEQ_SHIFT) & 0x3f;
			if (s == seq) {
				wr32(ACPM_SRAM + rxr_at, rf);
				wr32(ACPM_MBOX + AP_INTCR, 1u << id);
				if ((c1 >> 24) & 0xff) {
					pr("touch: PMIC read %x:%02x: ACPM returned error %u\n", type, reg, (c1 >> 24) & 0xff);
					return -1;
				}
				return (int)((c1 >> 8) & 0xff);
			}
		}
	}
	pr("touch: PMIC read %x:%02x: no answer from ACPM in 50 ms (INTSR0 0x%08x, rx front %u rear %u)\n", type, reg,
	   rd32(ACPM_MBOX + AP_INTSR), sram32(rxf_at), sram32(rxr_at));
	return -1;
}

static void pin(const char *name, u64 bank, u32 n)
{
	int ok;
	u32 con = safe_rd32(bank, &ok), dat = ok ? rd32(bank + 4) : 0, pud = ok ? rd32(bank + 8) : 0;

	if (!ok) {
		pr("touch: %s pin: bank 0x%llx not readable\n", name, bank);
		return;
	}
	pr("touch: %s pin %u: function %u, level %u, pull %u (CON 0x%08x DAT 0x%08x)\n", name, n,
	   (con >> (4 * n)) & 0xf, (dat >> n) & 1, (pud >> (4 * n)) & 0xf, con, dat);
}

static const char *ldo_state(int v)
{
	/* S2MPG14 LDO CTRL: bits 7:6 the enable mode (0 off), bits 5:0 the voltage step */
	if (v < 0)
		return "?";
	return (v >> 6) & 3 ? "ON" : "off";
}

void touch_probe(void)
{
	u32 ipc_channels, ap_max, ch_off, i;
	int ok, v;

	pr("\n--- touch screen (read-only) ---\n");
	log_flush();
	ipc_channels = sram32(ACPM_INITDATA + 8);
	ap_max = sram32(ACPM_INITDATA + 24);
	pr("touch: ACPM initdata: %u plugins, ipc_channels at 0x%x, %u channels, %u for the AP\n",
	   sram32(ACPM_INITDATA + 4), ipc_channels, sram32(ACPM_INITDATA + 12), ap_max);
	if (ipc_channels >= 0x58000 || ap_max <= MFD_CHANNEL || ap_max > 32) {
		pr("touch: ACPM initdata does not look right; no PMIC reads\n");
	} else {
		ch_off = ipc_channels + MFD_CHANNEL * CH_SIZE;
		ch_base = ch_off;
		pr("touch: ACPM channel %u: id %u, queue %u x %u bytes, AP polls %u; to APM front %u rear %u (at 0x%x),"
		   " from APM front %u rear %u\n", MFD_CHANNEL, sram32(ch_off + CH_ID), sram32(ch_off + CH_Q_LEN),
		   sram32(ch_off + CH_Q_ELEM), sram32(ch_off + CH_AP_POLL),
		   sram32(ch_off + CH_RX_FRONT) < 0x58000 ? sram32(sram32(ch_off + CH_RX_FRONT)) : 0xdead,
		   sram32(ch_off + CH_RX_REAR) < 0x58000 ? sram32(sram32(ch_off + CH_RX_REAR)) : 0xdead,
		   sram32(ch_off + CH_RX_FRONT),
		   sram32(ch_off + CH_TX_FRONT) < 0x58000 ? sram32(sram32(ch_off + CH_TX_FRONT)) : 0xdead,
		   sram32(ch_off + CH_TX_REAR) < 0x58000 ? sram32(sram32(ch_off + CH_TX_REAR)) : 0xdead);
		v = pmic_read(PMIC_TOP, TOP_CHIPID);
		pr("touch: S2MPG14 chip id 0x%02x (the bootloader read 0x10)\n", v);
		if (v >= 0) {
			v = pmic_read(PMIC_PM, PM_L4M_CTRL);
			pr("touch: LDO4M  (TS_AVDD 3.3 V)  CTRL 0x%02x: %s\n", v, ldo_state(v));
			v = pmic_read(PMIC_PM, PM_L25M_CTRL);
			pr("touch: LDO25M (TSP_DVDD 1.8 V) CTRL 0x%02x: %s\n", v, ldo_state(v));
		}
	}
	pin("reset", PIN_RESET_BANK, PIN_RESET);
	pin("interrupt", PIN_IRQ_BANK, PIN_IRQ);
	pr("touch: SPI controller @ 0x%llx:", TOUCH_SPI);
	for (i = 0; i < 0x40; i += 4) {
		u32 r = safe_rd32(TOUCH_SPI + i, &ok);

		if (!ok) {
			pr(" (read at +0x%x aborted: clock or power off)", i);
			break;
		}
		pr(" %08x", r);
	}
	pr("\n");
	log_flush();
}

#endif
