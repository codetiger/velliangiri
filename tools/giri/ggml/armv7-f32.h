// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARMv7-A NEON f32 dot products for ggml (Cortex-A9 class), used by the
 * attention matmuls (K*Q and KQ*V with an f32 KV cache, flash attention off).
 *
 * ggml's generic f32 dot reduces with vaddvq_f32, whose ARMv7 fallback is
 * four NEON->ARM lane transfers (each stalls this core), and it runs one row
 * per call. At head size 64 the reduction costs more than the 16 multiplies.
 * Here:
 *  - 4 independent accumulators hide the vmla.f32 latency;
 *  - the result is reduced and stored from NEON (vpadd + vst1), never
 *    through an ARM register;
 *  - a 2x2 tile (ggml type_traits nrows = 2) computes x0.y0, x1.y0, x0.y1,
 *    x1.y1 from one pass over the four rows, stored as s[0], s[1], s[bs],
 *    s[bs + 1].
 * Not bit-identical to the generic code (different summation order).
 *
 * Valid as C and C++ (included from vec.cpp).
 */
#ifndef GIRI_ARMV7_F32_H
#define GIRI_ARMV7_F32_H

#include <arm_neon.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__ARM_NEON) && !defined(__aarch64__) && !defined(GGML_ARMV7_DISABLE)
#define GGML_ARMV7_F32_KERNELS 1

static inline float32x2_t armv7_f32_hsum2(float32x4_t a)
{
	float32x2_t t = vadd_f32(vget_low_f32(a), vget_high_f32(a));

	return vpadd_f32(t, t);
}

static inline void ggml_armv7_vec_dot_f32(int n, float *s, const float *x,
					  const float *y)
{
	float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0;
	float32x2_t r;
	int i = 0;

	for (; i + 16 <= n; i += 16) {
		a0 = vmlaq_f32(a0, vld1q_f32(x + i), vld1q_f32(y + i));
		a1 = vmlaq_f32(a1, vld1q_f32(x + i + 4), vld1q_f32(y + i + 4));
		a2 = vmlaq_f32(a2, vld1q_f32(x + i + 8), vld1q_f32(y + i + 8));
		a3 = vmlaq_f32(a3, vld1q_f32(x + i + 12), vld1q_f32(y + i + 12));
	}
	for (; i + 4 <= n; i += 4)
		a0 = vmlaq_f32(a0, vld1q_f32(x + i), vld1q_f32(y + i));
	r = armv7_f32_hsum2(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)));
	/* scalar tail stays in VFP/NEON registers (s-regs alias d-regs) */
	for (; i < n; i++)
		r = vmla_n_f32(r, vdup_n_f32(x[i]), y[i]);
	vst1_lane_f32(s, r, 0);
}

static inline void ggml_armv7_vec_dot_f32_2x2(int n, float *s, size_t bs,
		const float *x0, size_t bx, const float *y0, size_t by)
{
	const float *x1 = (const float *)((const uint8_t *)x0 + bx);
	const float *y1 = (const float *)((const uint8_t *)y0 + by);
	float32x4_t a00 = vdupq_n_f32(0.0f), a10 = a00, a01 = a00, a11 = a00;
	float32x2_t r0, r1;
	float32x4_t res;
	int i = 0;

	for (; i + 4 <= n; i += 4) {
		const float32x4_t vx0 = vld1q_f32(x0 + i), vx1 = vld1q_f32(x1 + i);
		const float32x4_t vy0 = vld1q_f32(y0 + i), vy1 = vld1q_f32(y1 + i);

		a00 = vmlaq_f32(a00, vx0, vy0);
		a10 = vmlaq_f32(a10, vx1, vy0);
		a01 = vmlaq_f32(a01, vx0, vy1);
		a11 = vmlaq_f32(a11, vx1, vy1);
	}
	/* {00, 10} and {01, 11} */
	r0 = vpadd_f32(vadd_f32(vget_low_f32(a00), vget_high_f32(a00)),
		       vadd_f32(vget_low_f32(a10), vget_high_f32(a10)));
	r1 = vpadd_f32(vadd_f32(vget_low_f32(a01), vget_high_f32(a01)),
		       vadd_f32(vget_low_f32(a11), vget_high_f32(a11)));
	res = vcombine_f32(r0, r1);
	for (; i < n; i++) {
		const float32x4_t xv = vcombine_f32(vset_lane_f32(x1[i], vdup_n_f32(x0[i]), 1),
						    vset_lane_f32(x1[i], vdup_n_f32(x0[i]), 1));
		const float32x4_t yv = vcombine_f32(vdup_n_f32(y0[i]), vdup_n_f32(y1[i]));

		res = vmlaq_f32(res, xv, yv);
	}
	vst1_f32(s, vget_low_f32(res));
	vst1_f32(s + bs, vget_high_f32(res));
}

#endif /* __ARM_NEON && !__aarch64__ && !GGML_ARMV7_DISABLE */
#endif /* GIRI_ARMV7_F32_H */
