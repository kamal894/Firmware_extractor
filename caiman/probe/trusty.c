/* SPDX-License-Identifier: MIT */
/*
 * A Trusty client for the AOC's start command (docs/aoc-boot-map.md): gsa.ko sends it to the
 * Trusty port "com.android.trusty.gsa.hwmgr.aoc". Linux talks to Trusty through virtio; this
 * uses QL-TIPC, the much smaller interface bootloaders use (trusty/lk/trusty
 * lib/trusty/tipc_dev_ql.c): one shared page, five commands, each a std call.
 *
 * The shared page's id depends on the API version Trusty settles on (Linux trusty.c
 * trusty_transfer_memory()). Before version 5 it is the physical address with the shareability in
 * bits 8-9 and the MAIR attribute in bits 48-55 (Trusty lib/sm/ns_mem.c). From version 5 it is an
 * FF-A 1.0 memory handle from FFA_MEM_SHARE to endpoint 0x8000 (Trusty).
 *
 * Trusty maps the QL-TIPC page cached (lib/trusty smcall.c passes no UNCACHED flag; sharing it
 * uncached made CREATE_QL_TIPC_DEV fail with -7), so it is shared as normal write-back memory.
 * Our MMU is off and our own accesses skip the caches: clean and invalidate the page by VA
 * around every call, so Trusty's cached view and DRAM agree.
 */
#include "probe.h"

typedef int s32;

#define SMC_FC_API_VERSION		0xbc00000bu	/* SMC_FASTCALL_NR(SECURE_MONITOR 60, 11) */
#define SMC_SC_RESTART_LAST		0x3c000000u	/* SMC_STDCALL_NR(SECURE_MONITOR, 0) */
#define SMC_SC_NOP			0x3c000003u	/* SMC_STDCALL_NR(SECURE_MONITOR, 3) */
#define SMC_SC_RESTART_FIQ		0x3c000002u
#define SMC_SC_CREATE_QL_TIPC_DEV	0x3200001eu	/* SMC_STDCALL_NR(TRUSTED_OS 50, 30) */
#define SMC_SC_SHUTDOWN_QL_TIPC_DEV	0x3200001fu
#define SMC_SC_HANDLE_QL_TIPC_DEV_CMD	0x32000020u

#define SM_ERR_INTERRUPTED		-3
#define SM_ERR_BUSY			-5
#define SM_ERR_FIQ_INTERRUPTED		-12
#define SM_ERR_CPU_IDLE			-13
#define SM_ERR_NOP_INTERRUPTED		-14
#define SM_ERR_NOP_DONE			-15

#define QL_TIPC_DEV_RESP		0x8000
#define QL_TIPC_DEV_CONNECT		0x1
#define QL_TIPC_DEV_GET_EVENT		0x2
#define QL_TIPC_DEV_SEND		0x3
#define QL_TIPC_DEV_RECV		0x4
#define QL_TIPC_DEV_DISCONNECT		0x5

#define IPC_HANDLE_POLL_READY		0x1
#define IPC_HANDLE_POLL_ERROR		0x2
#define IPC_HANDLE_POLL_HUP		0x4
#define IPC_HANDLE_POLL_MSG		0x8

#define FFA_ERROR			0x84000060u
#define FFA_SUCCESS			0x84000061u
#define FFA_VERSION			0x84000063u
#define FFA_FEATURES			0x84000064u
#define FFA_RXTX_MAP64			0xc4000066u
#define FFA_RXTX_UNMAP			0x84000067u
#define FFA_ID_GET			0x84000069u
#define FFA_MEM_SHARE			0x84000073u
#define FFA_MEM_RECLAIM			0x84000077u
#define FFA_ERROR_DENIED		-6
#define FFA_TRUSTY_ID			0x8000u
#define FFA_ATTR_CACHED_INNER		0x2fu	/* NORMAL_MEMORY_CACHED_WB | INNER_SHAREABLE */
#define FFA_PERM_RW			0x2u

#define QL_PAGE				0x1000u
#define NS_ATTR_CACHED_RW		((0xffull << 48) | (3ull << 8))

struct ql_hdr {
	u16 opcode;
	u16 flags;
	u32 status;
	u32 handle;
	u32 payload_len;
};

static u8 ql_page[QL_PAGE] __attribute__((aligned(QL_PAGE)));
static u8 ffa_tx[0x1000] __attribute__((aligned(0x1000)));
static u8 ffa_rx[0x1000] __attribute__((aligned(0x1000)));
static u64 ql_handle;	/* the page's id as Trusty knows it */
static u16 ffa_local_id;

struct smc_ret4 {
	u64 r0, r1, r2, r3;
};

static struct smc_ret4 smc4(u32 fn, u64 a1, u64 a2, u64 a3, u64 a4)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a1;
	register u64 x2 __asm__("x2") = a2;
	register u64 x3 __asm__("x3") = a3;
	register u64 x4 __asm__("x4") = a4;
	struct smc_ret4 r;

	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4) : : "x5", "x6", "x7", "x8",
			 "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	r.r0 = x0;
	r.r1 = x1;
	r.r2 = x2;
	r.r3 = x3;
	return r;
}

/* Linux trusty.c trusty_init_msg_buf(). Returns 0 when the RX/TX pair is mapped. */
static int ffa_init(void)
{
	struct smc_ret4 r = smc4(FFA_VERSION, 0x10000, 0, 0, 0);

	pr("trusty: FFA_VERSION(1.0) = 0x%llx\n", r.r0);
	if ((r.r0 >> 16) != 1)
		return -1;
	r = smc4(FFA_FEATURES, FFA_MEM_SHARE, 0, 0, 0);
	pr("trusty: FFA_FEATURES(MEM_SHARE) = 0x%llx\n", r.r0);
	if ((u32)r.r0 != FFA_SUCCESS)
		return -1;
	r = smc4(FFA_ID_GET, 0, 0, 0, 0);
	ffa_local_id = (u16)r.r2;
	pr("trusty: FFA_ID_GET = 0x%llx, id 0x%x\n", r.r0, ffa_local_id);
	if ((u32)r.r0 != FFA_SUCCESS)
		return -1;
	r = smc4(FFA_RXTX_MAP64, (u64)(uptr)ffa_tx, (u64)(uptr)ffa_rx, 1, 0);
	if ((u32)r.r0 == FFA_ERROR && (s32)r.r2 == FFA_ERROR_DENIED) {
		/* the bootloader's pair may still be mapped: unmap it and map ours */
		struct smc_ret4 u = smc4(FFA_RXTX_UNMAP, (u64)ffa_local_id << 16, 0, 0, 0);

		pr("trusty: FFA_RXTX_MAP denied; FFA_RXTX_UNMAP = 0x%llx\n", u.r0);
		r = smc4(FFA_RXTX_MAP64, (u64)(uptr)ffa_tx, (u64)(uptr)ffa_rx, 1, 0);
	}
	pr("trusty: FFA_RXTX_MAP(0x%llx, 0x%llx) = 0x%llx 0x%llx 0x%llx\n", (u64)(uptr)ffa_tx,
	   (u64)(uptr)ffa_rx, r.r0, r.r1, r.r2);
	return (u32)r.r0 == FFA_SUCCESS ? 0 : -1;
}

/* One FF-A 1.0 memory transaction descriptor: one page, shared read-write with Trusty. */
static int ffa_share_page(u64 pa, u64 *handle)
{
	struct smc_ret4 r;

	memset(ffa_tx, 0, 80);
	*(u16 *)(ffa_tx + 0) = ffa_local_id;		/* mtd.sender_id */
	ffa_tx[2] = FFA_ATTR_CACHED_INNER;		/* mtd.memory_region_attributes */
	*(u32 *)(ffa_tx + 28) = 1;			/* mtd.emad_count */
	*(u16 *)(ffa_tx + 32) = FFA_TRUSTY_ID;		/* emad.mapd.endpoint_id */
	ffa_tx[34] = FFA_PERM_RW;			/* emad.mapd.memory_access_permissions */
	*(u32 *)(ffa_tx + 36) = 48;			/* emad.comp_mrd_offset */
	*(u32 *)(ffa_tx + 48) = 1;			/* comp_mrd.total_page_count */
	*(u32 *)(ffa_tx + 52) = 1;			/* comp_mrd.address_range_count */
	*(u64 *)(ffa_tx + 64) = pa;			/* cons_mrd.address */
	*(u32 *)(ffa_tx + 72) = 1;			/* cons_mrd.page_count */
	r = smc4(FFA_MEM_SHARE, 80, 80, 0, 0);
	pr("trusty: FFA_MEM_SHARE(0x%llx) = 0x%llx 0x%llx 0x%llx 0x%llx\n", pa, r.r0, r.r1, r.r2, r.r3);
	if ((u32)r.r0 != FFA_SUCCESS)
		return -1;
	*handle = (r.r2 & 0xffffffffu) | (r.r3 << 32);
	return 0;
}

/* Write back and drop every cached line of the page (inner shareable: other cores' too). */
static void ql_sync(void)
{
	u64 a;

	for (a = (u64)(uptr)ql_page; a < (u64)(uptr)ql_page + QL_PAGE; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" : : : "memory");
}

static u64 ql_id(void)
{
	return ql_handle;
}

static s32 smc3(u32 fn, u32 a0, u32 a1, u32 a2)
{
	register u64 x0 __asm__("x0") = fn;
	register u64 x1 __asm__("x1") = a0;
	register u64 x2 __asm__("x2") = a1;
	register u64 x3 __asm__("x3") = a2;

	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : "x4", "x5", "x6", "x7", "x8",
			 "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (s32)x0;
}

/* Linux trusty.c trusty_std_call32(), without other CPUs: retry busy, restart when interrupted. */
static s32 std_call(u32 fn, u32 a0, u32 a1, u32 a2)
{
	s32 ret = 0;
	int tries;

	for (tries = 0; tries < 1000; tries++) {
		ret = smc3(tries && ret != SM_ERR_BUSY ? SMC_SC_RESTART_LAST : fn,
			   tries && ret != SM_ERR_BUSY ? 0 : a0, tries && ret != SM_ERR_BUSY ? 0 : a1,
			   tries && ret != SM_ERR_BUSY ? 0 : a2);
		while (ret == SM_ERR_FIQ_INTERRUPTED)
			ret = smc3(SMC_SC_RESTART_FIQ, 0, 0, 0);
		if (ret != SM_ERR_BUSY && ret != SM_ERR_INTERRUPTED && ret != SM_ERR_CPU_IDLE)
			return ret;
		if (ret == SM_ERR_BUSY)
			delay_us(1000);
	}
	pr("trusty: std call 0x%x gave up after %d tries (last %d)\n", fn, tries, ret);
	return ret;
}

/* Let Trusty's threads run (Linux nop_work_func), e.g. the service accepting our connection. */
static void trusty_nop(void)
{
	u32 i;
	s32 ret;

	for (i = 0; i < 100; i++) {
		ret = std_call(SMC_SC_NOP, 0, 0, 0);
		if (ret != SM_ERR_NOP_INTERRUPTED)
			break;
	}
}

/* Run one QL-TIPC command already written into the page; the reply overwrites the header. */
static s32 ql_cmd(u16 opcode, u32 handle, u32 payload_len)
{
	struct ql_hdr *h = (struct ql_hdr *)ql_page;
	u64 id = ql_id();
	s32 ret;

	ql_sync();	/* before our writes: no dirty line of Trusty's may land on them later */
	h->opcode = opcode;
	h->flags = 0;
	h->status = 0;
	h->handle = handle;
	h->payload_len = payload_len;
	ql_sync();
	ret = std_call(SMC_SC_HANDLE_QL_TIPC_DEV_CMD, (u32)id, (u32)(id >> 32), sizeof(*h) + payload_len);
	ql_sync();	/* Trusty's reply out of its cache */
	if (h->opcode != (opcode | QL_TIPC_DEV_RESP)) {
		pr("trusty: ql cmd %u: smc %d, reply opcode 0x%x\n", opcode, ret, h->opcode);
		return ret ? ret : -1;
	}
	return h->status ? (ret ? ret : -1) : 0;
}

/* Returns the handle (>= 32), or a negative error. */
static s32 ql_connect(const char *port)
{
	struct ql_hdr *h = (struct ql_hdr *)ql_page;
	u8 *p = ql_page + sizeof(*h);
	u32 n = 0;
	s32 ret;

	memset(p, 0, 16);
	p[0] = 0x51;	/* cookie */
	while (port[n]) {
		p[16 + n] = (u8)port[n];
		n++;
	}
	p[16 + n] = 0;	/* Trusty checks strnlen(name) < the length it is given: send the NUL too */
	ret = ql_cmd(QL_TIPC_DEV_CONNECT, 0, 16 + n + 1);
	if (ret)
		return ret;
	return (s32)h->handle;
}

/* Wait for an event on a handle, running Trusty between polls. Returns the event mask. */
static u32 ql_wait_event(u32 handle, u32 polls)
{
	u32 *evt = (u32 *)(ql_page + sizeof(struct ql_hdr));
	u32 i;

	for (i = 0; i < polls; i++) {
		memset(evt, 0, 16);
		if (ql_cmd(QL_TIPC_DEV_GET_EVENT, handle, 8))
			return 0;
		if (evt[0])
			return evt[0];
		trusty_nop();
	}
	return 0;
}

static s32 ql_ver;

/* API version, FF-A share of the page, CREATE_QL_TIPC_DEV. Returns 0 when the device is up. */
int trusty_ql_open(void)
{
	u64 id;
	s32 ret;

	ql_ver = smc3(SMC_FC_API_VERSION, 5, 0, 0);
	pr("trusty: API_VERSION(5) = %d\n", ql_ver);
	if (ql_ver < 3 || ql_ver > 5)
		return -1;
	if (ql_ver < 5)
		ql_handle = (u64)(uptr)ql_page | NS_ATTR_CACHED_RW;
	else if (ffa_init() || ffa_share_page((u64)(uptr)ql_page, &ql_handle))
		return -1;
	id = ql_id();
	ql_sync();
	memset(ql_page, 0, QL_PAGE);
	ql_sync();
	ret = std_call(SMC_SC_CREATE_QL_TIPC_DEV, (u32)id, (u32)(id >> 32), QL_PAGE);
	ql_sync();	/* every call ends with this, so the page never holds a dirty line of Trusty's */
	pr("trusty: CREATE_QL_TIPC_DEV(0x%llx, 0x%x) = %d\n", id, QL_PAGE, ret);
	log_flush();
	return ret ? -1 : 0;
}

void trusty_ql_close(void)
{
	u64 id = ql_id();
	s32 ret = std_call(SMC_SC_SHUTDOWN_QL_TIPC_DEV, (u32)id, (u32)(id >> 32), QL_PAGE);

	pr("trusty: SHUTDOWN_QL_TIPC_DEV = %d\n", ret);
	if (ql_ver >= 5) {
		struct smc_ret4 r = smc4(FFA_MEM_RECLAIM, (u32)ql_handle, ql_handle >> 32, 0, 0);

		pr("trusty: FFA_MEM_RECLAIM = 0x%llx\n", r.r0);
		r = smc4(FFA_RXTX_UNMAP, (u64)ffa_local_id << 16, 0, 0, 0);
		pr("trusty: FFA_RXTX_UNMAP = 0x%llx\n", r.r0);
	}
	log_flush();
}

/* Connect and wait for READY. Returns the handle (>= 32) or a negative error. */
s32 trusty_ql_connect(const char *port)
{
	s32 h = ql_connect(port);
	u32 ev;

	pr("trusty: connect %s = %d\n", port, h);
	if (h <= 0)
		return h ? h : -1;
	ev = ql_wait_event((u32)h, 200);
	pr("trusty: event on handle 0x%x: 0x%x%s%s%s\n", (u32)h, ev,
	   (ev & IPC_HANDLE_POLL_READY) ? " READY" : "", (ev & IPC_HANDLE_POLL_HUP) ? " HUP" : "",
	   (ev & IPC_HANDLE_POLL_ERROR) ? " ERROR" : "");
	log_flush();
	if (!(ev & IPC_HANDLE_POLL_READY)) {
		ql_cmd(QL_TIPC_DEV_DISCONNECT, (u32)h, 0);
		return -1;
	}
	return h;
}

void trusty_ql_disconnect(s32 h)
{
	pr("trusty: disconnect 0x%x = %d\n", (u32)h, ql_cmd(QL_TIPC_DEV_DISCONNECT, (u32)h, 0));
}

/* One request, one reply (gsa.ko gsa_tz_chan_msg_xchg). Returns the reply length or < 0. */
int trusty_ql_xchg(s32 h, const void *req, u32 req_len, void *rsp, u32 rsp_max, u32 polls)
{
	struct ql_hdr *hd = (struct ql_hdr *)ql_page;
	u32 ev, n;
	s32 ret;

	ql_sync();
	memcpy(ql_page + sizeof(*hd), req, req_len);
	ret = ql_cmd(QL_TIPC_DEV_SEND, (u32)h, req_len);
	pr("trusty: send %u bytes = %d\n", req_len, ret);
	if (ret)
		return -1;
	ev = ql_wait_event((u32)h, polls);
	pr("trusty: event after send: 0x%x\n", ev);
	if (!(ev & IPC_HANDLE_POLL_MSG))
		return -1;
	ret = ql_cmd(QL_TIPC_DEV_RECV, (u32)h, 0);
	n = hd->payload_len;
	pr("trusty: recv = %d, %u bytes\n", ret, n);
	if (ret)
		return -1;
	if (n > rsp_max)
		n = rsp_max;
	memcpy(rsp, ql_page + sizeof(*hd), n);
	return (int)n;
}

void trusty_aoc_probe(void)
{
	s32 h;

	pr("\n--- trusty (QL-TIPC) ---\n");
	if (trusty_ql_open())
		return;
	h = trusty_ql_connect("com.android.trusty.gsa.hwmgr.aoc");
	if (h > 0)
		trusty_ql_disconnect(h);
	trusty_ql_close();
}
