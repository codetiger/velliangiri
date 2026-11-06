// SPDX-License-Identifier: GPL-2.0-only
/*
 * Raw Cortex-A9 NEON issue rates (cycles per instruction at 850 MHz) for the
 * instructions the Giri kernels are built from. Independent operations, no
 * memory traffic except the L1-resident load test.
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define MHZ 850.0
#define N 2000000

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define RATE(name, ops_per_iter, body)                                       \
	do {                                                                 \
		double t0 = now_s();                                         \
		for (int i = 0; i < N; i++) {                                \
			__asm__ volatile(body ::: "q0", "q1", "q2", "q3", "q4", \
					 "q5", "q6", "q7", "q8", "q9", "q10", \
					 "q11", "q12", "q13", "q14", "q15", "memory"); \
		}                                                            \
		double t = now_s() - t0;                                     \
		printf("%-34s %5.2f cycles/insn\n", name,                    \
		       t * MHZ * 1e6 / ((double)N * (ops_per_iter)));        \
	} while (0)

static float buf[64] __attribute__((aligned(64)));

int main(void)
{
	register float *p __asm__("r4") = buf;

	RATE("vmla.f32 q (8 independent)", 8,
	     "vmla.f32 q0, q8, q9\n vmla.f32 q1, q8, q9\n vmla.f32 q2, q8, q9\n vmla.f32 q3, q8, q9\n"
	     "vmla.f32 q4, q8, q9\n vmla.f32 q5, q8, q9\n vmla.f32 q6, q8, q9\n vmla.f32 q7, q8, q9\n");
	RATE("vmul.f32 q (8 independent)", 8,
	     "vmul.f32 q0, q8, q9\n vmul.f32 q1, q8, q9\n vmul.f32 q2, q8, q9\n vmul.f32 q3, q8, q9\n"
	     "vmul.f32 q4, q8, q9\n vmul.f32 q5, q8, q9\n vmul.f32 q6, q8, q9\n vmul.f32 q7, q8, q9\n");
	RATE("vadd.f32 q (8 independent)", 8,
	     "vadd.f32 q0, q8, q9\n vadd.f32 q1, q8, q9\n vadd.f32 q2, q8, q9\n vadd.f32 q3, q8, q9\n"
	     "vadd.f32 q4, q8, q9\n vadd.f32 q5, q8, q9\n vadd.f32 q6, q8, q9\n vadd.f32 q7, q8, q9\n");
	RATE("vmlal.s8 q,d,d (8 independent)", 8,
	     "vmlal.s8 q0, d16, d17\n vmlal.s8 q1, d16, d17\n vmlal.s8 q2, d16, d17\n vmlal.s8 q3, d16, d17\n"
	     "vmlal.s8 q4, d16, d17\n vmlal.s8 q5, d16, d17\n vmlal.s8 q6, d16, d17\n vmlal.s8 q7, d16, d17\n");
	RATE("vpadal.s16 q (8 independent)", 8,
	     "vpadal.s16 q0, q8\n vpadal.s16 q1, q8\n vpadal.s16 q2, q8\n vpadal.s16 q3, q8\n"
	     "vpadal.s16 q4, q8\n vpadal.s16 q5, q8\n vpadal.s16 q6, q8\n vpadal.s16 q7, q8\n");
	RATE("vand q (8 independent)", 8,
	     "vand q0, q8, q9\n vand q1, q8, q9\n vand q2, q8, q9\n vand q3, q8, q9\n"
	     "vand q4, q8, q9\n vand q5, q8, q9\n vand q6, q8, q9\n vand q7, q8, q9\n");
	{
		double t0 = now_s();
		for (int i = 0; i < N; i++)
			__asm__ volatile(
				"vld1.32 {d0-d1}, [%0:128]\n vld1.32 {d2-d3}, [%0:128]\n"
				"vld1.32 {d4-d5}, [%0:128]\n vld1.32 {d6-d7}, [%0:128]\n"
				"vld1.32 {d8-d9}, [%0:128]\n vld1.32 {d10-d11}, [%0:128]\n"
				"vld1.32 {d12-d13}, [%0:128]\n vld1.32 {d14-d15}, [%0:128]\n"
				:: "r"(p) : "q0","q1","q2","q3","q4","q5","q6","q7","memory");
		double t = now_s() - t0;
		printf("%-34s %5.2f cycles/insn\n", "vld1.32 {q} L1 aligned (8)",
		       t * MHZ * 1e6 / ((double)N * 8));
	}
	{
		double t0 = now_s();
		for (int i = 0; i < N; i++)
			__asm__ volatile(
				"vld1.32 {d0-d1}, [%0:128]\n vmla.f32 q8, q0, q4\n"
				"vld1.32 {d2-d3}, [%0:128]\n vmla.f32 q9, q1, q4\n"
				"vld1.32 {d4-d5}, [%0:128]\n vmla.f32 q10, q2, q4\n"
				"vld1.32 {d6-d7}, [%0:128]\n vmla.f32 q11, q3, q4\n"
				:: "r"(p) : "q0","q1","q2","q3","q8","q9","q10","q11","memory");
		double t = now_s() - t0;
		printf("%-34s %5.2f cycles/pair\n", "vld1 + vmla.f32 interleaved (4)",
		       t * MHZ * 1e6 / ((double)N * 4));
	}
	{
		double t0 = now_s();
		for (int i = 0; i < N; i++)
			__asm__ volatile(
				"vld1.32 {d0-d1}, [%0:128]\n vmla.f32 q8, q12, q13\n"
				"vld1.32 {d2-d3}, [%0:128]\n vmla.f32 q9, q12, q13\n"
				"vld1.32 {d4-d5}, [%0:128]\n vmla.f32 q10, q12, q13\n"
				"vld1.32 {d6-d7}, [%0:128]\n vmla.f32 q11, q12, q13\n"
				:: "r"(p) : "q0","q1","q2","q3","q8","q9","q10","q11","memory");
		double t = now_s() - t0;
		printf("%-34s %5.2f cycles/pair\n", "vld1 + independent vmla.f32 (4)",
		       t * MHZ * 1e6 / ((double)N * 4));
	}
	{
		double t0 = now_s();
		for (int i = 0; i < N; i++)
			__asm__ volatile(
				"vld1.8 {d0-d1}, [%0]\n vmlal.s8 q8, d24, d25\n vmlal.s8 q9, d24, d25\n"
				"vld1.8 {d2-d3}, [%0]\n vmlal.s8 q10, d24, d25\n vmlal.s8 q11, d24, d25\n"
				"vld1.8 {d4-d5}, [%0]\n vmlal.s8 q8, d26, d27\n vmlal.s8 q9, d26, d27\n"
				"vld1.8 {d6-d7}, [%0]\n vmlal.s8 q10, d26, d27\n vmlal.s8 q11, d26, d27\n"
				:: "r"(p) : "q0","q1","q2","q3","q8","q9","q10","q11","memory");
		double t = now_s() - t0;
		printf("%-34s %5.2f cycles/load\n", "vld1 + 2 independent vmlal.s8 (4)",
		       t * MHZ * 1e6 / ((double)N * 4));
	}
	{
		double t0 = now_s();
		for (int i = 0; i < N; i++)
			__asm__ volatile(
				"vmlal.s8 q8, d24, d25\n vmlal.s8 q8, d26, d27\n"
				"vmlal.s8 q8, d28, d29\n vmlal.s8 q8, d30, d31\n"
				::: "q8","memory");
		double t = now_s() - t0;
		printf("%-34s %5.2f cycles/insn\n", "vmlal.s8 same accumulator (4)",
		       t * MHZ * 1e6 / ((double)N * 4));
	}
	return 0;
}
