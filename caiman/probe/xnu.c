/* SPDX-License-Identifier: MIT */
/*
 * Start HackiOS's XNU from the probe (build with LOADER=1).
 *
 * Follows HackiOS's own loaders, both MIT licensed:
 *   hakios-mt6768/mtkloader/mtkloader.c   run_xnu(): payload, Mach-O, boot_args, watcher
 *   hackios-xnu-kernel/xnuloader/loader.c run_kernel(): the Apple device tree for one kernel
 *                                         that runs on many boards
 * Copyright (c) the HakiOS developers; adapted for the Pixel 9 Pro (caiman).
 *
 * The boot image's kernel is this program followed by an "XNUP" payload
 * (tools/mkxnuimg.py) whose offset is in our Image header's res3 field.
 */
#include "probe.h"

/*
 * Who Darwin shall think it runs on: the iPhone's own device-tree properties, made from the builder's
 * IPSW by HackiOS's xnuloader/mkidentity.sh and copied here as identity.h (never committed: it is
 * Apple's data). Without it the device tree carries a placeholder: enough for the kernel and an
 * Apple-free root, not for Darwin's user space.
 */
#if __has_include("identity.h")
#include "identity.h"
#define HAVE_IDENTITY 1
#else
#define HAVE_IDENTITY 0
#endif

/*
 * The iOS system image (the logical partition xnusys of super, docs/m4-system-image-plan.md), read
 * from UFS at XNU's window + 128 MiB, as HackiOS's loader keeps its resident image
 * (RESIDENT_RD_OFFSET): it becomes /dev/md0, the root, and the payload's RAM disk /dev/md1.
 */
#define RESIDENT_RD_OFFSET	0x08000000ULL
#define RESIDENT_RD_RESERVE	0x30000000ULL	/* RAM that must remain for the kernel itself */

#define PAYLOAD_MAGIC		0x50554e58u	/* "XNUP" */
#define MH_MAGIC_64		0xfeedfacfu
#define LC_SEGMENT_64		0x19
#define LC_SYMTAB		0x2
#define LC_UNIXTHREAD		0x5

#define KERNEL_VIRT_BASE	0xfffffff004000000ULL	/* boot_args.virtBase */
#define XNU_PAGE		0x4000ULL
#define BOOT_LINE_LENGTH	608
#define NVRAM_BANK_SIZE		0x2000u
#define PANIC_LOG_SIZE		0x80000u
#define DT_MAX			0x40000u

struct boot_video {
	u64 v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

/* pexpert/pexpert/arm64/boot.h */
struct boot_args {
	u16 revision;
	u16 version;
	u64 virtBase;
	u64 physBase;
	u64 memSize;
	u64 topOfKernelData;
	struct boot_video video;
	u32 machineType;
	u64 deviceTreeP;
	u32 deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	u64 bootFlags;
	u64 memSizeActual;
};
_Static_assert(__builtin_offsetof(struct boot_args, video) == 40, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, deviceTreeP) == 96, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, commandLine) == 108, "boot_args");
_Static_assert(__builtin_offsetof(struct boot_args, bootFlags) == 720, "boot_args");

struct mach_header_64 {
	u32 magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct load_command {
	u32 cmd, cmdsize;
};
struct segment_command_64 {
	u32 cmd, cmdsize;
	char segname[16];
	u64 vmaddr, vmsize, fileoff, filesize;
	u32 maxprot, initprot, nsects, flags;
};
struct symtab_command {
	u32 cmd, cmdsize, symoff, nsyms, stroff, strsize;
};
struct nlist_64 {
	u32 n_strx;
	u8 n_type, n_sect;
	u16 n_desc;
	u64 n_value;
};

struct payload {
	const u8 *kernel, *ramdisk;
	u32 kernel_size, ramdisk_size;
	const char *cmdline;
};

/* shared with watcher.c */
struct xnu_info xnu;

/* In our own image (outside XNU's memory): the kernel's panic log, and a page of RAM that the
 * kernel's PL011 driver takes for its registers where the board has no usable UART. */
static u8 pram[PANIC_LOG_SIZE] __attribute__((aligned(0x4000)));
static u32 fake_uart[1024] __attribute__((aligned(0x4000)));

static u64 kva_to_pa(u64 va)
{
	return va - KERNEL_VIRT_BASE + PLAT_XNU_PHYS;
}

/* Copies and fills of megabytes with the MMU off: a word at a time when both ends allow it. */
static void copy(u64 dst, const void *src, u64 n)
{
	const u8 *s = src;

	if (!((dst | (uptr)s) & 7)) {
		volatile u64 *d8 = (volatile u64 *)(uptr)dst;
		const u64 *s8 = (const u64 *)s;

		for (; n >= 8; n -= 8)
			*d8++ = *s8++;
		dst = (u64)(uptr)d8;
		s = (const u8 *)s8;
	}
	while (n--)
		*(volatile u8 *)(uptr)dst++ = *s++;
}

static void fill0(u64 dst, u64 n)
{
	for (; n && (dst & 7); n--)
		*(volatile u8 *)(uptr)dst++ = 0;
	for (; n >= 8; n -= 8, dst += 8)
		*(volatile u64 *)(uptr)dst = 0;
	while (n--)
		*(volatile u8 *)(uptr)dst++ = 0;
}

static u32 rd_le32(const u8 *p)
{
	return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* ---- payload -------------------------------------------------------------- */

static int payload_parse(struct payload *pl)
{
	u64 off = *(volatile u64 *)(uptr)(g.load_base + 0x30);	/* Image header res3 */
	const u8 *p, *end;

	memset(pl, 0, sizeof(*pl));
	if (!off || (off & 15)) {
		pr("xnu: no payload offset in the Image header (res3 = 0x%llx)\n", off);
		return 0;
	}
	p = (const u8 *)(uptr)(g.load_base + off);
	if (rd_le32(p) != PAYLOAD_MAGIC) {
		pr("xnu: no XNUP payload at +0x%llx\n", off);
		return 0;
	}
	end = p + 8 + rd_le32(p + 4);
	p += 8;
	while (p + 24 <= end && p[0]) {
		const char *name = (const char *)p;
		u32 size = rd_le32(p + 16);
		const u8 *data = p + 24;

		if (!strcmp(name, "kernel")) {
			pl->kernel = data;
			pl->kernel_size = size;
		} else if (!strcmp(name, "ramdisk")) {
			pl->ramdisk = data;
			pl->ramdisk_size = size;
		} else if (!strcmp(name, "cmdline")) {
			pl->cmdline = (const char *)data;
		}
		p = data + ALIGN_UP(size, 16);
	}
	return pl->kernel && rd_le32(pl->kernel) == MH_MAGIC_64;
}

static void find_syms(const u8 *k)
{
	const struct mach_header_64 *mh = (const void *)k;
	const struct load_command *lc = (const void *)(mh + 1);
	const struct symtab_command *st = NULL;
	u32 i;

	for (i = 0; i < mh->ncmds; i++, lc = (const void *)((const u8 *)lc + lc->cmdsize))
		if (lc->cmd == LC_SYMTAB)
			st = (const void *)lc;
	if (!st)
		return;
	for (i = 0; i < st->nsyms; i++) {
		const struct nlist_64 *s = (const void *)(k + st->symoff + (u64)i * sizeof(*s));
		const char *n = (const char *)k + st->stroff + s->n_strx;

		if (n[0] != '_' || (n[1] != 'l' && n[1] != 'd' && n[1] != 'v') || !s->n_value ||
		    (s->n_type & 0xe0))	/* debugging (stab) entries */
			continue;
		if (!strcmp(n, "_loader_log"))
			xnu.log_va = s->n_value;
		else if (!strcmp(n, "_loader_log_len"))
			xnu.log_len_va = s->n_value;
		else if (!strcmp(n, "_loader_log_putc"))
			xnu.log_putc_va = s->n_value;
		else if (!strcmp(n, "_debugger_panic_str"))
			xnu.panic_str_va = s->n_value;
		else if (!strcmp(n, "_vmapple_uart_transmit_ready"))
			xnu.tx_ready_va = s->n_value;
		else if (!strcmp(n, "_vmapple_uart_transmit_data"))
			xnu.tx_data_va = s->n_value;
		else if (!strcmp(n, "_vmapple_uart_receive_ready"))
			xnu.rx_ready_va = s->n_value;
	}
}

/*
 * The kernel's console is its PL011 driver (vmapple_uart). As mtkloader does on the MediaTek
 * phone: every byte it would send goes to loader_log_putc instead (a tail call: the byte is
 * already in w0), so it lands in the kernel's own loader_log, which the watcher copies to
 * pstore; the driver never waits for the UART and never reads input from it.
 */
static void patch_console(void)
{
	volatile u32 *at;
	s64 rel;

	if (!xnu.tx_ready_va || !xnu.tx_data_va || !xnu.log_putc_va) {
		pr("xnu: vmapple_uart/loader_log_putc not found: console NOT redirected to loader_log\n");
		return;
	}
	at = (volatile u32 *)(uptr)kva_to_pa(xnu.tx_ready_va);
	at[0] = 0x52800020u;	/* mov w0, #1 */
	at[1] = 0xd65f03c0u;	/* ret */
	rel = (s64)(xnu.log_putc_va - xnu.tx_data_va) >> 2;
	if (rel < -(1 << 25) || rel >= (1 << 25)) {
		pr("xnu: loader_log_putc out of branch range\n");
		return;
	}
	at = (volatile u32 *)(uptr)kva_to_pa(xnu.tx_data_va);
	at[0] = 0x14000000u | ((u32)rel & 0x03ffffffu);	/* b loader_log_putc */
	if (xnu.rx_ready_va) {
		at = (volatile u32 *)(uptr)kva_to_pa(xnu.rx_ready_va);
		at[0] = 0x52800000u;	/* mov w0, #0 */
		at[1] = 0xd65f03c0u;	/* ret */
	}
	pr("xnu: console -> loader_log (vmapple_uart transmit patched, receive off)\n");
}

/* ---- Apple device tree ---------------------------------------------------- */
/* node := u32 nProperties, u32 nChildren, props[], children[]
 * prop := char name[32], u32 length, value padded to 4 bytes */

struct dt_writer {
	u8 *base, *cur, *end;
	u32 *counts[16];
	int depth, overflow;
};

static void dt_prop(struct dt_writer *w, const char *name, const void *data, u32 len)
{
	u32 padded = (len + 3) & ~3u;

	if (w->overflow || strlen(name) > 31 || (u64)(w->end - w->cur) < 36 + padded) {
		w->overflow = 1;
		return;
	}
	memset(w->cur, 0, 36 + padded);
	memcpy(w->cur, name, strlen(name));
	memcpy(w->cur + 32, &len, 4);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + padded;
	w->counts[w->depth - 1][0]++;
}

static void dt_str(struct dt_writer *w, const char *name, const char *v)
{
	dt_prop(w, name, v, (u32)strlen(v) + 1);
}

static void dt_u32(struct dt_writer *w, const char *name, u32 v)
{
	dt_prop(w, name, &v, 4);
}

static void dt_u64(struct dt_writer *w, const char *name, u64 v)
{
	dt_prop(w, name, &v, 8);
}

static void dt_padded(struct dt_writer *w, const char *name, const char *v, u32 size)
{
	u8 field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, v, strlen(v));
	dt_prop(w, name, field, size);
}

static void dt_begin(struct dt_writer *w, const char *name)
{
	static u32 scratch[2];

	if (w->depth > 0)
		w->counts[w->depth - 1][1]++;
	if (w->overflow || w->depth >= 15 || (u64)(w->end - w->cur) < 8) {
		w->overflow = 1;
		w->counts[w->depth++] = scratch;
		return;
	}
	w->counts[w->depth++] = (u32 *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_str(w, "name", name);
}

static void dt_end(struct dt_writer *w)
{
	w->depth--;
}

static void nvram_header(u8 *h, u8 sig, u32 bytes, const char *name)
{
	u16 blocks = (u16)(bytes / 16), sum;
	int i;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (u8)blocks;
	h[3] = (u8)(blocks >> 8);
	memcpy(h + 4, name, strlen(name));
	sum = h[0];
	for (i = 2; i < 16; i++)
		sum += h[i];
	while (sum > 0xff)
		sum = (sum & 0xff) + (sum >> 8);
	h[1] = (u8)sum;
}

/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
static void nvram_image(u8 *img)
{
	u32 lo = 1, hi = 0, gen = 1, i, adler;

	memset(img, 0, NVRAM_BANK_SIZE);
	nvram_header(img, 0x5a, 0x20, "nvram");
	nvram_header(img + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common");
	memcpy(img + 20, &gen, 4);
	for (i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + img[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	adler = (hi << 16) | lo;
	memcpy(img + 16, &adler, 4);
}

#if HAVE_IDENTITY
/*
 * The iPhone's properties, but one, as HackiOS's loader: "artwork-display-gamut" P3 makes WebKit draw
 * ten-bit surfaces that nothing here shows; the screen is what the frame buffer is, sRGB.
 */
static void dt_identity(struct dt_writer *w, const struct identity_prop *props, u32 count, const u8 *data)
{
	u32 i;

	for (i = 0; i < count; i++) {
		if (!strcmp(props[i].name, "artwork-display-gamut"))
			dt_prop(w, props[i].name, "sRGB", 5);
		else
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}
#endif

static u64 boot_epoch;	/* xnu_boot(): seconds since 1970 now, 0 if unknown */

static u32 build_adt(u64 dt_pa, u64 rd_pa, u64 rd_size, u64 res_pa, u64 res_size)
{
	static u8 nvram[NVRAM_BANK_SIZE];
	static const u8 wifi_mac[6] = { 0x02, 0x43, 0x41, 0x49, 0x00, 0x01 };
	static const u8 bt_mac[6] = { 0x02, 0x43, 0x41, 0x49, 0x00, 0x02 };
	static const u8 manifest[48] = { 'c', 'a', 'i', 'm', 'a', 'n' };
	static const u8 no_behavior[16];
	struct dt_writer w;
	u64 ranges[3] = { 0, PLAT_IO_BASE, PLAT_IO_SIZE };
	u64 gic_reg[4] = { PLAT_GICD - PLAT_IO_BASE, 0x10000, PLAT_GICR - PLAT_IO_BASE, PLAT_GICR_SIZE };
	u64 uart_reg[2], pram_reg[2];
	u64 cntfrq = read_sysreg(cntfrq_el0);
	u8 seed[64];
	u32 i;

	/* addresses are taken at run time: an initializer would bake in the link address */
	pram_reg[0] = (u64)(uptr)pram;
	pram_reg[1] = PANIC_LOG_SIZE;
	memset(&w, 0, sizeof(w));
	w.base = w.cur = (u8 *)(uptr)dt_pa;
	w.end = w.base + DT_MAX;
	for (i = 0; i < sizeof(seed); i++)
		seed[i] = (u8)(timer_ticks() >> (i & 7)) ^ (u8)(i * 37);
	nvram_image(nvram);
	fill0((u64)(uptr)pram, PANIC_LOG_SIZE);

	dt_begin(&w, "device-tree");
#if HAVE_IDENTITY
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
#else
	/* identity: none (placeholder, as xnuloader/mkidentity.py --placeholder): enough for the kernel */
	dt_prop(&w, "hakios-placeholder", "", 0);
#endif
	dt_str(&w, "platform-name", "t8015");
	dt_padded(&w, "serial-number", "CAIMAN0000000000", 32);
	dt_padded(&w, "mlb-serial-number", "CAIMAN0000000000MLB", 32);
	dt_padded(&w, "model-number", "MQAC2", 32);
	dt_padded(&w, "region-info", "LL/A", 32);
	dt_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_u32(&w, "#address-cells", 2);
	dt_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_u64(&w, "dram-base", PLAT_XNU_PHYS);
	dt_u64(&w, "dram-size", PLAT_XNU_SIZE);
	dt_u32(&w, "debug-enabled", 1);
	dt_u32(&w, "display-scale", 3);
	dt_u32(&w, "display-rotation", 0);
	dt_str(&w, "psci-method", g.psci_hvc ? "hvc" : "smc");
	if (boot_epoch)
		dt_u64(&w, "boot-epoch", boot_epoch);
	dt_u64(&w, "unique-chip-id", 0x000043414930001ULL);
	dt_u32(&w, "chip-id", 0x8015);
	dt_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_mac, sizeof(wifi_mac));
	dt_prop(&w, "mac-address-bluetooth0", bt_mac, sizeof(bt_mac));
	dt_prop(&w, "boot-manifest-hash", manifest, sizeof(manifest));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_u32(&w, "certificate-production-status", 1);
	dt_u32(&w, "certificate-security-mode", 1);
	dt_u32(&w, "effective-production-status-ap", 1);
	dt_u32(&w, "effective-security-mode-ap", 1);
	dt_u32(&w, "secure-boot", 1);
	dt_u32(&w, "production-cert", 1);
	dt_u32(&w, "development-cert", 0);
	dt_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	dt_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_u32(&w, "nvram-bank-count", 1);
	dt_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram, sizeof(nvram));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" is /dev/md0, the root; "RAMDisk1" /dev/md1. With xnusys in memory it is the root and
	 * the payload's small image (HackiOS's root.apfs, mounted on /redwood) the second disk.
	 */
	if (res_size) {
		u64 rd[2] = { res_pa, res_size };

		dt_prop(&w, "RAMDisk", rd, sizeof(rd));
		if (rd_size) {
			u64 rd1[2] = { rd_pa, rd_size };

			dt_prop(&w, "RAMDisk1", rd1, sizeof(rd1));
		}
	} else if (rd_size) {
		u64 rd[2] = { rd_pa, rd_size };

		dt_prop(&w, "RAMDisk", rd, sizeof(rd));
	}
	dt_end(&w);
	dt_end(&w);	/* chosen */

	dt_begin(&w, "defaults");
	dt_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	dt_begin(&w, "product");
#if HAVE_IDENTITY
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]),
		    identity_product_data);
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]),
		    identity_haptics_data);
	dt_end(&w);
#else
	dt_prop(&w, "hakios-placeholder", "", 0);
	dt_begin(&w, "haptics");
	dt_prop(&w, "hakios-placeholder", "", 0);
	dt_end(&w);
#endif
	dt_end(&w);

	dt_begin(&w, "options");	/* IOKit publishes NVRAM here; it asserts without the node */
	dt_end(&w);

	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/* one processor: the one we run on, Aff1/Aff0 as the kernel compares them */
	dt_begin(&w, "cpus");
	dt_begin(&w, "cpu0");
	dt_str(&w, "device_type", "cpu");
	dt_str(&w, "state", "running");
	dt_u32(&w, "reg", (u32)(read_sysreg(mpidr_el1) & 0xffffff));
	dt_u32(&w, "cpu-id", 0);
	dt_u64(&w, "timebase-frequency", cntfrq);
	dt_u64(&w, "fixed-frequency", cntfrq);
	dt_u64(&w, "clock-frequency", 1800000000ULL);
	dt_u64(&w, "bus-frequency", 100000000ULL);
	dt_u64(&w, "memory-frequency", 100000000ULL);
	dt_u64(&w, "peripheral-frequency", 24000000ULL);
	dt_end(&w);
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_str(&w, "device_type", PLAT_ADT_NAME);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_u32(&w, "#address-cells", 2);
	dt_u32(&w, "#size-cells", 2);
	dt_begin(&w, "gic");
	dt_str(&w, "device_type", "interrupt-controller");
	dt_str(&w, "compatible", "arm,gic-v3");
	dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	dt_end(&w);
	dt_begin(&w, "uart0");
	dt_str(&w, "device_type", "uart");
	dt_str(&w, "compatible", "arm,pl011");
	uart_reg[0] = xnu.uart_pa - PLAT_IO_BASE;
	uart_reg[1] = 0x1000;
	dt_prop(&w, "reg", uart_reg, sizeof(uart_reg));
	dt_u32(&w, "boot-console", 1);
	dt_end(&w);
	dt_end(&w);	/* arm-io */

	dt_end(&w);	/* device-tree */

	if (w.overflow) {
		pr("xnu: device tree does not fit in 0x%x bytes\n", DT_MAX);
		return 0;
	}
	return (u32)(w.cur - w.base);
}

/* ---- boot ------------------------------------------------------------------ */

/*
 * "epoch=SECONDS" in the payload's command line is the host's clock when it sent the image (the run
 * script writes it just before fastboot boot). The kernel takes epoch= before the device tree's
 * boot-epoch and as the time at its start, so the loader drops it from the boot-args and dates the
 * kernel itself: that time plus the ~90 s spent here (mostly reading xnusys).
 */
static u64 payload_epoch(const char *c)
{
	u64 v = 0;

	for (; c && *c; c++) {
		if (!strncmp(c, "epoch=", 6)) {
			for (c += 6; *c >= '0' && *c <= '9'; c++)
				v = v * 10 + (u64)(*c - '0');
			return v;
		}
		while (*c && *c != ' ')
			c++;
		if (!*c)
			break;
	}
	return 0;
}

/* Append s without its epoch= word. */
static u32 append_no_epoch(char *dst, u32 n, const char *s)
{
	while (*s && n < BOOT_LINE_LENGTH - 1) {
		if (!strncmp(s, "epoch=", 6)) {
			while (*s && *s != ' ')
				s++;
			while (*s == ' ')
				s++;
			continue;
		}
		while (*s && *s != ' ' && n < BOOT_LINE_LENGTH - 1)
			dst[n++] = *s++;
		while (*s == ' ' && n < BOOT_LINE_LENGTH - 1)
			dst[n++] = *s++;
	}
	while (n && dst[n - 1] == ' ')
		n--;
	dst[n] = 0;
	return n;
}

static u32 append(char *dst, u32 n, const char *s)
{
	while (*s && n < BOOT_LINE_LENGTH - 1)
		dst[n++] = *s++;
	dst[n] = 0;
	return n;
}

void xnu_boot(void)
{
	struct payload pl;
	const struct mach_header_64 *mh;
	const struct load_command *lc;
	struct boot_args *ba;
	u64 vm_hi = 0, entry_va = 0, kern_hi, rd_pa, rd_size, dt_pa, ba_pa, top, res_pa = 0, res_size = 0;
	u32 i, dt_len, n;

	pr("\n--- xnu ---\n");
	if (!payload_parse(&pl)) {
		pr("xnu: no usable kernel in the payload; not starting XNU\n");
		return;
	}
	pr("xnu: kernel %u bytes, ramdisk %u bytes, XNU memory 0x%llx + 0x%llx\n", pl.kernel_size,
	   pl.ramdisk_size, PLAT_XNU_PHYS, PLAT_XNU_SIZE);
	if (overlaps(PLAT_XNU_PHYS, PLAT_XNU_SIZE, g.load_base, g.image_size) ||
	    overlaps(PLAT_XNU_PHYS, PLAT_XNU_SIZE, g.dtb, g.fdt.totalsize) ||
	    overlaps(PLAT_XNU_PHYS, PLAT_XNU_SIZE, g.console.base, g.console.size) ||
	    (g.nmem && !addr_in_dram(PLAT_XNU_PHYS, PLAT_XNU_SIZE))) {
		pr("xnu: XNU's memory window is not free DRAM by the device tree; not starting\n");
		return;
	}

	/*
	 * The watcher core now, not after the long read below: started ~100 s into the boot (after the
	 * UFS load) it never ran (PSCI said yes, the core never reached its first instruction). It only
	 * feeds the watchdog until watcher_go().
	 */
	watcher_start();
#if PROBE_UFS >= 2
	/* the system image first: ~2 minutes of READ(10)s, the watchdog kicked all along */
	res_pa = PLAT_XNU_PHYS + RESIDENT_RD_OFFSET;
	res_size = ufs_load_lp("xnusys", res_pa, PLAT_XNU_SIZE - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE);
	if (res_size)
		res_size = ALIGN_UP(res_size, XNU_PAGE);
	else
		pr("xnu: no xnusys loaded: booting without the system image\n");
	pr("xnu: identity: %s\n", HAVE_IDENTITY ? "the iPhone's (identity.h)" : "placeholder (no identity.h)");
#endif

	/* the kernel's segments, at their place in XNU's window */
	mh = (const void *)pl.kernel;
	lc = (const void *)(mh + 1);
	pr("xnu: copying segments (MMU off: this takes a moment)\n");
	log_flush();
	wdt_kick();	/* the bootloader's watchdog: ~60 s from its start, and it is not ours yet */
	for (i = 0; i < mh->ncmds; i++, lc = (const void *)((const u8 *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;

			if (!sg->vmsize)
				continue;
			if (sg->vmaddr + sg->vmsize > vm_hi)
				vm_hi = sg->vmaddr + sg->vmsize;
			fill0(kva_to_pa(sg->vmaddr), sg->vmsize);
			copy(kva_to_pa(sg->vmaddr), pl.kernel + sg->fileoff, sg->filesize);
		} else if (lc->cmd == LC_UNIXTHREAD) {
			const u8 *t = (const u8 *)lc + 16 + 32 * 8;	/* x0..x28, fp, lr, sp, then pc */

			entry_va = (u64)rd_le32(t) | (u64)rd_le32(t + 4) << 32;
		}
	}
	if (!entry_va) {
		pr("xnu: no LC_UNIXTHREAD entry point\n");
		return;
	}
	wdt_kick();
	find_syms(pl.kernel);
	patch_console();
	xnu.log_pa = xnu.log_va ? kva_to_pa(xnu.log_va) : 0;
	xnu.log_len_pa = xnu.log_len_va ? kva_to_pa(xnu.log_len_va) : 0;
	pr("xnu: entry 0x%llx (pa 0x%llx), top 0x%llx; loader_log %s\n", entry_va,
	   kva_to_pa(entry_va), kva_to_pa(vm_hi), xnu.log_pa ? "found" : "NOT found");

	/* ramdisk, device tree, boot_args, one after the other above the kernel */
	kern_hi = ALIGN_UP(kva_to_pa(vm_hi), XNU_PAGE);
	rd_pa = kern_hi;
	rd_size = ALIGN_UP(pl.ramdisk_size, XNU_PAGE);
	if (rd_size) {
		fill0(rd_pa, rd_size);
		copy(rd_pa, pl.ramdisk, pl.ramdisk_size);
	}
	xnu.uart_pa = PLAT_UART_FOR_XNU ? PLAT_UART_FOR_XNU : (u64)(uptr)fake_uart;
	if (!PLAT_UART_FOR_XNU) {
		fill0(xnu.uart_pa, sizeof(fake_uart));
		fake_uart[0x18 / 4] = 0x90;	/* FR: transmit FIFO empty, receive FIFO empty */
	}
	if (res_size && rd_pa + rd_size > res_pa) {
		pr("xnu: kernel and ramdisk (up to 0x%llx) overlap xnusys at 0x%llx; not starting\n", rd_pa + rd_size,
		   res_pa);
		return;
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover xnusys too */
	dt_pa = res_size ? res_pa + res_size : rd_pa + rd_size;
	boot_epoch = payload_epoch(pl.cmdline);
	if (boot_epoch) {
		u64 here = (timer_ticks() - g.entry_ticks) / timer_freq();

		boot_epoch += here + 1;		/* + the image's transfer before we started */
		pr("xnu: time: payload epoch + %llu s here = %llu\n", here + 1, boot_epoch);
	}
	dt_len = build_adt(dt_pa, rd_pa, rd_size, res_pa, res_size);
	if (!dt_len)
		return;
	ba_pa = ALIGN_UP(dt_pa + dt_len, XNU_PAGE);
	top = ba_pa + XNU_PAGE;

	ba = (struct boot_args *)(uptr)ba_pa;
	fill0(ba_pa, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = PLAT_XNU_PHYS;
	ba->memSize = PLAT_XNU_SIZE;
	ba->memSizeActual = PLAT_XNU_SIZE;
	ba->topOfKernelData = top;
	ba->deviceTreeP = dt_pa - PLAT_XNU_PHYS + KERNEL_VIRT_BASE;
	ba->deviceTreeLength = dt_len;
	if (g.fb_addr && g.fb_bpp == 4) {
		ba->video.v_baseAddr = g.fb_addr;
		ba->video.v_display = 0;	/* the kernel's text console (-v) */
		ba->video.v_rowBytes = g.fb_stride;
		ba->video.v_width = g.fb_width;
		ba->video.v_height = g.fb_height;
		ba->video.v_depth = 32;
	}
	n = append(ba->commandLine, 0, PLAT_XNU_ARGS);
	if (rd_size || res_size)
		n = append(ba->commandLine, n, " rd=md0");
	if (pl.cmdline && pl.cmdline[0]) {
		n = append(ba->commandLine, n, " ");
		n = append_no_epoch(ba->commandLine, n, pl.cmdline);
	}
	pr("xnu: device tree 0x%llx (%u bytes), boot_args 0x%llx, top 0x%llx\n", dt_pa, dt_len, ba_pa,
	   top);
	pr("xnu: uart for the kernel 0x%llx%s, framebuffer 0x%llx\n", xnu.uart_pa,
	   PLAT_UART_FOR_XNU ? "" : " (a page of RAM: the console goes only to loader_log)",
	   ba->video.v_baseAddr);
	pr("xnu: boot-args: %s\n", ba->commandLine);

	/* everything we wrote went to DRAM uncached; make sure no stale line shadows it */
	for (u64 a = PLAT_XNU_PHYS; a < top; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	dsb_sy();

	if (g.fb_addr)
		display_send_frames();
	watcher_go();
	pr("xnu: jumping to the kernel at EL1\n");
	log_flush();
	jump_to_xnu(kva_to_pa(entry_va), ba_pa);
}
