// SPDX-License-Identifier: MIT
/*
 * PROBE_WIFI=1: the Wi-Fi's PCIe link as the bootloader leaves it, read only (docs/wifi-plan.md). The BCM4390 is
 * on HSI2's PCIe root complex 1 (DT pcie@13120000): ELBI 0x13120000, SoC 0x13130000, UDBG 0x13150000, PHY
 * 0x13160000, SYSREG 0x13020000, DBI 0x13400000, PCS 0x13140000; its pins (PERST# gph3-0, CLKREQ# gph3-1,
 * WL_REG_ON gph3-4, DEV_WAKE gph3-5) in HSI2's pinctrl 0x13040000, whose bank layout is not known here, so the
 * first 0x200 bytes of it are shown. Every first read of a block is one that survives a block whose clock is off.
 */
#include "probe.h"
#include "wifi_handoff.h"

#if PROBE_WIFI >= 3
#define ELBI	((void *)0x13120000ULL)
#define SOC	((void *)0x13130000ULL)
#define PCS	((void *)0x13140000ULL)
#define UDBG	((void *)0x13150000ULL)
#define PHY	((void *)0x13160000ULL)
#define DBI	0x13400000ULL
#define PIN	0x13040000ULL		/* HSI2 pinctrl bank +0x00 = gph3 (run wifi1): CON +0, DAT +4 */
#define EP_TYPE	1			/* DT ep-device-type 1 = EP_BCM_WIFI (cmp #1 in the CAL module's phy_config) */
#define EP_BCM_WIFI 1
#define EP_QC_WIFI 4
#define EP_SAMSUNG_WIFI 5
#define PCIE_SLV_PEND_SEL_NAK 0x03D8

static inline u32 readl(void *a) { return *(volatile u32 *)a; }
static inline void writel(u32 v, void *a) { *(volatile u32 *)a = v; }

/* Linux's check_exynos_pcie_reg_status: up to 1000 x 10 us for the bit */
static u32 reg_wait(void *base, int off, int bit, int *cnt)
{
	u32 st = 0;
	int i;

	for (i = 0; i < 1000; i++) {
		delay_us(10);
		st = readl((char *)base + off) & (1u << bit);
		if (st)
			break;
	}
	*cnt = i;
	return st;
}

#include "wifi_cal.inc"

static void pin(u32 n, u32 func, int out)
{
	u32 con = rd32(PIN), dat = rd32(PIN + 4);

	if (out >= 0)
		wr32(PIN + 4, out ? dat | 1u << n : dat & ~(1u << n));
	wr32(PIN, (con & ~(0xfu << (4 * n))) | func << (4 * n));
}

#if PROBE_WIFI >= 8
extern const u8 wifi_blob_start[] __attribute__((visibility("hidden")));	/* (hidden: no GOT entry to relocate) */

/* dongle RAM through BAR2 (DHD's BAR1): its window at config 0x84 picks the 4 MiB the address is in */
static u32 win = 0xffffffff;	/* the window last written; forgotten when the chip may have changed it */

static u64 bar2_at(u64 cfg, u64 bar2, u32 a)
{
	if ((a & ~0x3fffffu) != win) {
		win = a & ~0x3fffffu;
		wr32(cfg + 0x84, win);
		(void)rd32(cfg + 0x84);
	}
	return bar2 + (a & 0x3fffff);
}

static void dram_w(u64 cfg, u64 bar2, u32 a, u32 v) { wr32(bar2_at(cfg, bar2, a), v); }
static u32 dram_r(u64 cfg, u64 bar2, u32 a) { return rd32(bar2_at(cfg, bar2, a)); }

#if PROBE_WIFI >= 9
/* the firmware's console (hnd_cons_t: +0x08 log.buf, +0x0c log.buf_size, +0x10 log.idx) from index 'from' on,
 * line by line as dhdpcie_bus_readconsole does; returns the index it read up to */
static u32 cons_print(u64 cfg, u64 bar2, u32 cons, u32 from)
{
	u32 buf = dram_r(cfg, bar2, cons + 8), bsize = dram_r(cfg, bar2, cons + 12), idx = dram_r(cfg, bar2, cons + 16);
	u32 i, j, n = 0;
	char line[161];

	if (buf < 0x2a0000 || buf >= 0x5c0000 || idx > bsize || bsize > 0x10000 || from > idx)
		return idx;
	for (i = from & ~3u; i < idx && i - from < 8192; i += 4) {
		u32 v = dram_r(cfg, bar2, buf + i);

		for (j = 0; j < 4 && i + j < idx; j++) {
			char c = (char)(v >> (8 * j));

			if (i + j < from)
				continue;
			if (c == '\n' || n == sizeof(line) - 1) {
				line[n] = 0;
				if (n)
					pr("wifi: fw| %s\n", line);
				n = 0;
			} else if (c >= 0x20 && c < 0x7f)
				line[n++] = c;
		}
	}
	line[n] = 0;
	if (n)
		pr("wifi: fw| %s\n", line);
	return idx;
}
#endif

#if PROBE_WIFI >= 10
/*
 * PROBE_WIFI=10: the first conversation with the firmware over msgbuf (Google's bcmdhd dhd_prot_init and
 * dhd_fillup_ioct_reqst; docs/bcm4390-msgbuf.md), polled, no MSI. Host memory is one static block in this image
 * (MMU and D-cache are off, so nothing to clean); the chip is handed only these addresses. Bus master goes on
 * just for this and off again before the CA7 is halted.
 */
#define WIFI_BAR0	0x60000000ULL
#define WIFI_PCIEREGS	(WIFI_BAR0 + 0x2000)	/* the PCIe core's registers at BAR0 + 8K (nci_corereg_addr, SI_FAST) */
#define WIFI_DB0	(WIFI_PCIEREGS + 0x140)	/* hosttodev0doorbell0: ring updates */
#define WIFI_DB1	(WIFI_PCIEREGS + 0x144)	/* hosttodev0doorbell1: host ready (dhd_bus_hostready) */

static u8 wifi_mem[0x13d000] __attribute__((aligned(4096)));	/* wifi27: room for 480 RX buffers; then TX (PROBE_WIFI=13) */

enum { R_CTRL_SUB, R_RXPOST, R_CTRL_CPL, R_TXCPL, R_RXCPL };
static const u16 ring_items[5] = { 64, 512, 64, 1024, 512 };
static const u16 ring_len[5] = { 40, 32, 24, 24, 40 };
static const u32 ring_off[5] = { 0x00000, 0x02000, 0x06000, 0x07000, 0x0d000 };
#define M_SCRATCH	0x12000
#define M_IOCTBUF	0x13000
#define M_TRAP		0x14000
#define M_RESP		0x16000	/* 8 KiB ioctl response buffer */
#define M_IDX		0x1a000	/* DMA index arrays (PCIE_SHARED_DMA_INDEX, as Android's DHD uses): H2D WR, H2D RD,
				 * D2H WR, D2H RD, 0x100 apart, 4-byte entries */
#define M_EVT		0x1c000	/* event buffers: 8 x 4 KiB (PROBE_WIFI >= 11) */
#define N_EVT		8
#define M_RX		0x24000	/* receive buffers: 32 x 2 KiB on the RX post ring (PROBE_WIFI >= 12): the chip DMAs
				 * received data frames, EAPOL too, straight into host buffers */
#define M_FLOWRING	0x114000	/* PROBE_WIFI=13: a TX flow ring for the kernel, 0x8000 bytes */
#define M_TX		0x11c000	/* ... and its buffers, 64 x 2 KiB */
#define N_TX		64
#define M_HANDOFF	0x13c000	/* the page for the kernel (wifi_handoff.h) */
#define N_RX		480	/* (ring 1 holds 512) wifi25: 32 left the firmware in its reserve mode ("pciedev_use_resv_rxpost"), dropping data */

/* the PCIe core at BAR0 + 8K: only the doorbells and the DAR block (+0xa00..) may be read; other offsets (+0x004,
 * +0x020..) are secure on this chip, and reading them from the host makes the firmware trap ("AXI Slave Err ...
 * Secure slave", docs/probe-log-wifi10-trap.txt) */

static u64 mem_pa(u32 off) { return (u64)wifi_mem + off; }
static void wr_le(u8 *p, u32 off, u32 v) { *(volatile u32 *)(p + off) = v; }

/* the conversation's state: dongle addresses of the index slots, ring positions and epochs */
struct mb {
	u64 cfg, bar2;
	u32 h2dw, h2dr, d2hw, d2hr, cons, cidx;
	u32 wr, rd;			/* H2D control submit write, D2H control complete read */
	u8 h2d_epoch, d2h_epoch;
	u16 trans;
	int done, status;		/* the ioctl completion seen, and its status */
	u32 resp_len, scan_bss, scan_done;
	u32 link, keyed, failed;	/* PROBE_WIFI >= 12: the join's outcome from its events */
	u32 rxwr, rxrd, rx_count;	/* RX post write, RX complete read, frames seen */
	u8 rx_epoch, rxc_epoch, rxrd_pending;
	const u8 *want;			/* wifi15: the network joined, and its best 2.4 GHz BSS from the scan */
	u32 want_len;
	u8 pin[6];
	u16 pin_cs;
	int pin_rssi;
};

static u32 rd_le32(const volatile u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u32 rd_be32(const volatile u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* one 40-byte item on the H2D control ring: w[0] bits 8..23 (if_id, flags) and w[1..9] from the caller, the
 * message type and epoch here; the host WR copy and the TCM slot are updated and doorbell 0 rung */
static int h2d_post(struct mb *m, u8 type, const u32 *w)
{
	u8 *it = wifi_mem + ring_off[R_CTRL_SUB] + 40 * m->wr;
	u32 i, next = (m->wr + 1) % ring_items[R_CTRL_SUB];
	u64 t0 = timer_us();

	while (next == *(volatile u32 *)(wifi_mem + M_IDX + 0x100))	/* the firmware's H2D RD, DMA'd here */
		if (timer_us() - t0 > 100000) {
			pr("wifi: H2D control ring full (wr %u)\n", m->wr);
			return -1;
		}
	for (i = 1; i < 10; i++)
		wr_le(it, 4 * i, w[i]);
	wr_le(it, 0, type | (w[0] & 0x00ffff00u) | (u32)m->h2d_epoch << 24);
	m->h2d_epoch = m->h2d_epoch == 252 ? 0 : m->h2d_epoch + 1;
	m->wr = next;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile u32 *)(wifi_mem + M_IDX) = m->wr;
	__asm__ volatile("dsb sy" ::: "memory");
	dram_w(m->cfg, m->bar2, m->h2dw, m->wr);
	wr32(WIFI_DB0, 0xff000000u | m->wr);	/* DHD_WRPTR_UPDATE_H2D_DB_MAGIC, ring 0 */
	return 0;
}

/* MSG_TYPE_IOCTLRESP_BUF_POST 0xb / MSG_TYPE_EVENT_BUF_POST 0xd: a host buffer the firmware may fill */
static int buf_post(struct mb *m, u8 type, u32 reqid, u32 off, u32 len)
{
	u32 w[10] = { 0, reqid, len, 0, (u32)mem_pa(off), 0, 0, 0, 0, 0 };

	return h2d_post(m, type, w);
}

/* MSG_TYPE_RXBUF_POST 0x11 on the RX post ring (H2D ring 1, 32-byte items): data_buf_len +10, data_buf_addr +24;
 * 'ring' says whether to update the index and ring the doorbell now */
static void rx_post(struct mb *m, u32 b, int ring)
{
	u8 *it = wifi_mem + ring_off[R_RXPOST] + 32 * m->rxwr;
	u32 i;

	for (i = 0; i < 32; i += 4)
		wr_le(it, i, 0);
	wr_le(it, 4, 0x300 + b);
	wr_le(it, 8, 2048u << 16);
	wr_le(it, 24, (u32)mem_pa(M_RX + 2048 * b));
	wr_le(it, 0, 0x11 | (u32)m->rx_epoch << 24);
	m->rx_epoch = m->rx_epoch == 252 ? 0 : m->rx_epoch + 1;
	m->rxwr = (m->rxwr + 1) % ring_items[R_RXPOST];
	if (!ring)
		return;
	__asm__ volatile("dsb sy" ::: "memory");
	*(volatile u32 *)(wifi_mem + M_IDX + 4) = m->rxwr;
	__asm__ volatile("dsb sy" ::: "memory");
	dram_w(m->cfg, m->bar2, m->h2dw + 4, m->rxwr);
	wr32(WIFI_DB0, 0xff000000u | 1u << 16 | m->rxwr);
}

/* the RX complete ring (D2H ring 4, 40-byte MSG_TYPE_RX_CMPLT 0x12 items): buffer by request id, data_len +14,
 * data_offset +16; the frame is printed by its ethertype, the buffer posted again */
static void rx_poll(struct mb *m)
{
	u32 dw = *(volatile u32 *)(wifi_mem + M_IDX + 0x200 + 8);

	while (m->rxrd != dw) {
		volatile u8 *c = wifi_mem + ring_off[R_RXCPL] + 40 * m->rxrd;
		u32 k, id, dl, doff;

		for (k = 0; k < 1000 && c[3] != m->rxc_epoch; k++)
			delay_us(50);
		id = rd_le32(c + 4) - 0x300;
		dl = c[14] | c[15] << 8;
		doff = c[16] | c[17] << 8;
		if (c[0] == 0x12 && id < N_RX) {
			const volatile u8 *f = wifi_mem + M_RX + 2048 * id + doff;
			u32 et = dl >= 14 ? (u32)f[12] << 8 | f[13] : 0;

			if (m->rx_count < 40)
				pr("wifi: RX frame %u: %u bytes, ethertype 0x%04x%s, from %02x:%02x:%02x:%02x:%02x:%02x\n",
				   m->rx_count, dl, et, et == 0x888e ? " (EAPOL)" : "", f[6], f[7], f[8], f[9], f[10], f[11]);
			if (m->rx_count < 6)	/* wifi27: the whole 40-byte completion (flags +0x12: 1 802.3, 2 802.11) */
				pr("wifi:   cmpl if %u id 0x%x status 0x%x meta %u len %u off %u flags 0x%x\n", c[1],
				   id + 0x300, rd_le32(c + 8), c[12] | c[13] << 8, dl, doff, c[18] | c[19] << 8);
			m->rx_count++;
			/* wifi27: RD published (host slot, TCM) and the dongle told (DB0 0xdd | ring 4 | RD) before the repost */
			m->rxrd_pending = 1;
		} else
			pr("wifi: RX complete item type 0x%x id 0x%x\n", c[0], id + 0x300);
		m->rxc_epoch = m->rxc_epoch == 252 ? 0 : m->rxc_epoch + 1;
		m->rxrd = (m->rxrd + 1) % ring_items[R_RXCPL];
		*(volatile u32 *)(wifi_mem + M_IDX + 0x300 + 8) = m->rxrd;
		dram_w(m->cfg, m->bar2, m->d2hr + 8, m->rxrd);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(WIFI_DB0, 0xdd000000u | 4u << 16 | m->rxrd);
		if (m->rxrd_pending) {
			m->rxrd_pending = 0;
			rx_post(m, id, 1);
		}
	}
}

/* an event the firmware DMA'd into event buffer 'b' (ethernet + bcmeth headers, then the big-endian
 * wl_event_msg_t at +24, its data at +72) */
static void wifi_event(struct mb *m, u32 b, u32 len)
{
	const volatile u8 *e = wifi_mem + M_EVT + 0x1000 * b;
	u32 type = rd_be32(e + 28), status = rd_be32(e + 32), dlen = rd_be32(e + 44);

	if (type == 69) {	/* WLC_E_ESCAN_RESULT: wl_escan_result (buflen, version, sync_id, bss_count), bss_info */
		const volatile u8 *r = e + 72, *bi = r + 12;
		u32 n = r[10] | r[11] << 8, k;

		for (k = 0; k < n && dlen >= 12 + 80; k++) {
			char ssid[33];
			u32 j, sl = bi[18] > 32 ? 32 : bi[18], blen = rd_le32(bi + 4);

			for (j = 0; j < sl; j++)
				ssid[j] = bi[19 + j] >= 0x20 && bi[19 + j] < 0x7f ? bi[19 + j] : '?';
			ssid[sl] = 0;
			pr("wifi: network %2u: \"%s\"  %02x:%02x:%02x:%02x:%02x:%02x  RSSI %d dBm  chanspec 0x%04x\n",
			   m->scan_bss, sl ? ssid : "(hidden)", bi[8], bi[9], bi[10], bi[11], bi[12], bi[13],
			   (int)(short)(bi[78] | bi[79] << 8), bi[72] | bi[73] << 8);
			if (m->want_len && sl == m->want_len) {
				int rssi = (short)(bi[78] | bi[79] << 8);

				for (j = 0; j < sl && bi[19 + j] == m->want[j]; j++)
					;
				if (j == sl && m->scan_bss < 200) {
					/* wifi17: what this BSS of ours advertises (wl_bss_info_t: ie_offset +116, ie_length +120) */
					u32 io = bi[116] | bi[117] << 8, il = rd_le32(bi + 120), q, w;

					for (q = 0; io + il <= blen && q + 2 <= il; q += 2 + bi[io + q + 1])
						if (bi[io + q] == 48) {
							pr("wifi:   our network %02x:%02x:%02x:xx:xx:xx chanspec 0x%04x RSN:", bi[8], bi[9],
							   bi[10], bi[72] | bi[73] << 8);
							for (w = 0; w < bi[io + q + 1] + 2u && w < 40; w++)
								pr(" %02x", bi[io + q + w]);
							pr("\n");
						}
				}
				if (j == sl && (bi[73] & 0xc0) == 0 && (!m->pin_cs || rssi > m->pin_rssi)) {
					for (j = 0; j < 6; j++)
						m->pin[j] = bi[8 + j];
					m->pin_cs = (u16)(bi[72] | bi[73] << 8);
					m->pin_rssi = rssi;
				}
			}
			m->scan_bss++;
			if (blen < 80 || blen > 4000)
				break;
			bi += blen;
		}
		if (status != 8) {	/* 8: WLC_E_STATUS_PARTIAL, more to come; 0: the scan is over */
			pr("wifi: scan finished, status %u, %u networks seen\n", status, m->scan_bss);
			m->scan_done = 1;
		}
	} else {
		/* 0 SET_SSID, 1 JOIN, 3 AUTH, 5 DEAUTH, 6 DEAUTH_IND, 7 ASSOC, 11 DISASSOC, 12 DISASSOC_IND, 16 LINK,
		 * 46 PSK_SUP, 47 COUNTRY_CODE_CHANGED, 54 IF (no string table: the image takes no relocations) */
		u32 flags = (u32)e[26] << 8 | e[27], reason = rd_be32(e + 36);

		pr("wifi: event %u status %u reason %u flags 0x%x, %u bytes, peer %02x:%02x:%02x:%02x:%02x:%02x\n", type, status,
		   reason, flags, dlen, e[48], e[49], e[50], e[51], e[52], e[53]);
		if (type == 46 && dlen <= 64) {	/* PSK_SUP's data, whole */
			u32 j;

			for (j = 0; j < dlen; j += 16)
				pr("wifi:   data +%02u: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
				   j, e[72 + j], e[73 + j], e[74 + j], e[75 + j], e[76 + j], e[77 + j], e[78 + j], e[79 + j],
				   e[80 + j], e[81 + j], e[82 + j], e[83 + j], e[84 + j], e[85 + j], e[86 + j], e[87 + j]);
		} else if (type == 7 && dlen < 1000) {	/* ASSOC: the IEs; the RSN IE (48) and its AKM/capabilities */
			u32 j = 0;

			while (j + 2 <= dlen) {
				u32 id = e[72 + j], l = e[73 + j], q;

				if (id == 48) {
					pr("wifi:   RSN IE at +%u, %u bytes:", j, l);
					for (q = 0; q < l && q < 40; q++)
						pr(" %02x", e[74 + j + q]);
					pr("\n");
				}
				j += 2 + l;
			}
		}
		if (type == 16)		/* WLC_E_LINK: flags bit 0 WLC_EVENT_MSG_LINK */
			m->link = flags & 1;
		else if (type == 46 && status == 6)	/* WLC_E_PSK_SUP, WLC_SUP_KEYED */
			m->keyed = 1;
		else if (type == 46 || type == 5 || type == 6 || type == 11 || type == 12 || (type == 0 && status != 0))
			m->failed = 1;
	}
	(void)len;
	buf_post(m, 0x0d, 0x200 + b, M_EVT + 0x1000 * b, 0x1000);	/* the buffer back to the firmware */
}

/* everything new on the D2H control complete ring, until 'stop' says so or 'us' passes */
static void d2h_poll(struct mb *m, u64 us, int (*stop)(struct mb *))
{
	u64 t0 = timer_us();

	while (timer_us() - t0 < us && !stop(m)) {
		u32 dw = *(volatile u32 *)(wifi_mem + M_IDX + 0x200);	/* D2H WR of ring 2, DMA'd by the firmware */

		while (m->rd != dw) {
			volatile u8 *c = wifi_mem + ring_off[R_CTRL_CPL] + 24 * m->rd;
			u32 k, t;

			for (k = 0; k < 1000 && c[3] != m->d2h_epoch; k++)
				delay_us(50);
			t = c[0];
			if (c[3] != m->d2h_epoch)
				pr("wifi: D2H item %u: epoch %u, expected %u\n", m->rd, c[3], m->d2h_epoch);
			if (t == 0x0c) {	/* MSG_TYPE_IOCTL_CMPLT: status +8, resp_len +12 */
				m->status = (short)(c[8] | c[9] << 8);
				m->resp_len = c[12] | c[13] << 8;
				m->done = 1;
			} else if (t == 0x0e) {	/* MSG_TYPE_WL_EVENT: the buffer by request id, length +12 */
				u32 id = rd_le32(c + 4);

				if (id >= 0x200 && id < 0x200 + N_EVT)
					wifi_event(m, id - 0x200, c[12] | c[13] << 8);
			} else if (t != 0x0a)	/* not the ioctl ACK */
				pr("wifi: D2H item type 0x%x: %08x %08x %08x %08x\n", t, rd_le32(c), rd_le32(c + 4),
				   rd_le32(c + 8), rd_le32(c + 12));
			m->d2h_epoch = m->d2h_epoch == 252 ? 0 : m->d2h_epoch + 1;
			m->rd = (m->rd + 1) % ring_items[R_CTRL_CPL];
			*(volatile u32 *)(wifi_mem + M_IDX + 0x300) = m->rd;
			dram_w(m->cfg, m->bar2, m->d2hr, m->rd);
		}
		rx_poll(m);
		delay_us(500);
	}
}

static int ioctl_done(struct mb *m) { return m->done; }
static int never(struct mb *m) { (void)m; return 0; }
static int scan_over(struct mb *m) { return (int)m->scan_done; }
static int join_over(struct mb *m) { return (m->link && m->keyed) || m->failed; }

/* one ioctl (dhd_fillup_ioct_reqst): a response buffer, then the request; 'buf' in, the answer back in it */
static int wifi_ioctl(struct mb *m, u32 cmd, void *buf, u32 len, u32 outlen)
{
	u32 w[10] = { 0 }, i;

	for (i = 0; i < len; i++)
		wifi_mem[M_IOCTBUF + i] = ((const u8 *)buf)[i];
	m->done = 0;
	m->trans++;
	if (buf_post(m, 0x0b, 0x100, M_RESP, 8192))
		return -100;
	w[1] = 0xfffe;	/* DHD_IOCTL_REQ_PKTID */
	w[2] = cmd;
	w[3] = m->trans | len << 16;
	w[4] = outlen;
	w[6] = (u32)mem_pa(M_IOCTBUF);
	if (h2d_post(m, 0x09, w))
		return -100;
	d2h_poll(m, 2000000, ioctl_done);
	if (!m->done)
		return -101;
	for (i = 0; i < m->resp_len && i < outlen; i++)
		((u8 *)buf)[i] = wifi_mem[M_RESP + i];
	return m->status;
}

/* "name\0" + value as WLC_SET_VAR (263) / WLC_GET_VAR (262), bcm_mkiovar's layout */
static int wifi_iovar(struct mb *m, int set, const char *name, const void *val, u32 vlen, void *out, u32 outlen)
{
	static u8 b[1600];
	u32 n = 0, i;
	int r;

	while (name[n]) {
		b[n] = (u8)name[n];
		n++;
	}
	b[n++] = 0;
	for (i = 0; i < vlen; i++)
		b[n + i] = ((const u8 *)val)[i];
	r = wifi_ioctl(m, set ? 263 : 262, b, n + vlen, set ? n + vlen : (outlen > n + vlen ? outlen : n + vlen));
	for (i = 0; out && i < outlen; i++)
		((u8 *)out)[i] = b[i];
	return r;
}

#if PROBE_WIFI >= 13
static int wifi_handed;	/* the chip is the kernel's now: leave it running (wifi_link's teardown skipped) */

u64 wifi_handoff_pa(void)
{
	return wifi_handed ? mem_pa(M_HANDOFF) : 0;
}

/*
 * PROBE_WIFI=13: the chip, up, to the kernel. The events a station needs, the RX pool (480, published 32 at a time:
 * fewer leave the firmware dropping every frame, docs/wifi-plan.md step 12) and bus:host_access, then the page
 * that says where everything is (wifi_handoff.h). Returns 0 when handed over.
 */
static int wifi_handoff(struct mb *m, u64 cfg, u64 bar2, u32 sh, u32 ri, u8 *mask)
{
	static const u8 ev[13] = { 0, 1, 3, 5, 6, 7, 11, 12, 16, 46, 47, 54, 69 };
	struct wifi_handoff *h = (struct wifi_handoff *)(wifi_mem + M_HANDOFF);
	u32 i, v;
	int r;

	for (i = 0; i < 27; i++)
		mask[4 + i] = 0;
	for (i = 0; i < 13; i++)
		mask[4 + ev[i] / 8] |= 1 << (ev[i] % 8);
	r = wifi_iovar(m, 1, "event_msgs_ext", mask, 4 + 27, 0, 0);
	pr("wifi: handoff: station events %d\n", r);
	for (i = 0; i < N_RX; i++)
		rx_post(m, i, (i & 31) == 31);
	v = 1;
	r = wifi_iovar(m, 1, "bus:host_access", &v, 4, 0, 0);
	pr("wifi: handoff: %u RX buffers posted, bus:host_access %d\n", N_RX, r);
	for (i = 0; i < sizeof(*h); i++)
		((u8 *)h)[i] = 0;
	h->magic = WIFI_HANDOFF_MAGIC;
	h->version = WIFI_HANDOFF_VERSION;
	h->size = sizeof(*h);
	h->mem_pa = mem_pa(0);
	h->mem_size = sizeof(wifi_mem);
	h->cfg_pa = cfg;
	h->bar0_pa = WIFI_BAR0;
	h->bar2_pa = bar2;
	h->bar2_win = win;
	h->sh = sh;
	h->ri = ri;
	h->h2dw = m->h2dw;
	h->h2dr = m->h2dr;
	h->d2hw = m->d2hw;
	h->d2hr = m->d2hr;
	h->cons = m->cons;
	h->cidx = m->cidx;
	for (i = 0; i < 5; i++) {
		h->ring_off[i] = ring_off[i];
		h->ring_items[i] = ring_items[i];
		h->ring_len[i] = ring_len[i];
	}
	h->m_idx = M_IDX;
	h->m_ioctbuf = M_IOCTBUF;
	h->m_resp = M_RESP;
	h->m_evt = M_EVT;
	h->n_evt = N_EVT;
	h->m_rx = M_RX;
	h->n_rx = N_RX;
	h->m_tx = M_TX;
	h->n_tx = N_TX;
	h->m_flowring = M_FLOWRING;
	h->flowring_items = 0x8000 / 48;
	h->wr = m->wr;
	h->rd = m->rd;
	h->rxwr = m->rxwr;
	h->rxrd = m->rxrd;
	h->txcrd = 0;
	h->trans = m->trans;
	h->h2d_epoch = m->h2d_epoch;
	h->d2h_epoch = m->d2h_epoch;
	h->rx_epoch = m->rx_epoch;
	h->rxc_epoch = m->rxc_epoch;
	h->txc_epoch = 1;
	r = wifi_iovar(m, 0, "cur_etheraddr", 0, 0, h->mac, 6);
	h->max_h2d = dram_r(cfg, bar2, ri + 0x34) >> 16;	/* LE16 +0x36, +0x38 */
	h->max_d2h = dram_r(cfg, bar2, ri + 0x38) & 0xffff;
	pr("wifi: handoff: page at 0x%llx, MAC %02x:%02x:%02x:xx:xx:xx (%d), rings H2D %u D2H %u; the chip is the kernel's\n",
	   mem_pa(M_HANDOFF), h->mac[0], h->mac[1], h->mac[2], r, h->max_h2d, h->max_d2h);
	__asm__ volatile("dsb sy" ::: "memory");
	wifi_handed = 1;
	return 0;
}
#endif

static void wifi_msgbuf(u64 cfg, u64 bar2, u32 sh, u32 ri, u32 cons, u32 cidx)
{
	struct mb mb = { 0 }, *m = &mb;
	u32 ringmem = dram_r(cfg, bar2, ri), i, cap, ver = 0;
	int r;

	m->cfg = cfg;
	m->bar2 = bar2;
#if PROBE_WIFI >= 12 && defined(WIFI_PSK_PATH)
	{
		extern const u8 wifi_ssid_start[] __attribute__((visibility("hidden")));
		extern const u8 wifi_ssid_end[] __attribute__((visibility("hidden")));

		m->want = wifi_ssid_start;
		m->want_len = (u32)(wifi_ssid_end - wifi_ssid_start);
		while (m->want_len && (m->want[m->want_len - 1] == '\n' || m->want[m->want_len - 1] == '\r'))
			m->want_len--;
	}
#endif
	m->h2dw = dram_r(cfg, bar2, ri + 4);
	m->h2dr = dram_r(cfg, bar2, ri + 8);
	m->d2hw = dram_r(cfg, bar2, ri + 12);
	m->d2hr = dram_r(cfg, bar2, ri + 16);
	m->cons = cons;
	m->cidx = cidx;
	m->h2d_epoch = m->d2h_epoch = m->rx_epoch = m->rxc_epoch = 1;
	pr("wifi: msgbuf: host block at 0x%llx (0x%x bytes); ringmem 0x%x, H2D w/r 0x%x/0x%x, D2H w/r 0x%x/0x%x\n",
	   mem_pa(0), (u32)sizeof(wifi_mem), ringmem, m->h2dw, m->h2dr, m->d2hw, m->d2hr);
	if (mem_pa(sizeof(wifi_mem)) > 0x100000000ULL) {
		pr("wifi: msgbuf: host block above 4 GiB, stopping\n");
		return;
	}
	/* host_cap (shared +0x54): IPC rev 9, host ready (the firmware advertises HOSTRDY_SUPPORT), no deep sleep,
	 * extended trap data, no trap on UR */
	cap = 9 | 0x400 | 0x1000 | 0x20000 | 0x800000;
	dram_w(cfg, bar2, sh + 0x54, cap);
	dram_w(cfg, bar2, sh + 0x58, (u32)mem_pa(M_TRAP));	/* host_trap_addr */
	dram_w(cfg, bar2, sh + 0x5c, 0);
	for (i = 0; i < 5; i++) {
		u32 rm = ringmem + 16 * i, slot = 4 * (i < 2 ? i : i - 2);

		dram_w(cfg, bar2, rm + 4, ring_items[i] | (u32)ring_len[i] << 16);	/* max_item, len_items */
		dram_w(cfg, bar2, rm + 8, (u32)mem_pa(ring_off[i]));
		dram_w(cfg, bar2, rm + 12, 0);
		dram_w(cfg, bar2, (i < 2 ? m->h2dw : m->d2hw) + slot, 0);
		dram_w(cfg, bar2, (i < 2 ? m->h2dr : m->d2hr) + slot, 0);
	}
	/* ring_info +0x14/+0x1c/+0x24/+0x2c: the host-side index arrays the firmware syncs by DMA */
	for (i = 0; i < 4; i++) {
		dram_w(cfg, bar2, ri + 0x14 + 8 * i, (u32)mem_pa(M_IDX + 0x100 * i));
		dram_w(cfg, bar2, ri + 0x18 + 8 * i, 0);
	}
	dram_w(cfg, bar2, sh + 0x34, 8);	/* host_dma_scratch_buffer_len, then its address */
	dram_w(cfg, bar2, sh + 0x38, (u32)mem_pa(M_SCRATCH));
	dram_w(cfg, bar2, sh + 0x3c, 0);

	wr32(cfg + 4, rd32(cfg + 4) | 4);	/* bus master on */
	/* host ready, as dhd_prot_init does when hostrdy_after_init (shared flags HOSTRDY_SUPPORT) */
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(WIFI_DB1, 0x12345678);
	delay_us(10000);

	r = wifi_ioctl(m, 1, &ver, 4, 4);	/* WLC_GET_VERSION */
	pr("wifi: WLC_GET_VERSION: status %d, version %u\n", r, ver);
#if PROBE_WIFI >= 11
	{
		/*
		 * PROBE_WIFI=11: the CLM blob, the escan result event, "up" and a scan of all channels, as Android's
		 * DHD does (dhd_apply_default_clm, wl_cfgscan); the networks found are printed
		 */
		extern const u8 wifi_clm_start[] __attribute__((visibility("hidden")));
		extern const u8 wifi_clm_end[] __attribute__((visibility("hidden")));
		static u8 chunk[12 + 1408 + 8];
		u32 len = (u32)(wifi_clm_end - wifi_clm_start), off = 0, n, st = 0xffffffff;
		u8 mask[4 + 27] = { 0 }, p[8 + 80] = { 0 };
		u64 t0 = timer_us();

		for (i = 0; i < N_EVT; i++)
			buf_post(m, 0x0d, 0x200 + i, M_EVT + 0x1000 * i, 0x1000);
		/* in 1408-byte pieces (dhd_download_blob): wl_dload_data_t flag (handler version 1 << 12, DL_BEGIN 2,
		 * DL_END 4), dload_type DL_TYPE_CLM 2, len, crc 0, data; each rounded up to 8 */
		r = 0;
		while (off < len && r == 0) {
			u32 flag = 0x1000 | (off == 0 ? 2 : 0);

			n = len - off > 1408 ? 1408 : len - off;
			if (off + n == len)
				flag |= 4;
			for (i = 0; i < 12 + n + 8; i++)
				chunk[i] = i >= 12 && i < 12 + n ? wifi_clm_start[off + i - 12] : 0;
			chunk[0] = (u8)flag;
			chunk[1] = (u8)(flag >> 8);
			chunk[2] = 2;
			chunk[4] = (u8)n;
			chunk[5] = (u8)(n >> 8);
			r = wifi_iovar(m, 1, "clmload", chunk, (12 + n + 7) & ~7u, 0, 0);
			off += n;
		}
		wifi_iovar(m, 0, "clmload_status", 0, 0, &st, 4);
		pr("wifi: CLM blob %u bytes loaded in %llu ms: last status %d, clmload_status %d\n", len,
		   (timer_us() - t0) / 1000, r, (int)st);
		m->cidx = cons_print(cfg, bar2, cons, m->cidx);

		/* eventmsgs_ext_t: ver 1, command EVENTMSGS_SET_MASK 3, mask length, maxgetsize, mask (this firmware
		 * has no plain "event_msgs": -23, wifi11) */
		mask[0] = 1;
		mask[1] = 3;
		mask[2] = 27;	/* (WLC_E_LAST 215 + 7) / 8 */
		mask[4 + 69 / 8] |= 1 << (69 % 8);	/* WLC_E_ESCAN_RESULT only */
		r = wifi_iovar(m, 1, "event_msgs_ext", mask, 4 + 27, 0, 0);
		pr("wifi: event_msgs_ext: status %d\n", r);
		{
			/*
			 * wifi20: the cal file's macaddr is Broadcom's placeholder 00:90:4c:12:d0:01 and Android always sets
			 * its own; a locally administered unicast one instead, before WLC_UP ("cur_etheraddr")
			 */
			u8 mac[8] = { 0 };
			u64 t = timer_us();

			r = wifi_iovar(m, 0, "cur_etheraddr", 0, 0, mac, 6);
			pr("wifi: MAC was %02x:%02x:%02x:%02x:%02x:%02x (%d)", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], r);
			mac[0] = 0x02;
			mac[1] = 0x1a;
			mac[2] = 0x11;
			mac[3] = (u8)(t >> 16);
			mac[4] = (u8)(t >> 8);
			mac[5] = (u8)t;
			r = wifi_iovar(m, 1, "cur_etheraddr", mac, 6, 0, 0);
			pr(", now 02:1a:11:%02x:%02x:%02x: %d\n", mac[3], mac[4], mac[5], r);
		}
		{
			/*
			 * wifi21: EHT off before WLC_UP (an EHT-capable, non-MLO STA on the MLD's 5 GHz link: docs/wifi-join-
			 * analysis.md, trials 14-20, 4): "eht" XTLV WL_EHT_CMD_ENAB (0), length 4, LE32; and the firmware's
			 * own RX buffer size ("rxbufpost_sz", LE16)
			 */
			u8 x[16] = { 0 };
			u32 k;

			x[2] = 4;
			r = wifi_iovar(m, 0, "eht", x, 8, x, 16);
			pr("wifi: eht get (%d):", r);
			for (k = 0; k < 12; k++)
				pr(" %02x", x[k]);
			for (k = 0; k < 16; k++)
				x[k] = 0;
			x[2] = 4;
			r = wifi_iovar(m, 1, "eht", x, 8, 0, 0);
			pr("; set 0: %d\n", r);
			for (k = 0; k < 16; k++)
				x[k] = 0;
			r = wifi_iovar(m, 0, "rxbufpost_sz", 0, 0, x, 4);
			pr("wifi: rxbufpost_sz %u (%d)\n", x[0] | x[1] << 8, r);
		}
		i = 0;
		r = wifi_ioctl(m, 2, &i, 4, 4);	/* WLC_UP */
		pr("wifi: WLC_UP: status %d\n", r);
		d2h_poll(m, 300000, never);
		m->cidx = cons_print(cfg, bar2, cons, m->cidx);
#if PROBE_WIFI >= 13
		if (wifi_handoff(m, cfg, bar2, sh, ri, mask) == 0)
			return;
#endif
		{
			u8 sv[8] = { 0 };

			r = wifi_iovar(m, 0, "scan_ver", 0, 0, sv, 8);	/* wl_scan_version_t */
			pr("wifi: scan_ver: status %d: version %u length %u scan interface major %u\n", r,
			   sv[0] | sv[1] << 8, sv[2] | sv[3] << 8, sv[4] | sv[5] << 8);
		}
		/* wl_escan_params_vN: version, action WL_SCAN_ACTION_START 1, sync id 0x1234, then
		 * wl_scan_params_vN (v2 and up: version, length of the fixed part, ssid, bssid, bss_type, ssid_type,
		 * scan_type[, scan_type_ext, v4], nprobes, active/passive/home time[, scan_tx_cnt], channel_num): no SSID,
		 * broadcast BSSID, any BSS type (2), defaults (-1), all channels. v1 was refused with -37 (BCME_VERSION) */
		{
			static const u8 vers[4][2] = { { 4, 76 }, { 4, 80 }, { 3, 72 }, { 2, 72 } };
			u32 k, o;

			r = -37;
			for (k = 0; k < 4 && r == -37; k++) {
				u32 v = vers[k][0], fixed = vers[k][1];

				for (i = 0; i < sizeof(p); i++)
					p[i] = 0;
				p[0] = (u8)v;
				p[4] = 1;
				p[6] = 0x34;
				p[7] = 0x12;
				p[8] = (u8)v;
				p[10] = (u8)fixed;
				for (i = 48; i < 54; i++)
					p[i] = 0xff;
				p[54] = 2;
				o = v >= 4 ? 64 : 60;	/* after scan_type (and scan_type_ext) */
				for (i = o; i < o + 16 + (fixed == 80 ? 4 : 0); i++)
					p[i] = 0xff;
				t0 = timer_us();
				r = wifi_iovar(m, 1, "escan", p, 8 + fixed, 0, 0);
				pr("wifi: escan v%u (params %u bytes): status %d\n", v, fixed, r);
			}
		}
		if (r == 0)
			d2h_poll(m, 10000000, scan_over);
		pr("wifi: scan: %llu ms, %u networks\n", (timer_us() - t0) / 1000, m->scan_bss);
#if PROBE_WIFI >= 12 && defined(WIFI_PSK_PATH)
		{
			/*
			 * PROBE_WIFI=12: join the owner's WPA2-PSK network with the firmware's own supplicant (sup_wpa),
			 * as brcmfmac does with firmware supplicant: infra 1, open auth 0, wsec AES 4, wpa_auth WPA2_AUTH_PSK
			 * 0x80, the passphrase as wsec_pmk_t (WLC_SET_WSEC_PMK 268, flags WSEC_PASSPHRASE 1), then
			 * WLC_SET_SSID 26; success is WLC_E_LINK up plus WLC_E_PSK_SUP keyed (6). The passphrase is never
			 * printed.
			 */
			extern const u8 wifi_ssid_start[] __attribute__((visibility("hidden")));
			extern const u8 wifi_ssid_end[] __attribute__((visibility("hidden")));
			extern const u8 wifi_psk_start[] __attribute__((visibility("hidden")));
			extern const u8 wifi_psk_end[] __attribute__((visibility("hidden")));
			static u8 pmk[264], ssid[52];
			u32 sl = (u32)(wifi_ssid_end - wifi_ssid_start), kl = (u32)(wifi_psk_end - wifi_psk_start), v;
			u8 bssid[8] = { 0 };

			while (kl != 32 && kl && (wifi_psk_start[kl - 1] == '\n' || wifi_psk_start[kl - 1] == '\r'))
				kl--;
			while (sl && (wifi_ssid_start[sl - 1] == '\n' || wifi_ssid_start[sl - 1] == '\r'))
				sl--;
			/* wifi22: an empty key file means an open network (no wsec, no AKM, no supplicant, no key) */
			if (sl > 32 || (kl && kl < 8) || kl > 64) {
				pr("wifi: join: SSID length %u or passphrase length %u out of range\n", sl, kl);
				goto join_out;
			}
			for (i = 0; i < 31; i++)
				mask[4 + i] = 0;
			for (i = 0; i < 13; i++) {
				static const u8 ev[13] = { 0, 1, 3, 5, 6, 7, 11, 12, 16, 46, 47, 54, 69 };

				mask[4 + ev[i] / 8] |= 1 << (ev[i] % 8);
			}
			r = wifi_iovar(m, 1, "event_msgs_ext", mask, 4 + 27, 0, 0);
			pr("wifi: join: event_msgs_ext %d\n", r);
			/* the RX pool first, then bus:host_access=1, as bcmdhd does before its preinit (docs/wifi-join-analysis.md) */
			/* wifi23: the buffers filled with 0xa5 first, to see whether the chip writes any of them at all */
			for (i = 0; i < N_RX * 2048; i++)
				wifi_mem[M_RX + i] = 0xa5;
			__asm__ volatile("dsb sy" ::: "memory");
			for (i = 0; i < N_RX; i++)
				rx_post(m, i, (i & 31) == 31);	/* published 32 at a time, as DHD does */
			v = 1;
			r = wifi_iovar(m, 1, "bus:host_access", &v, 4, 0, 0);
			pr("wifi: join: %u receive buffers posted; bus:host_access=1: %d\n", N_RX, r);
			/* brcmfmac's order: infra, auth, wsec, the AKM, then sup_wpa, then the key; each status on its own */
			v = 1;
			r = wifi_ioctl(m, 20, &v, 4, 4);	/* WLC_SET_INFRA */
			pr("wifi: join: infra 1: %d\n", r);
			v = 0;
			r = wifi_ioctl(m, 22, &v, 4, 4);	/* WLC_SET_AUTH: open */
			pr("wifi: join: auth open: %d\n", r);
			v = kl ? 4 : 0;
			r = wifi_ioctl(m, 134, &v, 4, 4);	/* WLC_SET_WSEC: AES_ENABLED (or none) */
			pr("wifi: join: wsec 0x%x: %d\n", v, r);
			v = kl ? 0x80 : 0;
			r = wifi_ioctl(m, 165, &v, 4, 4);	/* WLC_SET_WPA_AUTH: WPA2_AUTH_PSK (or none) */
			pr("wifi: join: wpa_auth 0x%x: %d\n", v, r);
			/* wifi19, an observation: the dongle's supplicant off, so M1 (if it comes) reaches the host */
			v = kl ? 1 : 0;
			r = wifi_iovar(m, 1, "sup_wpa", &v, 4, 0, 0);
			pr("wifi: join: sup_wpa %u: %d\n", v, r);
			{
				/*
				 * wifi14: the network is a Wi-Fi 7 multi-link one, and Android joins it with SAE over three links;
				 * Google's driver keeps a PSK join off MLO (wl_mlo_sta_config). "mlo" XTLVs: LE16 id, LE16 length,
				 * value. WL_MLO_CMD_CAP (5) for max_mlo_links (byte 4), then WL_MLO_CMD_CONFIG (1), all zero but
				 * mode 0xff at byte 9, sized for those links (docs/wifi-join-analysis.md, trial 1)
				 */
				static u8 xb[96];
				u32 nl, xl;

				for (i = 0; i < sizeof(xb); i++)
					xb[i] = 0;
				xb[0] = 5;
				xb[2] = 8;
				xb[4] = 1;
				xb[6] = 8;
				r = wifi_iovar(m, 0, "mlo", xb, 12, xb, 32);
				pr("wifi: join: mlo cap (%d):", r);
				for (i = 0; i < 16; i++)
					pr(" %02x", xb[i]);
				pr("\n");
				nl = xb[4] > 4 ? 4 : xb[4];
				xl = 16 + 8 * nl;
				for (i = 0; i < sizeof(xb); i++)
					xb[i] = 0;
				xb[0] = 1;
				xb[2] = (u8)xl;
				xb[4 + 9] = 0xff;
				r = wifi_iovar(m, 1, "mlo", xb, 4 + xl, 0, 0);
				pr("wifi: join: mlo config off (%u links, %u bytes): %d\n", nl, xl, r);
			}
			/*
			 * wsec_pmk_t: a 32-byte file is the PMK itself (PBKDF2 of passphrase and SSID, made on the Mac), sent as
			 * Google's PSK-offload path does: flags 0, sizeof(wsec_pmk_t) + 1 = 263 bytes; anything else is a
			 * passphrase (flags WSEC_PASSPHRASE 1)
			 */
			for (i = 0; i < sizeof(pmk); i++)
				pmk[i] = i >= 4 && i < 4 + kl ? wifi_psk_start[i - 4] : 0;
			pmk[0] = (u8)kl;
			pmk[2] = kl == 32 ? 0 : 1;
			r = kl ? wifi_ioctl(m, 268, pmk, 263, 263) : 0;
			pr("wifi: join: PMK (%s, %u bytes): %d\n", kl == 32 ? "raw" : "passphrase", kl, r);
			for (i = 0; i < sizeof(pmk); i++)	/* no copy of the key left behind */
				pmk[i] = 0;
			for (i = 0; i < 0x1000; i++)
				wifi_mem[M_IOCTBUF + i] = 0;
			for (i = 0; i < sizeof(ssid); i++)
				ssid[i] = i >= 4 && i < 4 + sl ? wifi_ssid_start[i - 4] : 0;
			ssid[0] = (u8)sl;
			m->link = m->keyed = m->failed = 0;
			m->keyed = !kl;		/* open: the link alone */
			t0 = timer_us();
			/*
			 * wifi15: pinned to the network's best 2.4 GHz BSS, as wl_join_params_t (wlc_ssid_t, then
			 * wl_assoc_params_t: bssid, bssid_cnt, chanspec_num, chanspec_list[1]): the 6 GHz one takes SAE only
			 */
			for (i = 36; i < 52; i++)
				ssid[i] = 0;
			for (i = 0; i < 6; i++)
				ssid[36 + i] = m->pin[i];
			ssid[42] = 1;
			ssid[44] = 1;
			ssid[48] = (u8)m->pin_cs;
			ssid[49] = (u8)(m->pin_cs >> 8);
			pr("wifi: join: best 2.4 GHz BSS %02x:%02x:%02x:xx:xx:xx, chanspec 0x%04x, RSSI %d dBm\n", m->pin[0], m->pin[1],
			   m->pin[2], m->pin_cs, m->pin_rssi);
			/*
			 * wifi15 trapped the firmware with an unversioned wl_join_params_t (bssid_cnt and chanspec_num both
			 * set); wifi16 keeps the plain SSID join and holds the radio to 2.4 GHz instead (WLC_SET_BAND 142,
			 * WLC_BAND_2G 2)
			 */
			/* wifi18: the AP's 5 and 2.4 GHz BSSes are plain WPA2-PSK/CCMP (wifi17), as we join; power save off */
			v = 0xff;
			r = wifi_ioctl(m, 85, &v, 4, 4);	/* WLC_GET_PM */
			pr("wifi: join: PM was %u (%d)", v, r);
			v = 0;
			r = wifi_ioctl(m, 86, &v, 4, 4);	/* WLC_SET_PM: PM_OFF */
			pr(", set to 0: %d\n", r);
			r = wifi_ioctl(m, 26, ssid, 36, 36);	/* WLC_SET_SSID */
			pr("wifi: join: WLC_SET_SSID (%u-character SSID): status %d\n", sl, r);
			d2h_poll(m, 15000000, join_over);
			if (m->link && m->keyed) {
				int rssi = 0;

				wifi_ioctl(m, 23, bssid, 6, 6);	/* WLC_GET_BSSID */
				wifi_ioctl(m, 127, &rssi, 4, 4);	/* WLC_GET_RSSI */
				pr("wifi: JOINED in %llu ms: BSSID %02x:%02x:%02x:%02x:%02x:%02x, RSSI %d dBm, WPA2 keys in place\n",
				   (timer_us() - t0) / 1000, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], rssi);
				d2h_poll(m, kl ? 3000000 : 15000000, never);	/* 3 s on the network (open: 15 s of whatever comes) */
				v = 0;
				r = wifi_ioctl(m, 52, &v, 4, 4);	/* WLC_DISASSOC: leave cleanly */
				pr("wifi: join: left the network, status %d\n", r);
				d2h_poll(m, 300000, never);
			} else
				pr("wifi: join: not joined after %llu ms (link %u, keyed %u, failed %u)\n",
				   (timer_us() - t0) / 1000, m->link, m->keyed, m->failed);
			pr("wifi: join: %u frames received; RX post: host wr %u, firmware rd TCM %u host %u\n", m->rx_count, m->rxwr,
			   dram_r(cfg, bar2, m->h2dr + 4), *(volatile u32 *)(wifi_mem + M_IDX + 0x100 + 4));
			{
				u32 b, k, n = 0, first = N_RX;

				for (b = 0; b < N_RX; b++) {
					for (k = 0; k < 2048 && wifi_mem[M_RX + 2048 * b + k] == 0xa5; k++)
						;
					if (k < 2048) {
						n++;
						first = first == N_RX ? b : first;
					}
				}
				pr("wifi: join: %u of %u RX buffers written by the chip\n", n, N_RX);
				for (k = 0; first < N_RX && k < 96; k += 16)
					pr("wifi:   buffer %u +%02u: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
					   first, k, wifi_mem[M_RX + 2048 * first + k], wifi_mem[M_RX + 2048 * first + k + 1],
					   wifi_mem[M_RX + 2048 * first + k + 2], wifi_mem[M_RX + 2048 * first + k + 3],
					   wifi_mem[M_RX + 2048 * first + k + 4], wifi_mem[M_RX + 2048 * first + k + 5],
					   wifi_mem[M_RX + 2048 * first + k + 6], wifi_mem[M_RX + 2048 * first + k + 7],
					   wifi_mem[M_RX + 2048 * first + k + 8], wifi_mem[M_RX + 2048 * first + k + 9],
					   wifi_mem[M_RX + 2048 * first + k + 10], wifi_mem[M_RX + 2048 * first + k + 11],
					   wifi_mem[M_RX + 2048 * first + k + 12], wifi_mem[M_RX + 2048 * first + k + 13],
					   wifi_mem[M_RX + 2048 * first + k + 14], wifi_mem[M_RX + 2048 * first + k + 15]);
			}
			pr("wifi: join: RX complete ring: firmware wr TCM %u host %u, host rd %u\n", dram_r(cfg, bar2, m->d2hw + 8),
			   *(volatile u32 *)(wifi_mem + M_IDX + 0x200 + 8), m->rxrd);
			{
				/* wifi13: what the firmware itself holds and sent: security read back, the association's IEs */
				static u8 ib[1024];
				u32 n, k;

				v = 0;
				r = wifi_ioctl(m, 133, &v, 4, 4);	/* WLC_GET_WSEC */
				pr("wifi: join: wsec now 0x%x (%d)", v, r);
				v = 0;
				r = wifi_ioctl(m, 164, &v, 4, 4);	/* WLC_GET_WPA_AUTH */
				pr(", wpa_auth 0x%x (%d)", v, r);
				for (i = 0; i < 8; i++)
					ib[i] = 0;
				r = wifi_iovar(m, 0, "sup_wpa", 0, 0, ib, 4);
				pr(", sup_wpa %u (%d)\n", ib[0] | ib[1] << 8, r);
				for (i = 0; i < sizeof(ib); i++)
					ib[i] = 0;
				r = wifi_iovar(m, 0, "assoc_info", 0, 0, ib, 64);
				pr("wifi: join: assoc_info (%d): req %u bytes, resp %u bytes, flags 0x%x\n", r,
				   ib[0] | ib[1] << 8 | ib[2] << 16, ib[4] | ib[5] << 8 | ib[6] << 16, ib[8] | ib[9] << 8);
				n = ib[0] | ib[1] << 8;
				for (i = 0; i < sizeof(ib); i++)
					ib[i] = 0;
				r = wifi_iovar(m, 0, "assoc_req_ies", 0, 0, ib, sizeof(ib));
				if (n > sizeof(ib))
					n = sizeof(ib);
				pr("wifi: join: assoc_req_ies (%d):", r);
				/* each IE's id and length; the SSID (0) left out, the RSN (48) printed whole */
				for (i = 0; i + 2 <= n && ib[i + 1] + i + 2 <= n; i += 2 + ib[i + 1]) {
					if (ib[i] == 255 && ib[i + 1])
						pr(" 255.%u/%u", ib[i + 2], ib[i + 1]);
					else
						pr(" %u/%u", ib[i], ib[i + 1]);
					if (i > 900)
						break;
				}
				pr("\n");
				for (i = 0; i + 2 <= n && ib[i + 1] + i + 2 <= n; i += 2 + ib[i + 1])
					if (ib[i] == 48) {
						pr("wifi: join: our RSN IE:");
						for (k = 0; k < ib[i + 1] + 2u; k++)
							pr(" %02x", ib[i + k]);
						pr("\n");
					}
				for (i = 0; i < sizeof(ib); i++)
					ib[i] = 0;
			}
join_out:
			m->cidx = cons_print(cfg, bar2, cons, m->cidx);
		}
#endif
	}
#endif
	pr("wifi: msgbuf: H2D ctrl w %u (TCM r %u), D2H ctrl r %u; S2MPU fault 0x%x; shared flags 0x%08x trap 0x%x\n",
	   m->wr, dram_r(cfg, bar2, m->h2dr), m->rd, rd32(0x131f0000ULL + 0x2000), dram_r(cfg, bar2, sh),
	   dram_r(cfg, bar2, sh + 4));
	cons_print(cfg, bar2, cons, m->cidx);
	wr32(cfg + 4, rd32(cfg + 4) & ~4u);	/* bus master off */
}
#endif

#if PROBE_WIFI >= 9
/*
 * PROBE_WIFI=9, read only: one second after the firmware came up, the whole shared area (pciedev_shared_t, bcmpcie.h:
 * 0x00 flags, 0x14 console_addr, 0x30 rings_info_ptr), the ring_info it points to, and the firmware's own console
 * (hnd_cons_t: +0x08 log.buf, +0x0c log.buf_size, +0x10 log.idx), printed line by line as dhdpcie_bus_readconsole does.
 */
static void wifi_look(u64 cfg, u64 bar2, u32 sh)
{
	u32 i, w[36], cons, idx;

	u32 f0 = 1, l0 = 1, c0 = 1;
	u64 t0 = timer_us();

	/* the first second: when the shared area's flags, the last word or the CA7 change */
	while (timer_us() - t0 < 1000000) {
		u32 f = dram_r(cfg, bar2, sh), l = dram_r(cfg, bar2, 0x5bfffc), c;

		wr32(cfg + 0x80, 0x18080000);
		c = rd32(0x60000000ULL);
		wr32(cfg + 0x80, 0x18000000);
		if (f != f0 || l != l0 || c != c0)
			pr("wifi: +%llu ms: flags 0x%08x, last word 0x%08x, CA7 +0 0x%08x, status 0x%04x\n",
			   (timer_us() - t0) / 1000, f, l, c, rd32(cfg + 4) >> 16);
		f0 = f;
		l0 = l;
		c0 = c;
		delay_us(2000);
	}
	pr("wifi: 1 s later: link state 0x%x, config +0x00 0x%08x +0x04 0x%08x, BAR0 window 0x%08x, BAR2 window 0x%08x\n",
	   rd32(0x13120000ULL + 0x2c8) & 0x3f, rd32(cfg), rd32(cfg + 4), rd32(cfg + 0x80), rd32(cfg + 0x84));
	win = 0xffffffff;
	/* wifi9: the first pointer (0x48a14c) is let go at ~850 ms and the last word moves to the real shared area */
	i = dram_r(cfg, bar2, 0x5bfffc);
	if (i != sh && i >= 0x2a0000 && i < 0x5bfffc) {
		pr("wifi: the shared area moved to 0x%x\n", i);
		sh = i;
	}
	for (i = 0; i < 36; i++)
		w[i] = dram_r(cfg, bar2, sh + 4 * i);
	for (i = 0; i < 36; i += 6)
		pr("wifi: shared +0x%02x: %08x %08x %08x %08x %08x %08x\n", 4 * i, w[i], w[i + 1], w[i + 2], w[i + 3],
		   w[i + 4], w[i + 5]);
	if (w[12] >= 0x2a0000 && w[12] < 0x5c0000)
		for (i = 0; i < 24; i += 6)
			pr("wifi: rings_info +0x%02x: %08x %08x %08x %08x %08x %08x\n", 4 * i,
			   dram_r(cfg, bar2, w[12] + 4 * i), dram_r(cfg, bar2, w[12] + 4 * i + 4),
			   dram_r(cfg, bar2, w[12] + 4 * i + 8), dram_r(cfg, bar2, w[12] + 4 * i + 12),
			   dram_r(cfg, bar2, w[12] + 4 * i + 16), dram_r(cfg, bar2, w[12] + 4 * i + 20));
	cons = w[5];
	if (cons < 0x2a0000 || cons >= 0x5c0000) {
		pr("wifi: no console address (0x%x)\n", cons);
		return;
	}
	pr("wifi: console at 0x%x: buffer 0x%x size 0x%x index 0x%x\n", cons, dram_r(cfg, bar2, cons + 8),
	   dram_r(cfg, bar2, cons + 12), dram_r(cfg, bar2, cons + 16));
	idx = cons_print(cfg, bar2, cons, 0);
#if PROBE_WIFI >= 10
	if (w[12] >= 0x2a0000 && w[12] < 0x5c0000)
		wifi_msgbuf(cfg, bar2, sh, w[12], cons, idx);
#endif
}
#endif

/*
 * PROBE_WIFI=8: the firmware in and the CA7 let go, as Google's bcmdhd does for this chip (CA7 already up and
 * halted, wifi7: DMP 0x21): RAM 0x2a0000 + 0x320000; last word 0 (dhdpcie_init_shared_addr); firmware at the base;
 * vars just below the last word and the length token in it (dhdpcie_bus_write_vars); 128 random bytes as the RNG
 * rTLV below the vars (dhdpcie_wrt_rnd, type 0xfeedc0de, no length checksum); the firmware's first word at address 0
 * (dhdpcie_bus_write_flops); CA7 DMP control bit 5 (CPUHALT) cleared; then the last word is polled for the
 * shared-area pointer. Bus master stays off: the firmware cannot write to the phone's RAM.
 */
static void wifi_boot(u64 cfg, u64 bar0, u64 bar2)
{
	const u32 *hdr = (const u32 *)wifi_blob_start;
	const u32 base = 0x2a0000, size = 0x320000, last = base + size - 4;
	u32 fwlen, varlen, v, i, token, addr0, dmp, p = 0;
	const u8 *fw, *vars;
	u64 t0;

	if (hdr[0] != 0x31424657) {	/* 'WFB1' */
		pr("wifi: no firmware blob in the image\n");
		return;
	}
	fwlen = hdr[1];
	varlen = hdr[2];
	fw = wifi_blob_start + 16;
	vars = fw + ((fwlen + 3) & ~3u);
	pr("wifi: firmware %u bytes, vars %u bytes\n", fwlen, varlen);

	dram_w(cfg, bar2, last, 0);
	t0 = timer_us();
	for (i = 0; i < fwlen; i += 4) {
		u32 w = fw[i] | (i + 1 < fwlen ? fw[i + 1] << 8 : 0) | (i + 2 < fwlen ? fw[i + 2] << 16 : 0) |
			(i + 3 < fwlen ? (u32)fw[i + 3] << 24 : 0);
		dram_w(cfg, bar2, base + i, w);
	}
	pr("wifi: firmware written in %llu ms; first words %08x %08x, read back %08x %08x\n", (timer_us() - t0) / 1000,
	   *(const u32 *)fw, *(const u32 *)(fw + 4), dram_r(cfg, bar2, base), dram_r(cfg, bar2, base + 4));
	v = last - varlen;
	for (i = 0; i < varlen; i += 4)
		dram_w(cfg, bar2, v + i, *(const u32 *)(vars + i));
	token = ((~(varlen / 4) & 0xffff) << 16) | ((varlen / 4) & 0xffff);
	dram_w(cfg, bar2, last, token);
	/* RNG rTLV: 128 bytes, then length 0x80, then type, ending at the vars */
	for (i = 0; i < 128; i += 4) {
		u64 r = timer_ticks();

		r ^= r >> 13;
		r *= 0x9e3779b97f4a7c15ULL;
		dram_w(cfg, bar2, v - 8 - 128 + i, (u32)(r >> 17) ^ (i * 0x01010101u));
	}
	dram_w(cfg, bar2, v - 8, 128);
	dram_w(cfg, bar2, v - 4, 0xfeedc0de);
	pr("wifi: vars at 0x%x, token 0x%08x at 0x%x, RNG rTLV at 0x%x\n", v, token, last, v - 8 - 128);

	addr0 = dram_r(cfg, bar2, 0);
	pr("wifi: address 0 reads 0x%08x%s\n", addr0, addr0 == 0xffffffff ? " (secured: the firmware cannot be started so)" : "");
	if (addr0 == 0xffffffff)
		return;
	dram_w(cfg, bar2, 0, *(const u32 *)fw);

	wr32(cfg + 0x80, 0x18006000);
	(void)rd32(cfg + 0x80);
	dmp = rd32(bar0 + 0x248);
	wr32(bar0 + 0x248, dmp & ~0x20u);
	pr("wifi: CA7 DMP control 0x%x -> 0x%x: the firmware runs\n", dmp, rd32(bar0 + 0x248));
	wr32(cfg + 0x80, 0x18000000);
	(void)rd32(cfg + 0x80);

	t0 = timer_us();
	while (timer_us() - t0 < 5000000) {
		p = dram_r(cfg, bar2, last);
		if (p != token && p != 0 && p != 0xffffffff)
			break;
		delay_us(1000);
	}
	pr("wifi: after %llu ms the last word is 0x%08x%s\n", (timer_us() - t0) / 1000, p,
	   p == token ? " (still the token: the firmware did not come up)" : "");
	if (p >= base && p < last) {
		u32 w[8];

		for (i = 0; i < 8; i++)
			w[i] = dram_r(cfg, bar2, p + 4 * i);
		pr("wifi: shared area at 0x%x: flags 0x%08x (version %u), trap 0x%x, assert 0x%x 0x%x line %u, console 0x%x, "
		   "msgtrace 0x%x\n", p, w[0], w[0] & 0xff, w[1], w[2], w[3], w[4], w[5], w[6]);
#if PROBE_WIFI >= 9
		wifi_look(cfg, bar2, p);
#endif
	}
	/* the ARM halted again before the chip goes off */
	wr32(cfg + 0x80, 0x18006000);
	(void)rd32(cfg + 0x80);
	wr32(bar0 + 0x248, rd32(bar0 + 0x248) | 0x20u);
	wr32(cfg + 0x80, 0x18000000);
	(void)rd32(cfg + 0x80);
	wr32(cfg + 0x84, 0);
	win = 0;
}
#endif

static u32 elbi(u32 off) { return rd32(0x13120000ULL + off); }
static void elbi_w(u32 off, u32 v) { wr32(0x13120000ULL + off, v); }

/*
 * PROBE_WIFI=3: the link, as Linux's exynos_pcie_rc_poweron + establish_link do for channel 1 (pcie-exynos-rc.c),
 * up to LTSSM in L0. Not yet: dw_pcie_setup_rc, iATU, the endpoint's config space.
 */
static void wifi_link(void)
{
	u32 st, last = 0xff, seen[16], ns = 0, i;

	/* the chip on (DHD's wlan power-on: WL_REG_ON high), PERST# held low, CLKREQ# to its function */
	pin(0, 1, 0);		/* PERST#: output, low */
	pin(1, 2, -1);		/* CLKREQ# */
	pin(4, 1, 1);		/* WL_REG_ON: output, high */
	pr("wifi: WL_REG_ON high, PERST# low: gph3 CON 0x%08x DAT 0x%08x\n", rd32(PIN), rd32(PIN + 4));
	delay_us(200000);

	wr32(0x13130000ULL + 0x4000, 0x15);			/* exynos_soc_write(0x15, 0x4000) */
	cal_pwrdn_clear();
	cal_phy_config();
	psci_call(0x82000504u, 0x15460000ULL + (0x3ec8 | 0xc000), 1, 10);	/* rmw_priv_reg(.., 0x400, 0x400) */
	wr32(0x13150000ULL + 0xc800, 0x421);
	wr32(0x13160000ULL + 0x32c, 0);
	wr32(DBI + 0x890, 0x12000);				/* EQ off */
	pin(0, 1, 1);						/* PERST# high */
	delay_us(15000);					/* DT perst-delay-us 0x3a98 */
	elbi_w(0x3bc, elbi(0x3bc) | 0x1 | 0x10);		/* APP_REQ_EXIT_L1_MODE, L1_REQ_NAK_CONTROL_MASTER */
	elbi_w(0x3a0, 0x2);					/* PCIE_LINKDOWN_RST_MANUAL */
	elbi_w(0x74, 0x1);					/* APP_XFER_PENDING */
	elbi_w(0x3a8, elbi(0x3a8) & ~0xfffu);			/* Q-channel: no gating (ip_ver 0x987500) */
	elbi_w(0x474, 0x1);					/* NACK on AXI pending */
	elbi_w(0x1078, 0x1);					/* DBI L1 exit disable */
	pr("wifi: before LTSSM: state 0x%x, DBI link status 0x%08x\n", elbi(0x2c8) & 0x3f, rd32(DBI + 0x80));
	elbi_w(0x54, 0x1);					/* LTSSM enable */
	for (i = 0; i < 4800; i++) {
		st = elbi(0x2c8) & 0x3f;
		if (st != last && ns < 16)
			seen[ns++] = st;
		last = st;
		if (st == 0x11)
			break;
		delay_us(10);
	}
	pr("wifi: LTSSM %s after %u us; states seen:", st == 0x11 ? "L0 (link up)" : "NOT in L0", i * 10);
	for (i = 0; i < ns; i++)
		pr(" 0x%x", seen[i]);
	pr("\n");
	pr("wifi: DBI link control/status 0x%08x (speed %u, width x%u)\n", rd32(DBI + 0x80),
	   (rd32(DBI + 0x80) >> 16) & 0xf, (rd32(DBI + 0x80) >> 20) & 0x3f);
#if PROBE_WIFI >= 4
	/*
	 * PROBE_WIFI=4: the endpoint's config space through outbound iATU region 0 as CFG0 to bus 1 device 0
	 * (exynos_pcie_rc_prog_viewport_cfg0; unrolled iATU at DBI+0x300000, pcie-exynos-rc.h), bridge buses 0/1/1.
	 */
	if (st == 0x11) {
		int ok;
		u32 k, w[16];

		wr32(DBI + 0x18, 0x00010100);			/* primary 0, secondary 1, subordinate 1 */
		wr32(DBI + 0x4, rd32(DBI + 0x4) | 0x6);		/* memory + bus master on the bridge */
		wr32(DBI + 0x300008, 0x60ffe000);
		wr32(DBI + 0x30000c, 0);
		wr32(DBI + 0x300010, 0x60ffe000 + 0x2000 - 1);
		wr32(DBI + 0x300014, 1u << 24);			/* bus 1, device 0, function 0 */
		wr32(DBI + 0x300018, 0);
		wr32(DBI + 0x300000, 0x4);			/* CFG0 */
		wr32(DBI + 0x300004, 0x80000000u);		/* enable */
		(void)rd32(DBI + 0x300004);
		for (k = 0; k < 16; k++) {
			ok = 1;
			w[k] = safe_rd32(0x60ffe000ULL + 4 * k, &ok);
			if (!ok)
				w[k] = 0xdeadbeef;
		}
		pr("wifi: endpoint 01:00.0: vendor 0x%04x device 0x%04x, class 0x%06x rev %u, header %u\n", w[0] & 0xffff,
		   w[0] >> 16, w[2] >> 8, w[2] & 0xff, (w[3] >> 16) & 0x7f);
		for (k = 0; k < 16; k += 4)
			pr("wifi: endpoint +0x%02x: %08x %08x %08x %08x\n", 4 * k, w[k], w[k + 1], w[k + 2], w[k + 3]);
#if PROBE_WIFI >= 5
		/*
		 * PROBE_WIFI=5: the BARs sized and placed in the RC's memory window (DT ranges: 0x60000000, 0xff0000),
		 * outbound iATU region 1 as MEM there, memory decoding on (no bus master: the chip cannot write to RAM),
		 * then the chip's own registers through BAR0 (Broadcom: the window at config 0x80 picks the backplane
		 * address; ChipCommon at 0x18000000 holds the chip ID at +0).
		 */
		{
			const u64 cfg = 0x60ffe000ULL;
			u64 at = 0x60000000ULL, bar[3] = { 0 }, size[3] = { 0 };
			u32 b;

			for (b = 0; b < 3; b++) {
				u32 off = 0x10 + 8 * b, lo, hi;

				wr32(cfg + off, 0xffffffffu);
				wr32(cfg + off + 4, 0xffffffffu);
				lo = rd32(cfg + off) & ~0xfu;
				hi = rd32(cfg + off + 4);
				size[b] = ~(((u64)hi << 32) | lo) + 1;
				if (size[b] == 0 || size[b] > 0x800000) {
					pr("wifi: BAR%u: size 0x%llx: not placed\n", 2 * b, size[b]);
					wr32(cfg + off, 0);
					wr32(cfg + off + 4, 0);
					continue;
				}
				at = (at + size[b] - 1) & ~(size[b] - 1);
				bar[b] = at;
				wr32(cfg + off, (u32)at);
				wr32(cfg + off + 4, 0);
				at += size[b];
				pr("wifi: BAR%u: 0x%llx bytes at 0x%llx\n", 2 * b, size[b], bar[b]);
			}
			wr32(DBI + 0x20, (0x60fu << 20) | (0x600u << 4));	/* bridge memory window 0x60000000-0x60ffffff */
			wr32(DBI + 0x300200, 0x0);				/* region 1: MEM */
			wr32(DBI + 0x300208, 0x60000000);
			wr32(DBI + 0x30020c, 0);
			wr32(DBI + 0x300210, 0x60feffff);
			wr32(DBI + 0x300214, 0x60000000);
			wr32(DBI + 0x300218, 0);
			wr32(DBI + 0x300204, 0x80000000u);
			(void)rd32(DBI + 0x300204);
			wr32(cfg + 0x4, (rd32(cfg + 0x4) & ~0x7u) | 0x2);	/* memory space only */
			pr("wifi: endpoint command 0x%08x, BAR0 window (config 0x80) 0x%08x\n", rd32(cfg + 0x4), rd32(cfg + 0x80));
			if (bar[0]) {
				u32 win = rd32(cfg + 0x80);

				for (k = 0; k < 8; k++) {
					ok = 1;
					w[k] = safe_rd32(bar[0] + 4 * k, &ok);
				}
				pr("wifi: BAR0 at the chip's window 0x%08x: %08x %08x %08x %08x %08x %08x %08x %08x\n", win, w[0], w[1],
				   w[2], w[3], w[4], w[5], w[6], w[7]);
				wr32(cfg + 0x80, 0x18000000);		/* the window to ChipCommon */
				(void)rd32(cfg + 0x80);
				for (k = 0; k < 8; k++) {
					ok = 1;
					w[k] = safe_rd32(bar[0] + 4 * k, &ok);
					if (!ok)
						w[k] = 0xdeadbeef;
				}
				pr("wifi: BAR0 +0x00: %08x %08x %08x %08x %08x %08x %08x %08x\n", w[0], w[1], w[2], w[3], w[4], w[5],
				   w[6], w[7]);
				pr("wifi: ChipCommon chip ID 0x%04x, rev %u, package %u, %u cores\n", w[0] & 0xffff, (w[0] >> 16) & 0xf,
				   (w[0] >> 20) & 0xf, (w[0] >> 24) & 0xf);
#if PROBE_WIFI >= 6
				/*
				 * PROBE_WIFI=6: the chip's core table (EROM: ChipCommon +0xfc, EromPtrOffset) read through the BAR0
				 * window; then a pattern written into dongle RAM near its top and read back through BAR2 (DHD's
				 * "BAR1": window at config 0x84, 4 MiB; RAM 0x2a0000..0x5c0000), the last word left alone.
				 */
				{
					u32 erom = rd32(bar[0] + 0xfc), e, pat[4] = { 0x12345678, 0x9abcdef0, 0x0f1e2d3c, 0xa5a55a5a };

					pr("wifi: EROM pointer (ChipCommon +0xfc) 0x%08x; BAR1 window (config 0x84) 0x%08x\n", erom,
					   rd32(cfg + 0x84));
					if (erom >= 0x18000000 && erom < 0x19000000) {
						/* the OOBR page first (EROM2 base & ~0x1fff: core info of the OOBR cores), then EROM1 */
						wr32(cfg + 0x80, erom & ~0x1fffu);
						(void)rd32(cfg + 0x80);
						for (e = 0; e < 0x1000; e += 32) {
							for (k = 0; k < 8; k++) {
								ok = 1;
								w[k] = safe_rd32(bar[0] + e + 4 * k, &ok);
							}
							pr("wifi: OOBR +0x%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", e, w[0], w[1], w[2],
							   w[3], w[4], w[5], w[6], w[7]);
						}
						wr32(cfg + 0x80, erom & ~0xfffu);
						(void)rd32(cfg + 0x80);
						for (e = 0; e < 0x1000; e += 32) {	/* the whole 4 KiB page from 0x18007000 */
							for (k = 0; k < 8; k++) {
								ok = 1;
								w[k] = safe_rd32(bar[0] + e + 4 * k, &ok);
							}
							pr("wifi: EROM +0x%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", e, w[0], w[1], w[2],
							   w[3], w[4], w[5], w[6], w[7]);
						}
						wr32(cfg + 0x80, 0x18000000);
					}
#if PROBE_WIFI >= 8
					wifi_boot(cfg, bar[0], bar[1]);
#endif
#if PROBE_WIFI >= 13
					if (wifi_handed)
						return;	/* the chip stays up, its link too: the kernel's now */
#endif
#if PROBE_WIFI >= 7
					/*
					 * PROBE_WIFI=7, read only: CA7's and SYSMEM's DMP control (OOBR +0x240/+0x4c0, +8:
					 * nciutils.c DMP_DMPCTRL_REG_OFFSET; bit 5 CPUHALT, bit 0 clock enable, bit 1 force gated
					 * clock) and the next words, CA7's own registers, and their nodes' IDM reset words at +0x140,
					 * +0x144 (offsets inferred, docs/bcm4390-fw-download.md).
					 */
					{
						/* (addresses only: a table of string pointers would need relocations in this image) */
						static const u32 look[] = { 0x18006248, 0x180064c8, 0x18080000, 0x18088000, 0x18504140,
									    0x18516140, 0x18505140 };
						u32 l;

						for (l = 0; l < sizeof(look) / sizeof(look[0]); l++) {
							wr32(cfg + 0x80, look[l] & ~0xfffu);
							(void)rd32(cfg + 0x80);
							for (k = 0; k < 4; k++) {
								ok = 1;
								w[k] = safe_rd32(bar[0] + (look[l] & 0xfff) + 4 * k, &ok);
								if (!ok)
									w[k] = 0xdeadbeef;
							}
							/* (no table of names: the compiler makes one of pointers, which this image cannot have) */
							pr("wifi: core look 0x%08x: %08x %08x %08x %08x\n", look[l], w[0], w[1], w[2], w[3]);
						}
						wr32(cfg + 0x80, 0x18000000);
					}
#endif
					if (bar[1]) {
						u64 a = 0x5b0000, base;

						wr32(cfg + 0x84, 0x400000);
						(void)rd32(cfg + 0x84);
						base = bar[1] + (a - 0x400000);
						for (k = 0; k < 4; k++)
							w[k] = rd32(base + 4 * k);
						pr("wifi: dongle RAM 0x%llx before: %08x %08x %08x %08x; last word 0x5bfffc: %08x\n", a, w[0], w[1],
						   w[2], w[3], rd32(bar[1] + 0x1bfffc));
						for (k = 0; k < 4; k++)
							wr32(base + 4 * k, pat[k]);
						for (k = 0; k < 4; k++)
							w[k] = rd32(base + 4 * k);
						pr("wifi: dongle RAM 0x%llx after writing: %08x %08x %08x %08x (%s)\n", a, w[0], w[1], w[2], w[3],
						   w[0] == pat[0] && w[3] == pat[3] ? "the RAM takes writes" : "NOT as written");
						wr32(cfg + 0x84, 0);
					}
				}
#endif
			}
		}
#endif
	}
#endif
	/* leave the chip off again and the link down */
	elbi_w(0x54, 0);
	pin(0, 1, 0);
	pin(4, 1, 0);
}
#endif

static void dump(const char *name, u64 base, u32 n)
{
	int ok = 1;
	u32 i;

	safe_rd32_async(base, &ok);
	if (!ok) {
		pr("wifi: %s 0x%llx: does not answer (clock or power off)\n", name, base);
		return;
	}
	for (i = 0; i < n; i += 16) {
		u32 v[4], k;

		for (k = 0; k < 4; k++) {
			ok = 1;
			v[k] = safe_rd32(base + i + 4 * k, &ok);
			if (!ok)
				v[k] = 0xdeadbeef;
		}
		pr("wifi: %s +0x%03x: %08x %08x %08x %08x\n", name, i, v[0], v[1], v[2], v[3]);
	}
}

void wifi_probe(void)
{
	int ok = 1;
	u32 v;

	pr("wifi: PCIe RC1 (BCM4390) as the bootloader left it, read only\n");
	dump("pinctrl-hsi2", 0x13040000ULL, 0x200);
	v = safe_rd32(0x15460000ULL + 0x3ec8, &ok);
	pr("wifi: PMU +0x3ec8 (PCIe 1 PHY isolation): 0x%08x%s\n", v, ok ? "" : " (fault)");
	dump("sysreg-hsi2", 0x13020000ULL, 0x40);
	dump("elbi", 0x13120000ULL, 0x40);
	dump("soc", 0x13130000ULL, 0x20);
	dump("phy", 0x13160000ULL, 0x20);
	dump("dbi", 0x13400000ULL, 0x40);
	/* the HSI2 S2MPU (s2mpu_s0_hsi2, google,s2mpu-v9, always-on): what stands between the chip's DMA and RAM */
	dump("s2mpu-hsi2", 0x131f0000ULL, 0x200);
	dump("s2mpu-hsi2+1000", 0x131f0000ULL + 0x1000, 0x40);
	dump("s2mpu-hsi2+2000", 0x131f0000ULL + 0x2000, 0x40);
	dump("s2mpu-hsi2+4000", 0x131f0000ULL + 0x4000, 0x80);
	dump("s2mpu-hsi2+8000", 0x131f0000ULL + 0x8000, 0x40);
#if PROBE_WIFI >= 2
	/*
	 * PROBE_WIFI=2: the PHY out of isolation, as Linux's exynos_pcie_phy_isolation(PCIE_PHY_BYPASS) at power-on:
	 * bit 0 of PMU+0x3ec8 (pcie-exynos-rc.h PCIE_PHY_CONTROL_MASK). EL3 owns PMU writes: the set-bit alias
	 * (offset | 0xc000) with the bit number, as ufs.c does for the UFS PHY.
	 */
	if (!(v & 1)) {
		s64 r = psci_call(0x82000504u, 0x15460000ULL + (0x3ec8 | 0xc000), 1, 0);

		ok = 1;
		pr("wifi: PCIe 1 PHY isolation bypass via SMC: %lld, PMU +0x3ec8 now 0x%08x\n", r,
		   safe_rd32(0x15460000ULL + 0x3ec8, &ok));
		delay_us(100);
	}
	dump("phy", 0x13160000ULL, 0x20);
	dump("udbg+c700", 0x13150000ULL + 0xc700, 0x10);
	dump("pcs", 0x13140000ULL, 0x20);
#endif
#if PROBE_WIFI >= 3
	wifi_link();
#endif
}
