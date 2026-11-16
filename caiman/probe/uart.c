/* SPDX-License-Identifier: MIT */
/*
 * Polled UART output. We never program the baud rate or clocks: if the
 * bootloader left the UART enabled (`fastboot oem uart enable` on Pixels)
 * we just push bytes into its FIFO, like Linux's earlycon does.
 */
#include "probe.h"

static u64 ubase;
static int dead;

#if defined(PLAT_UART_SAMSUNG)
/* Samsung/Exynos UART (Linux: drivers/tty/serial/samsung_tty.c, exynos4210 earlycon) */
#define ULCON		0x00
#define UCON		0x04
#define UFCON		0x08
#define UTRSTAT		0x10
#define UFSTAT		0x18
#define UTXH		0x20
#define UCON_TXMODE(v)	(((v) >> 2) & 3)	/* 0 = transmitter disabled */
#define UFCON_FIFO_EN	(1u << 0)
#define UTRSTAT_TXFE	(1u << 1)		/* TX holding buffer empty */
#define UFSTAT_TXFULL	(1u << 24)

static int fifo;

int uart_init(u64 base)
{
	int ok1, ok2, ok3;
	u32 ulcon, ucon, ufcon;

	ubase = base;
	pr("uart: samsung @ 0x%llx: reading ULCON/UCON/UFCON\n", base);
	log_flush();
	ulcon = safe_rd32(base + ULCON, &ok1);
	ucon = safe_rd32(base + UCON, &ok2);
	ufcon = safe_rd32(base + UFCON, &ok3);
	if (!ok1 || !ok2 || !ok3) {
		pr("uart: register read aborted: UART block not accessible\n");
		return 0;
	}
	pr("uart: ULCON=0x%08x UCON=0x%08x UFCON=0x%08x\n", ulcon, ucon, ufcon);
	if (UCON_TXMODE(ucon) == 0) {
		pr("uart: transmitter disabled (UCON tx mode 0): bootloader did not enable the\n"
		   "      debug UART. Try `fastboot oem uart enable` before `fastboot boot`.\n");
		return 0;
	}
	fifo = (ufcon & UFCON_FIFO_EN) != 0;
	return 1;
}

static int tx_ready(void)
{
	if (fifo)
		return !(rd32(ubase + UFSTAT) & UFSTAT_TXFULL);
	return (rd32(ubase + UTRSTAT) & UTRSTAT_TXFE) != 0;
}

static void tx(char c)
{
	wr32(ubase + UTXH, (u8)c);	/* 32-bit access only on this USI UART */
}

#elif defined(PLAT_UART_PL011)
#define DR		0x000
#define FR		0x018
#define CR		0x030
#define FR_TXFF		(1u << 5)

int uart_init(u64 base)
{
	int ok;
	u32 cr;

	ubase = base;
	cr = safe_rd32(base + CR, &ok);
	if (!ok)
		return 0;
	pr("uart: pl011 @ 0x%llx CR=0x%08x\n", base, cr);
	return 1;
}

static int tx_ready(void)
{
	return !(rd32(ubase + FR) & FR_TXFF);
}

static void tx(char c)
{
	wr32(ubase + DR, (u8)c);
}
#else
#error "no UART type selected for this platform"
#endif

void uart_putc(char c)
{
	u64 start, limit;

	if (dead)
		return;
	/* 115200 baud drains a byte in ~87 us; give up after 10 ms. */
	start = timer_ticks();
	limit = timer_freq() / 100;
	while (!tx_ready()) {
		if (timer_ticks() - start > limit) {
			dead = 1;
			return;
		}
	}
	tx(c);
}
