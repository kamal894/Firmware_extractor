/* SPDX-License-Identifier: MIT */
/*
 * Read-only flattened device tree walker. All multi-byte values are read
 * a byte at a time, so the blob's alignment never matters.
 */
#include "probe.h"

#define FDT_MAGIC	0xd00dfeedu
#define FDT_BEGIN_NODE	1u
#define FDT_END_NODE	2u
#define FDT_PROP	3u
#define FDT_NOP		4u
#define FDT_END		9u
#define FDT_MAXDEPTH	32

u32 be32(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

u64 fdt_cells(const u8 *p, u32 n)
{
	u64 v = 0;
	u32 i;

	for (i = 0; i < n; i++)
		v = (v << 32) | be32(p + 4 * i);
	return v;
}

int fdt_init(struct fdt *f, u64 addr)
{
	const u8 *b = (const u8 *)(uptr)addr;

	memset(f, 0, sizeof(*f));
	if (!addr || (addr & 3))
		return -1;
	if (be32(b) != FDT_MAGIC)
		return -2;
	f->base = b;
	f->totalsize = be32(b + 4);
	f->off_struct = be32(b + 8);
	f->off_strings = be32(b + 12);
	f->version = be32(b + 20);
	f->size_strings = be32(b + 32);
	f->size_struct = f->version >= 17 ? be32(b + 36) : f->totalsize - f->off_struct;
	if (f->totalsize < 40 || f->totalsize > 0x1000000 ||
	    f->off_struct >= f->totalsize || f->off_strings >= f->totalsize ||
	    f->off_struct + f->size_struct > f->totalsize ||
	    f->off_strings + f->size_strings > f->totalsize)
		return -3;
	return 0;
}

static u32 tag_at(const struct fdt *f, u32 off)
{
	if (off + 4 > f->size_struct)
		return FDT_END;
	return be32(f->base + f->off_struct + off);
}

static const char *str_at(const struct fdt *f, u32 nameoff)
{
	if (nameoff >= f->size_strings)
		return "";
	return (const char *)(f->base + f->off_strings + nameoff);
}

static u32 skip_name(const struct fdt *f, u32 off)
{
	/* off points at FDT_BEGIN_NODE; returns offset of the first tag after the name */
	const char *n = (const char *)(f->base + f->off_struct + off + 4);
	u32 len = 0;

	while (off + 4 + len < f->size_struct && n[len])
		len++;
	return (u32)ALIGN_UP(off + 4 + len + 1, 4);
}

const char *fdt_node_name(const struct fdt *f, int node)
{
	return (const char *)(f->base + f->off_struct + (u32)node + 4);
}

const u8 *fdt_getprop(const struct fdt *f, int node, const char *name, u32 *len)
{
	u32 off = skip_name(f, (u32)node);

	for (;;) {
		u32 tag = tag_at(f, off);

		if (tag == FDT_NOP) {
			off += 4;
		} else if (tag == FDT_PROP) {
			u32 plen = be32(f->base + f->off_struct + off + 4);
			u32 nameoff = be32(f->base + f->off_struct + off + 8);

			if (off + 12 + plen > f->size_struct)
				return NULL;
			if (!strcmp(str_at(f, nameoff), name)) {
				if (len)
					*len = plen;
				return f->base + f->off_struct + off + 12;
			}
			off = (u32)ALIGN_UP(off + 12 + plen, 4);
		} else {
			return NULL;	/* properties end at the first subnode/end */
		}
	}
}

int fdt_walk(const struct fdt *f, fdt_visit_fn visit, void *ctx)
{
	u32 ac[FDT_MAXDEPTH], sc[FDT_MAXDEPTH];
	int depth = -1;
	u32 off = 0;

	for (;;) {
		u32 tag = tag_at(f, off);

		switch (tag) {
		case FDT_BEGIN_NODE: {
			u32 v;
			int node = (int)off;

			if (++depth >= FDT_MAXDEPTH)
				return -1;
			ac[depth] = 2;
			sc[depth] = 1;
			if (!fdt_read_u32(f, node, "#address-cells", &v))
				ac[depth] = v;
			if (!fdt_read_u32(f, node, "#size-cells", &v))
				sc[depth] = v;
			if (visit(f, node, depth, depth ? ac[depth - 1] : 2,
				  depth ? sc[depth - 1] : 1, ctx))
				return 1;
			off = skip_name(f, off);
			break;
		}
		case FDT_END_NODE:
			if (--depth < -1)
				return -1;
			off += 4;
			break;
		case FDT_PROP:
			off = (u32)ALIGN_UP(off + 12 + be32(f->base + f->off_struct + off + 4), 4);
			break;
		case FDT_NOP:
			off += 4;
			break;
		case FDT_END:
			return 0;
		default:
			return -1;
		}
		if (off >= f->size_struct)
			return -1;
	}
}

int fdt_stringlist_index(const struct fdt *f, int node, const char *prop, const char *str)
{
	u32 len, i = 0;
	const u8 *p = fdt_getprop(f, node, prop, &len);
	int idx = 0;

	if (!p)
		return -1;
	while (i < len) {
		const char *s = (const char *)p + i;
		u32 l = 0;

		while (i + l < len && s[l])
			l++;
		if (l == strlen(str) && !memcmp(s, str, l))
			return idx;
		i += l + 1;
		idx++;
	}
	return -1;
}

int fdt_prop_has_string(const struct fdt *f, int node, const char *prop, const char *str)
{
	return fdt_stringlist_index(f, node, prop, str) >= 0;
}

int fdt_read_u32(const struct fdt *f, int node, const char *name, u32 *out)
{
	u32 len;
	const u8 *p = fdt_getprop(f, node, name, &len);

	/* like of_property_read_u32(): first cell, any length >= 4 */
	if (!p || len < 4)
		return -1;
	*out = be32(p);
	return 0;
}

int fdt_reg(const struct fdt *f, int node, const char *prop, u32 ac, u32 sc,
	    int idx, u64 *addr, u64 *size)
{
	u32 len, stride = 4 * (ac + sc);
	const u8 *p = fdt_getprop(f, node, prop, &len);

	if (!p || !stride || ac > 2 || sc > 2 || (u32)(idx + 1) * stride > len)
		return -1;
	p += (u32)idx * stride;
	*addr = fdt_cells(p, ac);
	*size = sc ? fdt_cells(p + 4 * ac, sc) : 0;
	return 0;
}

struct find_ctx {
	const char *compat;
	u32 phandle;
	int start_after;
	int found;
	u32 pac, psc;
};

static int visit_compat(const struct fdt *f, int node, int depth, u32 pac, u32 psc, void *c)
{
	struct find_ctx *x = c;

	(void)depth;
	if (node <= x->start_after)
		return 0;
	if (fdt_prop_has_string(f, node, "compatible", x->compat)) {
		x->found = node;
		x->pac = pac;
		x->psc = psc;
		return 1;
	}
	return 0;
}

int fdt_find_compatible(const struct fdt *f, const char *compat, int start_after,
			u32 *pac, u32 *psc)
{
	struct find_ctx x = { .compat = compat, .start_after = start_after, .found = -1 };

	fdt_walk(f, visit_compat, &x);
	if (x.found >= 0) {
		if (pac)
			*pac = x.pac;
		if (psc)
			*psc = x.psc;
	}
	return x.found;
}

static int visit_phandle(const struct fdt *f, int node, int depth, u32 pac, u32 psc, void *c)
{
	struct find_ctx *x = c;
	u32 v;

	(void)depth;
	if ((!fdt_read_u32(f, node, "phandle", &v) && v == x->phandle) ||
	    (!fdt_read_u32(f, node, "linux,phandle", &v) && v == x->phandle)) {
		x->found = node;
		x->pac = pac;
		x->psc = psc;
		return 1;
	}
	return 0;
}

int fdt_find_phandle(const struct fdt *f, u32 phandle, u32 *pac, u32 *psc)
{
	struct find_ctx x = { .phandle = phandle, .found = -1 };

	fdt_walk(f, visit_phandle, &x);
	if (x.found >= 0) {
		if (pac)
			*pac = x.pac;
		if (psc)
			*psc = x.psc;
	}
	return x.found;
}
