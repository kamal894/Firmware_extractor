// SPDX-License-Identifier: MIT
/*
 * PROBE_PANEL=1: talk to the panel over DSIM0 (0x19440000) the way Google's exynos_drm_dsim.c does when it has
 * packet-go off: payload words into DSIM_PAYLOAD, then the header into DSIM_PKTHDR; a read is "set maximum return
 * packet size" then the read request with bus turn-around, and the answer comes out of DSIM_RXFIFO once INTSRC says
 * RX_DATA_DONE. Registers from Google's cal_9865/regs-dsim.h (kernel/google-modules/display).
 *
 * What it does: shows the link as the bootloader left it, reads the panel's power mode (DCS 0x0a) and brightness
 * (0x52), sets the brightness lower (0x51), reads it back, and puts the old value back. A screen that is dimmer for
 * a moment is all the panel sees. This is the ground for the screen going dark when iOS locks it.
 */
#include "probe.h"

#define DSIM0			0x19440000ULL
#define DSIM_LINK_STATUS0	0x0008
#define DSIM_CONFIG		0x004c
#define  CONFIG_VIDEO_MODE	(1u << 18)
#define DSIM_INTSRC		0x0050
#define  INT_PL_FIFO_EMPTY	(1u << 29)
#define  INT_PH_FIFO_EMPTY	(1u << 28)
#define  INT_RX_DATA_DONE	(1u << 18)
#define  INT_RX_ACK		(1u << 16)
#define DSIM_PKTHDR		0x0058
#define DSIM_PAYLOAD		0x005c
#define DSIM_RXFIFO		0x0060
#define DSIM_FIFOCTRL		0x0068
#define  FIFO_EMPTY_RX		(1u << 12)
#define  FIFO_EMPTY_PH		(1u << 10)
#define  FIFO_EMPTY_PL		(1u << 8)
#define DSIM_CMD_CONFIG		0x0080
#define  CMD_PKT_GO_EN		(1u << 16)

#define DT_DCS_SHORT_WRITE	0x05
#define DT_DCS_SHORT_WRITE1	0x15
#define DT_MAX_RETURN		0x37
#define DT_DCS_READ		0x06
#define DT_DCS_LONG_WRITE	0x39

static u32 dr(u32 off) { return *(volatile u32 *)(uptr)(DSIM0 + off); }
static void dw(u32 off, u32 v) { *(volatile u32 *)(uptr)(DSIM0 + off) = v; }

static int wait_bits(u32 off, u32 bits, u64 us)
{
	u64 end = timer_us() + us;

	while ((dr(off) & bits) != bits)
		if (timer_us() > end)
			return -1;
	return 0;
}

static int send(u8 di, const u8 *p, u32 n)
{
	u32 i, w;

	dw(DSIM_INTSRC, INT_PH_FIFO_EMPTY | INT_PL_FIFO_EMPTY);
	if (di == DT_DCS_LONG_WRITE) {
		for (i = 0; i < n; i += 4) {
			w = 0;
			for (u32 k = 0; k < 4 && i + k < n; k++)
				w |= (u32)p[i + k] << (8 * k);
			dw(DSIM_PAYLOAD, w);
		}
		dw(DSIM_PKTHDR, di | (n & 0xff) << 8 | (n >> 8) << 16);
	} else {
		dw(DSIM_PKTHDR, di | (u32)p[0] << 8 | (n > 1 ? (u32)p[1] << 16 : 0));
	}
	return wait_bits(DSIM_FIFOCTRL, FIFO_EMPTY_PH | FIFO_EMPTY_PL, 100000);
}

/* a DCS read of up to 2 bytes; the raw first RX word in *raw */
static int dcs_read(u8 cmd, u32 len, u8 *out, u32 *raw)
{
	u32 rx;

	dw(DSIM_INTSRC, INT_RX_DATA_DONE | INT_PH_FIFO_EMPTY);
	while (!(dr(DSIM_FIFOCTRL) & FIFO_EMPTY_RX))
		(void)dr(DSIM_RXFIFO);
	dw(DSIM_PKTHDR, DT_MAX_RETURN | len << 8);
	dw(DSIM_PKTHDR, DT_DCS_READ | (u32)cmd << 8 | 1u << 24);
	if (wait_bits(DSIM_INTSRC, INT_RX_DATA_DONE, 100000)) {
		*raw = dr(DSIM_INTSRC);
		return -1;
	}
	rx = dr(DSIM_RXFIFO);
	*raw = rx;
	switch (rx & 0xff) {
	case 0x22: case 0x12:		/* 2-byte short responses */
		out[1] = (u8)(rx >> 16);
		/* fall through */
	case 0x21: case 0x11:
		out[0] = (u8)(rx >> 8);
		return 0;
	case 0x1c: case 0x1a: {		/* long responses */
		u32 size = (rx >> 8) & 0xffff, i;

		for (i = 0; i < size; i += 4) {
			u32 w = dr(DSIM_RXFIFO);
			for (u32 k = 0; k < 4 && i + k < size; k++)
				if (i + k < len)
					out[i + k] = (u8)(w >> (8 * k));
		}
		return 0;
	}
	}
	return -2;
}

void panel_probe(void)
{
	int ok = 1;
	u32 cfg, raw;
	u8 pm[2] = { 0 }, br[2] = { 0 }, br2[2] = { 0 }, set[3];
	int r1, r2, r3;

	safe_rd32_async(DSIM0 + DSIM_CONFIG, &ok);
	if (!ok) {
		pr("panel: DSIM0 does not answer: not used\n");
		return;
	}
	cfg = dr(DSIM_CONFIG);
	pr("panel: DSIM0 CONFIG 0x%08x (%s mode) CMD_CONFIG 0x%08x (packet-go %s) LINK0 0x%08x INTSRC 0x%08x "
	   "FIFOCTRL 0x%08x\n", cfg, cfg & CONFIG_VIDEO_MODE ? "video" : "command", dr(DSIM_CMD_CONFIG),
	   dr(DSIM_CMD_CONFIG) & CMD_PKT_GO_EN ? "on" : "off", dr(DSIM_LINK_STATUS0), dr(DSIM_INTSRC), dr(DSIM_FIFOCTRL));
	if (dr(DSIM_CMD_CONFIG) & CMD_PKT_GO_EN) {
		pr("panel: packet-go is on: not sending anything\n");
		return;
	}
	r1 = dcs_read(0x0a, 1, pm, &raw);
	pr("panel: power mode (0x0a): %d 0x%02x (raw 0x%08x)\n", r1, pm[0], raw);
	r2 = dcs_read(0x52, 2, br, &raw);
	pr("panel: brightness (0x52): %d 0x%02x 0x%02x (raw 0x%08x)\n", r2, br[0], br[1], raw);
	if (r2)
		return;
	set[0] = 0x51;
	set[1] = br[0] >> 1;
	set[2] = (u8)((br[0] & 1) << 7 | br[1] >> 1);
	r3 = send(DT_DCS_LONG_WRITE, set, 3);
	delay_us(20000);
	r2 = dcs_read(0x52, 2, br2, &raw);
	pr("panel: brightness set to half (0x51 %02x %02x): send %d, reads back %d 0x%02x 0x%02x\n",
	   set[1], set[2], r3, r2, br2[0], br2[1]);
	delay_us(1000000);
	set[1] = br[0];
	set[2] = br[1];
	r3 = send(DT_DCS_LONG_WRITE, set, 3);
	delay_us(20000);
	r2 = dcs_read(0x52, 2, br2, &raw);
	pr("panel: brightness back: send %d, reads back %d 0x%02x 0x%02x\n", r3, r2, br2[0], br2[1]);
}
