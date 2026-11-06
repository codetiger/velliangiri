// SPDX-License-Identifier: GPL-2.0-only
/*
 * Phase 0 DRAM bandwidth probe: streaming NEON read (the LLM decode access
 * pattern: every weight read once per token) and memcpy, over a buffer much
 * larger than L2.
 *
 *   membw [MiB]   (default 64)
 */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t neon_read(const uint8_t *p, size_t n)
{
	uint32x4_t acc0 = vdupq_n_u32(0), acc1 = vdupq_n_u32(0);
	size_t i;

	for (i = 0; i + 64 <= n; i += 64) {
		__builtin_prefetch(p + i + 256);
		acc0 = vaddq_u32(acc0, vreinterpretq_u32_u8(vld1q_u8(p + i)));
		acc1 = vaddq_u32(acc1, vreinterpretq_u32_u8(vld1q_u8(p + i + 16)));
		acc0 = vaddq_u32(acc0, vreinterpretq_u32_u8(vld1q_u8(p + i + 32)));
		acc1 = vaddq_u32(acc1, vreinterpretq_u32_u8(vld1q_u8(p + i + 48)));
	}
	acc0 = vaddq_u32(acc0, acc1);
	return vgetq_lane_u32(acc0, 0) ^ vgetq_lane_u32(acc0, 3);
}

int main(int argc, char **argv)
{
	size_t mib = argc > 1 ? strtoul(argv[1], NULL, 0) : 64;
	size_t n = mib << 20;
	uint8_t *a = aligned_alloc(64, n), *b = aligned_alloc(64, n);
	volatile uint32_t sink = 0;
	double t, best;
	int r;

	if (!a || !b) {
		fprintf(stderr, "alloc %zu MiB failed\n", mib);
		return 1;
	}
	memset(a, 1, n);
	memset(b, 2, n);

	best = 1e9;
	for (r = 0; r < 5; r++) {
		t = now_s();
		sink ^= neon_read(a, n);
		t = now_s() - t;
		if (t < best)
			best = t;
	}
	printf("neon read : %7.1f MB/s\n", n / best / 1e6);

	best = 1e9;
	for (r = 0; r < 5; r++) {
		t = now_s();
		memcpy(b, a, n);
		t = now_s() - t;
		if (t < best)
			best = t;
	}
	printf("memcpy    : %7.1f MB/s (read+write %7.1f)\n",
	       n / best / 1e6, 2 * n / best / 1e6);
	(void)sink;
	free(a);
	free(b);
	return 0;
}
