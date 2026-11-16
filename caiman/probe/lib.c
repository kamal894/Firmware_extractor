/* SPDX-License-Identifier: MIT */
/* Minimal libc pieces. Byte-wise on purpose: MMU-off memory is Device
 * memory, where unaligned accesses fault. */
#include "probe.h"

void *memset(void *d, int c, unsigned long n)
{
	u8 *p = d;

	while (n--)
		*p++ = (u8)c;
	return d;
}

void *memcpy(void *d, const void *s, unsigned long n)
{
	u8 *dp = d;
	const u8 *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, unsigned long n)
{
	u8 *dp = d;
	const u8 *sp = s;

	if (dp == sp || n == 0)
		return d;
	if (dp < sp) {
		while (n--)
			*dp++ = *sp++;
	} else {
		dp += n;
		sp += n;
		while (n--)
			*--dp = *--sp;
	}
	return d;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
	const u8 *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

unsigned long strlen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, unsigned long n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (u8)*a - (u8)*b;
		if (!*a)
			return 0;
	}
	return 0;
}
