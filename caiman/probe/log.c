/* SPDX-License-Identifier: MIT */
/*
 * Log sinks:
 *
 * 1. The pstore/ramoops console zone. We write it in the exact format of
 *    Linux's struct persistent_ram_buffer (fs/pstore/ram_core.c):
 *        u32 sig = 0x43474244 ("DBGC"); u32 start; u32 size; u8 data[];
 *    After the phone reboots into Android, Linux finds a valid buffer and
 *    publishes it as /sys/fs/pstore/console-ramoops-0. With the MMU off
 *    every store goes straight to DRAM, so whatever was logged before a
 *    hang survives (as long as the reset keeps DRAM powered).
 *
 * 2. The debug UART, if the bootloader left it enabled.
 *
 * Everything is also mirrored into a small RAM buffer so it can be
 * replayed when the ramoops zone is (re)located from the device tree.
 */
#include "probe.h"

#define PRZ_SIG		0x43474244u
#define PRZ_HDR		12u
#define MIRROR_SZ	(32 * 1024)

static struct {
	volatile u8 *zone;
	u64 base;
	u32 cap, pos;
	int attached;
} rz;

static char mirror[MIRROR_SZ];
static u32 mirror_len;
static int uart_on, uart_quiet;

static void rz_sync(void)
{
	volatile u32 *h = (volatile u32 *)rz.zone;

	/* start == size: Linux reads data[start..size) + data[0..start) = all of it */
	h[1] = rz.pos;
	h[2] = rz.pos;
}

void log_attach_ramoops(u64 zone_base, u64 zone_size)
{
	volatile u32 *h;
	u32 i;

	if (zone_size <= PRZ_HDR + 4096 || (zone_base & 3))
		return;
	rz.zone = (volatile u8 *)(uptr)zone_base;
	rz.base = zone_base;
	rz.cap = (u32)((zone_size - PRZ_HDR) > 0x1000000 ? 0x1000000 : zone_size - PRZ_HDR);
	rz.pos = 0;
	h = (volatile u32 *)rz.zone;
	h[1] = 0;
	h[2] = 0;
	h[0] = PRZ_SIG;
	rz.attached = 1;
	for (i = 0; i < mirror_len && rz.pos < rz.cap; i++)
		rz.zone[PRZ_HDR + rz.pos++] = (u8)mirror[i];
	rz_sync();
}

int log_ramoops_attached(void)
{
	return rz.attached;
}

u64 log_ramoops_base(void)
{
	return rz.attached ? rz.base : 0;
}

u64 log_space_left(void)
{
	return rz.attached ? rz.cap - rz.pos : 0;
}

void log_uart_enable(int on)
{
	uart_on = on;
}

void log_uart_mute(int mute)
{
	uart_quiet = mute;
}

/* Send everything logged so far (up to the mirror size) to the UART. */
void log_uart_replay(void)
{
	u32 i;

	if (!uart_on)
		return;
	for (i = 0; i < mirror_len; i++) {
		if (mirror[i] == '\n')
			uart_putc('\r');
		uart_putc(mirror[i]);
	}
}

void log_flush(void)
{
	if (rz.attached)
		rz_sync();
	dsb_sy();
}

void log_putc(char c)
{
	if (mirror_len < MIRROR_SZ)
		mirror[mirror_len++] = c;
	if (rz.attached && rz.pos < rz.cap) {
		rz.zone[PRZ_HDR + rz.pos++] = (u8)c;
		if (c == '\n')
			rz_sync();
	}
	if (uart_on && !uart_quiet) {
		if (c == '\n')
			uart_putc('\r');
		uart_putc(c);
	}
}

void log_puts(const char *s)
{
	while (*s)
		log_putc(*s++);
}

static void put_num(u64 v, unsigned base, int width, int zero, int neg, int upper)
{
	char buf[24];
	int n = 0, pad;

	do {
		unsigned d = (unsigned)(v % base);

		buf[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
		v /= base;
	} while (v);

	pad = width - n - (neg ? 1 : 0);
	if (!zero)
		while (pad-- > 0)
			log_putc(' ');
	if (neg)
		log_putc('-');
	if (zero)
		while (pad-- > 0)
			log_putc('0');
	while (n)
		log_putc(buf[--n]);
}

void log_vprintf(const char *fmt, va_list ap)
{
	for (; *fmt; fmt++) {
		int zero = 0, width = 0, lng = 0, left = 0;
		u64 v;

		if (*fmt != '%') {
			log_putc(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == '-') {
			left = 1;
			fmt++;
		}
		if (*fmt == '0') {
			zero = 1;
			fmt++;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z') {
			lng++;
			fmt++;
		}

		switch (*fmt) {
		case 'd':
		case 'i': {
			s64 sv = lng ? va_arg(ap, s64) : (s64)va_arg(ap, int);

			put_num(sv < 0 ? (u64)-sv : (u64)sv, 10, width, zero, sv < 0, 0);
			break;
		}
		case 'u':
			v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
			put_num(v, 10, width, zero, 0, 0);
			break;
		case 'x':
		case 'X':
			v = lng ? va_arg(ap, u64) : (u64)va_arg(ap, unsigned);
			put_num(v, 16, width, zero, 0, *fmt == 'X');
			break;
		case 'p':
			log_puts("0x");
			put_num((u64)(uptr)va_arg(ap, void *), 16, 16, 1, 0, 0);
			break;
		case 'c':
			log_putc((char)va_arg(ap, int));
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len;

			if (!s)
				s = "(null)";
			len = (int)strlen(s);
			if (!left)
				while (width-- > len)
					log_putc(' ');
			log_puts(s);
			if (left)
				while (width-- > len)
					log_putc(' ');
			break;
		}
		case '%':
			log_putc('%');
			break;
		case '\0':
			return;
		default:
			log_putc('%');
			log_putc(*fmt);
			break;
		}
	}
}

void log_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_vprintf(fmt, ap);
	va_end(ap);
}
