/* SPDX-License-Identifier: MIT */
/*
 * Find the frame buffer the bootloader is scanning out.
 *
 * Two independent sources:
 *  1. The device tree: Google's DECON driver takes its "handover" buffer
 *     from a `memory-region` on the samsung,exynos-decon node
 *     (exynos_drm_fb.c: exynos_rmem_register). The bootloader adds it at
 *     boot time, so it is only in the DT it passes us, not in the dts.
 *  2. The hardware: DECON0's WINCON registers say which windows are on
 *     and which DMA channel feeds each; that channel's RDMA registers hold
 *     the buffer address, size, format and stride.
 *
 * Register layout from Google's cal_9865 (zuma/zumapro) display driver;
 * see caiman/reference/google/regs-decon-9865.h and regs-dpp-9865.h.
 * DECON0 and DPUF0 are live while the splash is on screen. Other DMA
 * blocks are only read if an enabled window points at them.
 */
#include "probe.h"

/* DECON main */
#define DECON_VERSION		0x0000
#define FRAME_COUNT		0x0004
#define GLOBAL_CON		0x0020
#define  GCON_OP_MODE_CMD	(1u << 8)
#define  GCON_IDLE_STATUS	(1u << 5)
#define  GCON_RUN_STATUS	(1u << 4)
#define  GCON_DECON_EN		(1u << 1)
#define  GCON_DECON_EN_F	(1u << 0)
#define TRIG_CON		0x0030
#define  HW_TRIG_EN		(1u << 0)
#define  SW_TRIG_DET_EN		(1u << 1)
#define  HW_TRIG_MASK_DECON	(1u << 4)
#define  SW_TRIG_EN		(1u << 8)
#define SHD_REG_UP_REQ		0x0050
#define  SHD_UP_REQ_CMP		(1u << 20)
#define  SHD_UP_REQ_GLOBAL	(1u << 31)
#define DECON_SHADOW		0x0800

/* DECON_WINCON / DECON_WIN: one 0x1000 page per window */
#define WINCON(w)		((u64)(w) * 0x1000)
#define  WIN_EN			(1u << 0)
#define  WIN_MAPCOLOR_EN	(1u << 1)
#define  WIN_CHMAP(v)		(((v) >> 4) & 0xf)
#define WIN_START_POS(w)	((u64)(w) * 0x1000 + 0xc)
#define WIN_END_POS(w)		((u64)(w) * 0x1000 + 0x10)

/* RDMA (per DMA channel) */
#define RDMA_ENABLE		0x00
#define RDMA_IN_CTRL_0		0x08
#define RDMA_SRC_WIDTH		0x10
#define RDMA_SRC_HEIGHT		0x14
#define RDMA_SRC_OFFSET		0x18
#define RDMA_IMG_SIZE		0x1c
#define RDMA_BASEADDR_P0	0x40
#define RDMA_BASEADDR_P1	0x44
#define RDMA_SRC_STRIDE_0	0x50
#define  STRIDE_SEL		(1u << 31)
#define DMA_SHADOW		0x400

#define MAX_DPP			16

struct dpu_map {
	u64 main, win, wincon;
	u32 nwin;
	u64 dma[MAX_DPP];	/* by dpp,id; 0 = unknown */
};

static const char *fmt_name(u32 f)
{
	switch (f) {
	case 0: return "BGRA8888";
	case 1: return "RGBA8888";
	case 2: return "ABGR8888";
	case 3: return "ARGB8888";
	case 4: return "BGRX8888";
	case 5: return "RGBX8888";
	case 6: return "XBGR8888";
	case 7: return "XRGB8888";
	case 8: return "BGR565";
	case 9: return "RGB565";
	case 16: return "BGRA1010102";
	case 17: return "RGBA1010102";
	case 18: return "ABGR2101010";
	case 19: return "ARGB2101010";
	case 24: return "NV21";
	case 25: return "NV12";
	case 32: return "ARGB_FP16";
	case 33: return "ABGR_FP16";
	case 34: return "RGBA_FP16";
	case 35: return "BGRA_FP16";
	default: return "other";
	}
}

static u32 fmt_bpp(u32 f)
{
	if (f <= 7 || (f >= 16 && f <= 19))
		return 4;
	if (f == 8 || f == 9)
		return 2;
	if (f >= 32 && f <= 35)
		return 8;
	return 0;
}

static void dt_lookup(struct dpu_map *m)
{
	const struct fdt *f = &g.fdt;
	u32 pac, psc, ph, id;
	u64 a, s;
	int n, i;

	if (!g.have_fdt)
		return;

	n = -1;
	while ((n = fdt_find_compatible(f, "samsung,exynos-decon", n, &pac, &psc)) >= 0) {
		if (!fdt_read_u32(f, n, "decon,id", &id) && id != 0)
			continue;
		pr("display: DT %s (decon 0)\n", fdt_node_name(f, n));
		i = fdt_stringlist_index(f, n, "reg-names", "main");
		if (!fdt_reg(f, n, "reg", pac, psc, i < 0 ? 0 : i, &a, &s))
			m->main = a;
		i = fdt_stringlist_index(f, n, "reg-names", "win");
		if (i >= 0 && !fdt_reg(f, n, "reg", pac, psc, i, &a, &s))
			m->win = a;
		i = fdt_stringlist_index(f, n, "reg-names", "wincon");
		if (i >= 0 && !fdt_reg(f, n, "reg", pac, psc, i, &a, &s)) {
			m->wincon = a;
			if (s >= 0x1000 && s / 0x1000 < 32)
				m->nwin = (u32)(s / 0x1000);
		}
		if (!fdt_read_u32(f, n, "memory-region", &ph)) {
			int r = fdt_find_phandle(f, ph, &pac, &psc);

			if (r >= 0 && !fdt_reg(f, r, "reg", pac, psc, 0, &a, &s)) {
				g.fb_handover.base = a;
				g.fb_handover.size = s;
				pr("display: DT handover memory-region -> %s: 0x%llx size 0x%llx\n",
				   fdt_node_name(f, r), a, s);
			} else {
				pr("display: DT memory-region phandle 0x%x not resolvable\n", ph);
			}
		} else {
			pr("display: DT decon node has no memory-region (no handover buffer declared)\n");
		}
		break;
	}

	n = -1;
	while ((n = fdt_find_compatible(f, "samsung,exynos-dpp", n, &pac, &psc)) >= 0) {
		if (fdt_read_u32(f, n, "dpp,id", &id) || id >= MAX_DPP)
			continue;
		i = fdt_stringlist_index(f, n, "reg-names", "dma");
		if (!fdt_reg(f, n, "reg", pac, psc, i < 0 ? 0 : i, &a, &s))
			m->dma[id] = a;
	}
}

static u64 dma_base_for(const struct dpu_map *m, u32 dpp)
{
	if (dpp < MAX_DPP && m->dma[dpp])
		return m->dma[dpp];
	if (dpp <= 6)
		return PLAT_DPUF0_DMA + 0x1000ULL * dpp;
	if (dpp <= 13)
		return PLAT_DPUF1_DMA + 0x1000ULL * (dpp - 7);
	return 0;
}

static int rd(u64 a, u32 *v)
{
	int ok;

	*v = safe_rd32(a, &ok);
	if (!ok)
		pr("display:   read 0x%llx ABORTED\n", a);
	return ok;
}

struct chan {
	u32 en, in_ctrl, sw, sh, soff, img, base0, base1, stride;
};

static int read_chan(u64 b, struct chan *c)
{
	return rd(b + RDMA_ENABLE, &c->en) && rd(b + RDMA_IN_CTRL_0, &c->in_ctrl) &&
	       rd(b + RDMA_SRC_WIDTH, &c->sw) && rd(b + RDMA_SRC_HEIGHT, &c->sh) &&
	       rd(b + RDMA_SRC_OFFSET, &c->soff) && rd(b + RDMA_IMG_SIZE, &c->img) &&
	       rd(b + RDMA_BASEADDR_P0, &c->base0) && rd(b + RDMA_BASEADDR_P1, &c->base1) &&
	       rd(b + RDMA_SRC_STRIDE_0, &c->stride);
}

static void print_chan(const char *tag, const struct chan *c)
{
	u32 fmt = (c->in_ctrl >> 8) & 0x3f;

	pr("display:   %s ENABLE=0x%08x IN_CTRL_0=0x%08x (fmt %u %s%s%s)\n", tag, c->en,
	   c->in_ctrl, fmt, fmt_name(fmt), (c->in_ctrl & (1u << 3)) ? " AFBC" : "",
	   (c->in_ctrl & (1u << 2)) ? " SBWC" : "");
	pr("display:   %s SRC %ux%u  OFFSET=0x%08x  IMG %ux%u\n", tag, c->sw & 0xffffff,
	   c->sh & 0xffffff, c->soff, c->img & 0xffff, c->img >> 16);
	pr("display:   %s BASEADDR_P0=0x%08x P1=0x%08x STRIDE_0=0x%08x\n", tag, c->base0,
	   c->base1, c->stride);
}

/* An opaque pixel of colour (r,g,b) in the scan-out format (drm fourcc naming, little endian). */
static u32 pixel(u32 fmt, u32 r, u32 g8, u32 b)
{
	switch (fmt) {
	case 0:	/* BGRA8888: [31:0] B:G:R:A */
	case 4:	/* BGRX8888 */
		return (b << 24) | (g8 << 16) | (r << 8) | 0xffu;
	case 1:	/* RGBA8888: [31:0] R:G:B:A */
	case 5:	/* RGBX8888 */
		return (r << 24) | (g8 << 16) | (b << 8) | 0xffu;
	case 2:	/* ABGR8888: [31:0] A:B:G:R */
	case 6:	/* XBGR8888 */
		return 0xff000000u | (b << 16) | (g8 << 8) | r;
	default: /* ARGB8888 / XRGB8888: [31:0] A:R:G:B */
		return 0xff000000u | (r << 16) | (g8 << 8) | b;
	}
}

/*
 * Command-mode panels keep showing their own frame memory until the DECON
 * sends a new frame. Do what the driver's decon_reg_update_req_and_unmask()
 * does: request a shadow update, then unmask the TE-driven (hw) trigger so a
 * frame goes out on every tearing-effect pulse from the panel.
 */
static void kick_frames(u64 main)
{
	u32 trig, fc0, fc1, up;
	int ok;

	trig = safe_rd32(main + TRIG_CON, &ok);
	if (!ok)
		return;
	fc0 = rd32(main + FRAME_COUNT);
	pr("kick: TRIG_CON=0x%08x (%s trigger, %s) FRAME_COUNT=%u\n", trig,
	   (trig & SW_TRIG_EN) ? "sw" : "hw", (trig & HW_TRIG_MASK_DECON) ? "masked" : "unmasked",
	   fc0);
	log_flush();
	up = rd32(main + SHD_REG_UP_REQ);
	wr32(main + SHD_REG_UP_REQ, up | SHD_UP_REQ_GLOBAL | SHD_UP_REQ_CMP);
	if (trig & SW_TRIG_EN)
		wr32(main + TRIG_CON, trig | SW_TRIG_EN | SW_TRIG_DET_EN);
	else
		wr32(main + TRIG_CON, (trig & ~HW_TRIG_MASK_DECON) | HW_TRIG_EN);
	delay_us(200000);
	fc1 = rd32(main + FRAME_COUNT);
	pr("kick: TRIG_CON now 0x%08x; FRAME_COUNT %u -> %u in 200 ms (%s)\n",
	   rd32(main + TRIG_CON), fc0, fc1,
	   fc1 != fc0 ? "frames are going out: the bars should be on screen"
		      : "still no frames");
}

static void paint(u64 decon_main)
{
	static const u8 rgb[8][3] = {
		{ 255, 255, 255 }, { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
		{ 255, 255, 0 }, { 0, 255, 255 }, { 255, 0, 255 }, { 128, 128, 128 },
	};
	u32 bars[8];
	u64 size = (u64)g.fb_stride * g.fb_height;
	u32 x, y, i;
	int panel_sized = g.fb_width == PLAT_PANEL_W && g.fb_height == PLAT_PANEL_H;

	if (g.fb_bpp != 4 || !g.fb_width || !g.fb_height || g.fb_stride < g.fb_width * 4 ||
	    (g.fb_addr & 3)) {
		pr("paint: skipped (need a 32bpp linear buffer)\n");
		return;
	}
	/*
	 * The bootloader may leave the splash buffer out of the DT memory nodes,
	 * so a panel-sized buffer the DMA is actively reading is trusted too.
	 */
	if (!(addr_in_dram(g.fb_addr, size) || (panel_sized && g.fb_addr >= 0x80000000ULL)) ||
	    overlaps(g.fb_addr, size, g.load_base, g.image_size) ||
	    (g.have_fdt && overlaps(g.fb_addr, size, g.dtb, g.fdt.totalsize)) ||
	    overlaps(g.fb_addr, size, g.console.base, g.console.size)) {
		pr("paint: skipped (buffer not in free DRAM)\n");
		return;
	}
	for (i = 0; i < 8; i++)
		bars[i] = pixel(g.fb_format, rgb[i][0], rgb[i][1], rgb[i][2]);
	pr("paint: colour bars into 0x%llx (%ux%u, stride %u)\n", g.fb_addr, g.fb_width,
	   g.fb_height, g.fb_stride);
	log_flush();
	for (y = 0; y < g.fb_height; y++) {
		volatile u32 *row = (volatile u32 *)(uptr)(g.fb_addr + (u64)y * g.fb_stride);

		for (x = 0; x < g.fb_width; x++)
			row[x] = bars[(x * 8) / g.fb_width];
	}
	/* the display DMA reads DRAM directly: push the pixels out of any cache */
	for (y = 0; y < g.fb_height; y++) {
		u64 a = g.fb_addr + (u64)y * g.fb_stride, e = a + (u64)g.fb_width * 4;

		for (a &= ~63ULL; a < e; a += 64)
			__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	}
	dsb_sy();
	pr("paint: done (white red green blue yellow cyan magenta grey, left to right)\n");
	if (g.decon_cmd_mode == 1)
		kick_frames(decon_main);
}

void display_probe(void)
{
	struct dpu_map m;
	u32 ver, fc0, fc1, gcon, gcon_shd, trig, w, best_area = 0;
	int found = 0;

	memset(&m, 0, sizeof(m));
	m.main = PLAT_DECON0_MAIN;
	m.win = PLAT_DECON0_WIN;
	m.wincon = PLAT_DECON0_WINCON;
	m.nwin = PLAT_DECON_NWIN;
	g.fb_channel = -1;
	g.decon_cmd_mode = -1;

	pr("\n--- display ---\n");
	dt_lookup(&m);
	pr("display: DECON0 main=0x%llx win=0x%llx wincon=0x%llx (%u windows)\n", m.main, m.win,
	   m.wincon, m.nwin);

	pr("display: reading DECON0 main registers (a hang right after this line means\n"
	   "         the DECON block is powered down)\n");
	log_flush();
	if (!rd(m.main + DECON_VERSION, &ver) || !rd(m.main + FRAME_COUNT, &fc0) ||
	    !rd(m.main + GLOBAL_CON, &gcon) || !rd(m.main + DECON_SHADOW + GLOBAL_CON, &gcon_shd) ||
	    !rd(m.main + TRIG_CON, &trig)) {
		pr("display: DECON0 registers not readable; giving up on the display\n");
		return;
	}
	g.decon_cmd_mode = (gcon & GCON_OP_MODE_CMD) ? 1 : 0;
	pr("display: VERSION=0x%08x FRAME_COUNT=%u GLOBAL_CON=0x%08x (shadow 0x%08x) TRIG_CON=0x%08x\n",
	   ver, fc0, gcon, gcon_shd, trig);
	pr("display: mode=%s enabled=%s run=%u idle=%u\n",
	   g.decon_cmd_mode ? "COMMAND" : "VIDEO",
	   (gcon & (GCON_DECON_EN | GCON_DECON_EN_F)) ? "yes" : "no",
	   (gcon & GCON_RUN_STATUS) ? 1 : 0, (gcon & GCON_IDLE_STATUS) ? 1 : 0);

	pr("display: reading WINCON 0..%u\n", m.nwin - 1);
	log_flush();
	for (w = 0; w < m.nwin; w++) {
		u32 v, hw_ch, dpp, sp, ep;
		u64 dma;
		struct chan live, shd;
		const struct chan *use;

		if (!rd(m.wincon + WINCON(w), &v))
			break;
		if (!(v & WIN_EN))
			continue;
		hw_ch = WIN_CHMAP(v);
		/* decon_reg_get_win_ch(): channel 7 does not exist on zuma, 8.. map to dpp 7.. */
		dpp = hw_ch > 7 ? hw_ch - 1 : hw_ch;
		sp = ep = 0;
		rd(m.win + WIN_START_POS(w), &sp);
		rd(m.win + WIN_END_POS(w), &ep);
		pr("display: window %u ON: WINCON=0x%08x chmap=%u -> dpp %u%s  pos (%u,%u)-(%u,%u)\n",
		   w, v, hw_ch, dpp, (v & WIN_MAPCOLOR_EN) ? " [solid colour, no DMA]" : "",
		   sp & 0x3fff, sp >> 16 & 0x3fff, ep & 0x3fff, ep >> 16 & 0x3fff);
		if (hw_ch == 7 || (v & WIN_MAPCOLOR_EN))
			continue;
		dma = dma_base_for(&m, dpp);
		if (!dma) {
			pr("display:   no DMA base known for dpp %u\n", dpp);
			continue;
		}
		pr("display:   reading RDMA @ 0x%llx\n", dma);
		log_flush();
		if (!read_chan(dma, &live) || !read_chan(dma + DMA_SHADOW, &shd))
			continue;
		print_chan("live  ", &live);
		print_chan("shadow", &shd);

		/* the shadow copy is what the hardware is using for the current frame */
		use = shd.base0 ? &shd : &live;
		{
			u32 fmt = (use->in_ctrl >> 8) & 0x3f;
			u32 bpp = fmt_bpp(fmt);
			u32 sw = use->sw & 0xffffff, sh = use->sh & 0xffffff;
			u32 area = sw * sh;
			u32 stride = (use->stride & STRIDE_SEL) ? (use->stride & 0xffffff) : sw * bpp;

			if (use->base0 && area > best_area) {
				best_area = area;
				found = 1;
				g.fb_addr = use->base0;
				g.fb_width = sw;
				g.fb_height = sh;
				g.fb_bpp = bpp;
				g.fb_format = fmt;
				g.fb_stride = stride;
				g.fb_channel = (int)dpp;
			}
		}
	}

	delay_us(100000);
	if (rd(m.main + FRAME_COUNT, &fc1)) {
		g.frame_count_delta = fc1 - fc0;
		pr("display: FRAME_COUNT %u -> %u over 100 ms (%s)\n", fc0, fc1,
		   fc1 != fc0 ? "panel is being refreshed" : "no new frames: idle/command mode");
	}

	if (found) {
		pr("display: FRAME BUFFER: 0x%llx  %ux%u  %s  stride %u bytes  (dpp %d)\n",
		   g.fb_addr, g.fb_width, g.fb_height, fmt_name(g.fb_format), g.fb_stride,
		   g.fb_channel);
		if (!addr_in_dram(g.fb_addr, 1))
			pr("display: note: address is outside the DT memory nodes (the bootloader may\n"
			   "         carve the splash buffer out of them, or it is an IOVA)\n");
		if (g.fb_handover.size)
			pr("display: DT handover region %s the register address\n",
			   (g.fb_addr >= g.fb_handover.base &&
			    g.fb_addr < g.fb_handover.base + g.fb_handover.size) ? "CONTAINS"
										 : "does NOT contain");
	} else {
		pr("display: no enabled DMA-fed window found\n");
	}

	if (PROBE_PAINT && found)
		paint(m.main);
}
