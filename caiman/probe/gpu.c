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
 */
#include "probe.h"

#if PROBE_GPU
#define GPU_BASE	0x1f000000ULL
#define PD_G3D		0x15461a80ULL
#define PD_EMB_G3D	0x15461c80ULL
#define CMU_G3D		0x1ee00000ULL

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
}
#endif
