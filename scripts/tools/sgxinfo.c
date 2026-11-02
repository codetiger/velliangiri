// SPDX-License-Identifier: GPL-2.0-only
/*
 * Print the SGX render node's parameters (/bin/sgxinfo)
 *
 * Usage:
 *   sgxinfo [/dev/dri/renderD128]
 *
 * Opens the render node and asks the kernel for every parameter the
 * experimental interface exposes.  It is the quickest way to check that the
 * microkernel is up and that the GPU address windows are where userspace
 * expects them.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define DRM_IOCTL_BASE		'd'
#define DRM_COMMAND_BASE	0x40
#define DRM_IOWR(nr, type)	_IOWR(DRM_IOCTL_BASE, nr, type)

struct drm_apple_sgx_get_param {
	uint32_t param;
	uint32_t pad;
	uint64_t value;
};

#define DRM_APPLE_SGX_GET_PARAM			0x00
#define DRM_IOCTL_APPLE_SGX_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GET_PARAM, struct drm_apple_sgx_get_param)

static const struct {
	uint32_t id;
	const char *name;
	const char *fmt;
} params[] = {
	{ 0,  "UAPI_VERSION",        "%llu" },
	{ 1,  "CORE_ID",             "0x%08llx" },
	{ 2,  "CORE_REVISION",       "0x%08llx" },
	{ 3,  "NUM_CORES",           "%llu" },
	{ 4,  "CLOCK_KHZ",           "%llu" },
	{ 5,  "UKERNEL_UP",          "%llu" },
	{ 6,  "VA_START",            "0x%08llx" },
	{ 7,  "VA_END",              "0x%08llx" },
	{ 8,  "CODE_BASE",           "0x%08llx" },
	{ 9,  "CODE_VA_START",       "0x%08llx" },
	{ 10, "CODE_VA_END",         "0x%08llx" },
	{ 11, "FB_VA",               "0x%08llx" },
	{ 12, "FB_WIDTH",            "%llu" },
	{ 13, "FB_HEIGHT",           "%llu" },
	{ 14, "FB_STRIDE",           "%llu" },
	{ 15, "RENDERS_SUBMITTED",   "%llu" },
	{ 16, "RENDERS_DONE",        "%llu" },
	{ 17, "RENDERS_TIMED_OUT",   "%llu" },
	{ 18, "UKERNEL_BOOTS",       "%llu" },
	{ 19, "PB_VA",               "0x%08llx" },
	{ 20, "TA_HEAP_START",       "0x%08llx" },
	{ 21, "TA_HEAP_END",         "0x%08llx" },
};

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	unsigned int i;
	int fd;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "sgxinfo: %s: %s\n", path, strerror(errno));
		return 1;
	}

	for (i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct drm_apple_sgx_get_param p = { .param = params[i].id };

		if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GET_PARAM, &p) < 0) {
			fprintf(stderr, "sgxinfo: %s: %s\n", params[i].name,
				strerror(errno));
			close(fd);
			return 1;
		}
		printf("%-20s ", params[i].name);
		printf(params[i].fmt, (unsigned long long)p.value);
		putchar('\n');
	}

	close(fd);
	return 0;
}
