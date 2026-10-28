// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memory latency profile (/bin/l2bench): dependent loads through a random
 * cycle over working sets from 16 KB to 4 MB, one node per 32-byte cache
 * line. Steps in the curve mark the cache sizes: the L1 data cache is 32 KB;
 * with the PL310 enabled the 64 KB .. 512 KB sets should drop well below
 * the DRAM latency seen from 1 MB up.
 *
 * It also times a sequential read of 256 KB (fits the L2) and 16 MB (DRAM)
 * to show what the L2 prefetchers do for streaming access.
 *
 * Usage: l2bench [-q]     (-q: one line, "L1/L2/DRAM" latencies in ns,
 *                          then the sequential read rates)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LINE		32
#define NODES_MAX	(4u << 20) / LINE

static const unsigned int sizes_kb[] = {
	16, 32, 64, 128, 256, 384, 512, 768, 1024, 2048, 4096,
};

struct node {
	struct node *next;
	char pad[LINE - sizeof(struct node *)];
};

static uint32_t rng = 0x9e3779b9u;

static uint32_t xorshift(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static double now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static double chase(struct node *nodes, unsigned int n)
{
	unsigned int *order = malloc(n * sizeof(*order));
	struct node *p;
	unsigned long steps = 4ul << 20, i;
	double t0, best = 1e18;
	int pass;

	if (!order)
		return -1;
	for (i = 0; i < n; i++)
		order[i] = i;
	/* Sattolo: a single cycle through every node */
	for (i = n - 1; i > 0; i--) {
		unsigned int j = xorshift() % i, t = order[i];

		order[i] = order[j];
		order[j] = t;
	}
	for (i = 0; i < n; i++)
		nodes[order[i]].next = &nodes[order[(i + 1) % n]];

	p = &nodes[order[0]];
	for (i = 0; i < n; i++)		/* warm up */
		p = p->next;
	for (pass = 0; pass < 3; pass++) {
		t0 = now_ns();
		for (i = 0; i < steps; i++)
			p = p->next;
		t0 = (now_ns() - t0) / steps;
		if (t0 < best)
			best = t0;
	}
	free(order);
	/* keep p live */
	__asm__ volatile("" : : "r"(p) : "memory");
	return best;
}

static double seq_read_mbs(const uint32_t *buf, size_t bytes)
{
	size_t words = bytes / sizeof(*buf), i, reps = (64u << 20) / bytes;
	uint32_t sum = 0;
	double t0, best = 1e18;
	int pass;

	for (pass = 0; pass < 3; pass++) {
		size_t r;

		t0 = now_ns();
		for (r = 0; r < reps; r++)
			for (i = 0; i < words; i += 8)
				sum += buf[i] + buf[i + 4];
		t0 = now_ns() - t0;
		if (t0 < best)
			best = t0;
	}
	__asm__ volatile("" : : "r"(sum) : "memory");
	/* two words per 32-byte line are read; the rate is the span covered */
	return (double)bytes * reps / (best / 1e9) / (1 << 20);
}

int main(int argc, char **argv)
{
	struct node *nodes = aligned_alloc(4096, NODES_MAX * sizeof(*nodes));
	double lat[sizeof(sizes_kb) / sizeof(sizes_kb[0])];
	unsigned int i;
	int quiet = argc > 1 && !strcmp(argv[1], "-q");

	if (!nodes) {
		perror("l2bench: alloc");
		return 1;
	}
	memset(nodes, 0, NODES_MAX * sizeof(*nodes));

	for (i = 0; i < sizeof(sizes_kb) / sizeof(sizes_kb[0]); i++) {
		lat[i] = chase(nodes, sizes_kb[i] * 1024 / LINE);
		if (!quiet)
			printf("%5u KB  %7.1f ns\n", sizes_kb[i], lat[i]);
	}
	{
		uint32_t *big = aligned_alloc(4096, 16u << 20);

		if (big) {
			memset(big, 1, 16u << 20);
			double l2 = seq_read_mbs(big, 256u << 10);
			double dram = seq_read_mbs(big, 16u << 20);

			if (quiet)
				printf("L1=%.1f L2=%.1f DRAM=%.1f ns, seq 256K=%.0f 16M=%.0f MB/s\n",
				       lat[0], lat[6], lat[10], l2, dram);
			else
				printf("seq read: 256 KB %.0f MB/s, 16 MB %.0f MB/s\n", l2, dram);
		}
	}
	return 0;
}
