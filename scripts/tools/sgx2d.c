// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise the SGX 2D engine (/bin/sgx2d)
 *
 * Usage:
 *   sgx2d [-f] [/dev/dri/renderD128]
 *
 * -f puts the buffers at the addresses the debugfs replay used
 * (0x98104000 and 0x9e000000) instead of the ones the kernel picks.
 *
 * Creates a 64x64 four-quadrant source buffer and scales it over two
 * destination buffers of different sizes with DRM_IOCTL_APPLE_SGX_BLIT, and
 * checks the four quadrant centres of each result.  Two sizes matter: the
 * engine's programs carry the destination's address, stride and size, so
 * the second blit makes the GPU read code it has run before at the same
 * address with different words in it.
 *
 * Nothing here goes through debugfs: the kernel builds the USSE and PDS
 * programs and the transfer command itself.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define DRM_IOCTL_BASE		'd'
#define DRM_COMMAND_BASE	0x40
#define DRM_IOWR(nr, type)	_IOWR(DRM_IOCTL_BASE, nr, type)

struct drm_apple_sgx_gem_create {
	uint64_t size;
	uint32_t flags;
	uint32_t handle;
	uint32_t va;
	uint32_t pad;
};

struct drm_apple_sgx_gem_mmap_offset {
	uint32_t handle;
	uint32_t flags;
	uint64_t offset;
};

struct drm_apple_sgx_blit {
	uint32_t src_handle;
	uint32_t dst_handle;
	uint32_t src_width, src_height, src_stride;
	uint32_t dst_width, dst_height, dst_stride;
	uint32_t flags;
	uint32_t pad;
};

#define DRM_APPLE_SGX_GEM_CREATE	0x01
#define DRM_APPLE_SGX_GEM_MMAP_OFFSET	0x02
#define DRM_APPLE_SGX_BLIT		0x07

#define DRM_IOCTL_APPLE_SGX_GEM_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_CREATE, struct drm_apple_sgx_gem_create)
#define DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_GEM_MMAP_OFFSET, struct drm_apple_sgx_gem_mmap_offset)
#define DRM_IOCTL_APPLE_SGX_BLIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APPLE_SGX_BLIT, struct drm_apple_sgx_blit)

#define APPLE_SGX_BO_FIXED_VA		(1u << 0)
#define APPLE_SGX_BO_CONTIGUOUS		(1u << 5)

#define SRC_W		64
#define SRC_H		64
#define SRC_STRIDE	(SRC_W * 4)

static int fd;
static int fixed_va;
static uint32_t src_quad[SRC_W * SRC_H];

#define SRC_FIXED_VA	0x98104000u
#define DST_FIXED_VA	0x9e000000u

static int fail(const char *what)
{
	fprintf(stderr, "sgx2d: %s: %s\n", what, strerror(errno));
	return 1;
}

static void *bo_map(struct drm_apple_sgx_gem_create *gc, size_t size)
{
	struct drm_apple_sgx_gem_mmap_offset gm = { .handle = gc->handle };
	void *p;

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &gm) < 0)
		exit(fail("GEM_MMAP_OFFSET"));
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, gm.offset);
	if (p == MAP_FAILED)
		exit(fail("mmap"));
	return p;
}

static struct drm_apple_sgx_gem_create bo_new(uint64_t size, uint32_t flags, uint32_t va)
{
	struct drm_apple_sgx_gem_create gc = { .size = size, .flags = flags, .va = va };

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &gc) < 0)
		exit(fail("GEM_CREATE"));
	return gc;
}

/* scale the source over a new destination of dw x dh and check it */
static uint32_t src_bo_va;

static int blit_to(uint32_t src_handle, uint32_t dw, uint32_t dh)
{
	struct drm_apple_sgx_gem_create dst_bo;
	struct drm_apple_sgx_blit blit = { 0 };
	uint32_t stride = dw * 4, *dst, x, y;
	int bad = 0;

	dst_bo = bo_new((uint64_t)stride * dh, APPLE_SGX_BO_CONTIGUOUS |
			(fixed_va ? APPLE_SGX_BO_FIXED_VA : 0),
			fixed_va ? DST_FIXED_VA : 0);
	dst = bo_map(&dst_bo, (size_t)stride * dh);

	blit.src_handle = src_handle;
	blit.dst_handle = dst_bo.handle;
	blit.src_width = SRC_W;
	blit.src_height = SRC_H;
	blit.src_stride = SRC_STRIDE;
	blit.dst_width = dw;
	blit.dst_height = dh;
	blit.dst_stride = stride;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_BLIT, &blit) < 0) {
		fail("BLIT");
		return 1;
	}
	printf("blit %ux%u -> %ux%u (GPU 0x%08x -> 0x%08x)\n", SRC_W, SRC_H, dw, dh,
	       src_bo_va, dst_bo.va);

	for (y = 0; y < 2; y++)
		for (x = 0; x < 2; x++) {
			uint32_t dx = dw / 4 + x * (dw / 2);
			uint32_t dy = dh / 4 + y * (dh / 2);
			uint32_t want = src_quad[(y * (SRC_H / 2) + SRC_H / 4) * SRC_W +
						 x * (SRC_W / 2) + SRC_W / 4];
			uint32_t got = dst[dy * (stride / 4) + dx];

			printf("  (%4u,%4u) 0x%08x %s\n", dx, dy, got,
			       got == want ? "ok" : "WRONG");
			if (got != want)
				bad = 1;
		}

	munmap(dst, (size_t)stride * dh);
	return bad;
}

int main(int argc, char **argv)
{
	const char *path = "/dev/dri/renderD128";
	struct drm_apple_sgx_gem_create src_bo;
	uint32_t *src, x, y;
	int bad = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-f"))
			fixed_va = 1;
		else
			path = argv[i];
	}
	fd = open(path, O_RDWR);
	if (fd < 0)
		return fail(path);

	src_bo = bo_new(SRC_STRIDE * SRC_H,
			fixed_va ? APPLE_SGX_BO_FIXED_VA : 0,
			fixed_va ? SRC_FIXED_VA : 0);
	src = bo_map(&src_bo, SRC_STRIDE * SRC_H);

	/* the four quadrants, as words; the engine copies them through */
	for (y = 0; y < SRC_H; y++)
		for (x = 0; x < SRC_W; x++) {
			uint32_t c = x < SRC_W / 2 ?
				(y < SRC_H / 2 ? 0xffff0000u : 0xff0000ffu) :
				(y < SRC_H / 2 ? 0xff00ff00u : 0xffffffffu);

			src[y * (SRC_STRIDE / 4) + x] = c;
			src_quad[y * SRC_W + x] = c;
		}

	/* two sizes: the second makes the GPU run code it has run before */
	src_bo_va = src_bo.va;
	bad |= blit_to(src_bo.handle, 1280, 720);
	bad |= blit_to(src_bo.handle, 640, 360);

	munmap(src, SRC_STRIDE * SRC_H);
	close(fd);
	if (bad) {
		fprintf(stderr, "sgx2d: the destination does not match\n");
		return 1;
	}
	printf("sgx2d: OK\n");
	return 0;
}
