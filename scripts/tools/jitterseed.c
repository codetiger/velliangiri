// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clocksource jitter entropy seeder for S5L8947X early initramfs
 *
 * Samples CLOCK_MONOTONIC_RAW (the 24 MHz hardware watchdog counter, 41 ns per
 * tick) across a cache/memory-walking loop of 16,384 iterations and credits
 * 256 bits of entropy to /dev/urandom via RNDADDENTROPY so the kernel CRNG
 * initializes in ~70 ms during early boot.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/random.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define SAMPLES	16384
#define CREDIT	256	/* bits */
#define WALK	4096	/* bytes of scratch buffer walked per sample */

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void)
{
	static unsigned char scratch[WALK];
	struct {
		struct rand_pool_info info;
		uint32_t buf[SAMPLES / 4];
	} e;
	uint64_t t0, t1, start = now_ns();
	uint32_t acc = 0x9e3779b9U, idx = 0;
	unsigned int distinct[256] = { 0 };
	int fd, i, n = 0;

	memset(&e, 0, sizeof(e));
	for (i = 0; i < SAMPLES; i++) {
		int k, steps = 64 + (int)(acc & 63U);
		unsigned char *p = (unsigned char *)e.buf;

		t0 = now_ns();
		for (k = 0; k < steps; k++) {
			idx = (idx * 1103515245U + 12345U + acc) & (WALK - 1);
			scratch[idx] += (unsigned char)(acc >> 7);
			acc ^= scratch[(idx + (uint32_t)k) & (WALK - 1)];
			acc = (acc << 5) | (acc >> 27);
		}
		t1 = now_ns();
		acc += (uint32_t)(t1 - t0);
		p[i] = (unsigned char)(t1 - t0) ^ (unsigned char)((t1 - t0) >> 8);
		distinct[p[i]]++;
	}

	for (i = 0; i < 256; i++) {
		if (distinct[i])
			n++;
	}

	if (n < 16) {
		printf("jitterseed: only %d distinct delta values in %d samples -- not credited\n",
		       n, SAMPLES);
		return 1;
	}

	fd = open("/dev/urandom", O_RDWR);
	if (fd < 0) {
		printf("jitterseed: /dev/urandom: %s\n", strerror(errno));
		return 1;
	}

	e.info.entropy_count = CREDIT;
	e.info.buf_size = SAMPLES;
	if (ioctl(fd, RNDADDENTROPY, &e) < 0) {
		printf("jitterseed: RNDADDENTROPY: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	close(fd);
	printf("jitterseed: %d samples, %d distinct delta values, %d bits credited in %llu ms\n",
	       SAMPLES, n, CREDIT,
	       (unsigned long long)((now_ns() - start) / 1000000ULL));
	return 0;
}
