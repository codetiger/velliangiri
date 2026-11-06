// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standalone correctness + speed test for tools/giri/ggml/armv7-kernels.h,
 * run on the Cortex-A9. Mirrors ggml's block layouts and scalar references.
 *
 *   test-armv7-kernels            # correctness, then timing
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define QK8_0 32
typedef uint16_t ggml_half;
typedef struct { ggml_half d; uint8_t qs[QK8_0 / 2]; } block_q4_0;
typedef struct { ggml_half d; int8_t qs[QK8_0]; } block_q8_0;

float ggml_table_f32_f16[1 << 16];

static float fp16_to_fp32(uint16_t h)
{
	uint32_t sign = (uint32_t)(h & 0x8000) << 16;
	uint32_t exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
	float f;

	if (exp == 0) {
		f = ldexpf((float)man, -24);
		return sign ? -f : f;
	}
	if (exp == 31)
		bits = sign | 0x7f800000u | (man << 13);
	else
		bits = sign | ((exp + 112) << 23) | (man << 13);
	memcpy(&f, &bits, 4);
	return f;
}

/* round-to-nearest-even, same results as ggml_compute_fp32_to_fp16 */
static uint16_t fp32_to_fp16(float f)
{
	const float scale_to_inf = 0x1.0p+112f, scale_to_zero = 0x1.0p-110f;
	float base = (fabsf(f) * scale_to_inf) * scale_to_zero;
	uint32_t w, shl1_w, sign, bias, bits, exp_bits, mantissa_bits, nonsign;

	memcpy(&w, &f, 4);
	shl1_w = w + w;
	sign = w & 0x80000000u;
	bias = shl1_w & 0xff000000u;
	if (bias < 0x71000000u)
		bias = 0x71000000u;
	bias = (bias >> 1) + 0x07800000u;
	{ float fb; memcpy(&fb, &bias, 4); base = fb + base; }
	memcpy(&bits, &base, 4);
	exp_bits = (bits >> 13) & 0x00007c00u;
	mantissa_bits = bits & 0x00000fffu;
	nonsign = exp_bits + mantissa_bits;
	return (uint16_t)((sign >> 16) | (shl1_w > 0xff000000u ? 0x7e00u : nonsign));
}

#define ARMV7_FP32_TO_FP16(f) fp32_to_fp16(f)
#include "ggml/armv7-kernels.h"
#include "ggml/armv7-f32.h"

/* ---- scalar references (ggml generic semantics) ---- */

static void ref_quantize_q8_0(const float *x, block_q8_0 *y, int k)
{
	for (int i = 0; i < k / QK8_0; i++) {
		float amax = 0.0f;
		for (int j = 0; j < QK8_0; j++)
			amax = fmaxf(amax, fabsf(x[i * QK8_0 + j]));
		const float d = amax / ((1 << 7) - 1);
		const float id = d ? 1.0f / d : 0.0f;
		y[i].d = fp32_to_fp16(d);
		for (int j = 0; j < QK8_0; j++)
			y[i].qs[j] = (int8_t)roundf(x[i * QK8_0 + j] * id);
	}
}

static double ref_dot_q4_0(int n, const block_q4_0 *x, const block_q8_0 *y)
{
	double s = 0;
	for (int i = 0; i < n / QK8_0; i++) {
		int si = 0;
		for (int j = 0; j < QK8_0 / 2; j++) {
			si += ((x[i].qs[j] & 0x0f) - 8) * y[i].qs[j];
			si += ((x[i].qs[j] >> 4) - 8) * y[i].qs[j + QK8_0 / 2];
		}
		s += (double)si * fp16_to_fp32(x[i].d) * fp16_to_fp32(y[i].d);
	}
	return s;
}

static double ref_dot_q8_0(int n, const block_q8_0 *x, const block_q8_0 *y)
{
	double s = 0;
	for (int i = 0; i < n / QK8_0; i++) {
		int si = 0;
		for (int j = 0; j < QK8_0; j++)
			si += x[i].qs[j] * y[i].qs[j];
		s += (double)si * fp16_to_fp32(x[i].d) * fp16_to_fp32(y[i].d);
	}
	return s;
}

/* ---- helpers ---- */

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }
static float frnd(void) { return ((float)(rnd() & 0xffff) / 32768.0f - 1.0f); }

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void fill_q4_0(block_q4_0 *b, int nb, int extreme)
{
	for (int i = 0; i < nb; i++) {
		b[i].d = fp32_to_fp16(0.001f + (rnd() & 0xff) * 1e-4f);
		for (int j = 0; j < 16; j++)
			b[i].qs[j] = extreme ? 0x00 : (uint8_t)rnd(); /* 0x00 = -8,-8 */
	}
}

static void fill_q8_0(block_q8_0 *b, int nb, int extreme)
{
	for (int i = 0; i < nb; i++) {
		b[i].d = fp32_to_fp16(0.001f + (rnd() & 0xff) * 1e-4f);
		for (int j = 0; j < 32; j++)
			b[i].qs[j] = extreme ? ((j & 1) ? 127 : -127)
					     : (int8_t)((int)(rnd() % 255) - 127);
	}
}

static int check_close(const char *what, int n, double ref, float got)
{
	double tol = 1e-4 * (fabs(ref) + 1e-3);
	if (fabs(ref - got) > tol) {
		printf("FAIL %s n=%d ref=%.9g got=%.9g\n", what, n, ref, got);
		return 1;
	}
	return 0;
}

int main(void)
{
	int fails = 0;

	for (int i = 0; i < (1 << 16); i++)
		ggml_table_f32_f16[i] = fp16_to_fp32((uint16_t)i);

	/* 1. quantiser: bit-exact incl. ties and the 0.49999997 edge */
	{
		enum { K = 32 * 64 };
		static float x[K];
		static block_q8_0 a[K / 32], b[K / 32];

		for (int i = 0; i < K; i++)
			x[i] = frnd() * (i % 7 ? 1.0f : 100.0f);
		/* block 0: amax 127 -> id 1: exact half-way and edge values */
		const float edge[] = { 127.0f, 0.5f, -0.5f, 1.5f, -2.5f, 0.49999997f,
				       -0.49999997f, 126.5f, -126.5f, 0.0f, -0.0f };
		for (unsigned i = 0; i < sizeof(edge) / sizeof(edge[0]); i++)
			x[i] = edge[i];
		memset(x + 32, 0, 32 * sizeof(float)); /* block 1: all zero */
		ref_quantize_q8_0(x, a, K);
		ggml_armv7_quantize_row_q8_0(x, b, K);
		if (memcmp(a, b, sizeof(a))) {
			for (int i = 0; i < K / 32; i++)
				if (memcmp(&a[i], &b[i], sizeof(a[i])))
					printf("FAIL quantize block %d differs\n", i);
			fails++;
		} else {
			printf("ok   quantize_row_q8_0 bit-exact (%d values)\n", K);
		}
	}

	/* 2. dot products vs reference, all tail lengths + int16 extremes */
	{
		static block_q4_0 x4[64];
		static block_q8_0 x8[64], y8[64];
		int nf = 0, cases = 0;

		for (int extreme = 0; extreme < 2; extreme++) {
			for (int nb = 1; nb <= 64; nb++) {
				float got;
				fill_q4_0(x4, nb, extreme);
				fill_q8_0(x8, nb, extreme);
				fill_q8_0(y8, nb, extreme);
				ggml_armv7_vec_dot_q4_0_q8_0(nb * 32, &got, x4, y8);
				nf += check_close("q4_0", nb * 32, ref_dot_q4_0(nb * 32, x4, y8), got);
				ggml_armv7_vec_dot_q8_0_q8_0(nb * 32, &got, x8, y8);
				nf += check_close("q8_0", nb * 32, ref_dot_q8_0(nb * 32, x8, y8), got);
				cases += 2;
			}
		}
		printf("%s dot products: %d/%d cases match\n", nf ? "FAIL" : "ok  ",
		       cases - nf, cases);
		fails += nf;
	}

	/* 2b. 2x2 tiles vs reference (strided rows, ggml output layout) */
	{
		enum { MAXNB = 48 };
		static block_q4_0 x4[2][MAXNB];
		static block_q8_0 x8[2][MAXNB], y8[2][MAXNB];
		int nf = 0, cases = 0;

		for (int extreme = 0; extreme < 2; extreme++) {
			for (int nb = 1; nb <= MAXNB; nb++) {
				float out[2 * 16];
				const size_t bx4 = sizeof(x4[0]), bx8 = sizeof(x8[0]), by = sizeof(y8[0]);
				for (int r = 0; r < 2; r++) {
					fill_q4_0(x4[r], nb, extreme);
					fill_q8_0(x8[r], nb, extreme);
					fill_q8_0(y8[r], nb, extreme);
				}
				ggml_armv7_vec_dot_q4_0_q8_0_2x2(nb * 32, out, 16, x4, bx4, y8, by);
				nf += check_close("q4_0 2x2 00", nb, ref_dot_q4_0(nb * 32, x4[0], y8[0]), out[0]);
				nf += check_close("q4_0 2x2 10", nb, ref_dot_q4_0(nb * 32, x4[1], y8[0]), out[1]);
				nf += check_close("q4_0 2x2 01", nb, ref_dot_q4_0(nb * 32, x4[0], y8[1]), out[16]);
				nf += check_close("q4_0 2x2 11", nb, ref_dot_q4_0(nb * 32, x4[1], y8[1]), out[17]);
				ggml_armv7_vec_dot_q8_0_q8_0_2x2(nb * 32, out, 16, x8, bx8, y8, by);
				nf += check_close("q8_0 2x2 00", nb, ref_dot_q8_0(nb * 32, x8[0], y8[0]), out[0]);
				nf += check_close("q8_0 2x2 10", nb, ref_dot_q8_0(nb * 32, x8[1], y8[0]), out[1]);
				nf += check_close("q8_0 2x2 01", nb, ref_dot_q8_0(nb * 32, x8[0], y8[1]), out[16]);
				nf += check_close("q8_0 2x2 11", nb, ref_dot_q8_0(nb * 32, x8[1], y8[1]), out[17]);
				cases += 8;
			}
		}
		printf("%s 2x2 tiles: %d/%d results match\n", nf ? "FAIL" : "ok  ",
		       cases - nf, cases);
		fails += nf;
	}

	/* 2c. f32 dot (1x1 and 2x2) vs double reference, every tail length */
	{
		enum { MAXN = 300 };
		static float fx[2][MAXN], fy[2][MAXN];
		int nf = 0, cases = 0;

		for (int r = 0; r < 2; r++)
			for (int i = 0; i < MAXN; i++) {
				fx[r][i] = frnd();
				fy[r][i] = frnd();
			}
		for (int n = 1; n <= MAXN; n++) {
			double ref[2][2];
			float out[2 * 16], one;
			for (int a = 0; a < 2; a++)
				for (int b = 0; b < 2; b++) {
					ref[a][b] = 0;
					for (int i = 0; i < n; i++)
						ref[a][b] += (double)fx[a][i] * fy[b][i];
				}
			ggml_armv7_vec_dot_f32(n, &one, fx[0], fy[0]);
			ggml_armv7_vec_dot_f32_2x2(n, out, 16, fx[0], sizeof(fx[0]), fy[0], sizeof(fy[0]));
			nf += fabs(one - ref[0][0]) > 1e-5 * n;
			nf += fabs(out[0] - ref[0][0]) > 1e-5 * n;
			nf += fabs(out[1] - ref[1][0]) > 1e-5 * n;
			nf += fabs(out[16] - ref[0][1]) > 1e-5 * n;
			nf += fabs(out[17] - ref[1][1]) > 1e-5 * n;
			cases += 5;
		}
		printf("%s f32 dot 1x1/2x2: %d/%d results match\n", nf ? "FAIL" : "ok  ",
		       cases - nf, cases);
		fails += nf;
	}

	/* 3a. attention-shaped f32 matmul: 256 keys x 64 dims x 64 queries */
	{
		enum { KEYS = 256, D = 64, Q = 64 };
		static float k[KEYS][D], q[Q][D], out[Q][KEYS];
		double t1, t2, t0;

		for (int i = 0; i < KEYS; i++) for (int j = 0; j < D; j++) k[i][j] = frnd();
		for (int i = 0; i < Q; i++) for (int j = 0; j < D; j++) q[i][j] = frnd();
		t0 = now_s();
		for (int r = 0; r < 10; r++)
			for (int c = 0; c < Q; c++)
				for (int i = 0; i < KEYS; i++)
					ggml_armv7_vec_dot_f32(D, &out[c][i], k[i], q[c]);
		t1 = (now_s() - t0) / 10;
		t0 = now_s();
		for (int r = 0; r < 10; r++)
			for (int c = 0; c < Q; c += 2)
				for (int i = 0; i < KEYS; i += 2)
					ggml_armv7_vec_dot_f32_2x2(D, &out[c][i], KEYS, k[i], sizeof(k[0]), q[c], sizeof(q[0]));
		t2 = (now_s() - t0) / 10;
		printf("f32 KQ 256x64x64     1x1 %6.2f ms %5.2f GMAC/s   2x2 %6.2f ms %5.2f GMAC/s\n",
		       t1 * 1e3, (double)KEYS * D * Q / t1 / 1e9, t2 * 1e3, (double)KEYS * D * Q / t2 / 1e9);
	}

	/* 3a'. same kernel, L1-resident (32 keys x 64 x 16 queries, 12 KB) */
	{
		enum { KEYS = 32, D = 64, Q = 16, REP = 400 };
		static float k[KEYS][D], q[Q][D], out[Q][KEYS];
		double t1, t2, t0;

		for (int i = 0; i < KEYS; i++) for (int j = 0; j < D; j++) k[i][j] = frnd();
		for (int i = 0; i < Q; i++) for (int j = 0; j < D; j++) q[i][j] = frnd();
		t0 = now_s();
		for (int r = 0; r < REP; r++)
			for (int c = 0; c < Q; c++)
				for (int i = 0; i < KEYS; i++)
					ggml_armv7_vec_dot_f32(D, &out[c][i], k[i], q[c]);
		t1 = (now_s() - t0) / REP;
		t0 = now_s();
		for (int r = 0; r < REP; r++)
			for (int c = 0; c < Q; c += 2)
				for (int i = 0; i < KEYS; i += 2)
					ggml_armv7_vec_dot_f32_2x2(D, &out[c][i], KEYS, k[i], sizeof(k[0]), q[c], sizeof(q[0]));
		t2 = (now_s() - t0) / REP;
		printf("f32 KQ L1-hot        1x1 %5.2f GMAC/s   2x2 %5.2f GMAC/s\n",
		       (double)KEYS * D * Q / t1 / 1e9, (double)KEYS * D * Q / t2 / 1e9);
	}

	/* 3. speed on SmolLM2-135M shapes (rows x n), one activation row */
	{
		static const struct { int rows, n; const char *name; } shp[] = {
			{ 576, 576, "attn_q/o  576x576" },
			{ 1536, 576, "ffn_up   1536x576" },
			{ 576, 1536, "ffn_down 576x1536" },
			{ 49152, 576, "output 49152x576" },
		};
		for (unsigned t = 0; t < sizeof(shp) / sizeof(shp[0]); t++) {
			const int rows = shp[t].rows, n = shp[t].n, nb = n / 32;
			block_q4_0 *w4 = malloc(sizeof(block_q4_0) * nb * rows);
			block_q8_0 *w8 = malloc(sizeof(block_q8_0) * nb * rows);
			block_q8_0 *y = malloc(sizeof(block_q8_0) * nb);
			float *out = malloc(sizeof(float) * rows);
			double t4 = 1e9, t8 = 1e9;

			fill_q4_0(w4, nb * rows, 0);
			fill_q8_0(w8, nb * rows, 0);
			fill_q8_0(y, nb, 0);
			for (int r = 0; r < 3; r++) {
				double s0 = now_s();
				for (int i = 0; i < rows; i++)
					ggml_armv7_vec_dot_q4_0_q8_0(n, &out[i], w4 + i * nb, y);
				double s1 = now_s();
				for (int i = 0; i < rows; i++)
					ggml_armv7_vec_dot_q8_0_q8_0(n, &out[i], w8 + i * nb, y);
				double s2 = now_s();
				if (s1 - s0 < t4) t4 = s1 - s0;
				if (s2 - s1 < t8) t8 = s2 - s1;
			}
			printf("%-20s q4_0 %8.3f ms %5.2f GMAC/s   q8_0 %8.3f ms %5.2f GMAC/s\n",
			       shp[t].name, t4 * 1e3, (double)rows * n / t4 / 1e9,
			       t8 * 1e3, (double)rows * n / t8 / 1e9);
			free(w4); free(w8); free(y); free(out);
		}
	}

	/* 3b. prompt processing: 1536x576 weights x 64 tokens, ggml 16x16 tiling */
	{
		enum { ROWS = 1536, N = 576, NB = N / 32, COLS = 64 };
		block_q4_0 *w4 = malloc(sizeof(block_q4_0) * NB * ROWS);
		block_q8_0 *w8 = malloc(sizeof(block_q8_0) * NB * ROWS);
		block_q8_0 *y = malloc(sizeof(block_q8_0) * NB * COLS);
		static float out[COLS][ROWS];
		double t[2][2];
		const double macs = (double)ROWS * N * COLS;

		fill_q4_0(w4, NB * ROWS, 0);
		fill_q8_0(w8, NB * ROWS, 0);
		fill_q8_0(y, NB * COLS, 0);
		for (int q = 0; q < 2; q++) {
			for (int tile = 0; tile < 2; tile++) {
				double t0 = now_s();
				for (int i1 = 0; i1 < COLS; i1 += 16)
				for (int i0 = 0; i0 < ROWS; i0 += 16)
				for (int c = i1; c < i1 + 16; c += 1 + tile)
				for (int r = i0; r < i0 + 16; r += 1 + tile) {
					const block_q8_0 *yc = y + c * NB;
					if (q == 0 && tile)
						ggml_armv7_vec_dot_q4_0_q8_0_2x2(N, &out[c][r], ROWS,
							w4 + r * NB, sizeof(block_q4_0) * NB, yc, sizeof(block_q8_0) * NB);
					else if (q == 0)
						ggml_armv7_vec_dot_q4_0_q8_0(N, &out[c][r], w4 + r * NB, yc);
					else if (tile)
						ggml_armv7_vec_dot_q8_0_q8_0_2x2(N, &out[c][r], ROWS,
							w8 + r * NB, sizeof(block_q8_0) * NB, yc, sizeof(block_q8_0) * NB);
					else
						ggml_armv7_vec_dot_q8_0_q8_0(N, &out[c][r], w8 + r * NB, yc);
				}
				t[q][tile] = now_s() - t0;
			}
		}
		printf("prefill 1536x576x64  q4_0 1x1 %5.2f / 2x2 %5.2f GMAC/s   q8_0 1x1 %5.2f / 2x2 %5.2f GMAC/s\n",
		       macs / t[0][0] / 1e9, macs / t[0][1] / 1e9, macs / t[1][0] / 1e9, macs / t[1][1] / 1e9);
		free(w4); free(w8); free(y);
	}

	/* 4. L1-resident: same 576-wide row repeated -> pure compute rate */
	{
		enum { NB = 18, REP = 20000 };
		static block_q4_0 w4[NB];
		static block_q8_0 w8[NB], y[NB];
		float out;
		double t0, t4, t8;

		fill_q4_0(w4, NB, 0);
		fill_q8_0(w8, NB, 0);
		fill_q8_0(y, NB, 0);
		t0 = now_s();
		for (int r = 0; r < REP; r++)
			ggml_armv7_vec_dot_q4_0_q8_0(NB * 32, &out, w4, y);
		t4 = now_s() - t0;
		t0 = now_s();
		for (int r = 0; r < REP; r++)
			ggml_armv7_vec_dot_q8_0_q8_0(NB * 32, &out, w8, y);
		t8 = now_s() - t0;
		printf("L1-hot 576 x %d      q4_0 %5.2f GMAC/s (%.1f cyc/block)   q8_0 %5.2f GMAC/s (%.1f cyc/block)\n",
		       REP, (double)REP * NB * 32 / t4 / 1e9, t4 * 850e6 / ((double)REP * NB),
		       (double)REP * NB * 32 / t8 / 1e9, t8 * 850e6 / ((double)REP * NB));
	}

	printf("%s\n", fails ? "FAILED" : "ALL OK");
	return fails ? 1 : 0;
}
