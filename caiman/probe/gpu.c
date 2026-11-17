/* SPDX-License-Identifier: MIT */
/*
 * The GPU, step 1 (PROBE_GPU=1): read only. The Mali-G715 (7 cores, r1p3, GPU_ID 0x0b080a02 under
 * Android: arch 11, "Valhall" with a command stream frontend) at 0x1f000000 (DT mali@1F000000, 0x480000).
 * Its two power domains are PMU blocks (DT pd-g3d@15461A80, "top", which needs an SMC to switch, and
 * pd-embedded_g3d@15461C80, "cores"; gpu_pmu_status_reg_offset 0x1c84, mask 1): their words are read here,
 * and the GPU's own first registers only through safe_rd32_async(), which survives the SError a block that
 * is off gives back. Nothing is written.
 *
 * Mali registers (Linux's panthor_regs.h / kbase mali_kbase_gpu_regmap.h): GPU_ID 0x0, L2_FEATURES 0x4,
 * CORE_FEATURES 0x8, TILER_FEATURES 0xc, MEM_FEATURES 0x10, MMU_FEATURES 0x14, AS_PRESENT 0x18,
 * CSF_ID 0x1c, GPU_IRQ_RAWSTAT 0x20, GPU_STATUS 0x34, SHADER_PRESENT 0x100, TILER_PRESENT 0x110,
 * L2_PRESENT 0x120, SHADER_READY 0x140, L2_READY 0x160, SHADER_PWRTRANS 0x200, L2_PWRTRANS 0x220,
 * MCU_CONTROL 0x700, MCU_STATUS 0x704.
 *
 * Step 2 (PROBE_GPU=2): the first writes, as Linux's panthor does before the firmware (panthor_gpu.c):
 * a soft reset (GPU_CMD 0x30 = 0x101, then RESET_COMPLETED, bit 8 of IRQ_RAWSTAT), then the L2
 * (L2_PWRON 0x1a0), the tiler (TILER_PWRON 0x190) and the shader cores (SHADER_PWRON 0x180), each
 * waited for in its READY word (L2 0x160, tiler 0x150, shaders 0x140) with its PWRTRANS word clear.
 * The GPU's clock and voltage are ACPM's (DVFS domain 7, G3D): its rate is read and printed.
 */
#include "probe.h"

#if PROBE_GPU
#define GPU_BASE	0x1f000000ULL
#define PD_G3D		0x15461a80ULL
#define PD_EMB_G3D	0x15461c80ULL
#define CMU_G3D		0x1ee00000ULL

#define GPU_IRQ_RAWSTAT	0x20
#define GPU_IRQ_CLEAR	0x24
#define GPU_IRQ_MASK	0x28
#define GPU_CMD		0x30
#define  GPU_SOFT_RESET	0x101
#define  IRQ_RESET_DONE	(1u << 8)
#define SHADER_PRESENT	0x100
#define TILER_PRESENT	0x110
#define L2_PRESENT	0x120
#define SHADER_READY	0x140
#define TILER_READY	0x150
#define L2_READY	0x160
#define SHADER_PWRON	0x180
#define TILER_PWRON	0x190
#define L2_PWRON	0x1a0
#define SHADER_PWRTRANS	0x200
#define TILER_PWRTRANS	0x210
#define L2_PWRTRANS	0x220

static const struct {
	char name[16];
	u32 off;
} gpu_reg[] = {
	{ "GPU_ID", 0x0 }, { "L2_FEATURES", 0x4 }, { "CORE_FEATURES", 0x8 }, { "TILER_FEATURES", 0xc },
	{ "MEM_FEATURES", 0x10 }, { "MMU_FEATURES", 0x14 }, { "AS_PRESENT", 0x18 }, { "CSF_ID", 0x1c },
	{ "IRQ_RAWSTAT", 0x20 }, { "GPU_STATUS", 0x34 }, { "SHADER_PRESENT", 0x100 }, { "TILER_PRESENT", 0x110 },
	{ "L2_PRESENT", 0x120 }, { "SHADER_READY", 0x140 }, { "L2_READY", 0x160 }, { "SHADER_PWRTRANS", 0x200 },
	{ "L2_PWRTRANS", 0x220 }, { "MCU_CONTROL", 0x700 }, { "MCU_STATUS", 0x704 },
};

static void words(const char *what, u64 base, u32 n)
{
	u32 i;
	int ok;

	pr("gpu: %s @ 0x%llx:", what, base);
	for (i = 0; i < n; i += 4) {
		u32 v = safe_rd32_async(base + i, &ok);

		if (ok)
			pr(" %08x", v);
		else
			pr(" --------");
	}
	pr("\n");
}

#if PROBE_GPU >= 3
static void gpu_firmware(void);
#endif
#if PROBE_GPU >= 2
static u32 gpu_rd(u32 off)
{
	return rd32(GPU_BASE + off);
}

static void gpu_wr(u32 off, u32 v)
{
	wr32(GPU_BASE + off, v);
}

/* One block on (panthor_gpu_block_power_on): its PWRON word, then READY == mask with PWRTRANS clear. */
static int gpu_block_on(const char *name, u32 pwron, u32 ready, u32 trans, u32 mask)
{
	u64 start = timer_ticks();

	gpu_wr(pwron, mask);
	while (timer_ticks() - start < timer_freq() / 50) {
		if ((gpu_rd(ready) & mask) == mask && !(gpu_rd(trans) & mask)) {
			pr("gpu: %s on after %llu us: READY 0x%08x\n", name,
			   (timer_ticks() - start) * 1000000 / timer_freq(), gpu_rd(ready));
			return 0;
		}
	}
	pr("gpu: %s did not come on in 20 ms: READY 0x%08x PWRTRANS 0x%08x\n", name, gpu_rd(ready), gpu_rd(trans));
	return -1;
}

static void gpu_power_up(void)
{
	u64 start;

	pr("gpu: soft reset\n");
	gpu_wr(GPU_IRQ_MASK, 0);
	gpu_wr(GPU_IRQ_CLEAR, ~0u);
	gpu_wr(GPU_CMD, GPU_SOFT_RESET);
	start = timer_ticks();
	while (!(gpu_rd(GPU_IRQ_RAWSTAT) & IRQ_RESET_DONE) && timer_ticks() - start < timer_freq() / 10)
		;
	if (!(gpu_rd(GPU_IRQ_RAWSTAT) & IRQ_RESET_DONE)) {
		pr("gpu: no RESET_COMPLETED in 100 ms (IRQ_RAWSTAT 0x%08x)\n", gpu_rd(GPU_IRQ_RAWSTAT));
		return;
	}
	pr("gpu: reset done after %llu us; GPU_ID 0x%08x\n", (timer_ticks() - start) * 1000000 / timer_freq(),
	   gpu_rd(0));
	gpu_wr(GPU_IRQ_CLEAR, ~0u);
	/* kbase, with the DT's l2-hash-values: ASN_HASH 0x2c0.. and L2_CONFIG (0x48) bit 24, while the L2 is off */
	gpu_wr(0x2c0, 0x36db6d);
	gpu_wr(0x2c4, 0x5b6db6);
	gpu_wr(0x2c8, 0);
	gpu_wr(0x48, gpu_rd(0x48) | (1u << 24));
	gpu_wr(0x304, 31);				/* COHERENCY_ENABLE: none */
	pr("gpu: L2_CONFIG now 0x%08x (Android reads 0x01140000)\n", gpu_rd(0x48));
	if (gpu_block_on("L2", L2_PWRON, L2_READY, L2_PWRTRANS, gpu_rd(L2_PRESENT)) ||
	    gpu_block_on("tiler", TILER_PWRON, TILER_READY, TILER_PWRTRANS, gpu_rd(TILER_PRESENT)) ||
	    gpu_block_on("shader cores", SHADER_PWRON, SHADER_READY, SHADER_PWRTRANS, gpu_rd(SHADER_PRESENT)))
		return;
	pr("gpu: all on; IRQ_RAWSTAT 0x%08x GPU_STATUS 0x%08x\n", gpu_rd(GPU_IRQ_RAWSTAT), gpu_rd(0x34));
	log_flush();
#if PROBE_GPU >= 3
	gpu_firmware();
#endif
}
#endif

#if PROBE_GPU >= 3
/*
 * Step 3 (PROBE_GPU=3): the firmware, as Linux's panthor_fw.c loads it. mali_csffw-r54p3.bin (the one
 * Android loads: "git_sha: 690855d0") travels after the probe in the kernel payload ("GPUBLOB",
 * tools/pack_aoc_payload.py). Its header (magic 0xc3f13a6e, u32 size of the entries) is followed by
 * entries, a u32 each first: type in bits 7:0, size in bits 15:8. Type 0, "interface memory": flags (RD 0,
 * WR 1, EX 2, cache mode 4:3, PROT 5, SHARED 30, ZERO 31), the MCU's VA start/end, the file's data
 * start/end. Each such section gets pages of its own from free DRAM (0xf0000000 up, below the modem's
 * 0xf6200000 and outside XNU's), its data copied in and the rest zeroed, and is mapped at its VA in the
 * GPU MMU's address space 0: AArch64 4K tables, 48-bit VA (MMU_FEATURES 0x2830), every page
 * non-cacheable (MEMATTR 0x4c in all eight slots: inner/outer non-cacheable) like this probe's own view
 * of memory, so nothing needs cleaning. Protected-mode sections (PROT) are left out, as panthor does
 * without protected memory. Then MCU_CONTROL = AUTO (2), and the firmware is up when it raises the
 * global interface interrupt (JOB_IRQ_RAWSTAT bit 31) and has written its interface's version at the
 * start of the shared section (VA 0x04000000).
 */
#define GPU_BLOB_OFFSET	0x100000ULL
#ifndef GPU_POOL_BASE
#define GPU_POOL_BASE	0xf0000000ULL	/* free DRAM; -DGPU_POOL_BASE= for another place (run gpu5: 0xd0000000) */
#endif
#define FW_MAGIC	0xc3f13a6eu
#define POOL_BASE	GPU_POOL_BASE
#define POOL_END	(GPU_POOL_BASE + 0x2000000ULL)
#define JOB_IRQ_RAWSTAT	0x1000
#define JOB_IRQ_CLEAR	0x1004
#define JOB_IRQ_MASK	0x1008
#define MMU_IRQ_RAWSTAT	0x2000
#define MMU_IRQ_CLEAR	0x2004
#define AS0		0x2400
#define AS_TRANSTAB	0x00
#define AS_MEMATTR	0x08
#define AS_COMMAND	0x18
#define AS_FAULTSTATUS	0x1c
#define AS_FAULTADDR	0x20
#define AS_STATUS	0x28
#define AS_TRANSCFG	0x30
#define COHERENCY_EN	0x304
#define MCU_CONTROL	0x700
#define MCU_STATUS	0x704
#define SHARED_VA	0x04000000u

static u64 pool = POOL_BASE, root;

static u32 le32(const u8 *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24;
}

static u64 page_alloc(u32 pages)
{
	u64 pa = pool;

	if (pool + (u64)pages * 0x1000 > POOL_END)
		return 0;
	pool += (u64)pages * 0x1000;
	memset((void *)(uptr)pa, 0, (u64)pages * 0x1000);
	return pa;
}

static u64 *table(u64 pa)
{
	return (u64 *)(uptr)pa;
}

/* The level-3 entry of va, the tables on the way made as needed. */
static u64 *pte_of(u32 va)
{
	u64 t = root;
	int level;

	for (level = 0; level < 3; level++) {
		u32 idx = (u32)(((u64)va >> (39 - 9 * level)) & 0x1ff);
		u64 e = table(t)[idx];

		if (!(e & 1)) {
			u64 next = page_alloc(1);

			if (!next)
				return NULL;
			table(t)[idx] = next | 0x403;		/* kbase entry_set_pte: table, AF */
			e = next | 0x403;
		}
		t = e & 0xfffffffff000ULL;
	}
	return &table(t)[(va >> 12) & 0x1ff];
}

/*
 * kbase's level-3 entry (entry_set_ate; docs/mali-boot.md): type 3, AF, MEMATTR index in bits 4:2, AP 01
 * (read/write) or 11 (read only), SH 0, XN (bit 54) unless executable. Index 0 is MEMATTR's 0x88 (the
 * non-coherent default), 5 its 0x4c for the shared section (flags bit 30).
 */
static int map_pages(u32 va, u64 pa, u32 bytes, u32 flags)
{
	u64 attr = 3 | (1u << 10) | (flags & 2 ? 1u << 6 : 3u << 6) | (flags & (1u << 30) ? 5u << 2 : 0) |
		   (flags & 4 ? 0 : 1ULL << 54);
	u32 i;

	for (i = 0; i < bytes; i += 0x1000) {
		u64 *pte = pte_of(va + i);

		if (!pte)
			return -1;
		*pte = (pa + i) | attr;
	}
	return 0;
}

static int as_command(u32 cmd)
{
	u64 start = timer_ticks();

	gpu_wr(AS0 + AS_COMMAND, cmd);
	while (gpu_rd(AS0 + AS_STATUS) & 1)
		if (timer_ticks() - start > timer_freq() / 100)
			return -1;
	return 0;
}

static void gpu_faults(const char *when)
{
	pr("gpu: %s: MCU_STATUS %u, JOB_IRQ 0x%08x, GPU_IRQ 0x%08x, GPU_FAULT 0x%08x @0x%08x, MMU_IRQ 0x%08x, "
	   "AS0 fault 0x%08x @0x%08x%08x status 0x%x\n", when, gpu_rd(MCU_STATUS), gpu_rd(JOB_IRQ_RAWSTAT),
	   gpu_rd(GPU_IRQ_RAWSTAT), gpu_rd(0x3c), gpu_rd(0x40), gpu_rd(MMU_IRQ_RAWSTAT), gpu_rd(AS0 + AS_FAULTSTATUS),
	   gpu_rd(AS0 + AS_FAULTADDR + 4), gpu_rd(AS0 + AS_FAULTADDR), gpu_rd(AS0 + AS_STATUS));
}

/*
 * The GPU's S2MPU (DT s2mpu_s0_g3d@1ee70000, "google,s2mpu-v9"): a stage-2 memory protection unit between
 * the GPU and DRAM that Android's pKVM programs from EL2 (kvm-arm.protected_modules=pkvm_s2mpu-v9). Run
 * gpu3: the GPU's first page-table read (PA 0xf0000000) came back a bus fault (GPU_FAULTSTATUS 0x6e000e80).
 * Its words are printed (CTRL0 0x0: bit 0 ENABLE; CTRL1 0x4; VERSION 0x60; STATUS 0x64; NUM_CONTEXT
 * 0x100; FAULT_STATUS 0x2000; the first L1 entries of context 0 at 0x4000, as pkvm's s2mpu.c names them),
 * and if it is on it is switched off (CTRL0 bit 0 clear), so that the GPU reaches all of memory as it does
 * with nobody's hypervisor in between. Guesses from the layout of the pKVM driver; checked by what reads back.
 */
#define S2MPU_G3D	0x1ee70000ULL

static void s2mpu_open(void)
{
	int ok;
	u32 ctrl0;

	words("s2mpu_g3d CTRL0..", S2MPU_G3D, 0x20);
	words("s2mpu_g3d VERSION/STATUS", S2MPU_G3D + 0x60, 0x8);
	words("s2mpu_g3d NUM_CONTEXT..", S2MPU_G3D + 0x100, 0x10);
	words("s2mpu_g3d FAULT", S2MPU_G3D + 0x2000, 0x20);
	words("s2mpu_g3d L1 ctx0", S2MPU_G3D + 0x4000, 0x40);
	ctrl0 = safe_rd32_async(S2MPU_G3D, &ok);
	if (ok && (ctrl0 & 1)) {
		wr32(S2MPU_G3D, ctrl0 & ~1u);
		dsb_sy();
		pr("gpu: s2mpu_g3d CTRL0 0x%08x -> 0x%08x (ENABLE cleared)\n", ctrl0, safe_rd32_async(S2MPU_G3D, &ok));
	}
	log_flush();
}

static void gpu_firmware(void)
{
	const u8 *b = (const u8 *)(uptr)(g.load_base + GPU_BLOB_OFFSET), *fw = b + 0x1000;
	u64 fw_size, shared_pa = 0, start;
	u32 off, end, sections = 0;

	if (memcmp(b, "GPUBLOB", 8) || le32(fw) != FW_MAGIC) {
		pr("gpu: no Mali firmware after the probe (GPUBLOB)\n");
		return;
	}
	fw_size = le32(b + 8);
	end = le32(fw + 16);
	pr("gpu: firmware %llu bytes, version 0x%08x, entries to 0x%x\n", fw_size, le32(fw + 8), end);
	s2mpu_open();
	root = page_alloc(1);
	for (off = 20; off < end && off < fw_size;) {
		u32 h = le32(fw + off), type = h & 0xff, size = (h >> 8) & 0xff;

		if (size < 4)
			break;
		if (type == 0) {
			u32 flags = le32(fw + off + 4), va = le32(fw + off + 8), va_end = le32(fw + off + 12);
			u32 ds = le32(fw + off + 16), de = le32(fw + off + 20), bytes = va_end - va;
			u64 pa;

			if (flags & (1u << 5)) {
				pr("gpu:   va 0x%08x-0x%08x flags 0x%08x: protected mode, left out\n", va, va_end, flags);
				off += size;
				continue;
			}
			pa = page_alloc((bytes + 0xfff) / 0x1000);
			if (!pa || de < ds || de - ds > bytes || de > fw_size) {
				pr("gpu:   va 0x%08x-0x%08x: no memory or bad data range\n", va, va_end);
				return;
			}
			memcpy((void *)(uptr)pa, fw + ds, de - ds);
			if (map_pages(va, pa, bytes, flags))
				return;
			if (va == SHARED_VA)
				shared_pa = pa;
			pr("gpu:   va 0x%08x-0x%08x flags 0x%08x data 0x%x bytes -> pa 0x%llx\n", va, va_end, flags, de - ds, pa);
			sections++;
		}
		off += size;
	}
	dsb_sy();
	pr("gpu: %u sections, tables at 0x%llx, %llu KiB of the pool used\n", sections, root, (pool - POOL_BASE) / 1024);

	/* address space 0 for the firmware's VM */
	/* kbase_mmu_get_as_setup / kbase_mmu_hw_configure, non-coherent: TRANSCFG, TRANSTAB, MEMATTR in that order */
	gpu_wr(AS0 + AS_TRANSCFG, 0x42000006);
	gpu_wr(AS0 + AS_TRANSCFG + 4, 0);
	gpu_wr(AS0 + AS_TRANSTAB, (u32)root);
	gpu_wr(AS0 + AS_TRANSTAB + 4, (u32)(root >> 32));
	gpu_wr(AS0 + AS_MEMATTR, 0x888d8f88);
	gpu_wr(AS0 + AS_MEMATTR + 4, 0x00084c8d);
	pr("gpu: AS0 TRANSCFG 0x%08x TRANSTAB 0x%08x%08x MEMATTR 0x%08x%08x; VA 0 pte 0x%llx\n", gpu_rd(AS0 + AS_TRANSCFG),
	   gpu_rd(AS0 + AS_TRANSTAB + 4), gpu_rd(AS0 + AS_TRANSTAB), gpu_rd(AS0 + AS_MEMATTR + 4), gpu_rd(AS0 + AS_MEMATTR),
	   *pte_of(0));
	while (gpu_rd(AS0 + AS_STATUS) & 1)
		;
	if (as_command(1)) {				/* UPDATE */
		pr("gpu: address space 0 did not take the update\n");
		return;
	}
	gpu_wr(MMU_IRQ_CLEAR, ~0u);
	gpu_wr(JOB_IRQ_CLEAR, ~0u);
	gpu_wr(JOB_IRQ_MASK, 0);
	gpu_faults("before the MCU");
	log_flush();

	gpu_wr(MCU_CONTROL, 2);				/* AUTO */
	start = timer_ticks();
	while (!(gpu_rd(JOB_IRQ_RAWSTAT) & (1u << 31)) && timer_ticks() - start < timer_freq()) {
		if (gpu_rd(MMU_IRQ_RAWSTAT) || gpu_rd(MCU_STATUS) == 3)
			break;
	}
	gpu_faults("after MCU_CONTROL = AUTO");
	if (gpu_rd(JOB_IRQ_RAWSTAT) & (1u << 31)) {
		const u32 *glb = (const u32 *)(uptr)shared_pa;

		pr("gpu: FIRMWARE BOOTED after %llu us: global interface version 0x%08x features 0x%08x input va 0x%08x "
		   "output va 0x%08x, %u CSGs (stride 0x%x)\n", (timer_ticks() - start) * 1000000 / timer_freq(),
		   glb[0], glb[1], glb[2], glb[3], glb[4], glb[5]);
	} else {
		pr("gpu: the firmware did not raise its interrupt in 1 s\n");
	}
	log_flush();
}
#endif

void gpu_probe(void)
{
	u32 i;
	int ok;

	pr("\n--- GPU (read only) ---\n");
	words("pd-g3d (top)", PD_G3D, 0x20);
	words("pd-embedded_g3d (cores)", PD_EMB_G3D, 0x20);
	log_flush();
	/*
	 * A block that is powered off may not answer at all (a hung bus, not an SError): its registers are
	 * read only when both domains' status words (+4; Android's kbase looks at bit 0 of 0x15461c84) say on.
	 */
	if (!(safe_rd32_async(PD_G3D + 4, &ok) & 1) || !ok || !(safe_rd32_async(PD_EMB_G3D + 4, &ok) & 1) || !ok) {
		pr("gpu: a power domain of the GPU is off: its registers are not read\n");
		log_flush();
		return;
	}
	words("CMU_G3D", CMU_G3D, 0x40);
	log_flush();
	for (i = 0; i < sizeof(gpu_reg) / sizeof(gpu_reg[0]); i++) {
		u32 v = safe_rd32_async(GPU_BASE + gpu_reg[i].off, &ok);

		if (ok)
			pr("gpu: +0x%03x %-15s 0x%08x\n", gpu_reg[i].off, gpu_reg[i].name, v);
		else
			pr("gpu: +0x%03x %-15s (aborted: the block is off or not ours)\n", gpu_reg[i].off, gpu_reg[i].name);
		if (!ok && i == 0)
			break;
	}
	log_flush();
#if PROBE_GPU >= 2
	gpu_power_up();
#endif
}
#endif
