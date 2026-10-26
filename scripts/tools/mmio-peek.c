// SPDX-License-Identifier: GPL-2.0-only
/*
 * Userspace MMIO diagnostic utility (/bin/peek) with SIGBUS/SIGSEGV recovery
 *
 * Usage:
 *   peek <phys_addr> [count_words]
 *   peek -w <phys_addr> <hex_val>
 */

#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf fault_env;
static volatile sig_atomic_t fault_sig;

static void fault_handler(int sig)
{
	fault_sig = sig;
	siglongjmp(fault_env, 1);
}

int main(int argc, char **argv)
{
	struct sigaction sa;
	int do_write = 0;
	unsigned long phys, count = 1, write_val = 0;
	unsigned long page_base, page_off, map_len;
	int fd;
	void *map;
	volatile uint32_t *p;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s [-w] <phys_addr> [count_or_val]\n", argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "-w") == 0) {
		if (argc < 4) {
			fprintf(stderr, "Usage: %s -w <phys_addr> <hex_val>\n", argv[0]);
			return 1;
		}
		do_write = 1;
		phys = strtoul(argv[2], NULL, 0);
		write_val = strtoul(argv[3], NULL, 0);
	} else {
		phys = strtoul(argv[1], NULL, 0);
		if (argc >= 3)
			count = strtoul(argv[2], NULL, 0);
		if (count == 0)
			count = 1;
		if (count > 4096)
			count = 4096;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fault_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);

	fd = open("/dev/mem", (do_write ? O_RDWR : O_RDONLY) | O_SYNC);
	if (fd < 0) {
		perror("open(/dev/mem)");
		return 1;
	}

	page_base = phys & ~0xfffUL;
	page_off = phys & 0xfffUL;
	map_len = ((page_off + count * 4 + 0xfffUL) & ~0xfffUL);

	map = mmap(NULL, map_len,
		   do_write ? (PROT_READ | PROT_WRITE) : PROT_READ,
		   MAP_SHARED, fd, (off_t)page_base);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	p = (volatile uint32_t *)((uintptr_t)map + page_off);

	if (do_write) {
		fault_sig = 0;
		if (sigsetjmp(fault_env, 1) == 0) {
			uint32_t rb;

			p[0] = (uint32_t)write_val;
			__asm__ volatile("dsb sy" ::: "memory");
			rb = p[0];
			printf("0x%08lx <= 0x%08lx (readback 0x%08x)\n",
			       phys, write_val, rb);
		} else {
			printf("0x%08lx <= <FAULT sig=%d>\n",
			       phys, (int)fault_sig);
		}
	} else {
		unsigned long i;

		for (i = 0; i < count; i++) {
			unsigned long cur = phys + i * 4;

			if ((i % 4) == 0) {
				if (i > 0)
					printf("\n");
				printf("0x%08lx:", cur);
			}
			fault_sig = 0;
			if (sigsetjmp(fault_env, 1) == 0) {
				uint32_t v = p[i];

				printf(" %08x", v);
			} else {
				printf(" <FAULT:%d>", (int)fault_sig);
			}
		}
		printf("\n");
	}

	munmap(map, map_len);
	close(fd);
	return 0;
}
