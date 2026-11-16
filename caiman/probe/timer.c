/* SPDX-License-Identifier: MIT */
/* Generic timer: CNTVCT_EL0 is readable from EL1 and EL2 alike. */
#include "probe.h"

static u64 t0, freq;

void timer_init(void)
{
	freq = read_sysreg(cntfrq_el0) & 0xffffffffULL;
	if (!freq)
		freq = PLAT_CNTFRQ_FALLBACK;
	t0 = timer_ticks();
}

u64 timer_ticks(void)
{
	isb();
	return read_sysreg(cntvct_el0);
}

u64 timer_freq(void)
{
	return freq;
}

u64 timer_us(void)
{
	u64 d = timer_ticks() - t0;

	/* split to avoid overflow: d * 1e6 overflows after ~12 min at 24 MHz */
	return (d / freq) * 1000000ULL + ((d % freq) * 1000000ULL) / freq;
}

void delay_us(u64 us)
{
	u64 start = timer_ticks();
	u64 n = (us * freq) / 1000000ULL;

	while (timer_ticks() - start < n)
		;
}
