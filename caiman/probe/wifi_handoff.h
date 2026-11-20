/*
 * The BCM4390 handed to the kernel (PROBE_WIFI=13): the loader brings the chip up -- PCIe link, firmware,
 * msgbuf rings, CLM, "up", the RX pool posted -- and leaves it running; this page says where everything is
 * and how far each ring has gone, and its address goes to XNU as bcm4390_shared_pa=. The kernel's
 * drivers/wifi/hakios.bcm4390.c carries on from here (it has a copy of this layout: keep the two the same).
 *
 * The host block is in the loader's own image, outside XNU's memory: the kernel maps it uncached.
 */
#ifndef WIFI_HANDOFF_H
#define WIFI_HANDOFF_H

#define WIFI_HANDOFF_MAGIC	0x48423431u	/* "14BH" */
#define WIFI_HANDOFF_VERSION	1u

struct wifi_handoff {
	u32 magic, version, size, pad0;
	u64 mem_pa;		/* the host block: rings, index arrays, buffers */
	u32 mem_size, pad1;
	u64 cfg_pa;		/* the chip's PCIe config space (BAR windows at +0x80, +0x84; command at +4) */
	u64 bar0_pa;		/* the backplane window (the PCIe core's registers at +0x2000: DB0 +0x140, DB1 +0x144) */
	u64 bar2_pa;		/* dongle RAM, 4 MiB at a time through the window at config +0x84 */
	u32 bar2_win;		/* that window as left */
	u32 sh;			/* the shared area (pciedev_shared_t) in dongle RAM */
	u32 ri;			/* ring_info */
	u32 h2dw, h2dr, d2hw, d2hr;	/* the TCM index arrays */
	u32 cons, cidx;		/* the firmware's console, and how far it was read */
	/* the host block's layout */
	u32 ring_off[5], ring_items[5], ring_len[5];	/* ctrl submit, rxpost, ctrl complete, txcpl, rxcpl */
	u32 m_idx;		/* index arrays: H2D WR, H2D RD, D2H WR, D2H RD, 0x100 apart */
	u32 m_ioctbuf, m_resp, m_evt, n_evt, m_rx, n_rx, m_tx, n_tx, m_flowring, flowring_items;
	/* how far the rings have gone */
	u32 wr, rd;		/* ctrl submit WR, ctrl complete RD */
	u32 rxwr, rxrd;		/* rxpost WR, rxcpl RD */
	u32 txcrd;		/* txcpl RD */
	u32 trans;		/* the last ioctl's transaction id */
	u8 h2d_epoch, d2h_epoch, rx_epoch, rxc_epoch, txc_epoch, pad2[3];
	u8 mac[6], pad3[2];	/* the station's address */
	u32 max_h2d, max_d2h;	/* ring_info's ring counts (for flow rings) */
};

#endif
