// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise the SGX render node's GEM path (/bin/sgxgem)
 *
 * Usage:
 *   sgxgem [-c] [/dev/dri/renderD128]
 *   sgxgem -i /dev/dri/card1
 *
 * Creates a small buffer, maps it write-combined, writes a pattern, asks the
 * kernel where it lives in the GPU address space and waits for it.  This is
 * the userspace half of the render node without a render: it checks the
 * GEM_CREATE, GEM_MMAP_OFFSET, GEM_INFO and GEM_WAIT ioctls.  With -c the
 * buffer is APPLE_SGX_BO_CONTIGUOUS and is also exported as a dma-buf, which
 * is the geometry a display controller imports.  With -i CARD it creates a
 * dumb buffer on CARD (the display), exports it and imports it into the
 * render node, checking that a foreign dma-buf gets mapped.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "sgx-uapi.h"

/* generic DRM ioctls, without libdrm */
struct drm_prime_handle {
	uint32_t handle;
	uint32_t flags;
	int32_t fd;
};

struct drm_mode_create_dumb {
	uint32_t height;
	uint32_t width;
	uint32_t bpp;
	uint32_t flags;
	uint32_t handle;
	uint32_t pitch;
	uint64_t size;
};

#define DRM_APPLE_PRIME_FD_TO_HANDLE	0x2e
#define DRM_APPLE_PRIME_HANDLE_TO_FD	0x2d
#define DRM_APPLE_MODE_CREATE_DUMB	0xb2

#define DRM_IOCTL_APPLE_PRIME_HANDLE_TO_FD \
	DRM_IOWR(DRM_APPLE_PRIME_HANDLE_TO_FD, struct drm_prime_handle)
#define DRM_IOCTL_APPLE_PRIME_FD_TO_HANDLE \
	DRM_IOWR(DRM_APPLE_PRIME_FD_TO_HANDLE, struct drm_prime_handle)
#define DRM_IOCTL_APPLE_MODE_CREATE_DUMB \
	DRM_IOWR(DRM_APPLE_MODE_CREATE_DUMB, struct drm_mode_create_dumb)

#define SGX_GEM_SIZE	4096

/* a display dumb buffer, exported and imported here */
static int fail(const char *what);

static int import_from(const char *card, const char *render)
{
	struct drm_mode_create_dumb cd = { .width = 64, .height = 64, .bpp = 32 };
	struct drm_prime_handle ph = { };
	struct drm_apple_sgx_gem_info gi = { };
	int disp, sgx, dmabuf, ret = 1;

	disp = open(card, O_RDWR);
	if (disp < 0)
		return fail(card);
	if (ioctl(disp, DRM_IOCTL_APPLE_MODE_CREATE_DUMB, &cd) < 0) {
		fail("MODE_CREATE_DUMB");
		close(disp);
		return 1;
	}
	printf("display dumb buffer %ux%u, pitch %u, %llu bytes\n", cd.width,
	       cd.height, cd.pitch, (unsigned long long)cd.size);

	ph.handle = cd.handle;
	ph.flags = O_CLOEXEC | O_RDWR;
	if (ioctl(disp, DRM_IOCTL_APPLE_PRIME_HANDLE_TO_FD, &ph) < 0) {
		fail("PRIME_HANDLE_TO_FD(display)");
		close(disp);
		return 1;
	}
	dmabuf = ph.fd;

	sgx = open(render, O_RDWR);
	if (sgx < 0) {
		fail(render);
		close(dmabuf);
		close(disp);
		return 1;
	}
	memset(&ph, 0, sizeof(ph));
	ph.fd = dmabuf;
	if (ioctl(sgx, DRM_IOCTL_APPLE_PRIME_FD_TO_HANDLE, &ph) < 0) {
		fail("PRIME_FD_TO_HANDLE(render node)");
		goto out;
	}
	gi.handle = ph.handle;
	if (ioctl(sgx, DRM_IOCTL_APPLE_SGX_GEM_INFO, &gi) < 0) {
		fail("GEM_INFO(imported)");
		goto out;
	}
	if (!gi.va || gi.size != cd.size) {
		fprintf(stderr, "sgxgem: imported buffer is not mapped (VA 0x%08x size %llu)\n",
			gi.va, (unsigned long long)gi.size);
		goto out;
	}
	printf("imported: handle %u at GPU 0x%08x, %llu bytes\n", ph.handle,
	       gi.va, (unsigned long long)gi.size);
	ret = 0;

out:
	close(sgx);
	close(dmabuf);
	close(disp);
	return ret;
}

static int fail(const char *what)
{
	fprintf(stderr, "sgxgem: %s: %s\n", what, strerror(errno));
	return 1;
}

int main(int argc, char **argv)
{
	const char *path = "/dev/dri/renderD128";
	struct drm_apple_sgx_get_param gp = { .param = 0 };
	struct drm_apple_sgx_gem_create gc = { .size = SGX_GEM_SIZE };
	struct drm_apple_sgx_gem_mmap_offset gm = { };
	struct drm_apple_sgx_gem_info gi = { };
	struct drm_apple_sgx_gem_wait gw = { };
	uint32_t *map;
	int contiguous = 0;
	int i, fd;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c")) {
			contiguous = 1;
		} else if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			return import_from(argv[++i], "/dev/dri/renderD128");
		} else {
			path = argv[i];
		}
	}
	gc.flags = contiguous ? APPLE_SGX_BO_CONTIGUOUS : 0;

	fd = open(path, O_RDWR);
	if (fd < 0)
		return fail(path);

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GET_PARAM, &gp) < 0)
		return fail("GET_PARAM");
	printf("UAPI_VERSION %llu\n", (unsigned long long)gp.value);

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &gc) < 0)
		return fail("GEM_CREATE");
	printf("GEM_CREATE handle %u, GPU VA 0x%08x\n", gc.handle, gc.va);

	gm.handle = gc.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &gm) < 0)
		return fail("GEM_MMAP_OFFSET");

	map = mmap(NULL, SGX_GEM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   gm.offset);
	if (map == MAP_FAILED)
		return fail("mmap");
	map[0] = 0x5a5a1234;
	map[SGX_GEM_SIZE / 4 - 1] = 0xa5a54321;
	if (map[0] != 0x5a5a1234 || map[SGX_GEM_SIZE / 4 - 1] != 0xa5a54321) {
		fprintf(stderr, "sgxgem: mmap did not keep the pattern\n");
		return 1;
	}
	printf("mmap offset 0x%llx, pattern written and read back\n",
	       (unsigned long long)gm.offset);

	gi.handle = gc.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_INFO, &gi) < 0)
		return fail("GEM_INFO");
	printf("GEM_INFO flags 0x%x, GPU VA 0x%08x, size %llu\n", gi.flags, gi.va,
	       (unsigned long long)gi.size);

	if (contiguous) {
		struct drm_prime_handle ph = { .handle = gc.handle,
					       .flags = O_CLOEXEC | O_RDWR };

		if (ioctl(fd, DRM_IOCTL_APPLE_PRIME_HANDLE_TO_FD, &ph) < 0)
			return fail("PRIME_HANDLE_TO_FD");
		printf("PRIME_HANDLE_TO_FD: dma-buf fd %d (one contiguous block)\n",
		       ph.fd);
		close(ph.fd);
	}

	gw.handle = gc.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_WAIT, &gw) < 0) {
		if (errno == EBUSY) {
			printf("GEM_WAIT: buffer busy (a render is using it)\n");
		} else {
			return fail("GEM_WAIT");
		}
	} else {
		printf("GEM_WAIT: idle\n");
	}

	munmap(map, SGX_GEM_SIZE);
	close(fd);
	printf("sgxgem: OK\n");
	return 0;
}
