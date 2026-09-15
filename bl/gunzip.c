// SPDX-License-Identifier: GPL-2.0
//
// Minimal gzip/DEFLATE decompressor for the NX809J EFI bootloader.
//
// The EFI environment GBL starts us in exposes neither block devices nor file
// systems (marker block 255 of recovery_a stayed empty and the bootloader fell
// back to its menu), so the kernel, devicetree and initramfs are embedded in
// the bootloader image itself and the kernel is inflated here.
//
// Plain DEFLATE: stored/fixed/dynamic blocks, canonical Huffman decoding, no
// checksum verification (the payload is covered by the sha256 we compare on
// the host side before flashing).
//
// Compile standalone with -DTEST_MAIN to check it against a real kernel image:
//     gcc -O2 -DTEST_MAIN bl/gunzip.c -o gunzip-test && ./gunzip-test Image.gz Image

#ifndef NX809J_TYPES
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long usize;
#define NX809J_TYPES
#endif

struct gz {
	const u8 *in;
	usize in_len;
	usize in_pos;
	u32 bitbuf;
	int bitcnt;
	u8 *out;
	usize out_len;
	usize out_cap;
	int err;
};

static void gz_bits(struct gz *g, int need)
{
	while (g->bitcnt < need) {
		if (g->in_pos >= g->in_len) {
			g->err = 1;
			return;
		}
		g->bitbuf |= (u32)g->in[g->in_pos++] << g->bitcnt;
		g->bitcnt += 8;
	}
}

static u32 gz_get(struct gz *g, int n)
{
	u32 v;

	gz_bits(g, n);
	v = g->bitbuf & ((1u << n) - 1);
	g->bitbuf >>= n;
	g->bitcnt -= n;
	return v;
}

// Huffman decoding is done the simple way: a canonical code table plus a
// linear scan over the code lengths, which is plenty for a one-shot boot path.
struct huff {
	u16 count[16];
	u16 sym[288];
};

static void huff_build(struct huff *h, const u8 *lens, int n)
{
	int i, left;
	u16 offs[16];

	for (i = 0; i < 16; i++)
		h->count[i] = 0;
	for (i = 0; i < n; i++)
		h->count[lens[i]]++;
	h->count[0] = 0;

	left = 1;
	for (i = 1; i < 16; i++) {
		left <<= 1;
		left -= h->count[i];
		if (left < 0) {
			h->count[1] = 0;	/* over-subscribed: mark invalid */
			return;
		}
	}
	offs[1] = 0;
	for (i = 1; i < 15; i++)
		offs[i + 1] = offs[i] + h->count[i];
	for (i = 0; i < n; i++)
		if (lens[i])
			h->sym[offs[lens[i]]++] = (u16)i;
}

static int huff_decode(struct gz *g, struct huff *h)
{
	int len, code = 0, first = 0, index = 0;

	for (len = 1; len <= 15; len++) {
		code |= (int)gz_get(g, 1);
		if (g->err)
			return -1;
		if (code - first < h->count[len])
			return h->sym[index + (code - first)];
		index += h->count[len];
		first = (first + h->count[len]) << 1;
		code <<= 1;
	}
	g->err = 1;
	return -1;
}

static void gz_put(struct gz *g, u8 b)
{
	if (g->out_len < g->out_cap)
		g->out[g->out_len] = b;
	else
		g->err = 1;
	g->out_len++;
}

static int inflate_block(struct gz *g, struct huff *lit, struct huff *dist)
{
	static const u16 len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17,
		19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2,
		2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const u16 dst_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49,
		65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
		6145, 8193, 12289, 16385, 24577 };
	static const u8 dst_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5,
		6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

	for (;;) {
		int sym = huff_decode(g, lit);
		if (sym < 0)
			return -1;
		if (sym < 256) {
			gz_put(g, (u8)sym);
		} else if (sym == 256) {
			return 0;
		} else {
			int len, back, i;
			sym -= 257;
			if (sym >= 29)
				return -1;
			len = len_base[sym] + (int)gz_get(g, len_extra[sym]);
			sym = huff_decode(g, dist);
			if (sym < 0 || sym >= 30)
				return -1;
			back = dst_base[sym] + (int)gz_get(g, dst_extra[sym]);
			if ((usize)back > g->out_len)
				return -1;
			for (i = 0; i < len; i++)
				gz_put(g, g->out[g->out_len - back]);
		}
		if (g->err)
			return -1;
	}
}

static int inflate_dynamic(struct gz *g, struct huff *lit, struct huff *dist)
{
	static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12,
		3, 13, 2, 14, 1, 15 };
	u8 lens[320];
	struct huff cl;
	int hlit, hdist, hclen, i, n = 0;

	hlit = (int)gz_get(g, 5) + 257;
	hdist = (int)gz_get(g, 5) + 1;
	hclen = (int)gz_get(g, 4) + 4;
	if (g->err || hlit > 286 || hdist > 30)
		return -1;

	for (i = 0; i < 19; i++)
		lens[i] = 0;
	for (i = 0; i < hclen; i++)
		lens[order[i]] = (u8)gz_get(g, 3);
	huff_build(&cl, lens, 19);
	if (g->err)
		return -1;

	while (n < hlit + hdist) {
		int sym = huff_decode(g, &cl);
		if (sym < 0)
			return -1;
		if (sym < 16) {
			lens[n++] = (u8)sym;
		} else {
			int rep, val = 0;
			if (sym == 16) {
				if (n == 0)
					return -1;
				val = lens[n - 1];
				rep = 3 + (int)gz_get(g, 2);
			} else if (sym == 17) {
				rep = 3 + (int)gz_get(g, 3);
			} else {
				rep = 11 + (int)gz_get(g, 7);
			}
			while (rep-- && n < hlit + hdist)
				lens[n++] = (u8)val;
		}
	}
	if (n != hlit + hdist)
		return -1;

	huff_build(lit, lens, hlit);
	huff_build(dist, lens + hlit, hdist);
	if (g->err || lit->count[1] == 0 && 0)
		return -1;
	return 0;
}

static void huff_fixed(struct huff *lit, struct huff *dist)
{
	u8 lens[288];
	int i;

	for (i = 0; i < 144; i++)
		lens[i] = 8;
	for (i = 144; i < 256; i++)
		lens[i] = 9;
	for (i = 256; i < 280; i++)
		lens[i] = 7;
	for (i = 280; i < 288; i++)
		lens[i] = 8;
	huff_build(lit, lens, 288);

	for (i = 0; i < 30; i++)
		lens[i] = 5;
	huff_build(dist, lens, 30);
}

// Inflate a gzip stream. Returns 0 on success and sets *out_len.
static int gunzip(const u8 *in, usize in_len, u8 *out, usize out_cap, usize *out_len)
{
	struct gz g;
	struct huff lit, dist;
	int last;

	g.in = in;
	g.in_len = in_len;
	g.in_pos = 0;
	g.bitbuf = 0;
	g.bitcnt = 0;
	g.out = out;
	g.out_len = 0;
	g.out_cap = out_cap;
	g.err = 0;

	if (in_len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
		return -1;
	{
		u8 flg = in[3];
		usize p = 10;
		if (flg & 4) {			/* FEXTRA */
			usize xl;
			if (p + 2 > in_len)
				return -1;
			xl = (usize)in[p] | ((usize)in[p + 1] << 8);
			p += 2 + xl;
		}
		if (flg & 8)			/* FNAME */
			while (p < in_len && in[p++])
				;
		if (flg & 16)			/* FCOMMENT */
			while (p < in_len && in[p++])
				;
		if (flg & 2)			/* FHCRC */
			p += 2;
		if (p >= in_len)
			return -1;
		g.in_pos = p;
	}

	do {
		last = (int)gz_get(&g, 1);
		{
			int type = (int)gz_get(&g, 2);
			if (g.err)
				return -1;
			if (type == 0) {			/* stored */
				u32 len, nlen;
				g.bitbuf = 0;
				g.bitcnt = 0;
				if (g.in_pos + 4 > g.in_len)
					return -1;
				len = (u32)g.in[g.in_pos] | ((u32)g.in[g.in_pos + 1] << 8);
				nlen = (u32)g.in[g.in_pos + 2] | ((u32)g.in[g.in_pos + 3] << 8);
				g.in_pos += 4;
				if ((len ^ 0xffff) != nlen || g.in_pos + len > g.in_len)
					return -1;
				while (len--)
					gz_put(&g, g.in[g.in_pos++]);
			} else if (type == 1) {			/* fixed */
				huff_fixed(&lit, &dist);
				if (inflate_block(&g, &lit, &dist))
					return -1;
			} else if (type == 2) {			/* dynamic */
				if (inflate_dynamic(&g, &lit, &dist))
					return -1;
				if (inflate_block(&g, &lit, &dist))
					return -1;
			} else {
				return -1;
			}
		}
		if (g.err)
			return -1;
	} while (!last);

	*out_len = g.out_len;
	return g.err ? -1 : 0;
}

#ifdef TEST_MAIN
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	usize n, outn = 0;
	u8 *in, *out;
	int r;

	fseek(f, 0, SEEK_END);
	n = (usize)ftell(f);
	fseek(f, 0, SEEK_SET);
	in = malloc(n);
	fread(in, 1, n, f);
	fclose(f);

	out = malloc(80u * 1024 * 1024);
	r = gunzip(in, n, out, 80u * 1024 * 1024, &outn);
	printf("gunzip: r=%d out_len=%lu\n", r, (unsigned long)outn);
	if (r)
		return 1;
	if (argc > 2) {
		FILE *g = fopen(argv[2], "rb");
		usize m;
		u8 *ref, *p;
		fseek(g, 0, SEEK_END);
		m = (usize)ftell(g);
		fseek(g, 0, SEEK_SET);
		ref = malloc(m);
		fread(ref, 1, m, g);
		fclose(g);
		if (m != outn) {
			printf("size mismatch: %lu vs %lu\n", (unsigned long)outn, (unsigned long)m);
			return 1;
		}
		for (p = ref; p < ref + m; p++)
			if (*p != out[p - ref]) {
				printf("first difference at %lu\n", (unsigned long)(p - ref));
				return 1;
			}
		printf("identical to %s (%lu bytes)\n", argv[2], (unsigned long)m);
	}
	return 0;
}
#endif
