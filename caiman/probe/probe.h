/* SPDX-License-Identifier: MIT */
/*
 * caiman-probe: a bare-metal program the Pixel 9 Pro's stock bootloader
 * starts in place of a Linux kernel (via `fastboot boot`).
 *
 * Runs with the MMU off, on one CPU, at whatever EL the bootloader used.
 * With the MMU off every data access is Device-nGnRnE: it must be naturally
 * aligned (build uses -mstrict-align) and goes straight to DRAM/MMIO.
 */
#ifndef PROBE_H
#define PROBE_H

#include <stdarg.h>

#pragma GCC visibility push(hidden)

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned long uptr;

#define NULL ((void *)0)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((u64)(a) - 1))

#include "platform.h"

/* ---- MMIO / system registers ---------------------------------------- */

static inline u32 rd32(u64 a) { return *(volatile u32 *)(uptr)a; }
static inline void wr32(u64 a, u32 v) { *(volatile u32 *)(uptr)a = v; }

#define read_sysreg(r) ({ u64 __v; __asm__ volatile("mrs %0, " #r : "=r"(__v)); __v; })
#define write_sysreg(r, v) __asm__ volatile("msr " #r ", %0" : : "r"((u64)(v)))

static inline void dsb_sy(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static inline void isb(void) { __asm__ volatile("isb" ::: "memory"); }

/* ---- lib.c ------------------------------------------------------------ */

void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
int memcmp(const void *a, const void *b, unsigned long n);
unsigned long strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);

/* ---- timer.c ---------------------------------------------------------- */

void timer_init(void);
u64 timer_ticks(void);
u64 timer_freq(void);
u64 timer_us(void);		/* microseconds since probe start */
void delay_us(u64 us);

/* ---- log.c ------------------------------------------------------------ */

void log_putc(char c);
void log_puts(const char *s);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_vprintf(const char *fmt, va_list ap);
void log_flush(void);
/* Attach the ramoops console zone; replays everything logged so far. */
void log_attach_ramoops(u64 zone_base, u64 zone_size);
int log_ramoops_attached(void);
u64 log_ramoops_base(void);
u64 log_space_left(void);
void log_uart_enable(int on);
void log_uart_mute(int mute);	/* keep UART quiet for bulk dumps */
void log_uart_replay(void);
u32 log_pos(void);
void log_rewind(u32 pos);
void log_write_raw(const volatile u8 *p, u32 n);

#define pr(...) log_printf(__VA_ARGS__)

/* ---- uart.c ----------------------------------------------------------- */

/* Returns 1 if the UART looks usable (bootloader left TX enabled). */
int uart_init(u64 base);
void uart_putc(char c);

/* ---- exception.c / guarded MMIO -------------------------------------- */

/*
 * Read a 32-bit register; if the access takes a synchronous external
 * abort the exception handler skips the load and *ok is set to 0.
 * (A powered-down block may instead hang the bus: that cannot be caught,
 * which is why every risky read is preceded by a breadcrumb in the log.)
 */
u32 safe_rd32(u64 addr, int *ok);
u32 safe_rd32_async(u64 addr, int *ok);
void exc_handler(u64 idx, u64 esr, u64 far, u64 elr, u64 spsr, u64 *frame);

/* ---- wifi.c (PROBE_WIFI) ----------------------------------------------- */
void wifi_probe(void);

/* ---- panel.c (PROBE_PANEL) --------------------------------------------- */
void panel_probe(void);

/* ---- battery.c (PROBE_BATTERY) ------------------------------------------ */
void battery_probe(void);
int battery_poll(int say);
u64 battery_shared_pa(void);
void wifi_handoff_look(const char *where);	/* PROBE_WIFI=13: is the handed chip still answering */
u64 wifi_handoff_pa(void);	/* PROBE_WIFI=13: the page for the kernel's BCM4390 driver, or 0 */

/* ---- fdt.c ------------------------------------------------------------ */

struct fdt {
	const u8 *base;
	u32 totalsize;
	u32 off_struct, size_struct;
	u32 off_strings, size_strings;
	u32 version;
};

/* Visitor: return non-zero to stop the walk. pac/psc: parent's cells. */
typedef int (*fdt_visit_fn)(const struct fdt *f, int node, int depth,
			    u32 pac, u32 psc, void *ctx);

int fdt_init(struct fdt *f, u64 addr);
int fdt_walk(const struct fdt *f, fdt_visit_fn visit, void *ctx);
const char *fdt_node_name(const struct fdt *f, int node);
const u8 *fdt_getprop(const struct fdt *f, int node, const char *name, u32 *len);
int fdt_prop_has_string(const struct fdt *f, int node, const char *prop,
			const char *str);
int fdt_stringlist_index(const struct fdt *f, int node, const char *prop,
			 const char *str);
int fdt_find_compatible(const struct fdt *f, const char *compat, int start_after,
			u32 *pac, u32 *psc);
int fdt_find_phandle(const struct fdt *f, u32 phandle, u32 *pac, u32 *psc);
int fdt_read_u32(const struct fdt *f, int node, const char *name, u32 *out);
/* Read entry `idx` of a reg-style property using the given cell counts. */
int fdt_reg(const struct fdt *f, int node, const char *prop, u32 ac, u32 sc,
	    int idx, u64 *addr, u64 *size);
u32 be32(const u8 *p);
u64 fdt_cells(const u8 *p, u32 n);

/* ---- psci.c ----------------------------------------------------------- */

#define PSCI_VERSION		0x84000000u
#define PSCI_SYSTEM_RESET	0x84000009u
#define PSCI_FEATURES		0x8400000Au
#define PSCI_SYSTEM_RESET2_64	0xC4000012u

void psci_set_conduit_hvc(int hvc);
s64 psci_call(u64 fn, u64 a1, u64 a2, u64 a3);
void psci_report(void);
void __attribute__((noreturn)) probe_finish(const char *why);

/* ---- display.c -------------------------------------------------------- */

void display_probe(void);
void ufs_probe(void);
void ufslog_snapshot(u64 zone_base, u64 zone_size);
void touch_probe(void);
void dvfs_probe(void);
void gpu_probe(void);		/* gpu.c, PROBE_GPU */
/* gpu.c (PROBE_GPU >= 4): the GPU as the probe leaves it, for XNU's /chosen/mali-handoff (xnu.c) */
struct gpu_handoff {
	u64 shared;	/* PA of the firmware's shared interface section (MCU VA 0x04000000) */
	u64 ring;	/* PA of CS 0's ring (AS1 VA 0x100000, 4 KiB) */
	u64 queue;	/* PA of its queue input page (output page + 0x1000) */
	u64 target;	/* PA of the AS1 page at VA 0x200000 */
	u64 insert;	/* the ring's INSERT so far, bytes */
	u64 as1_root;	/* PA of AS1's level-0 table */
	u64 pool_next;	/* the free DRAM after the probe's GPU allocations, to pool_end: for XNU's page tables */
	u64 pool_end;
	struct {
		u64 ring, ring_size, queue, insert;	/* stream s of group 0: ring PA and size, queue pages PA, INSERT */
	} stream[3];
};
extern struct gpu_handoff gpu_handoff;		/* dvfs.c, PROBE_DVFS */
void trusty_aoc_probe(void);
int trusty_ql_open(void);
void trusty_ql_close(void);
int trusty_ql_connect(const char *port);
void trusty_ql_disconnect(int h);
int trusty_ql_xchg(int h, const void *req, u32 req_len, void *rsp, u32 rsp_max, u32 polls);
void aoc_start(void);
int aoc_tbn_to_ap(void);
u64 ufs_load_lp(const char *name, u64 dst, u64 max);
void display_send_frames(void);
void kick_frames(u64 decon_main);

/* ---- xnu.c / watcher.c / handoff.S (LOADER=1) -------------------------- */

struct xnu_info {
	u64 log_va, log_len_va, panic_str_va;	/* kernel virtual addresses of its symbols */
	u64 log_putc_va, tx_ready_va, tx_data_va, rx_ready_va;
	u64 log_pa, log_len_pa;			/* ... and where they are in RAM */
	u64 uart_pa;				/* what the kernel's PL011 driver is given */
};
extern struct xnu_info xnu;

void xnu_boot(void);
void watcher_start(void);
void watcher_main(void);
void watcher_entry(void);
void secondary_entry(void);	/* handoff.S: the kernel's other cores, EL2 to EL1 */
void watcher_go(void);
void mmu_on(u64 l1_table);
void __attribute__((noreturn)) jump_to_xnu(u64 entry_pa, u64 boot_args_pa);

/* ---- wdt.c ------------------------------------------------------------ */

void wdt_report(void);
void wdt_kick(void);

/* ---- main.c: shared state -------------------------------------------- */

#define MAX_MEM_RANGES 16

struct region {
	u64 base, size;
};

struct probe_state {
	u32 el;			/* CurrentEL at entry: 1 or 2 */
	u64 load_base;		/* where the bootloader put us */
	u64 entry_ticks;	/* the counter at entry (the loader dates the kernel from it) */
	u64 image_size;		/* text+data+bss+stack */
	u64 dtb;		/* x0 at entry */
	u64 initrd_start, initrd_end;	/* DT /chosen linux,initrd-* */
	int have_fdt;
	struct fdt fdt;

	struct region mem[MAX_MEM_RANGES];
	int nmem;

	/* ramoops as Linux will compute it */
	struct region ramoops;		/* whole ramoops reservation */
	struct region console;		/* console zone inside it */
	u32 ramoops_ecc;

	/* frame buffer the bootloader hands over (DT memory-region) */
	struct region fb_handover;

	/* frame buffer read back from the display DMA registers */
	u64 fb_addr;
	u32 fb_width, fb_height, fb_stride, fb_bpp, fb_format;
	int fb_channel;
	int decon_cmd_mode;		/* -1 unknown, 0 video, 1 command */
	u32 frame_count_delta;

	u64 decon_main;
	int uart_ok;
	int psci_hvc;
	s64 psci_version;
	s64 psci_reset2;		/* PSCI_FEATURES(SYSTEM_RESET2) */
	int faults;			/* guarded reads that aborted */

	int wdt_running;
	u32 wdt_period_ms, wdt_left_ms;
};

extern struct probe_state g;

/* linker.ld symbols (hidden, so they are reached PC-relative, not via a GOT) */
extern char __image_start[], __image_end[], __file_end[];

int addr_in_dram(u64 base, u64 size);
int overlaps(u64 a, u64 as, u64 b, u64 bs);

#pragma GCC visibility pop

#endif
