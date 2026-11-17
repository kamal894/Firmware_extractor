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
static int pmic_req(u32 type, u32 reg, u32 func, u32 val)
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
	wr32(ACPM_SRAM + txb + esz * txf + 4, func | (val << 8));	/* FUNC_READ 0, FUNC_WRITE 1 + value */
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
					pr("touch: PMIC %s %x:%02x: ACPM returned error %u\n", func ? "write" : "read", type, reg,
					   (c1 >> 24) & 0xff);
					return -1;
				}
				return (int)((c1 >> 8) & 0xff);
			}
		}
	}
	pr("touch: PMIC %s %x:%02x: no answer from ACPM in 50 ms (INTSR0 0x%08x, rx front %u rear %u)\n", func ? "write" : "read",
	   type, reg, rd32(ACPM_MBOX + AP_INTSR), sram32(rxf_at), sram32(rxr_at));
	return -1;
}

static int pmic_read(u32 type, u32 reg)
{
	return pmic_req(type, reg, 0, 0);
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

#if PROBE_TOUCH >= 2
/*
 * Power the chip and take it out of reset, as the vendor driver's power-on does (synaptics,power-
 * delay-ms 200, reset-active-ms 2, reset-delay-ms 50; reset active low), then watch the interrupt
 * line: the chip pulls it low when it has a report for the host (the identify report after a
 * reset). Only the enable bits of the two LDOs change; their voltages are as the PMIC has them.
 */
static void set_pin(u64 bank, u32 n, u32 func, int level)
{
	u32 m = 0xfu << (4 * n);

	wr32(bank + 8, rd32(bank + 8) & ~m);				/* no pull, as Android's ts-irq/ts-reset */
	if (level >= 0)
		wr32(bank + 4, (rd32(bank + 4) & ~(1u << n)) | ((u32)level << n));
	wr32(bank, (rd32(bank) & ~m) | (func << (4 * n)));
}

static void power_on(int l4, int l25)
{
	u64 start, first_low = 0;
	u32 lows = 0, ms;
	int v;

	pr("touch: powering the chip: reset held low, LDO25M then LDO4M on\n");
	set_pin(PIN_IRQ_BANK, PIN_IRQ, 0, -1);			/* input */
	set_pin(PIN_RESET_BANK, PIN_RESET, 1, 0);		/* output, low: in reset */
	if (pmic_req(PMIC_PM, PM_L25M_CTRL, 1, (u32)(l25 & 0x3f) | 0xc0) < 0 ||
	    pmic_req(PMIC_PM, PM_L4M_CTRL, 1, (u32)(l4 & 0x3f) | 0xc0) < 0)
		return;
	v = pmic_read(PMIC_PM, PM_L4M_CTRL);
	pr("touch: LDO4M  CTRL now 0x%02x: %s\n", v, ldo_state(v));
	v = pmic_read(PMIC_PM, PM_L25M_CTRL);
	pr("touch: LDO25M CTRL now 0x%02x: %s\n", v, ldo_state(v));
	delay_us(200000);
	pin("interrupt (powered, in reset)", PIN_IRQ_BANK, PIN_IRQ);
	delay_us(2000);
	set_pin(PIN_RESET_BANK, PIN_RESET, 1, 1);		/* out of reset */
	start = timer_ticks();
	for (ms = 0; ms < 3000; ms++) {
		if (!((rd32(PIN_IRQ_BANK + 4) >> PIN_IRQ) & 1)) {
			if (!lows)
				first_low = timer_ticks();
			lows++;
		}
		delay_us(1000);
	}
	if (lows)
		pr("touch: interrupt line LOW %u of 3000 ms, first %llu ms after reset: the chip is up and has a report\n",
		   lows, (first_low - start) * 1000 / timer_freq());
	else
		pr("touch: interrupt line stayed high for 3 s after reset: no sign of the chip\n");
	pin("interrupt (after)", PIN_IRQ_BANK, PIN_IRQ);
	pin("reset (after)", PIN_RESET_BANK, PIN_RESET);
}
#endif

#if PROBE_TOUCH >= 2
/*
 * The USI SPI (Google's spi-s3c64xx.c, "samsung,exynos-spi": clock from the CMU, FIFO 64, RX level
 * at STATUS bits 15+), polled, chip select by hand. SPI mode 0, 8 bits.
 */
#define USI_SYSREG	(0x11020000ULL + 0x101c)	/* sysreg_hsi0 + samsung,usi-offset */
#define SPI_CH_CFG	0x00
#define SPI_MODE_CFG	0x08
#define SPI_SLAVE_SEL	0x0c
#define SPI_INT_EN	0x10
#define SPI_STATUS	0x14
#define SPI_TX_DATA	0x18
#define SPI_RX_DATA	0x1c
#define SPI_PACKET_CNT	0x20
#define SPI_PENDING_CLR	0x24
#define SPI_SWAP_CFG	0x28
#define SPI_USI_CON	0xc4
#define SPI_USI_OPTION	0xc8
#define CH_SW_RST	(1u << 5)
#define CH_RXCH_ON	(1u << 1)
#define CH_TXCH_ON	(1u << 0)
#define SLAVE_SIG_INACT	(1u << 0)
#define PACKET_CNT_EN	(1u << 16)
#define RX_LVL(st)	(((st) >> 15) & 0x7f)
#define TX_LVL(st)	(((st) >> 6) & 0x7f)

static void spi_flush(void)
{
	const u64 b = TOUCH_SPI;

	wr32(b + SPI_CH_CFG, rd32(b + SPI_CH_CFG) | CH_SW_RST);
	delay_us(10);
	wr32(b + SPI_CH_CFG, rd32(b + SPI_CH_CFG) & ~(CH_SW_RST | CH_RXCH_ON | CH_TXCH_ON));
	wr32(b + SPI_PACKET_CNT, 0);
}

/*
 * CMU_HSI0's clocks of USI2 (the SPI's usi-offset 0x101c is HSI0's third USI), at the offsets of
 * Google's zuma CAL tables (cal-if/zuma/cmucal-sfr.c); the mux (0x101c), divider (0x181c) and
 * Q-channel (0x30cc) are the same in this phone's cmupmucal.ko (vendor_kernel_boot). A gate:
 * bit 20 manual, bit 21 the clock's value when manual, bit 28 automatic gating.
 */
#define CMU_HSI0	0x11000000ULL
static const struct { char name[20]; u32 off; int gate; } usi2_clk[] = {
	{ "MUX  USI2", 0x101c, 0 }, { "DIV  USI2", 0x181c, 0 }, { "GATE USI2", 0x2120, 1 },
	{ "GATE USI2 IPCLK", 0x20fc, 1 }, { "GATE USI2 PCLK", 0x2100, 1 }, { "GATE USI2 RSTNSYNC", 0x20bc, 1 },
};

static void usi2_clocks(void)
{
	u32 i, v;
	int ok;

	/*
	 * The Q-channel of USI2 (QCH_CON_USI2_HSI0_QCH, CMU_HSI0 + 0x30cc in Google's zuma tables and in
	 * this phone's own cmupmucal.ko): with it enabled and no clock request the hardware may keep
	 * the USI's clock stopped. Bit 0 ENABLE, bit 1 CLOCK_REQ, bit 2 IGNORE_FORCE_PM_EN.
	 */
	v = safe_rd32(CMU_HSI0 + 0x30cc, &ok);
	if (ok) {
		pr("touch: CMU_HSI0 +0x30cc QCH USI2         0x%08x (ENABLE %u CLOCK_REQ %u)\n", v, v & 1, (v >> 1) & 1);
		if (!(v & ~0x7u) && !(v & 2)) {
			wr32(CMU_HSI0 + 0x30cc, v | 2);
			pr("touch:   CLOCK_REQ set: 0x%08x\n", rd32(CMU_HSI0 + 0x30cc));
		}
	}

	for (i = 0; i < sizeof(usi2_clk) / sizeof(usi2_clk[0]); i++) {
		v = safe_rd32(CMU_HSI0 + usi2_clk[i].off, &ok);
		if (!ok) {
			pr("touch: CMU_HSI0 +0x%04x %s: read aborted\n", usi2_clk[i].off, usi2_clk[i].name);
			continue;
		}
		pr("touch: CMU_HSI0 +0x%04x %-18s 0x%08x%s\n", usi2_clk[i].off, usi2_clk[i].name, v,
		   usi2_clk[i].gate ? ((v & (1u << 20)) ? ((v & (1u << 21)) ? " (manual, on)" : " (manual, OFF)")
					      : ((v & (1u << 28)) ? " (automatic)" : " (?)")) : "");
		/* a gate as the CAL tables describe it (only bits 20, 21, 28 and the low status bits): force it on */
		if (usi2_clk[i].gate && !(v & ~((1u << 20) | (1u << 21) | (1u << 28) | 0xffu)) &&
		    !((v & (1u << 20)) && (v & (1u << 21)))) {
			wr32(CMU_HSI0 + usi2_clk[i].off, v | (1u << 20) | (1u << 21));
			pr("touch:   set to manual, on: 0x%08x\n", rd32(CMU_HSI0 + usi2_clk[i].off));
		}
	}
}

static int spi_init(void)
{
	const u64 b = TOUCH_SPI;
	u32 conf = rd32(USI_SYSREG), v;

	pr("touch: USI mode register 0x%08x (SW_CONF %u; 2 = SPI)\n", conf, conf & 7);
	usi2_clocks();
	if ((conf & 7) != 2) {
		wr32(USI_SYSREG, (conf & ~7u) | 2);
		pr("touch: USI mode register after writing SPI: 0x%08x%s\n", rd32(USI_SYSREG),
		   (rd32(USI_SYSREG) & 7) == 2 ? "" : " (the write did not take: the register is not ours)");
	}
	/*
	 * exynos_usi_configure() (mainline drivers/soc/samsung/exynos-usi.c): ask for the clock and do
	 * not let hardware auto clock gating stop it (CLKREQ_ON, not CLKSTOP_ON), then out of reset
	 */
	wr32(b + SPI_USI_OPTION, (rd32(b + SPI_USI_OPTION) & ~(1u << 2)) | (1u << 1));
	wr32(b + SPI_USI_CON, 0);			/* USI_RESET released */
	wr32(b + SPI_SLAVE_SEL, SLAVE_SIG_INACT);
	wr32(b + SPI_INT_EN, 0);
	wr32(b + SPI_MODE_CFG, 0);
	wr32(b + SPI_PACKET_CNT, 0);
	wr32(b + SPI_PENDING_CLR, 0x1e);
	wr32(b + SPI_PENDING_CLR, 0);
	wr32(b + SPI_SWAP_CFG, 0);
	wr32(b + SPI_MODE_CFG, 0x3ffu << 19);		/* trailing count; byte transfers */
	spi_flush();
	wr32(b + SPI_CH_CFG, rd32(b + SPI_CH_CFG) & ~0x1cu);	/* master, CPOL 0, CPHA 0 */
	v = rd32(b + SPI_MODE_CFG);
	pr("touch: SPI after set-up: USI_CON 0x%08x USI_OPTION 0x%08x MODE_CFG 0x%08x (want 0x%08x) CH_CFG 0x%08x"
	   " STATUS 0x%08x\n", rd32(b + SPI_USI_CON), rd32(b + SPI_USI_OPTION), v, 0x3ffu << 19, rd32(b + SPI_CH_CFG),
	   rd32(b + SPI_STATUS));
	if (v != 0x3ffu << 19) {
		pr("touch: the SPI controller does not keep what is written: its clock is off\n");
		return -1;
	}
	return 0;
}

/* Clock n bytes in (0xff out) with chip select low. Returns how many came back. */
static u32 spi_read(u8 *buf, u32 n)
{
	const u64 b = TOUCH_SPI;
	u64 start;
	u32 i, got = 0;

	spi_flush();
	wr32(b + SPI_PACKET_CNT, n & 0xffff);
	wr32(b + SPI_PACKET_CNT, (n & 0xffff) | PACKET_CNT_EN);
	wr32(b + SPI_SLAVE_SEL, 0);				/* chip select on (active low) */
	delay_us(10);
	for (i = 0; i < n; i++)
		wr32(b + SPI_TX_DATA, 0xff);
	wr32(b + SPI_CH_CFG, rd32(b + SPI_CH_CFG) | CH_RXCH_ON | CH_TXCH_ON);
	start = timer_ticks();
	while (got < n && timer_ticks() - start < timer_freq() / 50) {
		u32 st = rd32(b + SPI_STATUS), lvl = RX_LVL(st);

		while (lvl-- && got < n)
			buf[got++] = (u8)rd32(b + SPI_RX_DATA);
	}
	wr32(b + SPI_CH_CFG, rd32(b + SPI_CH_CFG) & ~(CH_RXCH_ON | CH_TXCH_ON));
	wr32(b + SPI_SLAVE_SEL, SLAVE_SIG_INACT);
	return got;
}

/*
 * Which pads carry HSI0's USI2? Android's device tree does not say (the SPI's pinctrl-0 is empty and
 * HSI0 has no GPIO block). HSI0's USIs reach PERIC0/1 pads as a pin function (HSI0's USI4 uses gpp2,
 * function 3). Try every 4-pin USI pad group of PERIC0/1 (the uartN_bus_single groups) with functions
 * 2, 3 and 4: bytes coming back means the clock reaches those pads; 0xa5 first is the chip. Every pin
 * is put back as it was after each try. Bank addresses: the banks' order in the device tree, 0x20 each.
 */
static const struct { u64 bank; u32 first; char name[8]; } pad_groups[] = {
	{ 0x10840000ULL + 2 * 0x20, 0, "gpp2" },   { 0x10840000ULL + 4 * 0x20, 0, "gpp4" },
	{ 0x10840000ULL + 6 * 0x20, 0, "gpp6" },   { 0x10840000ULL + 8 * 0x20, 0, "gpp8" },
	{ 0x10840000ULL + 10 * 0x20, 0, "gpp10" }, { 0x10840000ULL + 12 * 0x20, 0, "gpp12" },
	{ 0x10840000ULL + 17 * 0x20, 0, "gpp16" },
	{ 0x10c40000ULL + 0 * 0x20, 0, "gpp19" },  { 0x10c40000ULL + 0 * 0x20, 4, "gpp19" },
	{ 0x10c40000ULL + 1 * 0x20, 0, "gpp20" },  { 0x10c40000ULL + 2 * 0x20, 0, "gpp21" },
	{ 0x10c40000ULL + 2 * 0x20, 4, "gpp21" },  { 0x10c40000ULL + 4 * 0x20, 0, "gpp22" },
	{ 0x10c40000ULL + 5 * 0x20, 0, "gpp23" },
};

static int spi_pad_scan(void)
{
	u8 buf[8];
	u32 g, f, got, i;
	int ok;

	pr("touch: scanning pad groups for HSI0 USI2 (functions 2, 3, 4; restored after each try)\n");
	for (g = 0; g < sizeof(pad_groups) / sizeof(pad_groups[0]); g++) {
		u64 b = pad_groups[g].bank;
		u32 con = safe_rd32(b, &ok), pud, m = 0xffffu << (4 * pad_groups[g].first);

		if (!ok) {
			pr("touch:   %s: not readable\n", pad_groups[g].name);
			continue;
		}
		pud = rd32(b + 8);
		for (f = 2; f <= 4; f++) {
			wr32(b + 8, pud & ~m);
			wr32(b, (con & ~m) | ((f * 0x1111u) << (4 * pad_groups[g].first)));
			memset(buf, 0, sizeof(buf));
			got = spi_read(buf, sizeof(buf));
			wr32(b, con);
			wr32(b + 8, pud);
			if (got) {
				pr("touch:   %s-%u..%u function %u: %u bytes:", pad_groups[g].name, pad_groups[g].first,
				   pad_groups[g].first + 3, f, got);
				for (i = 0; i < got; i++)
					pr(" %02x", buf[i]);
				pr("%s\n", buf[0] == 0xa5 ? "  <- the touch chip" : "");
				if (buf[0] == 0xa5) {
					/* leave these pads on the USI */
					wr32(b + 8, pud & ~m);
					wr32(b, (con & ~m) | ((f * 0x1111u) << (4 * pad_groups[g].first)));
					return 0;
				}
			}
		}
	}
	pr("touch: no pad group gave the clock a way out\n");
	return -1;
}

/*
 * The touch SPI's pads belong to the AOC: its firmware (aoc.bin, the A32 core) keeps one CSR word per
 * pad at AOC address 0x81408000 + 4 * pad (the PCU, blk_aoc + pcu-base 0x1400000, + 0x8000; the AOC's
 * 0x80000000 is the AP's blk_aoc 0x17000000). The byte: bits 0-2 function, bit 3 input, bits 4-5
 * pull, bits 6-7 strength. Its touch bus switch writes these six pads; "on" is what Android's awake
 * screen shows (dbg gpio): 106 0x00, 62 0x08, 72-75 0x2a (function 2); "off" is 0x28 for all six.
 */
#define AOC_PAD_CSR	0x18408000ULL
static const u8 aoc_touch_on[][2] = {
	{ 106, 0x00 }, { 62, 0x08 }, { 72, 0x2a }, { 73, 0x2a }, { 74, 0x2a }, { 75, 0x2a },
};

/*
 * The same switch also drives three AOC GPIOs (pads 87 and 105 once, the first time; one more on
 * every "on"), all function 0 outputs in Android. The AOC's GPIO block (blk_aoc + gpio-base
 * 0x1c00000; its driver: 32 pins per bank at 12-byte strides, +0 output level, +4 output enable,
 * inputs at +0x50 + 4 * bank). Android with touch working (AOC console "mem dump 0x81c00000 256",
 * 2026-11-17): only 106 of the three is an output, and it is low; 67 is an output high; 25, 57-59,
 * 70, 75 and 80 are outputs low. Copy that state, bank by bank: { out, oe }.
 */
#define AOC_GPIO	0x18c00000ULL
static const u32 aoc_gpio_android[4][2] = {
	{ 0, 0x02000000 }, { 0, 0x0e000000 }, { 0x00000008, 0x00010848 }, { 0, 0x00000400 },
};

static void aoc_gpio_dump(const char *when)
{
	u32 b;
	int ok;

	pr("touch: AOC GPIO %s:", when);
	for (b = 0; b < 4; b++) {
		u32 out = safe_rd32(AOC_GPIO + 12 * b, &ok);

		if (!ok) {
			pr(" (read aborted)\n");
			return;
		}
		pr(" [%u] out %08x oe %08x +8 %08x in %08x", b, out, rd32(AOC_GPIO + 12 * b + 4),
		   rd32(AOC_GPIO + 12 * b + 8), rd32(AOC_GPIO + 0x50 + 4 * b));
	}
	pr("\n");
}

static void aoc_touch_gpios(void)
{
	u32 i;
	int ok;

	aoc_gpio_dump("before");
	safe_rd32(AOC_GPIO, &ok);
	if (!ok)
		return;
	for (i = 0; i < 4; i++) {
		wr32(AOC_GPIO + 12 * i, aoc_gpio_android[i][0]);
		wr32(AOC_GPIO + 12 * i + 4, aoc_gpio_android[i][1]);
	}
	delay_us(20000);
	aoc_gpio_dump("after copying Android's outputs");
	pin("interrupt (AOC GPIOs as in Android)", PIN_IRQ_BANK, PIN_IRQ);
}

/*
 * aoc_core.ko's request_aoc_on(): write 1 to aoc_req (the DT's "aoc_req", 0x154b0000) and wait for
 * +0x40 to read non-zero (wait_for_aoc_status). This is what Linux does before it touches the
 * AOC: the request for the AOC's power and clocks. Also show pd_aoc (PMU 0x15462280).
 */
#define AOC_REQ		0x154b0000ULL
#define PD_AOC		0x15462280ULL

static void aoc_request_on(void)
{
	u32 ms, st;
	int ok;

	st = safe_rd32(AOC_REQ + 0x40, &ok);
	if (!ok) {
		pr("touch: aoc_req: read aborted\n");
		return;
	}
	pr("touch: aoc_req 0x%08x status 0x%08x; pd_aoc conf 0x%08x status 0x%08x\n", rd32(AOC_REQ), st,
	   safe_rd32(PD_AOC, &ok), safe_rd32(PD_AOC + 4, &ok));
	wr32(AOC_REQ, 1);
	for (ms = 0; ms < 500 && !(st = rd32(AOC_REQ + 0x40)); ms++)
		delay_us(1000);
	pr("touch: aoc_req set: status 0x%08x after %u ms; pd_aoc status 0x%08x\n", st, ms, safe_rd32(PD_AOC + 4, &ok));
	log_flush();
}

static int aoc_touch_pads(void)
{
	u32 i, v;
	int ok;

	aoc_request_on();

	for (i = 0; i < sizeof(aoc_touch_on) / sizeof(aoc_touch_on[0]); i++) {
		u64 a = AOC_PAD_CSR + 4 * aoc_touch_on[i][0];

		v = safe_rd32(a, &ok);
		if (!ok) {
			pr("touch: AOC pad %u (0x%llx): read aborted (the AOC block is off?)\n", aoc_touch_on[i][0], a);
			return -1;
		}
		wr32(a, aoc_touch_on[i][1]);
		pr("touch: AOC pad %3u: 0x%08x -> 0x%02x, reads 0x%08x\n", aoc_touch_on[i][0], v, aoc_touch_on[i][1],
		   rd32(a));
	}
	aoc_touch_gpios();
	log_flush();
	return 0;
}

/*
 * The AOC's PCU (blk_aoc + 0x1400000, 11 pages) and GPIO block (+0x1c00000, 5 pages): every
 * non-zero word, under its AOC address, to diff against the same ranges read in Android with the
 * AOC console's "mem dump <addr> 256" while touch works there (docs/touch-plan.md).
 */
static const struct { u64 base; u32 pages; } aoc_dump_ranges[] = {
	{ 0x18400000ULL, 11 }, { 0x18c00000ULL, 5 },
};

static void aoc_regs_dump(void)
{
	u32 r, off, v, n = 0;
	int ok;

	for (r = 0; r < sizeof(aoc_dump_ranges) / sizeof(aoc_dump_ranges[0]); r++)
		for (off = 0; off < aoc_dump_ranges[r].pages * 0x1000; off += 4) {
			u64 a = aoc_dump_ranges[r].base + off;

			v = safe_rd32(a, &ok);
			if (!ok) {
				pr("touch: aocreg 0x%08llx: read aborted\n", a - 0x17000000ULL + 0x80000000ULL);
				off |= 0xffc;	/* skip the rest of this page */
				continue;
			}
			if (v) {
				pr("touch: aocreg 0x%08llx = 0x%08x\n", a - 0x17000000ULL + 0x80000000ULL, v);
				n++;
			}
		}
	pr("touch: aocreg: %u non-zero words\n", n);
	log_flush();
}

/*
 * HSI0's sysreg (0x11020000, regmap range 0-0xfffc in Android): USI2's signals reach AOC pads 72-75
 * without AP pins, so a route select may live here. Map which words exist (holes may come back as
 * SErrors) and log the non-zero ones; Android is then read only at offsets found here.
 */
#define SYSREG_HSI0	0x11020000ULL

static void hsi0_sysreg_dump(void)
{
	u32 off, v, n = 0, hole = 0, hole_start = 0;
	int ok;

	for (off = 0; off <= 0x10000; off += 4) {
		ok = 0;
		if (off < 0x10000)
			v = safe_rd32_async(SYSREG_HSI0 + off, &ok);
		if (!ok) {
			if (!hole++)
				hole_start = off;
			continue;
		}
		if (hole) {
			pr("touch: hsi0 sysreg +0x%04x..+0x%04x: aborted\n", hole_start, off - 4);
			hole = 0;
		}
		if (v) {
			pr("touch: hsi0 sysreg +0x%04x = 0x%08x\n", off, v);
			n++;
		}
	}
	if (hole)
		pr("touch: hsi0 sysreg +0x%04x..+0x%04x: aborted\n", hole_start, 0xfffc);
	pr("touch: hsi0 sysreg: %u non-zero words\n", n);
	log_flush();
}

/*
 * The readable words that differ from Android with touch working (regmap debugfs, one line at a
 * time, 2026-11-17): +0x104 is 0x8007ffff there (0 here), +0x700 is 0x11 there (0x3011 here).
 */
static const u32 hsi0_sysreg_android[][2] = { { 0x104, 0x8007ffff }, { 0x700, 0x00000011 } };

/*
 * AOC PCU +0xa000 (AP 0x1840a000): in Android, bit 8 is set with the screen on and clear while it
 * dozes, when the AOC takes the touch bus for tap-to-wake (AOC console dumps, 2026-11-17). Our boot
 * has it clear. Set it: the bus owner select, if that is what it is.
 */
#define AOC_PCU_A000	0x1840a000ULL

static void aoc_bus_to_ap(void)
{
	u32 v;
	int ok;

	v = safe_rd32(AOC_PCU_A000, &ok);
	if (!ok) {
		pr("touch: AOC PCU +0xa000: read aborted\n");
		return;
	}
	wr32(AOC_PCU_A000, v | 0x100);
	delay_us(1000);
	pr("touch: AOC PCU +0xa000: 0x%08x -> reads 0x%08x\n", v, rd32(AOC_PCU_A000));
	log_flush();
}

static void hsi0_sysreg_copy(void)
{
	u32 i, v;
	int ok;

	for (i = 0; i < sizeof(hsi0_sysreg_android) / sizeof(hsi0_sysreg_android[0]); i++) {
		u64 a = SYSREG_HSI0 + hsi0_sysreg_android[i][0];

		v = safe_rd32_async(a, &ok);
		if (!ok)
			continue;
		wr32(a, hsi0_sysreg_android[i][1]);
		pr("touch: hsi0 sysreg +0x%03x: 0x%08x -> 0x%08x, reads 0x%08x\n", hsi0_sysreg_android[i][0], v,
		   hsi0_sysreg_android[i][1], safe_rd32_async(a, &ok));
	}
	log_flush();
}

static void spi_identify(void)
{
	u8 buf[48];
	u32 got, i, t;

	if (spi_init())
		return;
	aoc_touch_pads();
	aoc_regs_dump();
	hsi0_sysreg_copy();
	aoc_bus_to_ap();
	/* the controller's own loopback (MODE_CFG bit 3): needs its clock, not the pins */
	wr32(TOUCH_SPI + SPI_MODE_CFG, rd32(TOUCH_SPI + SPI_MODE_CFG) | (1u << 3));
	got = spi_read(buf, 16);
	pr("touch: SPI loopback: %u of 16 bytes back (%02x %02x ...), STATUS 0x%08x: %s\n", got, buf[0], buf[1],
	   rd32(TOUCH_SPI + SPI_STATUS), got == 16 ? "the clock runs: the pins are the problem" : "the clock does NOT run");
	wr32(TOUCH_SPI + SPI_MODE_CFG, rd32(TOUCH_SPI + SPI_MODE_CFG) & ~(1u << 3));
	for (t = 0; t < 3; t++) {
		memset(buf, 0, sizeof(buf));
		got = spi_read(buf, sizeof(buf));
		pr("touch: SPI read %u: %u of %u bytes:", t, got, (u32)sizeof(buf));
		for (i = 0; i < got; i++)
			pr(" %02x", buf[i]);
		pr("\n");
		if (got >= 4 && buf[0] == 0xa5) {
			pr("touch: Synaptics TCM message: status 0x%02x, %u bytes of payload%s\n", buf[1],
			   buf[2] | (buf[3] << 8), buf[1] == 0x10 ? " (the identify report)" : "");
			break;
		}
		delay_us(20000);
	}
	if (t == 3) {
		/* the AOC's boot table has 72-75 on function 1 (0x89): try each function on them */
		u32 f, k;

		for (f = 0; f < 16; f++) {
			/* functions 0-3 on the pads, and the four feedback-clock delays (FB_CLK, +0x2c) */
			for (k = 72; k <= 75; k++)
				wr32(AOC_PAD_CSR + 4 * k, 0x88 | (f & 3));
			wr32(TOUCH_SPI + 0x2c, f >> 2);
			delay_us(1000);
			memset(buf, 0, sizeof(buf));
			got = spi_read(buf, 16);
			pr("touch: AOC pads 72-75 = 0x%02x, FB_CLK %u: %u of 16 bytes, STATUS 0x%08x:", 0x88 | (f & 3), f >> 2,
			   got, rd32(TOUCH_SPI + SPI_STATUS));
			for (i = 0; i < got; i++)
				pr(" %02x", buf[i]);
			pr("%s\n", got && buf[0] == 0xa5 ? "  <- the touch chip" : "");
			log_flush();
			if (got && buf[0] == 0xa5)
				break;
		}
	}
	pin("interrupt (after SPI)", PIN_IRQ_BANK, PIN_IRQ);
}
#endif

#if PROBE_TOUCH == 3
/*
 * Read-only: where is GPB10 (the touch SPI's pads, Google's zuma-caimito-common-touch.dtsi)? Not in
 * any pin controller Android registers: look at the 64 KiB slots of HSI0's address range that no
 * device-tree node claims. A GPIO block: banks of 0x20 bytes (CON, DAT, PUD, DRV, CONPDN, PUDPDN).
 */
static void hsi0_scan(void)
{
	static const u32 slots[] = {
		0x11010000, 0x11030000, 0x11050000, 0x11060000, 0x11080000, 0x11090000, 0x110a0000, 0x110b0000,
		0x110c0000, 0x110e0000, 0x11150000, 0x11160000, 0x11170000, 0x11180000, 0x11190000, 0x111a0000,
	};
	u32 i, j, v;
	int ok;

	pr("touch: HSI0 slots no node claims (read-only):\n");
	for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
		pr("touch:   0x%08x:", slots[i]);
		for (j = 0; j < 0x40; j += 4) {
			v = safe_rd32(slots[i] + j, &ok);
			if (!ok) {
				pr(" (aborted at +0x%x)", j);
				break;
			}
			pr(" %08x", v);
		}
		if (ok) {
			v = safe_rd32(slots[i] + 0x700, &ok);
			pr(ok ? "  +0x700 %08x" : "  +0x700 aborted", v);
		}
		pr("\n");
		log_flush();
	}
}
#endif

#if PROBE_TOUCH == 4
/*
 * Read-only: the three buttons (Android's gpio_keys; GPIO_ALIVE 0x154d0000, whose banks are gpa4 at
 * +0x80 and gpa6 at +0xa0 in this phone's pinctrl table): power gpa4-0, volume down gpa4-1, volume up
 * gpa6-2, all low while pressed. 20 s of looking, each change logged.
 */
static void buttons_watch(void)
{
	const u64 gpa4 = 0x154d0000ULL + 0x80, gpa6 = 0x154d0000ULL + 0xa0;
	u32 last = 0xff, now, ms;
	int ok;

	pr("touch: buttons: gpa4 CON 0x%08x DAT 0x%08x PUD 0x%08x; gpa6 CON 0x%08x DAT 0x%08x PUD 0x%08x\n",
	   safe_rd32(gpa4, &ok), rd32(gpa4 + 4), rd32(gpa4 + 8), rd32(gpa6), rd32(gpa6 + 4), rd32(gpa6 + 8));
	pr("touch: buttons: watching for 20 s (press them now; short presses)\n");
	log_flush();
	for (ms = 0; ms < 20000; ms += 10) {
		now = (rd32(gpa4 + 4) & 3) | ((rd32(gpa6 + 4) >> 2 & 1) << 2);
		if (now != last) {
			pr("touch: buttons at %u ms: power %s, volume down %s, volume up %s\n", ms, now & 1 ? "up" : "DOWN",
			   now & 2 ? "up" : "DOWN", now & 4 ? "up" : "DOWN");
			log_flush();
			last = now;
		}
		if (!(ms % 1000))
			wdt_kick();
		delay_us(10000);
	}
}
#endif

/*
 * Two gates before starting the AOC ourselves (docs/aoc-boot-map.md). Only harmless calls:
 * - Trusty: SMC_FC_GET_VERSION_STR (SMC_FASTCALL_NR(SMC_ENTITY_SECURE_MONITOR = 60, 10)), which only
 *   reads the version string; an unknown call returns SMC_UNK (-1).
 * - GSA mailbox (0x16490000): gsa.ko's wake (0x96) and sleep (0x97) commands, no arguments, the
 *   reply polled at +0x30 bit 0 instead of taken as an interrupt.
 */
#define TRUSTY_FC_GET_VERSION_STR	0xbc00000aULL
#define GSA_MBOX		0x16490000ULL

static u64 smc1(u64 fn, u64 a1)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a1;

	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1) : : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9",
			 "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void trusty_version(void)
{
	char str[96];
	u64 len = smc1(TRUSTY_FC_GET_VERSION_STR, (u64)-1), i;

	pr("touch: trusty GET_VERSION_STR(-1) = 0x%llx\n", len);
	if ((s64)len <= 0 || len > 0x1000) {
		log_flush();
		return;
	}
	for (i = 0; i < len && i < sizeof(str) - 1; i++)
		str[i] = (char)smc1(TRUSTY_FC_GET_VERSION_STR, i);
	str[i] = 0;
	pr("touch: trusty version: \"%s\"\n", str);
	log_flush();
}

static int gsa_mbox_cmd(u32 cmd)
{
	u32 ms, st = 0;
	int ok;

	safe_rd32_async(GSA_MBOX + 0x30, &ok);
	if (!ok) {
		pr("touch: GSA mailbox: read aborted\n");
		return -1;
	}
	pr("touch: GSA mailbox before 0x%x: +0x28 0x%08x +0x30 0x%08x +0x80 0x%08x +0x84 0x%08x\n", cmd,
	   rd32(GSA_MBOX + 0x28), rd32(GSA_MBOX + 0x30), rd32(GSA_MBOX + 0x80), rd32(GSA_MBOX + 0x84));
	wr32(GSA_MBOX + 0x80, cmd);
	wr32(GSA_MBOX + 0x84, 0);
	wr32(GSA_MBOX + 0x24, 1);
	wr32(GSA_MBOX + 0x28, rd32(GSA_MBOX + 0x28) & ~1u);
	wr32(GSA_MBOX + 0x40, 1);
	for (ms = 0; ms < 2000 && !((st = rd32(GSA_MBOX + 0x30)) & 1); ms++)
		delay_us(1000);
	pr("touch: GSA mailbox 0x%x: +0x30 0x%08x after %u ms; reply cmd 0x%08x status 0x%08x count %u\n", cmd, st,
	   ms, rd32(GSA_MBOX + 0x80), rd32(GSA_MBOX + 0x84), rd32(GSA_MBOX + 0x88));
	wr32(GSA_MBOX + 0x24, 1);
	wr32(GSA_MBOX + 0x28, rd32(GSA_MBOX + 0x28) | 1);
	log_flush();
	return (st & 1) && rd32(GSA_MBOX + 0x80) == (cmd | 0x80000000u) ? 0 : -1;
}

static void aoc_gates(void)
{
	trusty_version();
	if (gsa_mbox_cmd(0x96) == 0)
		gsa_mbox_cmd(0x97);
	trusty_aoc_probe();
}

void touch_probe(void)
{
	u32 ipc_channels, ap_max, ch_off, i;
	int ok, v;

	pr("\n--- touch screen (read-only) ---\n");
	log_flush();
#if PROBE_TOUCH == 5
	aoc_gates();
	return;
#elif PROBE_TOUCH == 4
	buttons_watch();
	return;
#elif PROBE_TOUCH >= 3
	hsi0_scan();
	return;
#endif
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
			int l4 = pmic_read(PMIC_PM, PM_L4M_CTRL), l25;

			pr("touch: LDO4M  (TS_AVDD 3.3 V)  CTRL 0x%02x: %s\n", l4, ldo_state(l4));
			l25 = pmic_read(PMIC_PM, PM_L25M_CTRL);
			pr("touch: LDO25M (TSP_DVDD 1.8 V) CTRL 0x%02x: %s\n", l25, ldo_state(l25));
#if PROBE_TOUCH >= 2
			/* only with the voltages this plan checked (docs/touch-plan.md): 3.3 V and 1.8 V */
			if ((l4 & 0x3f) == 0x3c && (l25 & 0x3f) == 0x2c) {
				pin("reset", PIN_RESET_BANK, PIN_RESET);
				pin("interrupt", PIN_IRQ_BANK, PIN_IRQ);
				power_on(l4, l25);
				spi_identify();
			} else {
				pr("touch: LDO voltages are not 0x3c/0x2c: not powering anything\n");
			}
#endif
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
