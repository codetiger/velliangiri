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
#include <unistd.h>

#include "sgx-uapi.h"

static const struct {
	uint32_t id;
	const char *name;
	const char *fmt;
} params[] = {
	{ APPLE_SGX_PARAM_UAPI_VERSION,      "UAPI_VERSION",      "%llu" },
	{ APPLE_SGX_PARAM_CORE_ID,           "CORE_ID",           "0x%08llx" },
	{ APPLE_SGX_PARAM_CORE_REVISION,     "CORE_REVISION",     "0x%08llx" },
	{ APPLE_SGX_PARAM_NUM_CORES,         "NUM_CORES",         "%llu" },
	{ APPLE_SGX_PARAM_CLOCK_KHZ,         "CLOCK_KHZ",         "%llu" },
	{ APPLE_SGX_PARAM_UKERNEL_UP,        "UKERNEL_UP",        "%llu" },
	{ APPLE_SGX_PARAM_VA_START,          "VA_START",          "0x%08llx" },
	{ APPLE_SGX_PARAM_VA_END,            "VA_END",            "0x%08llx" },
	{ APPLE_SGX_PARAM_CODE_BASE,         "CODE_BASE",         "0x%08llx" },
	{ APPLE_SGX_PARAM_CODE_VA_START,     "CODE_VA_START",     "0x%08llx" },
	{ APPLE_SGX_PARAM_CODE_VA_END,       "CODE_VA_END",       "0x%08llx" },
	{ APPLE_SGX_PARAM_FB_VA,             "FB_VA",             "0x%08llx" },
	{ APPLE_SGX_PARAM_FB_WIDTH,          "FB_WIDTH",          "%llu" },
	{ APPLE_SGX_PARAM_FB_HEIGHT,         "FB_HEIGHT",         "%llu" },
	{ APPLE_SGX_PARAM_FB_STRIDE,         "FB_STRIDE",         "%llu" },
	{ APPLE_SGX_PARAM_RENDERS_SUBMITTED, "RENDERS_SUBMITTED", "%llu" },
	{ APPLE_SGX_PARAM_RENDERS_DONE,      "RENDERS_DONE",      "%llu" },
	{ APPLE_SGX_PARAM_RENDERS_TIMED_OUT, "RENDERS_TIMED_OUT", "%llu" },
	{ APPLE_SGX_PARAM_UKERNEL_BOOTS,     "UKERNEL_BOOTS",     "%llu" },
	{ APPLE_SGX_PARAM_PB_VA,             "PB_VA",             "0x%08llx" },
	{ APPLE_SGX_PARAM_TA_HEAP_START,     "TA_HEAP_START",     "0x%08llx" },
	{ APPLE_SGX_PARAM_TA_HEAP_END,       "TA_HEAP_END",       "0x%08llx" },
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
