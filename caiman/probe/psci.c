/* SPDX-License-Identifier: MIT */
/* PSCI (via the conduit named in the DT's /psci node) and the exit path. */
#include "probe.h"

static int use_hvc = PLAT_PSCI_HVC;

void psci_set_conduit_hvc(int hvc)
{
	use_hvc = hvc;
}

s64 psci_call(u64 fn, u64 a1, u64 a2, u64 a3)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;
	register u64 x3 __asm__("x3") = a3;

	if (use_hvc)
		__asm__ volatile("hvc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
				 :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	else
		__asm__ volatile("smc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
				 :
				 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
				   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (s64)x0;
}

void psci_report(void)
{
	s64 v = psci_call(PSCI_VERSION, 0, 0, 0);

	g.psci_version = v;
	g.psci_reset2 = -1;
	pr("psci: conduit=%s version=%lld.%lld (raw 0x%llx)\n", use_hvc ? "hvc" : "smc",
	   (v >> 16) & 0xffff, v & 0xffff, (u64)v);
	if (v >= 0x10000) {
		g.psci_reset2 = psci_call(PSCI_FEATURES, PSCI_SYSTEM_RESET2_64, 0, 0);
		pr("psci: SYSTEM_RESET2 %s (PSCI_FEATURES=%lld)\n",
		   g.psci_reset2 >= 0 ? "supported" : "not supported", g.psci_reset2);
	}
}

void probe_finish(const char *why)
{
	int mode = PROBE_FINISH;

	pr("\nfinish: %s; ", why);
	if (mode == 2) {
		pr("hanging here (PROBE_FINISH=2). Hold Power+Vol-down to reset.\n");
		log_flush();
		for (;;)
			__asm__ volatile("wfe");
	}
	pr("rebooting in %u ms (%s)\n", PROBE_FINISH_DELAY_MS,
	   mode == 0 && g.psci_reset2 >= 0 ? "PSCI SYSTEM_RESET2 warm" : "PSCI SYSTEM_RESET");
	pr("=== caiman-probe end ===\n");
	log_flush();
	{
		u32 ms;

		for (ms = 0; ms < PROBE_FINISH_DELAY_MS; ms += 250) {
			if (PROBE_WDT_KICK)
				wdt_kick();
			delay_us(250000);
		}
	}

#ifdef PROBE_TO_FASTBOOT
	/* into fastboot, not the boot partition (which may hold iOS now): reboot mode "bootloader" (watcher.c) */
	psci_call(0x82000504, 0x15460810, 1, 0xfc);
#endif
	if (mode == 0 && g.psci_reset2 >= 0)
		psci_call(PSCI_SYSTEM_RESET2_64, 0 /* SYSTEM_WARM_RESET */, 0, 0);
	psci_call(PSCI_SYSTEM_RESET, 0, 0, 0);

	pr("finish: PSCI reset returned; hanging\n");
	log_flush();
	for (;;)
		__asm__ volatile("wfe");
}
