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
	if (gpu_block_on("L2", L2_PWRON, L2_READY, L2_PWRTRANS, gpu_rd(L2_PRESENT)))
		return;
#if PROBE_GPU < 5
	/*
	 * With the firmware running (step 5 on), the tiler and shader cores are left to it, as kbase leaves them
	 * (docs/mali-endpoints.md): run gpu13, cores powered here, a compute job never got any.
	 */
	if (gpu_block_on("tiler", TILER_PWRON, TILER_READY, TILER_PWRTRANS, gpu_rd(TILER_PRESENT)) ||
	    gpu_block_on("shader cores", SHADER_PWRON, SHADER_READY, SHADER_PWRTRANS, gpu_rd(SHADER_PRESENT)))
		return;
#endif
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
#if PROBE_GPU >= 4
#define Q_VA		0x05000000u	/* a stream's queue input and output pages, in AS0 */
#define SUSP_VA		0x05100000u	/* group 0's suspend buffer, in AS0 */
#define SUSP_PAGES	64
#define SHADER_VA	0x00300000u	/* step 5: the shader, in AS1 */
#define DESC_VA		0x00400000u	/* step 5: its SPD (+0) and TSD (+0x40), in AS1 */
static u64 q_pa, susp_pa, job_root, target_page, code_page, desc_page;
static volatile u32 *kick_csg_in, *kick_csg_out, *kick_glb_in, *kick_glb_out;
static void gpu_first_job(u64 shared_pa);
#endif

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
	pr("gpu: %s: AS1 fault 0x%08x @0x%08x%08x status 0x%x\n", when, gpu_rd(0x2440 + AS_FAULTSTATUS),
	   gpu_rd(0x2440 + AS_FAULTADDR + 4), gpu_rd(0x2440 + AS_FAULTADDR), gpu_rd(0x2440 + AS_STATUS));
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

#if PROBE_GPU >= 4
/*
 * Step 4 (PROBE_GPU=4): the first job, polled (docs/mali-csf-submit.md, from mali_kbase.ko r54p3 and Mesa's
 * arch 11 CS encodings). The firmware's interface is in its shared section (VA 0x04000000, here at
 * shared_pa): global control +0, CSG 0 control at 0x04001000, CS 0 control at CSG control + 0x40; the
 * input/output pages they point to. The job runs in AS1 (CSG_CONFIG = 1), tables of its own: a ring of one
 * page at VA 0x100000 and the target page at 0x200000, both non-cacheable (MEMATTR slot 5, 0x4c). Its
 * queue input/output pages and the group's suspend buffer are in AS0 (mapped before the MCU started).
 * The program: r0:r1 = 0x200000, r2 = 0x89abcdef, scoreboard slot 0 for stores, STORE_MULTIPLE r2 ->
 * [r0], WAIT slot 0; INSERT = 64. Kicked through the group's doorbell and the global one (bell 0 at
 * 0x1f080000, bell n at +n * 0x10000). Success: the CPU reads 0x89abcdef at the target.
 */
#define DOORBELL(n)	(0x80000u + (n) * 0x10000u)
#define RING_VA		0x00100000u
#define TARGET_VA	0x00200000u
#define AS1		0x2440

static u32 *sh(u64 shared_pa, u32 va)
{
	return (u32 *)(uptr)(shared_pa + (va - SHARED_VA));
}

/* toggle the bits of mask in *req to differ from ack, as kbase does for a request */
static void toggle(volatile u32 *req, u32 ack, u32 mask)
{
	*req = (*req & ~mask) | ((ack ^ mask) & mask);
}

static int wait_bits(volatile u32 *req, volatile u32 *ack, u32 mask, const char *what)
{
	u64 start = timer_ticks();

	while (((*req ^ *ack) & mask) && timer_ticks() - start < timer_freq() / 10)
		;
	if ((*req ^ *ack) & mask) {
		pr("gpu: %s: no ack in 100 ms (req 0x%08x ack 0x%08x)\n", what, *req, *ack);
		return -1;
	}
	return 0;
}

#if PROBE_GPU >= 5
/*
 * Step 5 (PROBE_GPU=5): the first shader. probe/gpu_id.asm, assembled by Mesa's Valhall assembler
 * (src/panfrost/compiler/bifrost/valhall/asm.py): each invocation stores its global id (preloaded in r60)
 * at 0x200000 + id * 4. Dispatched on the same stream as step 4 with RUN_COMPUTE (Mesa genxml v11.xml,
 * as panvk's csf cmd_dispatch does): the compute staging registers SRT r0, FAU r8, SPD r16, TSD r24 (all
 * pairs), GLOBAL_ATTRIBUTE_OFFSET r32, WG_SIZE r33, JOB_OFFSET r34-36, JOB_SIZE r37-39. One workgroup of
 * 64. SPD: type 8 (shader), stage 1 (compute), 64 registers a thread, preload r60 (bit 12), binary at
 * 0x300000 (an executable page). TSD: no thread or workgroup storage (WLS instances log2 = 31).
 */

static const u64 shader_id[4] = {
	0x00b4c00119c0c83cULL,	/* LSHIFT_OR.i32 r0, r60, 0x3020100.b2222, 0x0 */
	0x0110c20020000040ULL,	/* IADD_IMM.i32 r2, r0^, #0x200000 */
	0x0110c300000000c0ULL,	/* IADD_IMM.i32 r3, 0x0, #0x0 */
	0x78617c0218000042ULL,	/* STORE.i32.slot0.end @r60, [r2^:r3^], offset:0 */
};

static u64 cs_move48(u32 reg, u64 v)
{
	return (0x01ULL << 56) | ((u64)reg << 48) | (v & 0xffffffffffffULL);
}

static u64 cs_move32(u32 reg, u32 v)
{
	return (0x02ULL << 56) | ((u64)reg << 48) | v;
}

static void gpu_first_compute(u64 *ring, volatile u64 *q_in, volatile u64 *q_out, volatile u32 *cs_out)
{
	volatile u32 *buf = (volatile u32 *)(uptr)target_page;
	u64 code_pa = code_page, desc_pa = desc_page, start;
	u32 *spd, *tsd, i, n = 8, good = 0;
	memcpy((void *)(uptr)code_pa, shader_id, sizeof(shader_id));
	spd = (u32 *)(uptr)desc_pa;
	spd[0] = 8 | (1u << 4);
	spd[1] = 1u << 12;
	spd[2] = SHADER_VA;
	spd[3] = 0;
	tsd = (u32 *)(uptr)(desc_pa + 0x40);
	tsd[1] = 31;
	for (i = 0; i < 64; i++)
		buf[i] = 0xdeadbeef;

	ring[n++] = cs_move48(0, 0);				/* SRT */
	ring[n++] = cs_move48(8, 0);				/* FAU */
	ring[n++] = cs_move48(16, DESC_VA);			/* SPD */
	ring[n++] = cs_move48(24, DESC_VA + 0x40);		/* TSD */
	ring[n++] = cs_move32(32, 0);
	ring[n++] = cs_move32(33, 63);				/* 64 x 1 x 1 */
	ring[n++] = cs_move32(34, 0);
	ring[n++] = cs_move32(35, 0);
	ring[n++] = cs_move32(36, 0);
	ring[n++] = cs_move32(37, 1);				/* one workgroup */
	ring[n++] = cs_move32(38, 1);
	ring[n++] = cs_move32(39, 1);
	ring[n++] = (0x04ULL << 56) | 1;			/* RUN_COMPUTE, task increment 1, axis X */
	ring[n++] = (0x03ULL << 56) | (0xffffULL << 16);	/* WAIT all scoreboards */
	while (n % 8)
		ring[n++] = 0;
	dsb_sy();
	q_in[0] = n * 8;
	dsb_sy();
	/* as for the first job: the stream's bell, then group 0's stream bit and the global bell */
	gpu_wr(DOORBELL(1), 1);
	toggle(&kick_csg_in[2], kick_csg_out[2], 1);
	toggle(&kick_glb_in[2], kick_glb_out[2], 1);
	dsb_sy();
	gpu_wr(DOORBELL(0), 1);
	start = timer_ticks();
	while (buf[63] != 63 && timer_ticks() - start < timer_freq())
		;
	for (i = 0; i < 64; i++)
		good += buf[i] == i;
	pr("gpu: compute: %u of 64 invocations wrote their id after %llu us%s; buf %08x %08x %08x .. %08x; extract %llu, "
	   "CS0 status 0x%08x blocked 0x%08x fault 0x%08x fatal 0x%08x (info 0x%08x%08x)\n", good,
	   (timer_ticks() - start) * 1000000 / timer_freq(), good == 64 ? ": THE SHADER CORES RAN OUR SHADER" : "",
	   buf[0], buf[1], buf[2], buf[63], q_out[0], cs_out[18], cs_out[24], cs_out[32], cs_out[33], cs_out[35], cs_out[34]);
	/* a fresh status snapshot (CSG_REQ bit 5, kbase's status update; docs/mali-endpoints.md) before reading */
	toggle(&kick_csg_in[0], kick_csg_out[0], 1u << 5);
	toggle(&kick_glb_in[2], kick_glb_out[2], 1);
	dsb_sy();
	gpu_wr(DOORBELL(0), 1);
	wait_bits(&kick_csg_in[0], &kick_csg_out[0], 1u << 5, "CSG0 status update");
	pr("gpu: compute state: SHADER_READY 0x%08x SHADER_PWRTRANS 0x%08x GPU_STATUS 0x%08x; CSG0 out %08x %08x %08x %08x "
	   "%08x %08x %08x %08x; CS0 out +0x40..: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x; GLB out %08x %08x %08x\n",
	   gpu_rd(SHADER_READY), gpu_rd(SHADER_PWRTRANS), gpu_rd(0x34), kick_csg_out[0], kick_csg_out[1], kick_csg_out[2],
	   kick_csg_out[3], kick_csg_out[4], kick_csg_out[5], kick_csg_out[6], kick_csg_out[7], cs_out[16], cs_out[17],
	   cs_out[18], cs_out[19], cs_out[20], cs_out[21], cs_out[22], cs_out[23], cs_out[24], cs_out[25],
	   kick_glb_out[0], kick_glb_out[1], kick_glb_out[2]);
	gpu_faults("after the compute job");
	log_flush();
}
#endif

static void gpu_first_job(u64 shared_pa)
{
	volatile u32 *glb_in = sh(shared_pa, 0x04002000), *glb_out = sh(shared_pa, 0x04006000);
	u32 *csg_ctl = sh(shared_pa, 0x04001000), *cs_ctl = sh(shared_pa, 0x04001040);
	volatile u32 *csg_in, *csg_out, *cs_in, *cs_out, *target;
	u64 ring_pa, target_pa, saved_root = root, as1_root, shaders = ((u64)gpu_rd(0x104) << 32) | gpu_rd(SHADER_PRESENT);
	u64 *ring, start;
	volatile u64 *q_in = (volatile u64 *)(uptr)q_pa, *q_out = (volatile u64 *)(uptr)(q_pa + 0x1000);

	pr("gpu: CSG0 control: features 0x%08x input 0x%08x output 0x%08x suspend %u/%u bytes, %u streams (stride 0x%x); "
	   "CS0 control: features 0x%08x input 0x%08x output 0x%08x\n", csg_ctl[0], csg_ctl[1], csg_ctl[2], csg_ctl[3], csg_ctl[4],
	   csg_ctl[5], csg_ctl[6], cs_ctl[0], cs_ctl[1], cs_ctl[2]);
	if (csg_ctl[3] > SUSP_PAGES * 0x1000) {
		pr("gpu: the suspend buffer needs more than %u pages\n", SUSP_PAGES);
		return;
	}
	csg_in = sh(shared_pa, csg_ctl[1]);
	csg_out = sh(shared_pa, csg_ctl[2]);
	cs_in = sh(shared_pa, cs_ctl[1]);
	cs_out = sh(shared_pa, cs_ctl[2]);

#if PROBE_GPU >= 5
	/* the tiler and shader cores, now that the firmware runs (run gpu14: it does not power them itself) */
	if (gpu_block_on("tiler", TILER_PWRON, TILER_READY, TILER_PWRTRANS, gpu_rd(TILER_PRESENT)) ||
	    gpu_block_on("shader cores", SHADER_PWRON, SHADER_READY, SHADER_PWRTRANS, gpu_rd(SHADER_PRESENT)))
		return;
#endif
	/* global: the shader cores the firmware may use (GLB_ALLOC_EN, request bit 2) */
	glb_in[6] = (u32)shaders;
	glb_in[7] = (u32)(shaders >> 32);
	dsb_sy();
	toggle(&glb_in[0], glb_out[0], 1u << 2);
	dsb_sy();
	gpu_wr(DOORBELL(0), 1);
	if (wait_bits(&glb_in[0], &glb_out[0], 1u << 2, "GLB ALLOC_EN"))
		return;
	pr("gpu: GLB_ALLOC_EN 0x%llx acknowledged\n", shaders);

	/* AS1: the job's own tables */
	as1_root = page_alloc(1);
	ring_pa = page_alloc(1);
	target_pa = page_alloc(1);
	root = as1_root;
	if (!as1_root || !ring_pa || !target_pa || map_pages(RING_VA, ring_pa, 0x1000, 0x40000007) ||
	    map_pages(TARGET_VA, target_pa, 0x1000, 0x40000003)) {
		root = saved_root;
		return;
	}
#if PROBE_GPU >= 5
	/* step 5's shader and descriptor pages, while AS1 is not yet live (no TLB maintenance needed) */
	code_page = page_alloc(1);
	desc_page = page_alloc(1);
	if (!code_page || !desc_page || map_pages(SHADER_VA, code_page, 0x1000, 0x40000005) ||
	    map_pages(DESC_VA, desc_page, 0x1000, 0x40000003)) {
		root = saved_root;
		return;
	}
#endif
	root = saved_root;
	job_root = as1_root;
	target_page = target_pa;
	target = (volatile u32 *)(uptr)target_pa;
	*target = 0x11111111;
	ring = (u64 *)(uptr)ring_pa;
	ring[0] = (0x01ULL << 56) | (0ULL << 48) | TARGET_VA;		/* MOVE48 r0:r1, target */
	ring[1] = (0x02ULL << 56) | (2ULL << 48) | 0x89abcdefULL;		/* MOVE32 r2 */
	ring[2] = (0x1cULL << 56) | (1ULL << 32) | 0;			/* SET_STATE_IMM32 SB_SEL_OTHER = 0 */
	ring[3] = (0x15ULL << 56) | (2ULL << 48) | (0ULL << 40) | (1ULL << 16);	/* STORE_MULTIPLE r2 -> [r0] */
	ring[4] = (0x03ULL << 56) | (1ULL << 16);				/* WAIT slot 0 */
	ring[5] = ring[6] = ring[7] = 0;					/* NOP */
	dsb_sy();
	gpu_wr(AS1 + AS_TRANSCFG, 0x42000006);
	gpu_wr(AS1 + AS_TRANSCFG + 4, 0);
	gpu_wr(AS1 + AS_TRANSTAB, (u32)as1_root);
	gpu_wr(AS1 + AS_TRANSTAB + 4, (u32)(as1_root >> 32));
	gpu_wr(AS1 + AS_MEMATTR, 0x888d8f88);
	gpu_wr(AS1 + AS_MEMATTR + 4, 0x00084c8d);
	gpu_wr(AS1 + AS_COMMAND, 1);
	start = timer_ticks();
	while ((gpu_rd(AS1 + AS_STATUS) & 1) && timer_ticks() - start < timer_freq() / 100)
		;

	/* CS 0 of group 0: ring, queue pages, priority 1, doorbell 1; then START */
	cs_in[4] = RING_VA;
	cs_in[5] = 0;
	cs_in[6] = 0x1000;
	cs_in[12] = Q_VA;
	cs_in[13] = 0;
	cs_in[14] = Q_VA + 0x1000;
	cs_in[15] = 0;
	cs_in[16] = cs_in[17] = 0;
	cs_in[1] = 1 | (1u << 8);
	cs_in[0] = (cs_in[0] & ~7u) | 1;

	/* group 0: all cores for compute/fragment, the tiler, priority 1, AS1, suspend buffer; then START */
	csg_in[6] = csg_in[7] = 0;
	csg_in[8] = (u32)shaders;
	csg_in[9] = (u32)(shaders >> 32);
	csg_in[10] = (u32)shaders;
	csg_in[11] = (u32)(shaders >> 32);
	csg_in[12] = 1;
	csg_in[13] = 7 | (7u << 8) | (1u << 16) | (1u << 28);
	csg_in[14] = 0;
	csg_in[16] = SUSP_VA;
	csg_in[17] = 0;
	csg_in[18] = csg_in[19] = 0;
	csg_in[20] = 1;
	csg_in[21] = 0;
	dsb_sy();
	csg_in[0] = (csg_in[0] & ~7u) | 1;
	toggle(&csg_in[2], csg_out[2], 1);			/* stream 0 changed */
	dsb_sy();
	toggle(&glb_in[2], glb_out[2], 1);			/* group 0 */
	dsb_sy();
	gpu_wr(DOORBELL(0), 1);
	start = timer_ticks();
	while (((csg_out[0] & 7) != 1 || (cs_out[0] & 7) != 1) && timer_ticks() - start < timer_freq() / 10)
		;
	pr("gpu: CSG0 ack 0x%08x (state %u), CS0 ack 0x%08x (state %u)\n", csg_out[0], csg_out[0] & 7, cs_out[0], cs_out[0] & 7);
	gpu_faults("group started");
	log_flush();

	/* the work: INSERT past the eight words, the stream's doorbell, and the kernel's way too */
	q_in[0] = 64;
	dsb_sy();
	gpu_wr(DOORBELL(1), 1);
	toggle(&csg_in[2], csg_out[2], 1);
	toggle(&glb_in[2], glb_out[2], 1);
	dsb_sy();
	gpu_wr(DOORBELL(0), 1);
	start = timer_ticks();
	while (*target != 0x89abcdef && timer_ticks() - start < timer_freq())
		;
	pr("gpu: target now 0x%08x after %llu us%s; queue extract %llu active %u; CS0 cmd ptr 0x%08x%08x status 0x%08x "
	   "blocked 0x%08x fault 0x%08x fatal 0x%08x\n", *target, (timer_ticks() - start) * 1000000 / timer_freq(),
	   *target == 0x89abcdef ? ": THE GPU RAN OUR FIRST JOB" : "", q_out[0], (u32)q_out[1], cs_out[17], cs_out[16],
	   cs_out[18], cs_out[24], cs_out[32], cs_out[33]);
	gpu_faults("after the job");
	log_flush();
#if PROBE_GPU >= 5
	kick_csg_in = csg_in;
	kick_csg_out = csg_out;
	kick_glb_in = glb_in;
	kick_glb_out = glb_out;
	if (*target == 0x89abcdef)
		gpu_first_compute(ring, q_in, q_out, cs_out);
#endif
}
#endif

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
#if PROBE_GPU >= 4
	/* for step 4: a stream's queue input/output pages and a group suspend buffer, in the firmware's AS0 */
	q_pa = page_alloc(2);
	susp_pa = page_alloc(SUSP_PAGES);
	if (!q_pa || !susp_pa || map_pages(Q_VA, q_pa, 0x2000, 0x40000003) ||
	    map_pages(SUSP_VA, susp_pa, SUSP_PAGES * 0x1000, 0x40000003))
		return;
#endif
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
#if PROBE_GPU >= 4
		log_flush();
		gpu_first_job(shared_pa);
#endif
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
