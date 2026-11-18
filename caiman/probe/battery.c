// SPDX-License-Identifier: MIT
/*
 * PROBE_BATTERY=1: the battery, read (never written) from the MAX77779's fuel gauge and charger on hsi2c_6, and
 * shared with XNU the way HackiOS's loader for another phone shares it (drivers/power/hakios.battery.c: struct
 * loader_battery in a page whose address goes in boot-arg battery_shared_pa; the kernel makes the IOPMPowerSource
 * that powerd, the status bar and Settings > Battery read).
 *
 * hsi2c_6 (0x108d0000, "samsung,exynos5-hsi2c", USI v2) is what Linux's i2c-exynos5.c drives; the bootloader has
 * used it (it shows the charge while off-mode charging), so its clock, pins and timing are as it left them and only
 * the transfer itself is done here, polled, as i2c-exynos5.c's HSI2C_POLLING mode does. Registers of the gauge
 * (0x36) and the charger (0x69) from Google's max77779_v20240115_regs.h; units from max77779_fg.c / maxfg_common.h.
 */
#include "probe.h"

typedef short s16;
typedef int s32;

#define HSI2C			0x108d0000ULL
#define HSI2C_CTL		0x00
#define HSI2C_FIFO_CTL		0x04
#define HSI2C_INT_ENABLE	0x20
#define HSI2C_INT_STATUS	0x24
#define HSI2C_FIFO_STATUS	0x30
#define HSI2C_TX_DATA		0x34
#define HSI2C_RX_DATA		0x38
#define HSI2C_CONF		0x40
#define HSI2C_AUTO_CONF		0x44
#define HSI2C_TIMEOUT		0x48
#define HSI2C_TRANS_STATUS	0x50
#define HSI2C_TIMING_FS1	0x60
#define HSI2C_TIMING_FS2	0x64
#define HSI2C_TIMING_FS3	0x68
#define HSI2C_ADDR		0x70
#define USI_CON			0xc4

#define CTL_MASTER		(1u << 3)
#define CTL_RXCHON		(1u << 6)
#define CTL_TXCHON		(1u << 7)
#define CTL_SW_RST		(1u << 31)
#define CONF_AUTO_MODE		(1u << 31)
#define AUTO_READ		(1u << 16)
#define AUTO_STOP		(1u << 17)
#define AUTO_RUN		(1u << 31)
#define INT_TRANSFER_DONE	(1u << 7)
#define INT_NODEV		(1u << 10)
#define FIFO_RX_EMPTY		(1u << 24)
#define FIFO_TX_LEVEL		0x7fu
#define FIFO_TX_EMPTY		(1u << 8)

#define FG			0x36
#define FG_REPCAP		0x06
#define FG_REPSOC		0x07
#define FG_FULLCAPREP		0x10
#define FG_CYCLES		0x17
#define FG_DESIGNCAP		0x18
#define FG_VCELL		0x1a
#define FG_TEMP			0x1b
#define FG_CURRENT		0x1c
#define FG_DEVNAME		0x21
#define FG_RSENSE		200	/* 10 uOhm units: max77779_fg.c's default (the DT gives none) */
#define CHG			0x69
#define CHG_INT_OK		0xb6	/* bit 6 CHGIN_OK, bit 5 WCIN_OK */
#define CHG_DETAILS_01		0xb8	/* bits 3:0 CHG_DTLS */

/* drivers/power/hakios.battery.c's struct loader_battery (version 1), then what this loader adds */
#define LOADER_BATTERY_MAGIC		0x2154544142574452ULL
#define LOADER_BATTERY_VALID		(1u << 0)
#define LOADER_BATTERY_PRESENT		(1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT	(1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID	(1u << 3)
#define LOADER_BATTERY_CHARGING		(1u << 4)

struct loader_battery {
	u64 magic;
	u32 version, size;
	volatile u32 sequence;
	u32 flags;
	s32 soc_percent, voltage_mv, current_ma, temperature_c;
	u32 charger_action, polls, failures, reserved;
	/* (size tells the kernel these are there) */
	s32 soc_hundredths, charge_mah, full_mah, design_mah, cycles, temperature_hundredths;
};

static u8 shared_page[16384] __attribute__((aligned(16384)));
static int bus_ok = -1;

static void dmb_sy(void)
{
	__asm__ volatile("dmb sy" ::: "memory");
}

static void dc_civac_range(const void *p, u64 n)
{
	u64 a = (u64)(uptr)p & ~63ULL, e = (u64)(uptr)p + n;

	for (; a < e; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	dsb_sy();
}

static u32 hr(u32 off)
{
	return rd32(HSI2C + off);
}

static void hw(u32 off, u32 v)
{
	wr32(HSI2C + off, v);
}

/* One message, as i2c-exynos5.c's exynos5_i2c_xfer_msg() in polling mode. 0 on success. */
static int msg(u8 addr, int read, u8 *buf, u32 len, int stop)
{
	u32 ctl = hr(HSI2C_CTL), conf = hr(HSI2C_AUTO_CONF), a, i = 0;
	u64 start = timer_ticks(), limit = timer_freq() / 100;	/* 10 ms */

	hw(HSI2C_TIMEOUT, hr(HSI2C_TIMEOUT) & ~(1u << 31));
	hw(HSI2C_FIFO_CTL, 3u | (1u << 4) | (1u << 16));	/* both FIFOs on, trigger levels 1 */
	if (read) {
		ctl = (ctl & ~CTL_TXCHON) | CTL_RXCHON;
		conf |= AUTO_READ;
	} else {
		ctl = (ctl & ~CTL_RXCHON) | CTL_TXCHON;
		conf &= ~AUTO_READ;
	}
	hw(HSI2C_INT_STATUS, hr(HSI2C_INT_STATUS));		/* pending ones cleared */
	conf = stop ? conf | AUTO_STOP : conf & ~AUTO_STOP;
	a = hr(HSI2C_ADDR) & ~((0x3ffu << 10) | 0x3ffu | (0xffu << 24));
	hw(HSI2C_ADDR, a | (0x7u << 24) | ((u32)(addr & 0x7f) << 10));
	hw(HSI2C_CTL, ctl);
	hw(HSI2C_INT_ENABLE, INT_TRANSFER_DONE);
	hw(HSI2C_AUTO_CONF, (conf & ~0xffffu) | len);
	hw(HSI2C_AUTO_CONF, hr(HSI2C_AUTO_CONF) | AUTO_RUN);
	if (read) {
		while (i < len && timer_ticks() - start < limit)
			if (!(hr(HSI2C_FIFO_STATUS) & FIFO_RX_EMPTY))
				buf[i++] = (u8)hr(HSI2C_RX_DATA);
		return i == len && !(hr(HSI2C_INT_STATUS) & INT_NODEV) ? 0 : -1;
	}
	while (i < len && timer_ticks() - start < limit)
		if ((hr(HSI2C_FIFO_STATUS) & FIFO_TX_LEVEL) < 64)
			hw(HSI2C_TX_DATA, buf[i++]);
	while (timer_ticks() - start < limit) {
		u32 st = hr(HSI2C_INT_STATUS);

		if (st & INT_NODEV)
			return -2;
		if ((st & INT_TRANSFER_DONE) && (hr(HSI2C_FIFO_STATUS) & FIFO_TX_EMPTY)) {
			hw(HSI2C_INT_STATUS, st);
			return 0;
		}
	}
	return -1;
}

/* A register: its address written, then 1 or 2 bytes read (little-endian), a repeated start between. */
static int reg_read(u8 addr, u8 reg, u32 n, u32 *val)
{
	u8 b[2] = { 0, 0 };

	if (msg(addr, 0, &reg, 1, 0) || msg(addr, 1, b, n, 1)) {
		/* (as the driver does after a failure: reset, its CTL and CONF set again) */
		hw(HSI2C_CTL, hr(HSI2C_CTL) | CTL_SW_RST);
		hw(HSI2C_CTL, (hr(HSI2C_CTL) & ~CTL_SW_RST) | CTL_MASTER);
		hw(HSI2C_CONF, hr(HSI2C_CONF) | CONF_AUTO_MODE);
		return -1;
	}
	*val = b[0] | ((u32)b[1] << 8);
	return 0;
}

static int bus_ready(void)
{
	int ok = 1;

	if (bus_ok >= 0)
		return bus_ok;
	/* (a block whose clock is off can fault instead of answering: the first look is one that survives that) */
	safe_rd32_async(HSI2C + HSI2C_CTL, &ok);
	if (!ok) {
		pr("battery: hsi2c_6 does not answer (its clock off?): not used\n");
		return bus_ok = 0;
	}
	pr("battery: hsi2c_6 as the bootloader left it: CTL 0x%08x CONF 0x%08x FS1 0x%08x FS2 0x%08x FS3 0x%08x "
	   "TRANS 0x%08x USI_CON 0x%08x\n", hr(HSI2C_CTL), hr(HSI2C_CONF), hr(HSI2C_TIMING_FS1), hr(HSI2C_TIMING_FS2),
	   hr(HSI2C_TIMING_FS3), hr(HSI2C_TRANS_STATUS), hr(USI_CON));
	if (!hr(HSI2C_TIMING_FS3) && !hr(HSI2C_TIMING_FS2)) {
		pr("battery: hsi2c_6 has no timing set (its clock off?): not used\n");
		return bus_ok = 0;
	}
	if (hr(USI_CON) & 1)
		hw(USI_CON, 0);				/* USI_RESET released, as exynos_usi_init() */
	hw(HSI2C_CTL, (hr(HSI2C_CTL) & ~CTL_SW_RST) | CTL_MASTER);
	hw(HSI2C_CONF, hr(HSI2C_CONF) | CONF_AUTO_MODE);
	return bus_ok = 1;
}

static void publish(struct loader_battery *b)
{
	volatile struct loader_battery *s = (volatile struct loader_battery *)(uptr)shared_page;

	s->sequence++;
	dmb_sy();
	b->sequence = s->sequence;
	memcpy((void *)(uptr)&s->magic, &b->magic, 16);	/* magic, version, size */
	s->flags = b->flags;
	s->soc_percent = b->soc_percent;
	s->voltage_mv = b->voltage_mv;
	s->current_ma = b->current_ma;
	s->temperature_c = b->temperature_c;
	s->charger_action = b->charger_action;
	s->polls = b->polls;
	s->failures = b->failures;
	s->soc_hundredths = b->soc_hundredths;
	s->charge_mah = b->charge_mah;
	s->full_mah = b->full_mah;
	s->design_mah = b->design_mah;
	s->cycles = b->cycles;
	s->temperature_hundredths = b->temperature_hundredths;
	dmb_sy();
	s->sequence++;
	dc_civac_range(shared_page, sizeof(struct loader_battery));
}

/* One reading of the gauge and the charger into the shared page; say: also a line in the log. 0 on success. */
int battery_poll(int say)
{
	static u32 polls, failures;
	struct loader_battery b;
	u32 soc, vcell, cur, temp, cap, full, design, cycles, ok = 0, dtls = 0;
	s32 ma;

	memset(&b, 0, sizeof(b));
	b.magic = LOADER_BATTERY_MAGIC;
	b.version = 1;
	b.size = sizeof(b);
	polls++;
	if (!bus_ready() || reg_read(FG, FG_REPSOC, 2, &soc) || reg_read(FG, FG_VCELL, 2, &vcell) ||
	    reg_read(FG, FG_CURRENT, 2, &cur) || reg_read(FG, FG_TEMP, 2, &temp) || reg_read(FG, FG_REPCAP, 2, &cap) ||
	    reg_read(FG, FG_FULLCAPREP, 2, &full) || reg_read(FG, FG_DESIGNCAP, 2, &design) ||
	    reg_read(FG, FG_CYCLES, 2, &cycles)) {
		failures++;
		if (say)
			pr("battery: the fuel gauge did not answer (%u of %u readings failed)\n", failures, polls);
		return -1;
	}
	/* the charger's state: what is plugged in and what it is doing (a failure here only leaves that out) */
	if (!reg_read(CHG, CHG_INT_OK, 1, &ok))
		reg_read(CHG, CHG_DETAILS_01, 1, &dtls);
	/* capacities: 5 uVh / Rsense a unit (0.5 mAh at 10 mOhm): 5 / (FG_RSENSE * 10 uOhm) mAh */
	/* the gauge counts current out of the battery as positive: max77779_fg.c reports -Current */
	ma = -(s32)(((s64)(s16)cur * 156250) / FG_RSENSE / 1000);
	b.soc_hundredths = (s32)(soc * 100 / 256);
	b.soc_percent = (s32)((soc + 128) / 256);
	if (b.soc_percent > 100)
		b.soc_percent = 100;
	b.voltage_mv = (s32)((u64)vcell * 78125 / 1000000);
	b.current_ma = ma;
	b.temperature_hundredths = (s32)((s16)temp * 100 / 256);
	b.temperature_c = (s32)((s16)temp / 256);
	b.charge_mah = (s32)((u64)cap * 500 / FG_RSENSE);
	b.full_mah = (s32)((u64)full * 500 / FG_RSENSE);
	b.design_mah = (s32)((u64)design * 500 / FG_RSENSE);
	b.cycles = (s32)(cycles / 100);		/* 1% of a cycle a unit (reg_to_cycles) */
	b.flags = LOADER_BATTERY_VALID | LOADER_BATTERY_PRESENT | LOADER_BATTERY_VOLTAGE_VALID;
	if (ok & ((1u << 6) | (1u << 5)))
		b.flags |= LOADER_BATTERY_CHARGER_PRESENT;
	if ((b.flags & LOADER_BATTERY_CHARGER_PRESENT) && (dtls & 0xf) <= 3)	/* pre, fast CC, fast CV, top-off */
		b.flags |= LOADER_BATTERY_CHARGING;
	b.charger_action = dtls & 0xf;
	b.polls = polls;
	b.failures = failures;
	publish(&b);
	if (say)
		pr("battery: %d.%02d%% (%d of %d mAh, design %d), %d mV, %d mA, %d.%02d C, %d cycles; charger INT_OK 0x%02x "
		   "CHG_DTLS %u, flags 0x%x%s\n", b.soc_hundredths / 100, b.soc_hundredths % 100, b.charge_mah, b.full_mah, b.design_mah,
		   b.voltage_mv, b.current_ma, b.temperature_hundredths / 100,
		   b.temperature_hundredths < 0 ? -b.temperature_hundredths % 100 : b.temperature_hundredths % 100, b.cycles, ok,
		   dtls & 0xf, b.flags, (b.flags & LOADER_BATTERY_CHARGING) ? " (charging)" : "");
	return 0;
}

/* Where XNU finds it (boot-arg battery_shared_pa) */
u64 battery_shared_pa(void)
{
	return (u64)(uptr)shared_page;
}

/* The gauge's name, to see that it is the chip it should be (max77779_check_devname: radix in the high byte) */
void battery_probe(void)
{
	u32 name = 0;

	if (bus_ready() && !reg_read(FG, FG_DEVNAME, 2, &name))
		pr("battery: fuel gauge at 0x%02x: DevName 0x%04x\n", FG, name);
	battery_poll(1);
}
