// SPDX-License-Identifier: GPL-2.0-only
/*
 * Carve the SGX543 microkernel out of the on-device system image (/bin/sgxcarve)
 *
 * Usage:
 *   sgxcarve <image> <output>
 *
 * The GPU microkernel and its start-up data are not separate files on the
 * System volume: they are arrays inside the GPU driver module that is
 * prelinked into the stock kernel image. This tool
 *
 *   1. unwraps the image container (tagged "DATA" payload) if there is one,
 *   2. inflates the LZSS payload (4 KiB window, checked against its Adler-32),
 *   3. walks the embedded 32-bit ARM Mach-O modules and picks the GPU module
 *      by its LC_UUID, so only a module build whose layout is known is used,
 *   4. copies the known regions (by virtual address) into <output>.
 *
 * Output format (all little endian), read by drivers/soc/apple/s5l8947x-sgx.c:
 *   u32 magic 'SGXF', u32 version (1), u32 count, u32 header bytes
 *   u8  uuid[16]                         module build the data came from
 *   count x { u32 id, u32 offset, u32 size, u32 reserved }
 *   region data (each at its offset, 64-byte aligned)
 *
 * Exits 0 on success, 1 when no known GPU module is found and 2 on error.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define SGXF_MAGIC	0x46584753u	/* "SGXF" */
#define SGXF_VERSION	1
#define SGXF_ALIGN	64

/* Container and compression tags, as the 32-bit words they appear as */
#define IMG_MAGIC_LE	0x496d6733u	/* container header, little endian */
#define TAG_DATA_LE	0x44415441u	/* "DATA" tag, little endian */
#define COMP_BE		0x636f6d70u	/* compression header word 0, big endian */
#define LZSS_BE		0x6c7a7373u	/* compression header word 1, big endian */
#define COMP_HDR_SIZE	0x180

#define MH_MAGIC_32	0xfeedfaceu
#define CPU_TYPE_ARM	12
#define LC_SEGMENT	0x1
#define LC_UUID		0x1b

struct region {
	uint32_t id;
	uint32_t va;
	uint32_t size;
};

struct layout {
	uint8_t uuid[16];
	const char *build;
	const struct region *regions;
	unsigned int count;
};

/*
 * Region ids are shared with the kernel driver:
 *   1 boot     start-up program, placed at offset 0 of the code buffer
 *   2 ukernel  microkernel program, placed at offset 0x1000 of the code buffer
 *   3..        start-up data for the microkernel's own structures, in the order
 *              the module allocates them
 */
static const struct region regions_12h1006[] = {
	{ 1, 0x80bc7560, 0x00248 },
	{ 2, 0x80bb1a80, 0x11f88 },
	{ 3, 0x80baf950, 0x00018 },
	{ 4, 0x80baf968, 0x00038 },
	{ 5, 0x80baf9a0, 0x00018 },
	{ 6, 0x80baf9b8, 0x00350 },
	{ 7, 0x80bafd08, 0x0009c },
	{ 8, 0x80bc3a08, 0x03b58 },
	{ 9, 0x80bafda4, 0x00258 },
	{ 10, 0x80bc78c8, 0x00140 },
	{ 11, 0x80bc77d8, 0x000f0 },
	{ 12, 0x80bc77b0, 0x00028 },
};

static const struct layout layouts[] = {
	{
		.uuid = { 0x0b, 0x34, 0x38, 0x5d, 0x59, 0xf2, 0x3d, 0x54,
			  0x90, 0xd2, 0x5c, 0x2d, 0x9a, 0xaf, 0xba, 0xe5 },
		.build = "12H1006",
		.regions = regions_12h1006,
		.count = sizeof(regions_12h1006) / sizeof(regions_12h1006[0]),
	},
};

static uint32_t rd32le(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t rd32be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static void wr32le(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static uint32_t adler32(const uint8_t *p, size_t n)
{
	uint32_t a = 1, b = 0;
#ifdef __ARM_NEON
	static const uint16_t weights[16] = {
		16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1
	};
	uint16x8_t w_lo = vld1q_u16(weights);
	uint16x8_t w_hi = vld1q_u16(weights + 8);
#endif

	while (n) {
		size_t k = n < 5552 ? n : 5552;

		n -= k;
#ifdef __ARM_NEON
		if (k >= 16) {
			uint32x4_t vs1 = vdupq_n_u32(0);
			uint32x4_t vs2 = vdupq_n_u32(0);
			uint32x4_t vcol = vdupq_n_u32(0);
			size_t blocks = k >> 4;

			k &= 15;
			b += a * (uint32_t)(blocks << 4);
			while (blocks--) {
				uint8x16_t v = vld1q_u8(p);
				uint16x8_t lo = vmovl_u8(vget_low_u8(v));
				uint16x8_t hi = vmovl_u8(vget_high_u8(v));

				p += 16;
				vs2 = vaddq_u32(vs2, vs1);
				vs1 = vpadalq_u16(vs1, vpaddlq_u8(v));
				vcol = vmlal_u16(vcol, vget_low_u16(lo), vget_low_u16(w_lo));
				vcol = vmlal_u16(vcol, vget_high_u16(lo), vget_high_u16(w_lo));
				vcol = vmlal_u16(vcol, vget_low_u16(hi), vget_low_u16(w_hi));
				vcol = vmlal_u16(vcol, vget_high_u16(hi), vget_high_u16(w_hi));
			}
			uint32x2_t s1_2 = vadd_u32(vget_low_u32(vs1), vget_high_u32(vs1));
			s1_2 = vpadd_u32(s1_2, s1_2);
			a += vget_lane_u32(s1_2, 0);

			vs2 = vshlq_n_u32(vs2, 4);
			uint32x2_t c_2 = vadd_u32(vget_low_u32(vcol), vget_high_u32(vcol));
			uint32x2_t s2_2 = vadd_u32(vget_low_u32(vs2), vget_high_u32(vs2));
			s2_2 = vadd_u32(s2_2, c_2);
			s2_2 = vpadd_u32(s2_2, s2_2);
			b += vget_lane_u32(s2_2, 0);
		}
#endif
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return b << 16 | a;
}

/* LZSS: 4 KiB ring initialised to spaces, 18-byte matches, flag byte LSB first */
static size_t lzss_inflate(uint8_t *dst, size_t dlen, const uint8_t *src, size_t slen)
{
	uint8_t ring[4096 + 17];
	unsigned int r = 4096 - 18, flags = 0;
	size_t si = 0, di = 0;

	memset(ring, ' ', 4096 - 18);
	for (;;) {
		flags >>= 1;
		if (!(flags & 0x100)) {
			if (si >= slen)
				break;
			flags = src[si++] | 0xff00;
		}
		if (flags & 1) {
			uint8_t c;

			if (si >= slen)
				break;
			c = src[si++];
			if (di < dlen)
				dst[di++] = c;
			ring[r++] = c;
			r &= 4095;
		} else {
			unsigned int i, j, k;

			if (si + 2 > slen)
				break;
			i = src[si++];
			j = src[si++];
			i |= (j & 0xf0) << 4;
			j = (j & 0x0f) + 2;
			for (k = 0; k <= j; k++) {
				uint8_t c = ring[(i + k) & 4095];

				if (di < dlen)
					dst[di++] = c;
				ring[r++] = c;
				r &= 4095;
			}
		}
	}
	return di;
}

static uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL;
	long n;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc(n);
		if (buf && fread(buf, 1, n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*len = n;
	}
	fclose(f);
	return buf;
}

/* Return the payload of the "DATA" tag of a tagged container, or the input itself */
static const uint8_t *unwrap(const uint8_t *buf, size_t len, size_t *plen)
{
	size_t o;

	*plen = len;
	if (len < 20 || rd32le(buf) != IMG_MAGIC_LE)
		return buf;
	for (o = 20; o + 12 <= len;) {
		uint32_t tag = rd32le(buf + o), total = rd32le(buf + o + 4);
		uint32_t dlen = rd32le(buf + o + 8);

		if (total < 12 || o + total > len)
			break;
		if (tag == TAG_DATA_LE && dlen <= total - 12) {
			*plen = dlen;
			return buf + o + 12;
		}
		o += total;
	}
	return NULL;
}

/* Inflate an LZSS payload if it has the compression header; returns malloc'd data */
static uint8_t *inflate(const uint8_t *p, size_t len, size_t *olen)
{
	uint32_t sum, ulen, clen;
	uint8_t *out;

	if (len < COMP_HDR_SIZE || rd32be(p) != COMP_BE || rd32be(p + 4) != LZSS_BE) {
		out = malloc(len);
		if (out)
			memcpy(out, p, len);
		*olen = len;
		return out;
	}
	sum = rd32be(p + 8);
	ulen = rd32be(p + 12);
	clen = rd32be(p + 16);
	if (clen > len - COMP_HDR_SIZE || ulen > 256u << 20) {
		fprintf(stderr, "bad compression header\n");
		return NULL;
	}
	out = malloc(ulen);
	if (!out)
		return NULL;
	if (lzss_inflate(out, ulen, p + COMP_HDR_SIZE, clen) != ulen) {
		fprintf(stderr, "short LZSS stream\n");
		free(out);
		return NULL;
	}
	if (adler32(out, ulen) != sum) {
		fprintf(stderr, "Adler-32 mismatch\n");
		free(out);
		return NULL;
	}
	*olen = ulen;
	return out;
}

struct module {
	const uint8_t *base;	/* image the segment file offsets refer to */
	size_t len;
	const uint8_t *hdr;
	uint32_t ncmds, sizeofcmds;
};

static const uint8_t *module_uuid(const struct module *m)
{
	const uint8_t *lc = m->hdr + 28, *end = lc + m->sizeofcmds;
	uint32_t i;

	for (i = 0; i < m->ncmds && lc + 8 <= end; i++) {
		uint32_t cmd = rd32le(lc), sz = rd32le(lc + 4);

		if (sz < 8 || lc + sz > end)
			return NULL;
		if (cmd == LC_UUID && sz >= 24)
			return lc + 8;
		lc += sz;
	}
	return NULL;
}

/*
 * Map a virtual address range to bytes in the image. The module's own segments
 * may give file offsets relative to the module or to the image, so use the
 * segments of the image itself (which cover every prelinked module).
 */
static const uint8_t *module_ptr(const struct module *m, uint32_t va, uint32_t size)
{
	const uint8_t *lc = m->hdr + 28, *end = lc + m->sizeofcmds;
	uint32_t i;

	for (i = 0; i < m->ncmds && lc + 8 <= end; i++) {
		uint32_t cmd = rd32le(lc), sz = rd32le(lc + 4);

		if (sz < 8 || lc + sz > end)
			return NULL;
		if (cmd == LC_SEGMENT && sz >= 56) {
			uint32_t vmaddr = rd32le(lc + 24), fileoff = rd32le(lc + 32);
			uint32_t filesize = rd32le(lc + 36);

			if (va >= vmaddr && va - vmaddr + (uint64_t)size <= filesize &&
			    (uint64_t)fileoff + filesize <= m->len)
				return m->base + fileoff + (va - vmaddr);
		}
		lc += sz;
	}
	return NULL;
}

static const struct layout *find_module(const uint8_t *img, size_t len, struct module *m)
{
	size_t o;
	unsigned int i;

	/* Prelinked modules start on 4 KiB boundaries; the image header is one too */
	for (o = 0; o + 28 <= len; o += 4096) {
		const uint8_t *uuid;

		if (rd32le(img + o) != MH_MAGIC_32 || rd32le(img + o + 4) != CPU_TYPE_ARM)
			continue;
		m->base = img;
		m->len = len;
		m->hdr = img + o;
		m->ncmds = rd32le(img + o + 16);
		m->sizeofcmds = rd32le(img + o + 20);
		if (o + 28 + (uint64_t)m->sizeofcmds > len)
			continue;
		uuid = module_uuid(m);
		if (!uuid)
			continue;
		for (i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++)
			if (!memcmp(uuid, layouts[i].uuid, 16))
				return &layouts[i];
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const struct layout *lay;
	const uint8_t *payload;
	uint8_t *raw, *img, *out;
	size_t rlen, plen, ilen, olen, off;
	struct module m, top;
	unsigned int i;
	FILE *f;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <image> <output>\n", argv[0]);
		return 2;
	}
	raw = read_file(argv[1], &rlen);
	if (!raw) {
		perror(argv[1]);
		return 2;
	}
	payload = unwrap(raw, rlen, &plen);
	if (!payload) {
		fprintf(stderr, "no data payload in container\n");
		return 2;
	}
	img = inflate(payload, plen, &ilen);
	free(raw);
	if (!img)
		return 2;

	lay = find_module(img, ilen, &m);
	if (!lay) {
		fprintf(stderr, "no GPU module with a known layout\n");
		free(img);
		return 1;
	}

	if (ilen < 28 || rd32le(img) != MH_MAGIC_32) {
		fprintf(stderr, "image is not a 32-bit Mach-O\n");
		free(img);
		return 2;
	}
	top.base = img;
	top.len = ilen;
	top.hdr = img;
	top.ncmds = rd32le(img + 16);
	top.sizeofcmds = rd32le(img + 20);
	if (28 + (uint64_t)top.sizeofcmds > ilen) {
		free(img);
		return 2;
	}

	olen = 16 + 16 + 16 * lay->count;
	for (i = 0; i < lay->count; i++)
		olen = (olen + SGXF_ALIGN - 1) / SGXF_ALIGN * SGXF_ALIGN + lay->regions[i].size;
	out = calloc(1, olen);
	if (!out) {
		free(img);
		return 2;
	}
	wr32le(out, SGXF_MAGIC);
	wr32le(out + 4, SGXF_VERSION);
	wr32le(out + 8, lay->count);
	wr32le(out + 12, 32 + 16 * lay->count);
	memcpy(out + 16, module_uuid(&m), 16);
	off = 32 + 16 * lay->count;
	for (i = 0; i < lay->count; i++) {
		const struct region *r = &lay->regions[i];
		const uint8_t *src = module_ptr(&top, r->va, r->size);
		uint8_t *e = out + 32 + 16 * i;

		if (!src) {
			fprintf(stderr, "region %u (0x%08x+0x%x) outside the module\n",
				r->id, r->va, r->size);
			free(out);
			free(img);
			return 2;
		}
		off = (off + SGXF_ALIGN - 1) / SGXF_ALIGN * SGXF_ALIGN;
		wr32le(e, r->id);
		wr32le(e + 4, off);
		wr32le(e + 8, r->size);
		memcpy(out + off, src, r->size);
		off += r->size;
	}
	free(img);

	f = fopen(argv[2], "wb");
	if (!f || fwrite(out, 1, olen, f) != olen || fclose(f)) {
		perror(argv[2]);
		free(out);
		return 2;
	}
	printf("build %s, %u regions, %zu bytes\n", lay->build, lay->count, olen);
	free(out);
	return 0;
}
