/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Common container unwrap, LZSS inflate, Adler-32 (NEON-accelerated) and file
 * I/O helpers shared by sgxcarve, vxdcarve and iopcarve.
 */
#ifndef S5L8947X_CARVE_COMMON_H
#define S5L8947X_CARVE_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

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

static inline uint32_t rd32le(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t rd32be(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

static inline void wr32le(uint8_t *p, uint32_t v)
{
	p[0] = v;
	p[1] = v >> 8;
	p[2] = v >> 16;
	p[3] = v >> 24;
}

static inline uint32_t adler32(const uint8_t *p, size_t n)
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
static inline size_t lzss_inflate(uint8_t *dst, size_t dlen, const uint8_t *src, size_t slen)
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

static inline uint8_t *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL;
	long n;

	*len = 0;
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
static inline const uint8_t *unwrap(const uint8_t *buf, size_t len, size_t *plen)
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
static inline uint8_t *inflate(const uint8_t *p, size_t len, size_t *olen)
{
	uint32_t sum, ulen, clen;
	uint8_t *out;

	*olen = 0;
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

#endif /* S5L8947X_CARVE_COMMON_H */
