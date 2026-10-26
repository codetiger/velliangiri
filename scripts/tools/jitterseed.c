// SPDX-License-Identifier: GPL-2.0-only
/*
 * Clocksource jitter entropy seeder for S5L8947X early initramfs
 *
 * Samples clock_gettime(CLOCK_MONOTONIC) deltas across short busy-wait loops
 * and credits 512 bits of entropy to /dev/urandom via RNDADDENTROPY so
 * Dropbear SSH starts immediately without blocking on CRNG initialization.
 */

#include <fcntl.h>
#include <linux/random.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define ENTROPY_WORDS	16	/* 64 bytes = 512 bits */
#define ROUNDS_PER_WORD	64

static uint32_t sample_word(void)
{
	struct timespec ts;
	uint32_t acc = 0x9e3779b9U;
	int r, k;

	for (r = 0; r < ROUNDS_PER_WORD; r++) {
		volatile int spin;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		acc ^= (uint32_t)ts.tv_nsec;
		acc = (acc << 7) | (acc >> 25);
		acc += 0x85ebca6bU;

		spin = (int)((acc & 0x3fU) + 17U);
		while (spin-- > 0)
			__asm__ volatile("" ::: "memory");
	}
	for (k = 0; k < 4; k++) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		acc ^= (uint32_t)ts.tv_nsec * 0xc2b2ae35U;
		acc = (acc << 13) | (acc >> 19);
	}
	return acc;
}

int main(void)
{
	struct {
		int entropy_count;
		int buf_size;
		uint32_t buf[ENTROPY_WORDS];
	} req;
	int fd, i;

	req.entropy_count = ENTROPY_WORDS * 32;
	req.buf_size = sizeof(req.buf);
	for (i = 0; i < ENTROPY_WORDS; i++)
		req.buf[i] = sample_word();

	fd = open("/dev/urandom", O_WRONLY);
	if (fd < 0)
		fd = open("/dev/random", O_WRONLY);
	if (fd < 0) {
		perror("jitterseed: open /dev/urandom");
		return 1;
	}

	if (ioctl(fd, RNDADDENTROPY, &req) < 0) {
		perror("jitterseed: RNDADDENTROPY");
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}
