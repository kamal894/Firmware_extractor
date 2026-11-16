/* SPDX-License-Identifier: MIT */
/* Exception reporting and guarded MMIO reads. */
#include "probe.h"

static volatile int guard_active, guard_faulted;
static int in_exc;

u32 safe_rd32(u64 addr, int *ok)
{
	u32 v = 0xdeadbeefu;

	guard_faulted = 0;
	guard_active = 1;
	__asm__ volatile("ldr %w0, [%1]" : "+r"(v) : "r"(addr) : "memory");
	guard_active = 0;
	if (guard_faulted) {
		g.faults++;
		*ok = 0;
		return 0xdeadbeefu;
	}
	*ok = 1;
	return v;
}

static const char *exc_kind(u64 idx)
{
	switch (idx & 3) {
	case 0: return "sync";
	case 1: return "irq";
	case 2: return "fiq";
	default: return "serror";
	}
}

static const char *exc_from(u64 idx)
{
	switch (idx >> 2) {
	case 0: return "current EL (SP0)";
	case 1: return "current EL (SPx)";
	case 2: return "lower EL (AArch64)";
	default: return "lower EL (AArch32)";
	}
}

static const char *ec_name(u32 ec)
{
	switch (ec) {
	case 0x00: return "unknown/undefined instruction";
	case 0x01: return "WFI/WFE trap";
	case 0x07: return "FP/SIMD access trap";
	case 0x0e: return "illegal execution state";
	case 0x15: return "SVC";
	case 0x16: return "HVC";
	case 0x17: return "SMC";
	case 0x18: return "MSR/MRS/system instruction trap";
	case 0x20: return "instruction abort (lower EL)";
	case 0x21: return "instruction abort (same EL)";
	case 0x22: return "PC alignment fault";
	case 0x24: return "data abort (lower EL)";
	case 0x25: return "data abort (same EL)";
	case 0x26: return "SP alignment fault";
	case 0x2f: return "SError";
	case 0x3c: return "BRK";
	default: return "other";
	}
}

void exc_handler(u64 idx, u64 esr, u64 far, u64 elr, u64 spsr, u64 *frame)
{
	u32 ec = (u32)(esr >> 26) & 0x3f;
	int i;

	/* A guarded read took a synchronous external abort: skip the load. */
	if ((idx & 3) == 0 && (ec == 0x24 || ec == 0x25) && guard_active && !in_exc) {
		guard_faulted = 1;
		frame[31] = elr + 4;
		return;
	}

	if (++in_exc > 1) {
		log_uart_enable(0);
		pr("\n!!! nested exception idx=%llu esr=0x%llx far=0x%llx elr=0x%llx\n",
		   idx, esr, far, elr);
		log_flush();
		for (;;)
			__asm__ volatile("wfe");
	}

	/* Don't let the report itself fault on a broken UART. */
	if (far >= PLAT_UART_BASE && far < PLAT_UART_BASE + 0x1000)
		log_uart_enable(0);

	pr("\n!!! EXCEPTION: %s from %s at EL%u\n", exc_kind(idx), exc_from(idx), g.el);
	pr("    ESR=0x%016llx  EC=0x%02x (%s)  IL=%llu  ISS=0x%07llx\n",
	   esr, ec, ec_name(ec), (esr >> 25) & 1, esr & 0x1ffffff);
	if (ec == 0x24 || ec == 0x25)
		pr("    data abort: DFSC=0x%02llx %s%s\n", esr & 0x3f,
		   (esr & (1 << 6)) ? "write" : "read",
		   (esr & 0x3f) == 0x10 ? " (synchronous external abort)" : "");
	pr("    FAR=0x%016llx\n", far);
	pr("    ELR=0x%016llx  (image offset 0x%llx)\n", elr, elr - g.load_base);
	pr("    SPSR=0x%016llx\n", spsr);
	for (i = 0; i < 31; i += 2) {
		if (i == 30)
			pr("    x30=%016llx\n", frame[30]);
		else
			pr("    x%-2u=%016llx  x%-2u=%016llx\n", i, frame[i], i + 1, frame[i + 1]);
	}
	log_flush();
	probe_finish("exception");
}
