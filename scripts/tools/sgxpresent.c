// SPDX-License-Identifier: GPL-2.0-only
/*
 * Show an SGX buffer on the HDMI output (/bin/sgxpresent)
 *
 * Usage:
 *   sgxpresent [-b] [seconds] [gpu-va] [/dev/dri/renderD128] [/dev/dri/card1]
 *
 * The clean presentation path, with no CPU copy and no debugfs:
 *
 *   1. a physically contiguous buffer on the SGX render node
 *      (APPLE_SGX_BO_CONTIGUOUS), so its dma-buf has a one-entry table;
 *   2. export it with PRIME_HANDLE_TO_FD;
 *   3. import the dma-buf into the display driver and scan it out.
 *
 * With -b the buffer is not filled by the CPU: the 2D engine draws a 64x64
 * four-quadrant source scaled over the whole screen (DRM_APPLE_SGX_BLIT),
 * so what the display shows came out of the GPU.
 *
 * The render node is closed once the display holds the buffer: the GPU
 * mapping stays as long as the imported framebuffer exists, and the micro-
 * kernel's debugfs controls are usable again for a transfer into the same
 * address.
 *
 * Without -b the buffer is filled with colour bars here; a GPU transfer
 * draws over it.  After the given number of seconds the display's previous
 * framebuffer is put back, so the console returns.
 */

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define SGX_COMMAND_BASE	0x40

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

struct drm_apple_sgx_gem_info {
	uint32_t handle;
	uint32_t flags;
	uint32_t va;
	uint32_t pad;
	uint64_t size;
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
#define DRM_APPLE_SGX_GEM_INFO		0x05
#define DRM_APPLE_SGX_BLIT		0x07

#define DRM_IOCTL_APPLE_SGX_GEM_CREATE \
	DRM_IOWR(SGX_COMMAND_BASE + DRM_APPLE_SGX_GEM_CREATE, struct drm_apple_sgx_gem_create)
#define DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET \
	DRM_IOWR(SGX_COMMAND_BASE + DRM_APPLE_SGX_GEM_MMAP_OFFSET, struct drm_apple_sgx_gem_mmap_offset)
#define DRM_IOCTL_APPLE_SGX_GEM_INFO \
	DRM_IOWR(SGX_COMMAND_BASE + DRM_APPLE_SGX_GEM_INFO, struct drm_apple_sgx_gem_info)
#define DRM_IOCTL_APPLE_SGX_BLIT \
	DRM_IOWR(SGX_COMMAND_BASE + DRM_APPLE_SGX_BLIT, struct drm_apple_sgx_blit)

#define APPLE_SGX_BO_FIXED_VA		(1u << 0)
#define APPLE_SGX_BO_CONTIGUOUS		(1u << 5)

/* Above the TA heap's bottom and below the framebuffer window the kernel
 * reserves, so it cannot collide with what the kernel picks by itself. */
#define DEFAULT_VA			0x8d000000u

#define ARRAY_SIZE(a)			(sizeof(a) / sizeof((a)[0]))

static void fail(const char *what)
{
	fprintf(stderr, "sgxpresent: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg) < 0)
		fail(what);
}

int main(int argc, char **argv)
{
	const char *render = "/dev/dri/renderD128";
	const char *card = NULL;
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo *modes, mode;
	struct drm_mode_crtc old_crtc = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	struct drm_mode_fb_cmd2 fb = { 0 };
	struct drm_prime_handle prime = { 0 };
	struct drm_apple_sgx_gem_create gc = { 0 };
	struct drm_apple_sgx_gem_mmap_offset gm = { 0 };
	struct drm_apple_sgx_gem_info gi = { 0 };
	uint32_t conn_ids[8], crtc_ids[8], enc_ids[8];
	uint32_t *px, x, y, pitch;
	uint32_t va = DEFAULT_VA;
	int seconds = 10, sgx, disp, dmabuf, blit = 0;

	if (argc > 1 && !strcmp(argv[1], "-b")) {
		blit = 1;
		argv++;
		argc--;
	}
	if (argc > 1)
		seconds = atoi(argv[1]);
	if (argc > 2) {
		va = strtoul(argv[2], NULL, 0);
		if (va & 0xffff) {
			fprintf(stderr, "sgxpresent: GPU address must be 64 KiB aligned\n");
			return 2;
		}
	}
	if (argc > 3)
		render = argv[3];
	if (argc > 4)
		card = argv[4];

	/*
	 * The SGX render node takes DRM minor 0, so the display is usually
	 * card1; try the obvious ones and keep the first that does KMS.
	 */
	{
		static const char *cards[] = { "/dev/dri/card1", "/dev/dri/card0" };
		unsigned int i;

		disp = -1;
		if (card) {
			disp = open(card, O_RDWR);
			if (disp < 0)
				fail(card);
		} else {
			for (i = 0; i < ARRAY_SIZE(cards); i++) {
				disp = open(cards[i], O_RDWR);
				if (disp >= 0 &&
				    ioctl(disp, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0)
					break;
				if (disp >= 0)
					close(disp);
				disp = -1;
				memset(&res, 0, sizeof(res));
			}
			if (disp < 0) {
				fprintf(stderr, "sgxpresent: no KMS display device\n");
				return 1;
			}
		}
	}
	sgx = open(render, O_RDWR);
	if (sgx < 0)
		fail(render);

	/* the display's current mode: the buffer has to match it */
	res.connector_id_ptr = (uintptr_t)conn_ids;
	res.crtc_id_ptr = (uintptr_t)crtc_ids;
	res.encoder_id_ptr = (uintptr_t)enc_ids;
	res.count_connectors = res.count_crtcs = res.count_encoders = 8;
	xioctl(disp, DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	if (!res.count_connectors || !res.count_crtcs) {
		fprintf(stderr, "sgxpresent: no connector or CRTC\n");
		return 1;
	}

	conn.connector_id = conn_ids[0];
	xioctl(disp, DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR");
	modes = calloc(conn.count_modes + 1, sizeof(*modes));
	if (!modes)
		fail("calloc");
	conn.modes_ptr = (uintptr_t)modes;
	conn.count_props = conn.count_encoders = 0;
	xioctl(disp, DRM_IOCTL_MODE_GETCONNECTOR, &conn, "GETCONNECTOR modes");
	if (!conn.count_modes) {
		fprintf(stderr, "sgxpresent: connector has no modes\n");
		return 1;
	}

	old_crtc.crtc_id = crtc_ids[0];
	xioctl(disp, DRM_IOCTL_MODE_GETCRTC, &old_crtc, "GETCRTC");
	mode = old_crtc.mode_valid ? old_crtc.mode : modes[0];

	/* one contiguous SGX buffer, at a GPU address the transfer can name */
	pitch = (mode.hdisplay * 4 + 63) & ~63u;
	gc.size = (uint64_t)pitch * mode.vdisplay;
	gc.flags = APPLE_SGX_BO_FIXED_VA | APPLE_SGX_BO_CONTIGUOUS;
	gc.va = va;
	xioctl(sgx, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &gc, "GEM_CREATE contiguous");
	gi.handle = gc.handle;
	xioctl(sgx, DRM_IOCTL_APPLE_SGX_GEM_INFO, &gi, "GEM_INFO");
	printf("SGX buffer: handle %u, GPU address 0x%08x, %llu bytes, %u x %u\n",
	       gc.handle, gc.va, (unsigned long long)gc.size, mode.hdisplay,
	       mode.vdisplay);

	gm.handle = gc.handle;
	xioctl(sgx, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &gm, "GEM_MMAP_OFFSET");
	px = mmap(NULL, gc.size, PROT_READ | PROT_WRITE, MAP_SHARED, sgx, gm.offset);
	if (px == MAP_FAILED)
		fail("mmap");
	for (y = 0; y < mode.vdisplay; y++)
		for (x = 0; x < mode.hdisplay; x++) {
			static const uint32_t bars[] = {
				0x000000, 0x000080, 0x008000, 0x008080,
				0x800000, 0x800080, 0x808000, 0x808080,
			};
			uint32_t c = bars[x * 8 / mode.hdisplay];

			if (x < 4 || y < 4 || x + 4 >= mode.hdisplay ||
			    y + 4 >= mode.vdisplay)
				c = 0xffffff;
			px[y * (pitch / 4) + x] = c;
		}

	/* with -b the screen's pixels come from the 2D engine instead */
	if (blit) {
		struct drm_apple_sgx_gem_create sgc = { .size = 64 * 64 * 4 };
		struct drm_apple_sgx_gem_mmap_offset sgm = { };
		struct drm_apple_sgx_blit b = { };
		uint32_t *sp, i;

		xioctl(sgx, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &sgc,
		       "GEM_CREATE(blit source)");
		sgm.handle = sgc.handle;
		xioctl(sgx, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &sgm,
		       "GEM_MMAP_OFFSET(blit source)");
		sp = mmap(NULL, sgc.size, PROT_READ | PROT_WRITE, MAP_SHARED, sgx,
			  sgm.offset);
		if (sp == MAP_FAILED)
			fail("mmap(blit source)");
		for (i = 0; i < sgc.size / 4; i++) {
			unsigned int sx = i % 64, sy = i / 64;

			sp[i] = sx < 32 ? (sy < 32 ? 0xffff0000u : 0xff0000ffu)
					: (sy < 32 ? 0xff00ff00u : 0xffffffffu);
		}
		munmap(sp, sgc.size);

		b.src_handle = sgc.handle;
		b.dst_handle = gc.handle;
		b.src_width = 64;
		b.src_height = 64;
		b.src_stride = 64 * 4;
		b.dst_width = mode.hdisplay;
		b.dst_height = mode.vdisplay;
		b.dst_stride = pitch;
		xioctl(sgx, DRM_IOCTL_APPLE_SGX_BLIT, &b, "BLIT");
		printf("2D engine: 64x64 -> %ux%u\n", mode.hdisplay,
		       mode.vdisplay);
	}
	munmap(px, gc.size);

	/* export it and let the display import it as a scanout buffer */
	prime.handle = gc.handle;
	prime.flags = DRM_CLOEXEC | DRM_RDWR;
	xioctl(sgx, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime, "PRIME_HANDLE_TO_FD");
	dmabuf = prime.fd;

	{
		struct drm_prime_handle imp = { .fd = dmabuf, .flags = 0 };

		xioctl(disp, DRM_IOCTL_PRIME_FD_TO_HANDLE, &imp,
		       "PRIME_FD_TO_HANDLE(display)");
		fb.handles[0] = imp.handle;
	}
	printf("dma-buf fd %d imported by the display, handle %u\n", dmabuf,
	       fb.handles[0]);

	fb.width = mode.hdisplay;
	fb.height = mode.vdisplay;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.pitches[0] = pitch;
	fb.offsets[0] = 0;
	xioctl(disp, DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2");

	crtc.crtc_id = crtc_ids[0];
	crtc.fb_id = fb.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)conn_ids;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	xioctl(disp, DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC");
	printf("scanning out %ux%u (pitch %u) for %d s\n", mode.hdisplay,
	       mode.vdisplay, pitch, seconds);
	fflush(stdout);

	/*
	 * The display's framebuffer holds the buffer now, so this file can go.
	 * The GPU mapping stays (the buffer object lives as long as the dma-buf
	 * does), and the microkernel's debugfs controls stop being refused as
	 * "render node busy", which is what a transfer into the fixed GPU
	 * address from another process needs.
	 */
	close(sgx);
	close(dmabuf);
	sgx = -1;

	sleep(seconds);

	crtc.fb_id = old_crtc.fb_id;
	crtc.mode = old_crtc.mode;
	crtc.mode_valid = old_crtc.mode_valid;
	xioctl(disp, DRM_IOCTL_MODE_SETCRTC, &crtc, "SETCRTC restore");
	close(disp);
	printf("sgxpresent: done\n");
	return 0;
}
