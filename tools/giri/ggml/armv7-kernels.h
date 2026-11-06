// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARMv7-A NEON kernels for ggml's Q4_0/Q8_0 paths, tuned for the Cortex-A9
 * (VFPv3 + NEON, 64-bit NEON datapath, no SDOT, no fp16 conversion).
 *
 * Why the generic ggml NEON code is slow on this core:
 *  - each block's fp16 scales are multiplied in VFP and then moved through an
 *    ARM core register into a NEON lane; a VFP/NEON -> ARM transfer stalls
 *    the A9 until the NEON queue drains;
 *  - every 8 int8 products are widened to int32 separately
 *    (vmull + vpaddl + vadd), and ARMv7 has no vcvtnq/vaddvq, so the
 *    fallbacks call roundf() and use lane extracts.
 *
 * Here:
 *  - products are accumulated in int16 with vmlal.s8 and widened once per
 *    block (bounds below);
 *  - 4 blocks per iteration; their scales are loaded from the fp16 lookup
 *    table straight into NEON lanes (vld1.32 {d[n]}), multiplied as a vector,
 *    and the 4 block sums are reduced with vpadd, so nothing leaves NEON
 *    inside the loop;
 *  - the final horizontal sum is stored from NEON to memory.
 *
 * int16 bounds (operands in [-127,127] as produced by ggml's q8_0 quantiser,
 * Q4_0 nibbles in [-8,7]):
 *  - Q4_0 x Q8_0: 4 products per lane, |sum| <= 4 * 8 * 128 = 4096;
 *  - Q8_0 x Q8_0: 2 products per lane, |sum| <= 2 * 127 * 127 = 32258.
 *
 * Expects block_q4_0, block_q8_0, ggml_table_f32_f16, QK8_0 and the macro
 * ARMV7_FP32_TO_FP16(f) from the includer (quants.c, or the standalone test).
 */
#ifndef GIRI_ARMV7_KERNELS_H
#define GIRI_ARMV7_KERNELS_H

#include <arm_neon.h>
#include <stdint.h>

#if defined(__ARM_NEON) && !defined(__aarch64__) && !defined(GGML_ARMV7_DISABLE)
#define GGML_ARMV7_KERNELS 1

/*
 * Weight rows are streamed from DRAM once per token and the L2 prefetcher is
 * off on this SoC (see s5l8947x.dtsi), so the kernels issue one pld per
 * 32-byte line this many bytes ahead. pld never faults, so running past the
 * end of a row is harmless (it touches the next row, read next anyway).
 */
#ifndef ARMV7_PF_DIST
#define ARMV7_PF_DIST 256
#endif

/* fp16 scale -> fp32, loaded from the lookup table directly into a lane. */
#define ARMV7_LD_SCALE(v, h, lane) \
	vld1q_lane_f32(&ggml_table_f32_f16[(h)], (v), (lane))

/* {sum(p0), sum(p1), sum(p2), sum(p3)} without leaving NEON. */
static inline int32x4_t armv7_hsum4_s32(int32x4_t p0, int32x4_t p1,
					int32x4_t p2, int32x4_t p3)
{
	int32x2_t r0 = vadd_s32(vget_low_s32(p0), vget_high_s32(p0));
	int32x2_t r1 = vadd_s32(vget_low_s32(p1), vget_high_s32(p1));
	int32x2_t r2 = vadd_s32(vget_low_s32(p2), vget_high_s32(p2));
	int32x2_t r3 = vadd_s32(vget_low_s32(p3), vget_high_s32(p3));

	return vcombine_s32(vpadd_s32(r0, r1), vpadd_s32(r2, r3));
}

static inline void armv7_store_hsum_f32(float *s, float32x4_t acc)
{
	float32x2_t t = vadd_f32(vget_low_f32(acc), vget_high_f32(acc));

	t = vpadd_f32(t, t);
	vst1_lane_f32(s, t, 0);
}

/* One Q4_0 x Q8_0 block -> 4 partial int32 sums. */
static inline int32x4_t armv7_dot_q4_0_q8_0_block(const block_q4_0 *x,
						  const block_q8_0 *y)
{
	const uint8x16_t m4 = vdupq_n_u8(0x0f);
	const int8x16_t s8 = vdupq_n_s8(8);
	const uint8x16_t q = vld1q_u8(x->qs);
	const int8x16_t lo = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(q, m4)), s8);
	const int8x16_t hi = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(q, 4)), s8);
	const int8x16_t y0 = vld1q_s8(y->qs);
	const int8x16_t y1 = vld1q_s8(y->qs + 16);
	int16x8_t a;

	a = vmull_s8(vget_low_s8(lo), vget_low_s8(y0));
	a = vmlal_s8(a, vget_high_s8(lo), vget_high_s8(y0));
	a = vmlal_s8(a, vget_low_s8(hi), vget_low_s8(y1));
	a = vmlal_s8(a, vget_high_s8(hi), vget_high_s8(y1));
	return vpaddlq_s16(a);
}

/* One Q8_0 x Q8_0 block -> 4 partial int32 sums. */
static inline int32x4_t armv7_dot_q8_0_q8_0_block(const block_q8_0 *x,
						  const block_q8_0 *y)
{
	const int8x16_t x0 = vld1q_s8(x->qs);
	const int8x16_t x1 = vld1q_s8(x->qs + 16);
	const int8x16_t y0 = vld1q_s8(y->qs);
	const int8x16_t y1 = vld1q_s8(y->qs + 16);
	int16x8_t a, b;

	a = vmull_s8(vget_low_s8(x0), vget_low_s8(y0));
	a = vmlal_s8(a, vget_high_s8(x0), vget_high_s8(y0));
	b = vmull_s8(vget_low_s8(x1), vget_low_s8(y1));
	b = vmlal_s8(b, vget_high_s8(x1), vget_high_s8(y1));
	return vpadalq_s16(vpaddlq_s16(a), b);
}

/*
 * Generic 4-blocks-per-iteration driver. BLOCK_FN computes one block's
 * partial sums; the tail (nb % 4) runs the same path with zeroed lanes.
 */
#define ARMV7_VEC_DOT_BODY(TX, BLOCK_FN)                                      \
	const TX *x = (const TX *)vx;                                         \
	const block_q8_0 *y = (const block_q8_0 *)vy;                        \
	const int nb = n / QK8_0;                                            \
	float32x4_t acc0 = vdupq_n_f32(0.0f);                                \
	float32x4_t acc1 = vdupq_n_f32(0.0f);                                \
	int ib = 0;                                                          \
	for (; ib + 8 <= nb; ib += 8) {                                      \
		float32x4_t dx0 = vdupq_n_f32(0.0f), dy0 = dx0;              \
		float32x4_t dx1 = dx0, dy1 = dx0;                            \
		int32x4_t s0, s1;                                            \
		{                                                            \
			const char *pf = (const char *)&x[ib] + ARMV7_PF_DIST;  \
			int o;                                               \
			for (o = 0; o < 8 * (int)sizeof(TX); o += 32)        \
				__builtin_prefetch(pf + o);                  \
		}                                                            \
		dx0 = ARMV7_LD_SCALE(dx0, x[ib + 0].d, 0);                   \
		dx0 = ARMV7_LD_SCALE(dx0, x[ib + 1].d, 1);                   \
		dx0 = ARMV7_LD_SCALE(dx0, x[ib + 2].d, 2);                   \
		dx0 = ARMV7_LD_SCALE(dx0, x[ib + 3].d, 3);                   \
		dy0 = ARMV7_LD_SCALE(dy0, y[ib + 0].d, 0);                   \
		dy0 = ARMV7_LD_SCALE(dy0, y[ib + 1].d, 1);                   \
		dy0 = ARMV7_LD_SCALE(dy0, y[ib + 2].d, 2);                   \
		dy0 = ARMV7_LD_SCALE(dy0, y[ib + 3].d, 3);                   \
		s0 = armv7_hsum4_s32(BLOCK_FN(&x[ib + 0], &y[ib + 0]),       \
				     BLOCK_FN(&x[ib + 1], &y[ib + 1]),       \
				     BLOCK_FN(&x[ib + 2], &y[ib + 2]),       \
				     BLOCK_FN(&x[ib + 3], &y[ib + 3]));      \
		dx1 = ARMV7_LD_SCALE(dx1, x[ib + 4].d, 0);                   \
		dx1 = ARMV7_LD_SCALE(dx1, x[ib + 5].d, 1);                   \
		dx1 = ARMV7_LD_SCALE(dx1, x[ib + 6].d, 2);                   \
		dx1 = ARMV7_LD_SCALE(dx1, x[ib + 7].d, 3);                   \
		dy1 = ARMV7_LD_SCALE(dy1, y[ib + 4].d, 0);                   \
		dy1 = ARMV7_LD_SCALE(dy1, y[ib + 5].d, 1);                   \
		dy1 = ARMV7_LD_SCALE(dy1, y[ib + 6].d, 2);                   \
		dy1 = ARMV7_LD_SCALE(dy1, y[ib + 7].d, 3);                   \
		s1 = armv7_hsum4_s32(BLOCK_FN(&x[ib + 4], &y[ib + 4]),       \
				     BLOCK_FN(&x[ib + 5], &y[ib + 5]),       \
				     BLOCK_FN(&x[ib + 6], &y[ib + 6]),       \
				     BLOCK_FN(&x[ib + 7], &y[ib + 7]));      \
		acc0 = vmlaq_f32(acc0, vcvtq_f32_s32(s0), vmulq_f32(dx0, dy0)); \
		acc1 = vmlaq_f32(acc1, vcvtq_f32_s32(s1), vmulq_f32(dx1, dy1)); \
	}                                                                    \
	for (; ib < nb; ib += 4) {                                           \
		const int32x4_t z = vdupq_n_s32(0);                          \
		int32x4_t p[4] = { z, z, z, z };                             \
		float32x4_t dx = vdupq_n_f32(0.0f), dy = dx;                 \
		int k;                                                       \
		for (k = 0; k < 4 && ib + k < nb; k++) {                     \
			p[k] = BLOCK_FN(&x[ib + k], &y[ib + k]);             \
			switch (k) {                                         \
			case 0: dx = ARMV7_LD_SCALE(dx, x[ib].d, 0);         \
				dy = ARMV7_LD_SCALE(dy, y[ib].d, 0); break;  \
			case 1: dx = ARMV7_LD_SCALE(dx, x[ib + 1].d, 1);     \
				dy = ARMV7_LD_SCALE(dy, y[ib + 1].d, 1); break; \
			case 2: dx = ARMV7_LD_SCALE(dx, x[ib + 2].d, 2);     \
				dy = ARMV7_LD_SCALE(dy, y[ib + 2].d, 2); break; \
			default: dx = ARMV7_LD_SCALE(dx, x[ib + 3].d, 3);    \
				dy = ARMV7_LD_SCALE(dy, y[ib + 3].d, 3); break; \
			}                                                    \
		}                                                            \
		acc0 = vmlaq_f32(acc0,                                       \
			vcvtq_f32_s32(armv7_hsum4_s32(p[0], p[1], p[2], p[3])), \
			vmulq_f32(dx, dy));                                  \
	}                                                                    \
	armv7_store_hsum_f32(s, vaddq_f32(acc0, acc1))

static inline void ggml_armv7_vec_dot_q4_0_q8_0(int n, float *s,
						const void *vx, const void *vy)
{
	ARMV7_VEC_DOT_BODY(block_q4_0, armv7_dot_q4_0_q8_0_block);
}

static inline void ggml_armv7_vec_dot_q8_0_q8_0(int n, float *s,
						const void *vx, const void *vy)
{
	ARMV7_VEC_DOT_BODY(block_q8_0, armv7_dot_q8_0_q8_0_block);
}

/*
 * 2x2 tiles for prompt processing (ggml type_traits nrows = 2): rows x0,x1
 * (x1 = vx + bx) against activation rows y0,y1 (y1 = vy + by). Each weight
 * block is unpacked once and each activation block loaded once for two dot
 * products. The four results of a block are reduced into one vector
 * {x0.y0, x1.y0, x0.y1, x1.y1}, which is ggml's output order:
 * s[0], s[1], s[bs], s[bs + 1].
 */
static inline int16x8_t armv7_q4_dot16(int8x16_t lo, int8x16_t hi,
				       int8x16_t ya, int8x16_t yb)
{
	int16x8_t a = vmull_s8(vget_low_s8(lo), vget_low_s8(ya));

	a = vmlal_s8(a, vget_high_s8(lo), vget_high_s8(ya));
	a = vmlal_s8(a, vget_low_s8(hi), vget_low_s8(yb));
	return vmlal_s8(a, vget_high_s8(hi), vget_high_s8(yb));
}

static inline int32x4_t armv7_q8_dot32(int8x16_t xa, int8x16_t xb,
				       int8x16_t ya, int8x16_t yb)
{
	int16x8_t a = vmull_s8(vget_low_s8(xa), vget_low_s8(ya));
	int16x8_t b = vmull_s8(vget_low_s8(xb), vget_low_s8(yb));

	a = vmlal_s8(a, vget_high_s8(xa), vget_high_s8(ya));
	b = vmlal_s8(b, vget_high_s8(xb), vget_high_s8(yb));
	return vpadalq_s16(vpaddlq_s16(a), b);
}

/* {dx0*dy0, dx1*dy0, dx0*dy1, dx1*dy1} */
static inline float32x4_t armv7_scale_2x2(uint16_t hx0, uint16_t hx1,
					  uint16_t hy0, uint16_t hy1)
{
	float32x2_t dx = vdup_n_f32(0.0f);
	float32x2_t dy0 = vld1_dup_f32(&ggml_table_f32_f16[hy0]);
	float32x2_t dy1 = vld1_dup_f32(&ggml_table_f32_f16[hy1]);

	dx = vld1_lane_f32(&ggml_table_f32_f16[hx0], dx, 0);
	dx = vld1_lane_f32(&ggml_table_f32_f16[hx1], dx, 1);
	return vmulq_f32(vcombine_f32(dx, dx), vcombine_f32(dy0, dy1));
}

static inline void armv7_store_2x2(float *s, size_t bs, float32x4_t acc)
{
	vst1_f32(s, vget_low_f32(acc));
	vst1_f32(s + bs, vget_high_f32(acc));
}

static inline void ggml_armv7_vec_dot_q4_0_q8_0_2x2(int n, float *s, size_t bs,
		const void *vx, size_t bx, const void *vy, size_t by)
{
	const block_q4_0 *x0 = (const block_q4_0 *)vx;
	const block_q4_0 *x1 = (const block_q4_0 *)((const uint8_t *)vx + bx);
	const block_q8_0 *y0 = (const block_q8_0 *)vy;
	const block_q8_0 *y1 = (const block_q8_0 *)((const uint8_t *)vy + by);
	const uint8x16_t m4 = vdupq_n_u8(0x0f);
	const int8x16_t s8 = vdupq_n_s8(8);
	const int nb = n / QK8_0;
	float32x4_t acc = vdupq_n_f32(0.0f);
	int i;

	for (i = 0; i < nb; i++) {
		const uint8x16_t q0 = vld1q_u8(x0[i].qs);
		const uint8x16_t q1 = vld1q_u8(x1[i].qs);
		const int8x16_t l0 = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(q0, m4)), s8);
		const int8x16_t h0 = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(q0, 4)), s8);
		const int8x16_t l1 = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(q1, m4)), s8);
		const int8x16_t h1 = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(q1, 4)), s8);
		int8x16_t ya, yb;
		int32x4_t p00, p10, p01, p11;

		__builtin_prefetch((const char *)&x0[i] + ARMV7_PF_DIST);
		__builtin_prefetch((const char *)&x1[i] + ARMV7_PF_DIST);
		ya = vld1q_s8(y0[i].qs);
		yb = vld1q_s8(y0[i].qs + 16);
		p00 = vpaddlq_s16(armv7_q4_dot16(l0, h0, ya, yb));
		p10 = vpaddlq_s16(armv7_q4_dot16(l1, h1, ya, yb));
		ya = vld1q_s8(y1[i].qs);
		yb = vld1q_s8(y1[i].qs + 16);
		p01 = vpaddlq_s16(armv7_q4_dot16(l0, h0, ya, yb));
		p11 = vpaddlq_s16(armv7_q4_dot16(l1, h1, ya, yb));
		acc = vmlaq_f32(acc, vcvtq_f32_s32(armv7_hsum4_s32(p00, p10, p01, p11)),
				armv7_scale_2x2(x0[i].d, x1[i].d, y0[i].d, y1[i].d));
	}
	armv7_store_2x2(s, bs, acc);
}

static inline void ggml_armv7_vec_dot_q8_0_q8_0_2x2(int n, float *s, size_t bs,
		const void *vx, size_t bx, const void *vy, size_t by)
{
	const block_q8_0 *x0 = (const block_q8_0 *)vx;
	const block_q8_0 *x1 = (const block_q8_0 *)((const uint8_t *)vx + bx);
	const block_q8_0 *y0 = (const block_q8_0 *)vy;
	const block_q8_0 *y1 = (const block_q8_0 *)((const uint8_t *)vy + by);
	const int nb = n / QK8_0;
	float32x4_t acc = vdupq_n_f32(0.0f);
	int i;

	for (i = 0; i < nb; i++) {
		const int8x16_t x0a = vld1q_s8(x0[i].qs), x0b = vld1q_s8(x0[i].qs + 16);
		const int8x16_t x1a = vld1q_s8(x1[i].qs), x1b = vld1q_s8(x1[i].qs + 16);
		int8x16_t ya, yb;
		int32x4_t p00, p10, p01, p11;

		__builtin_prefetch((const char *)&x0[i] + ARMV7_PF_DIST);
		__builtin_prefetch((const char *)&x1[i] + ARMV7_PF_DIST);
		ya = vld1q_s8(y0[i].qs);
		yb = vld1q_s8(y0[i].qs + 16);
		p00 = armv7_q8_dot32(x0a, x0b, ya, yb);
		p10 = armv7_q8_dot32(x1a, x1b, ya, yb);
		ya = vld1q_s8(y1[i].qs);
		yb = vld1q_s8(y1[i].qs + 16);
		p01 = armv7_q8_dot32(x0a, x0b, ya, yb);
		p11 = armv7_q8_dot32(x1a, x1b, ya, yb);
		acc = vmlaq_f32(acc, vcvtq_f32_s32(armv7_hsum4_s32(p00, p10, p01, p11)),
				armv7_scale_2x2(x0[i].d, x1[i].d, y0[i].d, y1[i].d));
	}
	armv7_store_2x2(s, bs, acc);
}

/* roundf() semantics (half away from zero), exact: the remainder after
 * truncation is computed without rounding error (Sterbenz). */
static inline int32x4_t armv7_roundf_s32(float32x4_t a)
{
	const int32x4_t t = vcvtq_s32_f32(a);
	const float32x4_t f = vsubq_f32(a, vcvtq_f32_s32(t));
	const uint32x4_t up = vcageq_f32(f, vdupq_n_f32(0.5f));
	/* +1 or -1 with the sign of a */
	const int32x4_t one = vorrq_s32(vshrq_n_s32(vreinterpretq_s32_f32(a), 31),
					vdupq_n_s32(1));

	return vaddq_s32(t, vandq_s32(one, vreinterpretq_s32_u32(up)));
}

/*
 * Activation quantiser, bit-exact with quantize_row_q8_0_ref: d = amax/127,
 * q = roundf(x * (1/d)). Only the per-block scale leaves NEON (it is
 * converted to fp16 on the integer side). The includer defines
 * ARMV7_FP32_TO_FP16(f).
 */
static inline void ggml_armv7_quantize_row_q8_0(const float *x, void *vy,
						int64_t k)
{
	block_q8_0 *y = (block_q8_0 *)vy;
	const int nb = (int)(k / QK8_0);
	int i, j;

	for (i = 0; i < nb; i++) {
		float32x4_t v[8], m;
		float32x2_t m2;
		float amax, d, id;

		for (j = 0; j < 8; j++)
			v[j] = vld1q_f32(x + i * 32 + 4 * j);
		m = vmaxq_f32(vmaxq_f32(vabsq_f32(v[0]), vabsq_f32(v[1])),
			      vmaxq_f32(vabsq_f32(v[2]), vabsq_f32(v[3])));
		m = vmaxq_f32(m, vmaxq_f32(vmaxq_f32(vabsq_f32(v[4]), vabsq_f32(v[5])),
					   vmaxq_f32(vabsq_f32(v[6]), vabsq_f32(v[7]))));
		m2 = vpmax_f32(vget_low_f32(m), vget_high_f32(m));
		m2 = vpmax_f32(m2, m2);
		amax = vget_lane_f32(m2, 0);

		d = amax / ((1 << 7) - 1);
		id = d ? 1.0f / d : 0.0f;
		y[i].d = ARMV7_FP32_TO_FP16(d);

		for (j = 0; j < 8; j += 2) {
			int32x4_t ia = armv7_roundf_s32(vmulq_n_f32(v[j], id));
			int32x4_t ib = armv7_roundf_s32(vmulq_n_f32(v[j + 1], id));
			int16x8_t h = vcombine_s16(vmovn_s32(ia), vmovn_s32(ib));

			vst1_s8(y[i].qs + 4 * j, vmovn_s16(h));
		}
	}
}

#endif /* __ARM_NEON && !__aarch64__ */
#endif /* GIRI_ARMV7_KERNELS_H */
