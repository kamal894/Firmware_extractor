/* SPDX-License-Identifier: MIT */
/*
 * Start the AOC firmware the way aoc_core.ko does on a gsa-enabled phone (docs/aoc-boot-map.md
 * sections 10-11, docs/aoc-fw-load.md):
 *
 *   1. aoc_req (0x154b0000) = 1                         request_aoc_on
 *   2. carveout 0x94000000 <- aoc.bin + 0x1000          _aoc_fw_commit
 *   3. GSA mailbox 0x5a(header copy, carveout)          aoc_fw_authenticate
 *   4. fw-info block at carveout + ipc offset           aoc_pass_fw_information
 *   5. Trusty com.android.trusty.gsa.hwmgr.aoc {1, 1}   gsa_send_aoc_cmd(1)
 *
 * aoc.bin rides in the boot image's kernel payload: the bootloader takes the ramdisk from
 * init_boot, not from our image. tools/pack_aoc_payload.py pads probe.Image to 1 MiB (past our
 * bss and stack), then writes a 0x1000-byte blob header ("AOCBLOB\0", u64 size) and aoc.bin.
 */
#include "probe.h"

#define AOC_REQ			0x154b0000ULL
#define AOC_CARVEOUT		0x94000000ULL
#define AOC_CARVEOUT_SIZE	0x3000000ULL
#define AOC_DRAM_IN_AOC		0x98000000u	/* where the AOC sees the carveout */
#define AOC_PAD_CSR		0x18408000ULL
#define GSA_MBOX		0x16490000ULL
#define CHIPID			0x10000000ULL

#define FW_MAGIC_AOC		0x00434f41u	/* "AOC\0" at +0x400 */
#define FW_MAGIC_HDR		0xaabbccddu	/* at +0x1000 */
#define FWINFO_MAGIC		0x0a0cda7au

static u8 aoc_hdr[0x1000] __attribute__((aligned(0x1000)));

static u32 le32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

#define AOC_BLOB_OFFSET		0x100000u

/* The aoc.bin copy at load base + 1 MiB, behind its blob header. */
static const u8 *find_aoc_bin(u64 *size)
{
	const u8 *b = (const u8 *)(uptr)(g.load_base + AOC_BLOB_OFFSET);
	const u8 *p = b + 0x1000;

	if (memcmp(b, "AOCBLOB", 8))
		return NULL;
	*size = le32(b + 8) | (u64)le32(b + 12) << 32;
	if (le32(p + 0x400) != FW_MAGIC_AOC || le32(p + 0x1000) != FW_MAGIC_HDR)
		return NULL;
	return p;
}

/* gsa.ko gsa_send_mbox_cmd_locked(), polled. Returns the reply status, or -1 with no reply. */
static int gsa_cmd(u32 cmd, const u32 *args, u32 n)
{
	u32 i, ms, st = 0;

	wr32(GSA_MBOX + 0x80, cmd);
	wr32(GSA_MBOX + 0x84, n);
	for (i = 0; i < n; i++)
		wr32(GSA_MBOX + 0x88 + 4 * i, args[i]);
	wr32(GSA_MBOX + 0x24, 1);
	wr32(GSA_MBOX + 0x28, rd32(GSA_MBOX + 0x28) & ~1u);
	wr32(GSA_MBOX + 0x40, 1);
	for (ms = 0; ms < 10000 && !((st = rd32(GSA_MBOX + 0x30)) & 1); ms++)
		delay_us(1000);
	pr("aoc: GSA 0x%x: +0x30 0x%x after %u ms; reply 0x%08x status 0x%x count %u [0x%x 0x%x]\n", cmd, st, ms,
	   rd32(GSA_MBOX + 0x80), rd32(GSA_MBOX + 0x84), rd32(GSA_MBOX + 0x88), rd32(GSA_MBOX + 0x8c),
	   rd32(GSA_MBOX + 0x90));
	wr32(GSA_MBOX + 0x24, 1);
	wr32(GSA_MBOX + 0x28, rd32(GSA_MBOX + 0x28) | 1);
	log_flush();
	if (!(st & 1) || rd32(GSA_MBOX + 0x80) != (cmd | 0x80000000u))
		return -1;
	return (int)rd32(GSA_MBOX + 0x84);
}

/* aoc_fw_callback 0x2d8-0x6d4 and docs/aoc-fw-load.md section A, for this phone. */
static void write_fwinfo(u64 at)
{
	u32 chip = rd32(CHIPID);
	const u32 kv[22][2] = {
		{ 0x1001, 0x00040902 },	/* aoc-board-id */
		{ 0x1002, 0x00010000 },	/* aoc-board-rev */
		{ 0x1003, 0 },
		{ 0x1005, (u32)AOC_CARVEOUT },
		{ 0x1006, (u32)AOC_CARVEOUT_SIZE },
		{ 0x1007, 0x9ac00000 },	/* sensor_direct_heap, AOC address */
		{ 0x1008, 0x00400000 },
		{ 0x100c, 0x9abfc000 },	/* aaudio_playback_heap */
		{ 0x100d, 0x00004000 },
		{ 0x100e, 0x9abec000 },	/* aaudio_capture_heap */
		{ 0x100f, 0x00010000 },
		{ 0x1009, 0 },		/* force-vnom = 0 */
		{ 0x100a, 0 },
		{ 0x100b, 0 },
		{ 0x1010, 0 },
		{ 0x1011, 0x5eed1234 },	/* get_random_u32() */
		{ 0x1012, 0x00000001 },	/* chip revision (Android sysfs) */
		{ 0x1013, chip & 0xff },	/* chip type */
		{ 0x1014, 2 },		/* gnss-type */
		{ 0x1015, 0 },
		{ 0x1016, chip & 0xfffff000 },	/* product id */
		{ 0x1017, 0 },		/* wifi-chip absent */
	};
	u32 i;

	wr32(at + 0, FWINFO_MAGIC);
	wr32(at + 4, 22);
	wr32(at + 8, 0x114);
	for (i = 0; i < 22; i++) {
		wr32(at + 12 + 12 * i, kv[i][0]);
		wr32(at + 16 + 12 * i, 4);
		wr32(at + 20 + 12 * i, kv[i][1]);
	}
	pr("aoc: fw-info at 0x%llx: chip 0x%08x (type 0x%x, product 0x%x)\n", at, chip, chip & 0xff,
	   chip & 0xfffff000);
}

static void aoc_watch(const char *when)
{
	int ok;
	u32 v = safe_rd32(AOC_PAD_CSR + 4 * 72, &ok);

	pr("aoc: %s: aoc_req 0x%x status 0x%x; pad 72 0x%x%s; ipc[0..3] %08x %08x %08x %08x\n", when,
	   rd32(AOC_REQ), rd32(AOC_REQ + 0x40), v, ok ? "" : " (aborted)", rd32(AOC_CARVEOUT + 0x1c00000),
	   rd32(AOC_CARVEOUT + 0x1c00004), rd32(AOC_CARVEOUT + 0x1c00008), rd32(AOC_CARVEOUT + 0x1c0000c));
	log_flush();
}

void aoc_start(void)
{
	const u8 *fw;
	u64 size, ipc, n;
	u32 args[4], req[2], rsp[3];
	int ok, ret, h;
	char ver[24];

	pr("\n--- AOC start ---\n");
	fw = find_aoc_bin(&size);
	if (!fw) {
		pr("aoc: no aoc.bin blob at load base + 1 MiB\n");
		log_flush();
		return;
	}
	ipc = le32(fw + 0x1000 + 0xc);
	memcpy(ver, fw + 0x1030, sizeof(ver) - 1);
	ver[sizeof(ver) - 1] = 0;
	pr("aoc: aoc.bin at 0x%llx, 0x%llx bytes, header v%u, ipc offset 0x%llx, version %s\n", (u64)(uptr)fw, size,
	   le32(fw + 0x404), ipc, ver);
	if (size - 0x1000 > AOC_CARVEOUT_SIZE || ipc + 0x114 > AOC_CARVEOUT_SIZE) {
		pr("aoc: does not fit the carveout\n");
		log_flush();
		return;
	}
	safe_rd32_async(AOC_CARVEOUT, &ok);
	if (!ok) {
		pr("aoc: carveout 0x%llx: read aborted\n", AOC_CARVEOUT);
		log_flush();
		return;
	}
	aoc_watch("before");

	/* 1 */
	wr32(AOC_REQ, 1);
	/* 2 */
	memcpy((void *)(uptr)AOC_CARVEOUT, fw + 0x1000, size - 0x1000);
	for (n = 0; n < size - 0x1000; n += 0x100000)
		if (*(volatile u32 *)(uptr)(AOC_CARVEOUT + n) != le32(fw + 0x1000 + n))
			break;
	pr("aoc: copied 0x%llx bytes to 0x%llx%s\n", size - 0x1000, AOC_CARVEOUT,
	   n < size - 0x1000 ? " (read-back mismatch)" : "");
	memcpy(aoc_hdr, fw, sizeof(aoc_hdr));
	__asm__ volatile("dsb sy" : : : "memory");
	log_flush();

	/* 3 */
	if (gsa_cmd(0x96, NULL, 0) < 0)
		return;
	args[0] = (u32)(uptr)aoc_hdr;
	args[1] = (u32)((u64)(uptr)aoc_hdr >> 32);
	args[2] = (u32)AOC_CARVEOUT;
	args[3] = (u32)(AOC_CARVEOUT >> 32);
	ret = gsa_cmd(0x5a, args, 4);
	gsa_cmd(0x97, NULL, 0);
	pr("aoc: GSA load (0x5a) = %d\n", ret);
	log_flush();
	if (ret)
		return;

	/* 4 */
	write_fwinfo(AOC_CARVEOUT + ipc);
	__asm__ volatile("dsb sy" : : : "memory");
	aoc_watch("loaded");

	/* 5 */
	if (trusty_ql_open())
		return;
	h = trusty_ql_connect("com.android.trusty.gsa.hwmgr.aoc");
	if (h > 0) {
		req[0] = 1;	/* AOC state command */
		req[1] = 1;	/* gsa_send_aoc_cmd(1) */
		memset(rsp, 0, sizeof(rsp));
		ret = trusty_ql_xchg(h, req, sizeof(req), rsp, sizeof(rsp), 500);
		pr("aoc: hwmgr.aoc {1, 1}: %d bytes: 0x%08x 0x%x 0x%x\n", ret, rsp[0], rsp[1], rsp[2]);
		log_flush();
		trusty_ql_disconnect(h);
	}
	trusty_ql_close();

	for (n = 0; n < 4; n++) {
		delay_us(500000);
		aoc_watch("after start");
	}
}
