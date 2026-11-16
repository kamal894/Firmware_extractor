/* SPDX-License-Identifier: MIT */
/*
 * caiman-probe main sequence.
 *
 * Order matters: the ramoops log is attached first (pure DRAM writes, no
 * risk) so that every later, riskier step leaves a breadcrumb that
 * survives a hang or a fault. Then: CPU facts, device tree facts, UART,
 * PSCI, the bootloader's own log, the display registers, a summary, and
 * a reboot back to Android.
 */
#include "probe.h"

struct probe_state g;

int overlaps(u64 a, u64 as, u64 b, u64 bs)
{
	return as && bs && a < b + bs && b < a + as;
}

/*
 * Is all of [base, base+size) DRAM by the device tree? The ranges may be split (caiman lists its
 * high bank as seven adjacent 2 GiB ranges), so walk from range to range.
 */
int addr_in_dram(u64 base, u64 size)
{
	u64 at = base, end = base + size;
	int i, progress = 1;

	while (at < end && progress) {
		progress = 0;
		for (i = 0; i < g.nmem; i++) {
			if (at >= g.mem[i].base && at < g.mem[i].base + g.mem[i].size) {
				at = g.mem[i].base + g.mem[i].size;
				progress = 1;
				break;
			}
		}
	}
	return at >= end;
}

static void hexdump(u64 addr, u32 len)
{
	u32 i, j;

	for (i = 0; i < len; i += 16) {
		const volatile u8 *p = (const volatile u8 *)(uptr)(addr + i);

		pr("  %08llx:", addr + i);
		for (j = 0; j < 16 && i + j < len; j++)
			pr(" %02x", p[j]);
		pr("  |");
		for (j = 0; j < 16 && i + j < len; j++)
			log_putc(p[j] >= 0x20 && p[j] < 0x7f ? (char)p[j] : '.');
		pr("|\n");
	}
}

/* ---- ramoops: replicate fs/pstore/ram.c's layout ---------------------- */

static u64 rounddown_pow2(u64 v)
{
	u64 p = 1;

	if (!v)
		return 0;
	while (p <= v / 2)
		p <<= 1;
	return p;
}

static int ramoops_console_zone(u64 base, u64 size, u64 record, u64 console, u64 ftrace,
				u64 pmsg, struct region *out)
{
	u64 dump, off = 0;

	record = rounddown_pow2(record);
	console = rounddown_pow2(console);
	ftrace = rounddown_pow2(ftrace);
	pmsg = rounddown_pow2(pmsg);
	if (!console || size < console + ftrace + pmsg)
		return -1;
	/* dmesg zones come first, then console, ftrace, pmsg */
	dump = size - console - ftrace - pmsg;
	if (dump && record) {
		u64 cnt = dump / record;

		if (!cnt)
			return -1;
		off = (dump / cnt) * cnt;
	}
	out->base = base + off;
	out->size = console;
	return 0;
}

static void ramoops_from_dt(void)
{
	const struct fdt *f = &g.fdt;
	u32 pac, psc, ph, rec = 0, con = 0, ftr = 0, pmsg = 0, ecc = 0;
	u64 a, s;
	int n, r;
	struct region z;

	n = fdt_find_compatible(f, "ramoops", -1, &pac, &psc);
	if (n < 0) {
		pr("ramoops: no compatible=\"ramoops\" node in DT\n");
		return;
	}
	if (fdt_reg(f, n, "reg", pac, psc, 0, &a, &s)) {
		if (fdt_read_u32(f, n, "memory-region", &ph) ||
		    (r = fdt_find_phandle(f, ph, &pac, &psc)) < 0 ||
		    fdt_reg(f, r, "reg", pac, psc, 0, &a, &s)) {
			pr("ramoops: node %s has no usable reg/memory-region\n", fdt_node_name(f, n));
			return;
		}
	}
	fdt_read_u32(f, n, "record-size", &rec);
	fdt_read_u32(f, n, "console-size", &con);
	fdt_read_u32(f, n, "ftrace-size", &ftr);
	fdt_read_u32(f, n, "pmsg-size", &pmsg);
	fdt_read_u32(f, n, "ecc-size", &ecc);
	pr("ramoops: DT %s: 0x%llx size 0x%llx record 0x%x console 0x%x ftrace 0x%x pmsg 0x%x ecc %u\n",
	   fdt_node_name(f, n), a, s, rec, con, ftr, pmsg, ecc);
	if (ramoops_console_zone(a, s, rec, con, ftr, pmsg, &z)) {
		pr("ramoops: DT layout has no console zone; Linux will not show our log\n");
		return;
	}
	g.ramoops.base = a;
	g.ramoops.size = s;
	g.ramoops_ecc = ecc;
	if (ecc)
		pr("ramoops: WARNING ecc-size=%u: Linux will run Reed-Solomon over our\n"
		   "         (un-encoded) text; expect 'uncorrectable' notes, text should remain\n", ecc);
	if (z.base != g.console.base || z.size != g.console.size) {
		pr("ramoops: console zone from DT is 0x%llx size 0x%llx; moving log there\n",
		   z.base, z.size);
		g.console = z;
		log_attach_ramoops(z.base, z.size);
	} else {
		pr("ramoops: console zone 0x%llx size 0x%llx matches the built-in default\n",
		   z.base, z.size);
	}
}

/* ---- CPU ------------------------------------------------------------- */

static const char *part_name(u64 midr)
{
	u32 impl = (u32)(midr >> 24) & 0xff, part = (u32)(midr >> 4) & 0xfff;

	if (impl != 0x41)
		return "non-Arm implementer";
	switch (part) {
	case 0xd05: return "Cortex-A55";
	case 0xd0b: return "Cortex-A76";
	case 0xd41: return "Cortex-A78";
	case 0xd44: return "Cortex-X1";
	case 0xd46: return "Cortex-A510";
	case 0xd47: return "Cortex-A710";
	case 0xd48: return "Cortex-X2";
	case 0xd4d: return "Cortex-A715";
	case 0xd4e: return "Cortex-X3";
	case 0xd80: return "Cortex-A520";
	case 0xd81: return "Cortex-A720";
	case 0xd82: return "Cortex-X4";
	case 0xd0f: return "AEMv8 (model)";
	default: return "unknown Arm part";
	}
}

static void cpu_report(void)
{
	u64 midr = read_sysreg(midr_el1), mpidr = read_sysreg(mpidr_el1);

	pr("\n--- cpu ---\n");
	pr("cpu: CurrentEL=%u  MPIDR=0x%llx (aff2.1.0 = %llu.%llu.%llu)\n", g.el, mpidr,
	   (mpidr >> 16) & 0xff, (mpidr >> 8) & 0xff, mpidr & 0xff);
	pr("cpu: MIDR=0x%08llx (%s r%llup%llu) REVIDR=0x%llx\n", midr, part_name(midr),
	   (midr >> 20) & 0xf, midr & 0xf, read_sysreg(revidr_el1));
	if (g.el == 2) {
		pr("cpu: SCTLR_EL2=0x%llx HCR_EL2=0x%llx CPTR_EL2=0x%llx CNTHCTL_EL2=0x%llx\n",
		   read_sysreg(sctlr_el2), read_sysreg(hcr_el2), read_sysreg(cptr_el2),
		   read_sysreg(cnthctl_el2));
		pr("cpu: entered at EL2 (hypervisor level): a loader for XNU must drop to EL1\n");
	} else {
		pr("cpu: SCTLR_EL1=0x%llx CPACR_EL1=0x%llx\n", read_sysreg(sctlr_el1),
		   read_sysreg(cpacr_el1));
	}
	pr("cpu: DAIF=0x%llx ISR_EL1=0x%llx CNTFRQ=%llu Hz CNTVCT=%llu\n", read_sysreg(daif),
	   read_sysreg(isr_el1), timer_freq(), timer_ticks());
	pr("cpu: ID_AA64PFR0=0x%016llx PFR1=0x%016llx\n", read_sysreg(id_aa64pfr0_el1),
	   read_sysreg(id_aa64pfr1_el1));
	pr("cpu: ID_AA64ISAR0=0x%016llx ISAR1=0x%016llx ISAR2=0x%016llx\n",
	   read_sysreg(id_aa64isar0_el1), read_sysreg(id_aa64isar1_el1),
	   read_sysreg(S3_0_C0_C6_2));
	pr("cpu: ID_AA64MMFR0=0x%016llx MMFR1=0x%016llx MMFR2=0x%016llx\n",
	   read_sysreg(id_aa64mmfr0_el1), read_sysreg(id_aa64mmfr1_el1),
	   read_sysreg(S3_0_C0_C7_2));
	pr("cpu: ID_AA64DFR0=0x%016llx ZFR0=0x%016llx SMFR0=0x%016llx\n",
	   read_sysreg(id_aa64dfr0_el1), read_sysreg(S3_0_C0_C4_4), read_sysreg(S3_0_C0_C4_5));
	pr("cpu: CTR_EL0=0x%llx DCZID_EL0=0x%llx CLIDR_EL1=0x%llx\n", read_sysreg(ctr_el0),
	   read_sysreg(dczid_el0), read_sysreg(clidr_el1));
}

/* ---- device tree ----------------------------------------------------- */

struct dt_ctx {
	int in_rm;		/* inside /reserved-memory */
	u64 bldr_base, bldr_size;
};

static void print_regs(const struct fdt *f, int node, u32 ac, u32 sc)
{
	u64 a, s;
	int i;

	for (i = 0; i < 8 && !fdt_reg(f, node, "reg", ac, sc, i, &a, &s); i++)
		pr("%s0x%llx+0x%llx", i ? ", " : " ", a, s);
}

static int visit_report(const struct fdt *f, int node, int depth, u32 pac, u32 psc, void *c)
{
	struct dt_ctx *x = c;
	const char *name = fdt_node_name(f, node);
	const u8 *p;
	u32 len;
	u64 a, s;
	int i;

	if (depth == 1) {
		x->in_rm = !strncmp(name, "reserved-memory", 15);
		if (fdt_prop_has_string(f, node, "device_type", "memory")) {
			for (i = 0; !fdt_reg(f, node, "reg", pac, psc, i, &a, &s); i++) {
				pr("dt: memory 0x%llx - 0x%llx (%llu MiB)\n", a, a + s, s >> 20);
				if (g.nmem < MAX_MEM_RANGES) {
					g.mem[g.nmem].base = a;
					g.mem[g.nmem].size = s;
					g.nmem++;
				}
			}
		}
		if (!strcmp(name, "chosen")) {
			p = fdt_getprop(f, node, "bootargs", &len);
			pr("dt: /chosen bootargs: %s\n", p ? (const char *)p : "(none)");
			p = fdt_getprop(f, node, "linux,initrd-start", &len);
			if (p) {
				a = fdt_cells(p, len >= 8 ? 2 : 1);
				p = fdt_getprop(f, node, "linux,initrd-end", &len);
				s = p ? fdt_cells(p, len >= 8 ? 2 : 1) : 0;
				pr("dt: /chosen initrd 0x%llx - 0x%llx\n", a, s);
			}
			p = fdt_getprop(f, node, "stdout-path", &len);
			if (p)
				pr("dt: /chosen stdout-path: %s\n", (const char *)p);
		}
		if (!strncmp(name, "psci", 4)) {
			p = fdt_getprop(f, node, "method", &len);
			pr("dt: psci method=%s\n", p ? (const char *)p : "(none)");
			if (p && !strcmp((const char *)p, "hvc"))
				g.psci_hvc = 1;
			else if (p && !strcmp((const char *)p, "smc"))
				g.psci_hvc = 0;
		}
	} else if (depth == 2 && x->in_rm) {
		pr("dt: rsv %-36s", name);
		print_regs(f, node, pac, psc);
		if (fdt_getprop(f, node, "no-map", &len))
			pr(" no-map");
		p = fdt_getprop(f, node, "compatible", &len);
		if (p)
			pr(" [%s]", (const char *)p);
		pr("\n");
		if (!strncmp(name, "bldr_log", 8) &&
		    !fdt_reg(f, node, "reg", pac, psc, 0, &a, &s)) {
			x->bldr_base = a;
			x->bldr_size = s;
		}
	}
	return 0;
}

static struct dt_ctx dtc;

static void dt_report(void)
{
	pr("\n--- device tree ---\n");
	if (!g.have_fdt) {
		pr("dt: no valid device tree at x0=0x%llx\n", g.dtb);
		return;
	}
	pr("dt: blob at 0x%llx, %u bytes, version %u\n", g.dtb, g.fdt.totalsize, g.fdt.version);
	g.psci_hvc = PLAT_PSCI_HVC;
	if (fdt_walk(&g.fdt, visit_report, &dtc) < 0)
		pr("dt: walk stopped on a malformed structure block\n");
	psci_set_conduit_hvc(g.psci_hvc);
	ramoops_from_dt();
}

/* ---- the bootloader's log ------------------------------------------- */

static int printable(u8 c)
{
	return (c >= 0x20 && c < 0x7f) || c == '\n' || c == '\t';
}

static void bldr_log_copy(u64 base, u64 size)
{
	u64 max = PROBE_BLDR_LOG_MAX, left = log_space_left(), i, out = 0;
	const volatile u8 *p = (const volatile u8 *)(uptr)base;
	u8 run[4];
	u32 rl = 0;
	int streaming = 0;
	char last = '\n';

	pr("\n--- bootloader log @ 0x%llx (0x%llx bytes) ---\n", base, size);
	if (!size || !addr_in_dram(base, size)) {
		pr("bldr: region not inside DT memory; skipped\n");
		return;
	}
	if (left < 192 * 1024) {
		pr("bldr: not enough room left in the ramoops zone; skipped\n");
		return;
	}
	if (max > left - 128 * 1024)
		max = left - 128 * 1024;
	pr("bldr: first 64 bytes:\n");
	hexdump(base, 64);
	pr("bldr: printable text (runs of >= 4 chars), up to %llu bytes:\n", max);
	log_flush();
	log_uart_mute(1);
	for (i = 0; i < size && out < max; i++) {
		u8 c = p[i];

		if (c == '\r')
			continue;
		if (!printable(c)) {
			if (streaming && last != '\n') {
				log_putc('\n');
				out++;
				last = '\n';
			}
			streaming = 0;
			rl = 0;
			continue;
		}
		if (streaming) {
			log_putc((char)c);
			last = (char)c;
			out++;
			continue;
		}
		run[rl++] = c;
		if (rl == 4) {
			u32 k;

			for (k = 0; k < 4; k++)
				log_putc((char)run[k]);
			last = (char)run[3];
			out += 4;
			streaming = 1;
			rl = 0;
		}
	}
	if (last != '\n')
		log_putc('\n');
	log_uart_mute(0);
	pr("--- end of bootloader log (%llu bytes copied) ---\n", out);
}

/* ---- summary --------------------------------------------------------- */

static void summary(void)
{
	pr("\n=== SUMMARY ===\n");
	pr("platform     : %s, probe %s\n", PLAT_NAME, PROBE_VERSION);
	pr("entry        : EL%u, MPIDR 0x%llx, image 0x%llx+0x%llx, DTB 0x%llx\n", g.el,
	   read_sysreg(mpidr_el1), g.load_base, g.image_size, g.dtb);
	pr("dram         : %d range(s) from DT\n", g.nmem);
	pr("ramoops log  : console zone 0x%llx size 0x%llx\n", g.console.base, g.console.size);
	pr("uart         : %s\n", g.uart_ok ? "TX enabled by bootloader (output sent)" :
					  PROBE_UART ? "not usable" : "disabled at build time");
	pr("psci         : version 0x%llx, SYSTEM_RESET2 %s\n", (u64)g.psci_version,
	   g.psci_reset2 >= 0 ? "yes" : "no");
	if (g.fb_addr)
		pr("fb (regs)    : 0x%llx %ux%u fmt %u bpp %u stride %u (dpp %d)\n", g.fb_addr,
		   g.fb_width, g.fb_height, g.fb_format, g.fb_bpp, g.fb_stride, g.fb_channel);
	else
		pr("fb (regs)    : not found\n");
	if (g.fb_handover.size)
		pr("fb (DT)      : 0x%llx size 0x%llx\n", g.fb_handover.base, g.fb_handover.size);
	else
		pr("fb (DT)      : no handover memory-region\n");
	pr("decon mode   : %s, %u frames in 100 ms\n",
	   g.decon_cmd_mode < 0 ? "unknown" : g.decon_cmd_mode ? "command" : "video",
	   g.frame_count_delta);
	pr("watchdog     : %s, period %u ms, %u ms left at probe time\n",
	   g.wdt_running ? "RUNNING (bootloader left it armed)" : "not running",
	   g.wdt_period_ms, g.wdt_left_ms);
	pr("aborted reads: %d\n", g.faults);
}

/* ---- entry ----------------------------------------------------------- */

void c_entry(u64 dtb, u64 load_base, u64 cur_el)
{
	struct region def;

	g.el = (u32)(cur_el >> 2) & 3;
	g.load_base = load_base;
	g.image_size = (u64)(__image_end - __image_start);
	g.dtb = dtb;
	g.psci_reset2 = -1;
	g.decon_cmd_mode = -1;
	g.fb_channel = -1;
	timer_init();

	g.have_fdt = fdt_init(&g.fdt, dtb) == 0;

	/* 1. ramoops at the built-in default, unless that would hit us or the DTB */
	if (!ramoops_console_zone(PLAT_RAMOOPS_BASE, PLAT_RAMOOPS_SIZE, PLAT_RAMOOPS_RECORD,
				  PLAT_RAMOOPS_CONSOLE, PLAT_RAMOOPS_FTRACE, PLAT_RAMOOPS_PMSG,
				  &def) &&
	    !overlaps(def.base, def.size, load_base, g.image_size) &&
	    !(g.have_fdt && overlaps(def.base, def.size, dtb, g.fdt.totalsize))) {
		g.console = def;
		log_attach_ramoops(def.base, def.size);
	}

	pr("=== caiman-probe %s on %s ===\n", PROBE_VERSION, PLAT_NAME);
	pr("hello from bare metal: loaded at 0x%llx (file 0x%llx bytes, 0x%llx with bss+stack),"
	   " x0/DTB=0x%llx\n", load_base, (u64)(__file_end - __image_start), g.image_size, dtb);
	{
		u64 sctlr = g.el == 2 ? read_sysreg(sctlr_el2) : read_sysreg(sctlr_el1);

		pr("entry: EL%u SCTLR=0x%llx MMU=%s D-cache=%s I-cache=%s%s\n", g.el, sctlr,
		   (sctlr & 1) ? "ON" : "off", (sctlr & 4) ? "ON" : "off",
		   (sctlr & (1 << 12)) ? "on" : "off",
		   (sctlr & 5) ? "  (boot protocol says MMU and D-cache off; log is flushed to RAM anyway)"
			       : "");
	}
	log_flush();

	cpu_report();
	dt_report();
	if (!log_ramoops_attached())
		pr("ramoops: NOT attached: this log only exists on the UART\n");

	pr("\n--- uart ---\n");
	if (PROBE_UART) {
		g.uart_ok = uart_init(PLAT_UART_BASE);
		if (g.uart_ok) {
			log_uart_enable(1);
			log_uart_replay();
			pr("uart: hello over serial from caiman-probe\n");
		}
	} else {
		pr("uart: disabled at build time (PROBE_UART=0)\n");
	}

	pr("\n--- psci ---\n");
	psci_report();

#ifdef PLAT_SELFTEST_ABORT_ADDR
	{
		int ok;

		safe_rd32(PLAT_SELFTEST_ABORT_ADDR, &ok);
		pr("selftest: guarded read of unmapped 0x%llx %s\n", PLAT_SELFTEST_ABORT_ADDR,
		   ok ? "did NOT fault (unexpected)" : "faulted and was skipped (ok)");
	}
#endif

	bldr_log_copy(dtc.bldr_size ? dtc.bldr_base : PLAT_BLDR_LOG_BASE,
		      dtc.bldr_size ? dtc.bldr_size : PLAT_BLDR_LOG_SIZE);

	wdt_report();
	if (PROBE_WDT_KICK)
		wdt_kick();

	display_probe();

	summary();
	log_flush();

#if PROBE_LOADER
	xnu_boot();	/* returns only if XNU could not be started */
#endif

	if (PROBE_CRASHTEST) {
		pr("\ncrashtest: executing an undefined instruction\n");
		__asm__ volatile("udf #0x42");
	}

	probe_finish("done");
}
