// SPDX-License-Identifier: GPL-2.0-only
/*
 * Giri on Velliangiri OS — 1280x720 Display Surface & v1.2 Compositor Primitives
 *
 * Uses a 1280x720 XRGB8888 backbuffer and copies damaged rectangles to the
 * scanout mapping (/dev/fb0 or /dev/dri/card0 dumb buffer). Bypasses the
 * hardware gamma LUT (gamma_brightness = 100 -> OUT_GAMMA_EN = 0) so dark
 * mountain facets and bright highlights retain full 24-bit RGB fidelity on TV.
 */

#include <errno.h>
#include <fcntl.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#ifdef __linux__
#include <linux/fb.h>
#include <linux/kd.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "tvshell.h"
#include "shell_assets.h"
#include "shell_gpu.h"

/* Minimal DRM/KMS UAPI definitions so static builds need no libdrm */
#define DRM_IOCTL_BASE			'd'
#define DRM_IO(nr)			_IO(DRM_IOCTL_BASE, nr)
#define DRM_IOWR(nr, type)		_IOWR(DRM_IOCTL_BASE, nr, type)

#define DRM_IOCTL_SET_MASTER		DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER		DRM_IO(0x1f)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD	DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_MODE_GETRESOURCES	DRM_IOWR(0xa0, struct drm_mode_card_res)
#define DRM_IOCTL_MODE_GETCRTC		DRM_IOWR(0xa1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC		DRM_IOWR(0xa2, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_GETCONNECTOR	DRM_IOWR(0xa7, struct drm_mode_get_connector)
#define DRM_IOCTL_MODE_ADDFB		DRM_IOWR(0xae, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_RMFB		DRM_IOWR(0xaf, unsigned int)
#define DRM_IOCTL_MODE_CREATE_DUMB	DRM_IOWR(0xb2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB		DRM_IOWR(0xb3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB	DRM_IOWR(0xb4, struct drm_mode_destroy_dumb)

struct drm_mode_modeinfo {
	uint32_t clock;
	uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t vrefresh;
	uint32_t flags;
	uint32_t type;
	char name[32];
};

struct drm_mode_card_res {
	uint64_t fb_id_ptr;
	uint64_t crtc_id_ptr;
	uint64_t connector_id_ptr;
	uint64_t encoder_id_ptr;
	uint32_t count_fbs;
	uint32_t count_crtcs;
	uint32_t count_connectors;
	uint32_t count_encoders;
	uint32_t min_width, max_width;
	uint32_t min_height, max_height;
};

struct drm_mode_crtc {
	uint64_t set_connectors_ptr;
	uint32_t count_connectors;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t x, y;
	uint32_t gamma_size;
	uint32_t mode_valid;
	struct drm_mode_modeinfo mode;
};

struct drm_mode_get_connector {
	uint64_t encoders_ptr;
	uint64_t modes_ptr;
	uint64_t props_ptr;
	uint64_t prop_values_ptr;
	uint32_t count_modes;
	uint32_t count_props;
	uint32_t count_encoders;
	uint32_t encoder_id;
	uint32_t connector_id;
	uint32_t connector_type;
	uint32_t connector_type_id;
	uint32_t connection;
	uint32_t mm_width, mm_height;
	uint32_t subpixel;
	uint32_t pad;
};

struct drm_mode_fb_cmd {
	uint32_t fb_id;
	uint32_t width, height;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t depth;
	uint32_t handle;
};

struct drm_mode_create_dumb {
	uint32_t height, width, bpp, flags;
	uint32_t handle, pitch;
	uint64_t size;
};

struct drm_mode_map_dumb {
	uint32_t handle;
	uint32_t pad;
	uint64_t offset;
};

struct drm_mode_destroy_dumb {
	uint32_t handle;
};

struct drm_prime_handle {
	uint32_t handle;
	uint32_t flags;
	int32_t fd;
};

#define SHELL_DRM_BUF_COUNT		3

struct drm_scanout_buf {
	uint32_t handle;
	uint32_t fb_id;
	uint32_t *map;
	int dmabuf_fd;
};

static struct {
	bool headless;
	bool use_drm;
	int drm_fd;
	int fb_fd;
	int tty_fd;
	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t fb_id;
	uint32_t orig_fb_id;
	struct drm_scanout_buf bufs[SHELL_DRM_BUF_COUNT];
	int active_buf;
	struct drm_mode_modeinfo mode_720p30;
	bool have_mode;

	uint32_t *scanout;
	size_t scanout_bytes;
	uint32_t scanout_pitch_px;
	uint32_t scanout_w;
	uint32_t scanout_h;

	uint32_t *backbuf;
	bool backbuf_owned;
	int last_gpu_buf;
#define MAX_DMG_RECTS		8
	struct {
		int x0, y0, x1, y1;
	} dmg_rects[MAX_DMG_RECTS];
	int dmg_count;
} disp = {
	.drm_fd = -1,
	.fb_fd = -1,
	.tty_fd = -1,
	.last_gpu_buf = -1,
	.bufs = {
		{ .dmabuf_fd = -1 },
		{ .dmabuf_fd = -1 },
		{ .dmabuf_fd = -1 },
	},
};

static void write_sysfs_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);

	if (fd >= 0) {
		(void)write(fd, val, strlen(val));
		close(fd);
	}
}

static inline uint32_t blend_pixel(uint32_t dst, uint32_t src, uint32_t a255)
{
	uint32_t a, inv_a, rb, g;

	if (a255 == 0)
		return dst;
	if (a255 >= 255)
		return src | 0xff000000U;

	a = a255 + (a255 >> 7);
	inv_a = 256U - a;
	rb = (((dst & 0x00ff00ffU) * inv_a + (src & 0x00ff00ffU) * a) >> 8) & 0x00ff00ffU;
	g  = (((dst & 0x0000ff00U) * inv_a + (src & 0x0000ff00U) * a) >> 8) & 0x0000ff00U;
	return 0xff000000U | rb | g;
}

static inline void fill_span_u32(uint32_t *dst, uint32_t color, int count)
{
	int i = 0;

#ifdef __ARM_NEON
	uint32x4_t v = vdupq_n_u32(color);

	for (; i + 8 <= count; i += 8) {
		vst1q_u32(dst + i, v);
		vst1q_u32(dst + i + 4, v);
	}
	for (; i + 4 <= count; i += 4)
		vst1q_u32(dst + i, v);
#endif
	for (; i < count; i++)
		dst[i] = color;
}

static inline void copy_span_u32(uint32_t *dst, const uint32_t *src, int count)
{
#ifdef __ARM_NEON
	int i = 0;

	for (; i + 16 <= count; i += 16) {
		__builtin_prefetch(src + i + 32, 0, 1);
		uint32x4_t v0 = vld1q_u32(src + i);
		uint32x4_t v1 = vld1q_u32(src + i + 4);
		uint32x4_t v2 = vld1q_u32(src + i + 8);
		uint32x4_t v3 = vld1q_u32(src + i + 12);

		vst1q_u32(dst + i, v0);
		vst1q_u32(dst + i + 4, v1);
		vst1q_u32(dst + i + 8, v2);
		vst1q_u32(dst + i + 12, v3);
	}
	for (; i + 4 <= count; i += 4)
		vst1q_u32(dst + i, vld1q_u32(src + i));
	for (; i < count; i++)
		dst[i] = src[i];
#else
	memcpy(dst, src, (size_t)count * sizeof(uint32_t));
#endif
}

static inline void blend_span_const_alpha(uint32_t *dst, int count,
					  uint32_t color, uint8_t alpha)
{
	int i = 0;

	if (count <= 0 || alpha == 0)
		return;
	if (alpha == 255) {
		fill_span_u32(dst, color | 0xff000000U, count);
		return;
	}

#ifdef __ARM_NEON
	if (count >= 8) {
		uint16x8_t sb = vdupq_n_u16((uint16_t)((color & 0xffU) * alpha));
		uint16x8_t sg = vdupq_n_u16((uint16_t)(((color >> 8) & 0xffU) * alpha));
		uint16x8_t sr = vdupq_n_u16((uint16_t)(((color >> 16) & 0xffU) * alpha));
		uint8x8_t inv_a = vdup_n_u8((uint8_t)(255U - alpha));
		uint8x8_t ff_v = vdup_n_u8(0xffU);

		for (; i + 8 <= count; i += 8) {
			uint8x8x4_t d = vld4_u8((const uint8_t *)(dst + i));

			d.val[0] = vrshrn_n_u16(vmlal_u8(sb, d.val[0], inv_a), 8);
			d.val[1] = vrshrn_n_u16(vmlal_u8(sg, d.val[1], inv_a), 8);
			d.val[2] = vrshrn_n_u16(vmlal_u8(sr, d.val[2], inv_a), 8);
			d.val[3] = ff_v;
			vst4_u8((uint8_t *)(dst + i), d);
		}
	}
#endif
	uint32_t a256 = (uint32_t)alpha + ((uint32_t)alpha >> 7);
	uint32_t inv_a256 = 256U - a256;
	uint32_t src_rb = (color & 0x00ff00ffU) * a256;
	uint32_t src_g  = (color & 0x0000ff00U) * a256;

	for (; i < count; i++) {
		uint32_t d = dst[i];
		uint32_t rb = (((d & 0x00ff00ffU) * inv_a256 + src_rb) >> 8) & 0x00ff00ffU;
		uint32_t g  = (((d & 0x0000ff00U) * inv_a256 + src_g) >> 8) & 0x0000ff00U;

		dst[i] = 0xff000000U | rb | g;
	}
}

/*
 * Precomputed 2x2 supersampled SDF corner coverage tables for radii 1..24 and
 * outline stroke thicknesses 1..3, eliminating all per-pixel sqrtf() and float
 * math during frame rendering.
 */
#define MAX_PRECOMP_RADIUS	24
#define MAX_PRECOMP_THICK	3

static bool g_corner_luts_ready;
static uint8_t g_corner_cov_lut[MAX_PRECOMP_RADIUS + 1][MAX_PRECOMP_RADIUS][MAX_PRECOMP_RADIUS];
static uint8_t g_corner_stroke_lut[MAX_PRECOMP_RADIUS + 1][MAX_PRECOMP_THICK + 1][MAX_PRECOMP_RADIUS][MAX_PRECOMP_RADIUS];

static inline float sdf_corner_dist(float px, float py, float r)
{
	float qx = r - px;
	float qy = r - py;
	float ox = qx > 0.0f ? qx : 0.0f;
	float oy = qy > 0.0f ? qy : 0.0f;
	float outside = sqrtf(ox * ox + oy * oy);
	float inside = (qx > qy ? qx : qy);

	if (inside > 0.0f)
		inside = 0.0f;
	return outside + inside - r;
}

static void init_corner_luts(void)
{
	int r, t, cy, cx, sy, sx;

	if (g_corner_luts_ready)
		return;

	for (r = 1; r <= MAX_PRECOMP_RADIUS; r++) {
		float rf = (float)r;

		for (cy = 0; cy < r; cy++) {
			for (cx = 0; cx < r; cx++) {
				float acc = 0.0f;

				for (sy = 0; sy < 2; sy++) {
					float py = (float)cy + 0.25f + 0.5f * (float)sy;

					for (sx = 0; sx < 2; sx++) {
						float px = (float)cx + 0.25f + 0.5f * (float)sx;
						float d = sdf_corner_dist(px, py, rf);
						float c = 0.5f - d;

						if (c > 1.0f)
							c = 1.0f;
						else if (c < 0.0f)
							c = 0.0f;
						acc += c;
					}
				}
				g_corner_cov_lut[r][cy][cx] = (uint8_t)(acc * 63.75f + 0.5f);

				for (t = 1; t <= MAX_PRECOMP_THICK; t++) {
					float tf = (float)t;
					float sacc = 0.0f;

					for (sy = 0; sy < 2; sy++) {
						float py = (float)cy + 0.25f + 0.5f * (float)sy;

						for (sx = 0; sx < 2; sx++) {
							float px = (float)cx + 0.25f + 0.5f * (float)sx;
							float d = sdf_corner_dist(px, py, rf);
							float outer_c = 0.5f - d;
							float inner_c = 0.5f + d + tf;

							if (outer_c > 1.0f)
								outer_c = 1.0f;
							else if (outer_c < 0.0f)
								outer_c = 0.0f;
							if (inner_c > 1.0f)
								inner_c = 1.0f;
							else if (inner_c < 0.0f)
								inner_c = 0.0f;
							sacc += outer_c * inner_c;
						}
					}
					g_corner_stroke_lut[r][t][cy][cx] = (uint8_t)(sacc * 63.75f + 0.5f);
				}
			}
		}
	}
	g_corner_luts_ready = true;
}

uint8_t shell_sdf_corner_cov(int radius, int cy, int cx)
{
	init_corner_luts();
	if (radius < 1 || radius > MAX_PRECOMP_RADIUS ||
	    cy < 0 || cy >= radius || cx < 0 || cx >= radius)
		return 255;
	return g_corner_cov_lut[radius][cy][cx];
}

uint8_t shell_sdf_corner_stroke(int radius, int thickness, int cy, int cx)
{
	init_corner_luts();
	if (radius < 1 || radius > MAX_PRECOMP_RADIUS ||
	    thickness < 1 || thickness > MAX_PRECOMP_THICK ||
	    cy < 0 || cy >= radius || cx < 0 || cx >= radius)
		return 0;
	return g_corner_stroke_lut[radius][thickness][cy][cx];
}

static bool drm_find_720p30(int fd)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t conns[8] = { 0 }, crtcs[8] = { 0 };
	struct drm_mode_get_connector conn = { 0 };
	struct drm_mode_modeinfo modes[64];
	uint32_t i;

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return false;
	if (res.count_connectors == 0 || res.count_crtcs == 0)
		return false;
	if (res.count_connectors > 8)
		res.count_connectors = 8;
	if (res.count_crtcs > 8)
		res.count_crtcs = 8;
	res.connector_id_ptr = (uintptr_t)conns;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.fb_id_ptr = 0;
	res.encoder_id_ptr = 0;
	res.count_fbs = 0;
	res.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return false;

	disp.conn_id = conns[0];
	disp.crtc_id = crtcs[0];

	conn.connector_id = disp.conn_id;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || conn.count_modes == 0)
		return false;
	if (conn.count_modes > 64)
		conn.count_modes = 64;
	conn.modes_ptr = (uintptr_t)modes;
	conn.props_ptr = 0;
	conn.prop_values_ptr = 0;
	conn.encoders_ptr = 0;
	conn.count_props = 0;
	conn.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		return false;

	for (i = 0; i < conn.count_modes; i++) {
		if (modes[i].hdisplay == 1280 && modes[i].vdisplay == 720 &&
		    modes[i].vrefresh == 30) {
			disp.mode_720p30 = modes[i];
			disp.have_mode = true;
			return true;
		}
	}
	{
		/* CEA-861 VIC 62: 1280x720@30 (74.25 MHz, positive syncs) */
		static const struct drm_mode_modeinfo cea_720p30 = {
			.clock = 74250,
			.hdisplay = 1280,
			.hsync_start = 3040,
			.hsync_end = 3080,
			.htotal = 3300,
			.vdisplay = 720,
			.vsync_start = 725,
			.vsync_end = 730,
			.vtotal = 750,
			.vrefresh = 30,
			.flags = 0x05,
			.type = 0x40,
			.name = "1280x720",
		};
		disp.mode_720p30 = cea_720p30;
		disp.have_mode = true;
		return true;
	}
}

static void destroy_drm_buf(int idx)
{
	struct drm_scanout_buf *b = &disp.bufs[idx];

	if (b->dmabuf_fd >= 0) {
		close(b->dmabuf_fd);
		b->dmabuf_fd = -1;
	}
	if (b->map) {
		munmap(b->map, disp.scanout_bytes);
		b->map = NULL;
	}
	if (disp.drm_fd >= 0 && b->fb_id) {
		(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_RMFB, &b->fb_id);
		b->fb_id = 0;
	}
	if (disp.drm_fd >= 0 && b->handle) {
		struct drm_mode_destroy_dumb dreq = { .handle = b->handle };

		(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		b->handle = 0;
	}
}

static int setup_drm_scanout(void)
{
	struct drm_mode_crtc crtc = { 0 };
	const char *cards[] = { "/dev/dri/card0", "/dev/dri/card1" };
	int i;

	for (i = 0; i < 2; i++) {
		int fd = open(cards[i], O_RDWR | O_CLOEXEC);

		if (fd < 0)
			continue;
		(void)ioctl(fd, DRM_IOCTL_SET_MASTER, 0);
		if (drm_find_720p30(fd)) {
			disp.drm_fd = fd;
			break;
		}
		close(fd);
	}
	if (disp.drm_fd < 0)
		return -1;

	{
		struct drm_mode_crtc orig_crtc = { .crtc_id = disp.crtc_id };

		if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_GETCRTC, &orig_crtc) == 0 &&
		    orig_crtc.fb_id != 0)
			disp.orig_fb_id = orig_crtc.fb_id;
	}

	for (i = 0; i < SHELL_DRM_BUF_COUNT; i++) {
		struct drm_mode_create_dumb creq = { 0 };
		struct drm_mode_fb_cmd fbcmd = { 0 };
		struct drm_mode_map_dumb mreq = { 0 };
		struct drm_prime_handle preq = { 0 };

		disp.bufs[i].dmabuf_fd = -1;
		creq.width = SHELL_SCREEN_W;
		creq.height = SHELL_SCREEN_H;
		creq.bpp = 32;
		if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
			goto err;

		disp.bufs[i].handle = creq.handle;
		disp.scanout_bytes = (size_t)creq.size;
		disp.scanout_pitch_px = creq.pitch / 4;
		disp.scanout_w = SHELL_SCREEN_W;
		disp.scanout_h = SHELL_SCREEN_H;

		fbcmd.width = SHELL_SCREEN_W;
		fbcmd.height = SHELL_SCREEN_H;
		fbcmd.pitch = creq.pitch;
		fbcmd.bpp = 32;
		fbcmd.depth = 24;
		fbcmd.handle = creq.handle;
		if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_ADDFB, &fbcmd) < 0)
			goto err;
		disp.bufs[i].fb_id = fbcmd.fb_id;

		mreq.handle = creq.handle;
		if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
			goto err;

		disp.bufs[i].map = mmap(NULL, disp.scanout_bytes,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					disp.drm_fd, (off_t)mreq.offset);
		if (disp.bufs[i].map == MAP_FAILED) {
			disp.bufs[i].map = NULL;
			goto err;
		}

		preq.handle = creq.handle;
		preq.flags = O_CLOEXEC | O_RDWR;
		if (ioctl(disp.drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &preq) == 0)
			disp.bufs[i].dmabuf_fd = preq.fd;
	}

	disp.active_buf = 0;
	disp.fb_id = disp.bufs[0].fb_id;
	disp.scanout = disp.bufs[0].map;

	crtc.crtc_id = disp.crtc_id;
	crtc.fb_id = disp.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
	crtc.count_connectors = 1;
	crtc.mode = disp.mode_720p30;
	crtc.mode_valid = 1;
	(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);

	disp.use_drm = true;
	return 0;

err:
	for (i = 0; i < SHELL_DRM_BUF_COUNT; i++)
		destroy_drm_buf(i);
	close(disp.drm_fd);
	disp.drm_fd = -1;
	return -1;
}

static int setup_fbdev_scanout(void)
{
#ifdef __linux__
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);

	if (fd < 0)
		return -1;
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		close(fd);
		return -1;
	}
	disp.scanout_w = var.xres;
	disp.scanout_h = var.yres;
	disp.scanout_pitch_px = fix.line_length / 4;
	disp.scanout_bytes = (size_t)fix.line_length * var.yres;
	disp.scanout = mmap(NULL, disp.scanout_bytes, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	if (disp.scanout == MAP_FAILED) {
		disp.scanout = NULL;
		close(fd);
		return -1;
	}
	disp.fb_fd = fd;
	return 0;
#else
	return -1;
#endif
}

static void ensure_720p30_and_gamma(void)
{
	char cur[64] = { 0 };
	int fd;

	write_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/gamma_brightness", "100\n");
	write_sysfs_str("/sys/bus/platform/devices/3a200000.display-pipe/gamma_brightness", "100\n");

	fd = open("/sys/module/s5l8947x_drm/parameters/preferred_mode", O_RDONLY);
	if (fd >= 0) {
		ssize_t n = read(fd, cur, sizeof(cur) - 1);

		close(fd);
		if (n > 0 && strncmp(cur, "1280x720@30", 11) == 0)
			return;
	}
	write_sysfs_str("/sys/module/s5l8947x_drm/parameters/preferred_mode", "1280x720@30\n");
}

int shell_display_init(bool headless)
{
	init_corner_luts();
	disp.headless = headless;
	disp.last_gpu_buf = -1;
	shell_display_damage_all();

	if (headless) {
		if (shell_gpu_init(-1, 0, NULL, SHELL_SCREEN_W, SHELL_SCREEN_H)) {
			disp.backbuf = (uint32_t *)shell_gpu_get_headless_pixels(0);
			disp.backbuf_owned = false;
			return 0;
		}
		disp.backbuf = calloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H,
				      sizeof(uint32_t));
		if (!disp.backbuf)
			return -ENOMEM;
		disp.backbuf_owned = true;
		return 0;
	}

	ensure_720p30_and_gamma();

	disp.tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (disp.tty_fd >= 0) {
		(void)write(disp.tty_fd, "\033[?25l", 6);
#ifdef __linux__
		(void)ioctl(disp.tty_fd, KDSETMODE, KD_GRAPHICS);
#endif
	}

	if (setup_drm_scanout() == 0) {
		int dmabuf_fds[SHELL_DRM_BUF_COUNT] = {
			disp.bufs[0].dmabuf_fd,
			disp.bufs[1].dmabuf_fd,
			disp.bufs[2].dmabuf_fd,
		};

		if (shell_gpu_init(disp.drm_fd, SHELL_DRM_BUF_COUNT, dmabuf_fds,
				   SHELL_SCREEN_W, SHELL_SCREEN_H)) {
			disp.backbuf = NULL;
			disp.backbuf_owned = false;
			return 0;
		}
		disp.backbuf = calloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H,
				      sizeof(uint32_t));
		if (!disp.backbuf)
			return -ENOMEM;
		disp.backbuf_owned = true;
		return 0;
	}

	disp.backbuf = calloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H,
			      sizeof(uint32_t));
	if (!disp.backbuf)
		return -ENOMEM;
	disp.backbuf_owned = true;

	if (setup_fbdev_scanout() == 0)
		return 0;

	fprintf(stderr, "tvshell: no /dev/fb0 or /dev/dri/card*, running headless\n");
	disp.headless = true;
	return 0;
}

void shell_display_close(void)
{
	shell_gpu_close();
	if (disp.tty_fd >= 0) {
#ifdef __linux__
		(void)ioctl(disp.tty_fd, KDSETMODE, KD_TEXT);
#endif
		(void)write(disp.tty_fd, "\033[?25h", 6);
		close(disp.tty_fd);
		disp.tty_fd = -1;
	}
	if (disp.use_drm && disp.drm_fd >= 0) {
		int i;

		shell_display_release();
		for (i = 0; i < SHELL_DRM_BUF_COUNT; i++)
			destroy_drm_buf(i);
		disp.scanout = NULL;
		close(disp.drm_fd);
		disp.drm_fd = -1;
	} else if (disp.scanout) {
		munmap(disp.scanout, disp.scanout_bytes);
		disp.scanout = NULL;
	}
	if (disp.fb_fd >= 0) {
		close(disp.fb_fd);
		disp.fb_fd = -1;
	}
	if (disp.backbuf_owned)
		free(disp.backbuf);
	disp.backbuf = NULL;
	disp.backbuf_owned = false;
	disp.last_gpu_buf = -1;
}

uint32_t *shell_display_backbuffer(void)
{
	return disp.backbuf;
}

void shell_display_damage(int x, int y, int w, int h)
{
	int x1, y1, i;

	if (w <= 0 || h <= 0)
		return;
	if (x < 0) {
		w += x;
		x = 0;
	}
	if (y < 0) {
		h += y;
		y = 0;
	}
	x1 = x + w;
	y1 = y + h;
	if (x1 > SHELL_SCREEN_W)
		x1 = SHELL_SCREEN_W;
	if (y1 > SHELL_SCREEN_H)
		y1 = SHELL_SCREEN_H;
	if (x >= x1 || y >= y1)
		return;

	for (i = 0; i < disp.dmg_count; i++) {
		if (x <= disp.dmg_rects[i].x1 && x1 >= disp.dmg_rects[i].x0 &&
		    y <= disp.dmg_rects[i].y1 && y1 >= disp.dmg_rects[i].y0) {
			if (x < disp.dmg_rects[i].x0)
				disp.dmg_rects[i].x0 = x;
			if (y < disp.dmg_rects[i].y0)
				disp.dmg_rects[i].y0 = y;
			if (x1 > disp.dmg_rects[i].x1)
				disp.dmg_rects[i].x1 = x1;
			if (y1 > disp.dmg_rects[i].y1)
				disp.dmg_rects[i].y1 = y1;
			return;
		}
	}

	if (disp.dmg_count < MAX_DMG_RECTS) {
		disp.dmg_rects[disp.dmg_count].x0 = x;
		disp.dmg_rects[disp.dmg_count].y0 = y;
		disp.dmg_rects[disp.dmg_count].x1 = x1;
		disp.dmg_rects[disp.dmg_count].y1 = y1;
		disp.dmg_count++;
	} else {
		if (x < disp.dmg_rects[0].x0)
			disp.dmg_rects[0].x0 = x;
		if (y < disp.dmg_rects[0].y0)
			disp.dmg_rects[0].y0 = y;
		if (x1 > disp.dmg_rects[0].x1)
			disp.dmg_rects[0].x1 = x1;
		if (y1 > disp.dmg_rects[0].y1)
			disp.dmg_rects[0].y1 = y1;
	}
}

void shell_display_damage_all(void)
{
	disp.dmg_rects[0].x0 = 0;
	disp.dmg_rects[0].y0 = 0;
	disp.dmg_rects[0].x1 = SHELL_SCREEN_W;
	disp.dmg_rects[0].y1 = SHELL_SCREEN_H;
	disp.dmg_count = 1;
}

void shell_display_present(void)
{
	int r, y;

	if (shell_gpu_active()) {
		if (shell_gpu_draw_count() == 0) {
			disp.dmg_count = 0;
			return;
		}
		if (disp.headless || !disp.use_drm || disp.drm_fd < 0) {
			(void)shell_gpu_flush(0, disp.last_gpu_buf);
			disp.last_gpu_buf = 0;
			disp.dmg_count = 0;
			return;
		}
		if (shell_gpu_flush(0, disp.last_gpu_buf) == 0) {
			disp.last_gpu_buf = 0;
			if (disp.active_buf != 0 && disp.have_mode &&
			    disp.bufs[0].fb_id != 0) {
				struct drm_mode_crtc crtc = { 0 };

				crtc.crtc_id = disp.crtc_id;
				crtc.fb_id = disp.bufs[0].fb_id;
				crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
				crtc.count_connectors = 1;
				crtc.mode = disp.mode_720p30;
				crtc.mode_valid = 1;
				(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
				disp.active_buf = 0;
				disp.fb_id = disp.bufs[0].fb_id;
				disp.scanout = disp.bufs[0].map;
			}
		}
		disp.dmg_count = 0;
		return;
	}

	if (disp.dmg_count <= 0)
		return;
	if (disp.headless || !disp.scanout) {
		disp.dmg_count = 0;
		return;
	}

	for (r = 0; r < disp.dmg_count; r++) {
		int x0 = disp.dmg_rects[r].x0;
		int y0 = disp.dmg_rects[r].y0;
		int x1 = disp.dmg_rects[r].x1;
		int y1 = disp.dmg_rects[r].y1;
		int copy_w;

		if (x1 > (int)disp.scanout_w)
			x1 = (int)disp.scanout_w;
		if (y1 > (int)disp.scanout_h)
			y1 = (int)disp.scanout_h;
		copy_w = x1 - x0;
		if (copy_w <= 0 || y0 >= y1)
			continue;

		for (y = y0; y < y1; y++) {
			const uint32_t *src = disp.backbuf + y * SHELL_SCREEN_W + x0;
			uint32_t *dst = disp.scanout + (size_t)y * disp.scanout_pitch_px + x0;

			copy_span_u32(dst, src, copy_w);
		}
	}
	if (disp.use_drm && disp.drm_fd >= 0 && disp.active_buf != 0 && disp.have_mode) {
		struct drm_mode_crtc crtc = { 0 };

		disp.active_buf = 0;
		disp.fb_id = disp.bufs[0].fb_id;
		crtc.crtc_id = disp.crtc_id;
		crtc.fb_id = disp.fb_id;
		crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = disp.mode_720p30;
		crtc.mode_valid = 1;
		(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
	}
	disp.dmg_count = 0;
}

int shell_display_get_video_dmabuf(int buf_idx)
{
	if (buf_idx < 0 || buf_idx >= SHELL_DRM_BUF_COUNT)
		return -1;
	if (disp.use_drm && disp.bufs[buf_idx].dmabuf_fd >= 0)
		return disp.bufs[buf_idx].dmabuf_fd;
	if (shell_gpu_active())
		return shell_gpu_target_dmabuf_fd(buf_idx);
	return -1;
}

int shell_display_prepare_video_backbuf(int *out_dmabuf_fd)
{
	int back = (disp.active_buf == 1) ? 2 : 1;

	if (disp.headless || !disp.use_drm || disp.drm_fd < 0) {
		int fd = shell_gpu_target_dmabuf_fd(1);

		if (!shell_gpu_active() || fd < 0)
			return -1;
		if (out_dmabuf_fd)
			*out_dmabuf_fd = fd;
		return 1;
	}
	if (disp.bufs[back].dmabuf_fd < 0 || !disp.bufs[back].map)
		back = 1;
	if (disp.bufs[back].dmabuf_fd < 0 || !disp.bufs[back].map)
		return -1;

	if (out_dmabuf_fd)
		*out_dmabuf_fd = disp.bufs[back].dmabuf_fd;
	return back;
}

void shell_display_present_video_backbuf(int buf_idx,
					 const int (*hud_rects)[4],
					 int num_hud_rects)
{
	uint32_t *dst_base;
	int i, y;

	if (buf_idx < 0 || buf_idx >= SHELL_DRM_BUF_COUNT)
		return;

	if (shell_gpu_active()) {
		if (shell_gpu_draw_count() > 0)
			(void)shell_gpu_flush(buf_idx, buf_idx);

		if (!disp.headless && disp.use_drm && disp.drm_fd >= 0 &&
		    disp.have_mode && disp.bufs[buf_idx].fb_id != 0) {
			struct drm_mode_crtc crtc = { 0 };

			crtc.crtc_id = disp.crtc_id;
			crtc.fb_id = disp.bufs[buf_idx].fb_id;
			crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
			crtc.count_connectors = 1;
			crtc.mode = disp.mode_720p30;
			crtc.mode_valid = 1;
			if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0) {
				disp.active_buf = buf_idx;
				disp.fb_id = disp.bufs[buf_idx].fb_id;
				disp.scanout = disp.bufs[buf_idx].map;
				disp.last_gpu_buf = buf_idx;
			}
		} else if (disp.headless) {
			disp.last_gpu_buf = buf_idx;
		}
		disp.dmg_count = 0;
		return;
	}

	if (!disp.use_drm || disp.drm_fd < 0)
		return;

	dst_base = disp.bufs[buf_idx].map;
	if (!dst_base || !disp.scanout)
		return;

	/*
	 * Composite the compact HUD pills from cached RAM onto the offscreen
	 * DMA video frame while respecting each pill's anti-aliased rounded
	 * corners, then flip the DRM CRTC directly to @buf_idx (zero 3.68 MB
	 * CPU copy).
	 */
	for (i = 0; i < num_hud_rects; i++) {
		int rx = hud_rects[i][0];
		int ry = hud_rects[i][1];
		int rw = hud_rects[i][2];
		int rh = hud_rects[i][3];
		int radius = (rh >= 150) ? 12 : ((rh >= 100) ? 14 : ((rh >= 38) ? 10 : 8));
		int py, px;

		if (rw <= 0 || rh <= 0)
			continue;
		if (radius * 2 > rw)
			radius = rw / 2;
		if (radius * 2 > rh)
			radius = rh / 2;

		for (py = 0; py < rh; py++) {
			int sy = ry + py;

			if (sy < 0 || sy >= (int)disp.scanout_h)
				continue;
			const uint32_t *src_row = disp.backbuf + sy * SHELL_SCREEN_W;
			uint32_t *dst_row = dst_base + (size_t)sy * disp.scanout_pitch_px;

			if (py >= radius && py < rh - radius) {
				int x0 = rx < 0 ? 0 : rx;
				int x1 = (rx + rw > (int)disp.scanout_w) ? (int)disp.scanout_w : (rx + rw);

				if (x1 > x0)
					copy_span_u32(dst_row + x0, src_row + x0, x1 - x0);
				continue;
			}

			int cy = (py < radius) ? py : (rh - 1 - py);

			for (px = 0; px < radius; px++) {
				int sx = rx + px;

				if (sx < 0 || sx >= (int)disp.scanout_w)
					continue;
				uint32_t cov = g_corner_luts_ready ? g_corner_cov_lut[radius][cy][px] : 255U;
				if (cov >= 255)
					dst_row[sx] = src_row[sx];
				else if (cov > 0)
					dst_row[sx] = blend_pixel(dst_row[sx], src_row[sx], cov);
			}

			int mid_x0 = rx + radius;
			int mid_x1 = rx + rw - radius;
			if (mid_x0 < 0)
				mid_x0 = 0;
			if (mid_x1 > (int)disp.scanout_w)
				mid_x1 = (int)disp.scanout_w;
			if (mid_x1 > mid_x0)
				copy_span_u32(dst_row + mid_x0, src_row + mid_x0, mid_x1 - mid_x0);

			for (px = rw - radius; px < rw; px++) {
				int sx = rx + px;
				int cx = rw - 1 - px;

				if (sx < 0 || sx >= (int)disp.scanout_w)
					continue;
				uint32_t cov = g_corner_luts_ready ? g_corner_cov_lut[radius][cy][cx] : 255U;
				if (cov >= 255)
					dst_row[sx] = src_row[sx];
				else if (cov > 0)
					dst_row[sx] = blend_pixel(dst_row[sx], src_row[sx], cov);
			}
		}
	}

	if (disp.have_mode && disp.bufs[buf_idx].fb_id != 0) {
		struct drm_mode_crtc crtc = { 0 };

		crtc.crtc_id = disp.crtc_id;
		crtc.fb_id = disp.bufs[buf_idx].fb_id;
		crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
		crtc.count_connectors = 1;
		crtc.mode = disp.mode_720p30;
		crtc.mode_valid = 1;
		if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc) == 0) {
			disp.active_buf = buf_idx;
			disp.fb_id = disp.bufs[buf_idx].fb_id;
			disp.dmg_count = 0;
			return;
		}
	}

	if (dst_base != disp.scanout) {
		for (y = 0; y < (int)disp.scanout_h; y++) {
			const uint32_t *src = dst_base + (size_t)y * disp.scanout_pitch_px;
			uint32_t *dst = disp.scanout + (size_t)y * disp.scanout_pitch_px;

			copy_span_u32(dst, src, (int)disp.scanout_w);
		}
	}
	disp.dmg_count = 0;
}

void shell_display_release(void)
{
	if (disp.use_drm && disp.drm_fd >= 0) {
		if (disp.orig_fb_id != 0 && disp.have_mode) {
			struct drm_mode_crtc crtc = { 0 };

			crtc.crtc_id = disp.crtc_id;
			crtc.fb_id = disp.orig_fb_id;
			crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
			crtc.count_connectors = 1;
			crtc.mode = disp.mode_720p30;
			crtc.mode_valid = 1;
			(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
		}
		(void)ioctl(disp.drm_fd, DRM_IOCTL_DROP_MASTER, 0);
		disp.active_buf = -1;
#ifdef __linux__
		{
			int fbfd = open("/dev/fb0", O_RDWR | O_CLOEXEC);

			if (fbfd >= 0) {
				struct fb_var_screeninfo vinfo;

				if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
					vinfo.activate |= FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
					vinfo.xoffset = 0;
					vinfo.yoffset = 0;
					(void)ioctl(fbfd, FBIOPUT_VSCREENINFO, &vinfo);
					(void)ioctl(fbfd, FBIOPAN_DISPLAY, &vinfo);
				}
				close(fbfd);
			}
		}
#endif
	}
}

int shell_display_acquire(void)
{
	ensure_720p30_and_gamma();

	if (disp.use_drm && disp.drm_fd >= 0) {
		struct drm_mode_crtc crtc = { 0 };

		(void)ioctl(disp.drm_fd, DRM_IOCTL_SET_MASTER, 0);
		disp.active_buf = 0;
		disp.last_gpu_buf = -1;
		disp.fb_id = disp.bufs[0].fb_id;
		disp.scanout = disp.bufs[0].map;
		if (disp.have_mode) {
			crtc.crtc_id = disp.crtc_id;
			crtc.fb_id = disp.fb_id;
			crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
			crtc.count_connectors = 1;
			crtc.mode = disp.mode_720p30;
			crtc.mode_valid = 1;
			(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
		}
	}
	shell_display_damage_all();
	shell_display_present();
	return 0;
}

int shell_display_dump_ppm(const char *path)
{
	const uint32_t *px = disp.backbuf;
	FILE *f;
	int i, total = SHELL_SCREEN_W * SHELL_SCREEN_H;

	if (shell_gpu_active()) {
		int hbuf = (disp.last_gpu_buf >= 0) ? disp.last_gpu_buf : 0;

		if (shell_gpu_draw_count() > 0) {
			int dst = disp.use_drm ? disp.active_buf : 0;

			(void)shell_gpu_flush(dst, disp.last_gpu_buf);
			disp.last_gpu_buf = dst;
			hbuf = dst;
		}
		px = disp.use_drm ? disp.bufs[disp.active_buf].map
				  : shell_gpu_get_headless_pixels(hbuf);
	}
	if (!px)
		return -EINVAL;

	f = fopen(path, "wb");
	if (!f)
		return -errno;
	fprintf(f, "P6\n%d %d\n255\n", SHELL_SCREEN_W, SHELL_SCREEN_H);
	for (i = 0; i < total; i++) {
		uint32_t c = px[i];
		uint8_t rgb[3] = {
			(uint8_t)((c >> 16) & 0xffU),
			(uint8_t)((c >> 8) & 0xffU),
			(uint8_t)(c & 0xffU),
		};

		if (fwrite(rgb, 1, 3, f) != 3) {
			fclose(f);
			return -EIO;
		}
	}
	fclose(f);
	return 0;
}

/* --- Drawing Primitives --- */

void shell_draw_rect(int x, int y, int w, int h, uint32_t color)
{
	int x0, y0, x1, y1, py;

	if (w <= 0 || h <= 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_rect(x, y, w, h, color, 255);
		shell_display_damage(x, y, w, h);
		return;
	}
	x0 = x < 0 ? 0 : x;
	y0 = y < 0 ? 0 : y;
	x1 = (x + w > SHELL_SCREEN_W) ? SHELL_SCREEN_W : (x + w);
	y1 = (y + h > SHELL_SCREEN_H) ? SHELL_SCREEN_H : (y + h);
	if (x0 >= x1 || y0 >= y1)
		return;

	color |= 0xff000000U;
	for (py = y0; py < y1; py++) {
		uint32_t *row = disp.backbuf + py * SHELL_SCREEN_W;

		fill_span_u32(row + x0, color, x1 - x0);
	}
	shell_display_damage(x0, y0, x1 - x0, y1 - y0);
}

void shell_draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
	int x0, y0, x1, y1, py;

	if (w <= 0 || h <= 0 || alpha == 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_rect(x, y, w, h, color, alpha);
		shell_display_damage(x, y, w, h);
		return;
	}
	if (alpha == 255) {
		shell_draw_rect(x, y, w, h, color);
		return;
	}
	x0 = x < 0 ? 0 : x;
	y0 = y < 0 ? 0 : y;
	x1 = (x + w > SHELL_SCREEN_W) ? SHELL_SCREEN_W : (x + w);
	y1 = (y + h > SHELL_SCREEN_H) ? SHELL_SCREEN_H : (y + h);
	if (x0 >= x1 || y0 >= y1)
		return;

	for (py = y0; py < y1; py++) {
		uint32_t *row = disp.backbuf + py * SHELL_SCREEN_W;

		blend_span_const_alpha(row + x0, x1 - x0, color, alpha);
	}
	shell_display_damage(x0, y0, x1 - x0, y1 - y0);
}

/*
 * Exact Euclidean Signed Distance Field (SDF) for a rounded rectangle of size
 * (w, h) and corner radius r at continuous pixel coordinates (px, py).
 * Returns < 0 inside, 0 on boundary, > 0 outside.
 */
static inline float sdf_rounded_rect_dist(float px, float py, float w, float h, float r)
{
	float hw = w * 0.5f;
	float hh = h * 0.5f;
	float qx = fabsf(px - hw) - (hw - r);
	float qy = fabsf(py - hh) - (hh - r);
	float ox = qx > 0.0f ? qx : 0.0f;
	float oy = qy > 0.0f ? qy : 0.0f;
	float outside = sqrtf(ox * ox + oy * oy);
	float inside = (qx > qy ? qx : qy);

	if (inside > 0.0f)
		inside = 0.0f;
	return outside + inside - r;
}

static inline uint32_t rounded_rect_cov255(int lx, int ly, int w, int h, int r)
{
	if (r <= 0)
		return 255;
	if (lx < 0 || ly < 0 || lx >= w || ly >= h)
		return 0;
	if ((lx >= r && lx < w - r) || (ly >= r && ly < h - r))
		return 255;

	int cx = (lx < r) ? lx : (w - 1 - lx);
	int cy = (ly < r) ? ly : (h - 1 - ly);

	if (r <= MAX_PRECOMP_RADIUS && g_corner_luts_ready)
		return g_corner_cov_lut[r][cy][cx];

	/* Fallback for radii > MAX_PRECOMP_RADIUS */
	int sx, sy;
	float acc = 0.0f;
	float wf = (float)w, hf = (float)h, rf = (float)r;

	for (sy = 0; sy < 2; sy++) {
		float py = (float)ly + 0.25f + 0.5f * (float)sy;

		for (sx = 0; sx < 2; sx++) {
			float px = (float)lx + 0.25f + 0.5f * (float)sx;
			float d = sdf_rounded_rect_dist(px, py, wf, hf, rf);
			float c = 0.5f - d;

			if (c > 1.0f)
				c = 1.0f;
			else if (c < 0.0f)
				c = 0.0f;
			acc += c;
		}
	}
	return (uint32_t)(acc * 63.75f + 0.5f);
}

void shell_draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color)
{
	shell_draw_rounded_rect_alpha(x, y, w, h, radius, color, 255);
}

void shell_draw_rounded_rect_alpha(int x, int y, int w, int h, int radius,
				   uint32_t color, uint8_t alpha)
{
	int py, px;

	if (w <= 0 || h <= 0 || alpha == 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_rounded_rect(x, y, w, h, radius, color, alpha);
		shell_display_damage(x, y, w, h);
		return;
	}
	if (radius <= 0) {
		shell_draw_rect_alpha(x, y, w, h, color, alpha);
		return;
	}
	if (radius * 2 > w)
		radius = w / 2;
	if (radius * 2 > h)
		radius = h / 2;

	color |= 0xff000000U;

	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		bool is_corner_row = (py < radius || py >= h - radius);

		if (!is_corner_row) {
			int sx0 = x < 0 ? 0 : x;
			int sx1 = (x + w > SHELL_SCREEN_W) ? SHELL_SCREEN_W : (x + w);

			if (sx1 > sx0)
				blend_span_const_alpha(row + sx0, sx1 - sx0, color, alpha);
			continue;
		}

		/* Left rounded corner [0, radius) */
		for (px = 0; px < radius; px++) {
			int sx = x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t cov = rounded_rect_cov255(px, py, w, h, radius);
			uint32_t a = (cov * alpha + 127U) >> 8;

			if (a >= 255)
				row[sx] = color;
			else if (a > 0)
				row[sx] = blend_pixel(row[sx], color, a);
		}

		/* Middle straight span [radius, w - radius) */
		int mid_x0 = x + radius;
		int mid_x1 = x + w - radius;
		if (mid_x0 < 0)
			mid_x0 = 0;
		if (mid_x1 > SHELL_SCREEN_W)
			mid_x1 = SHELL_SCREEN_W;
		if (mid_x1 > mid_x0)
			blend_span_const_alpha(row + mid_x0, mid_x1 - mid_x0, color, alpha);

		/* Right rounded corner [w - radius, w) */
		for (px = w - radius; px < w; px++) {
			int sx = x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t cov = rounded_rect_cov255(px, py, w, h, radius);
			uint32_t a = (cov * alpha + 127U) >> 8;

			if (a >= 255)
				row[sx] = color;
			else if (a > 0)
				row[sx] = blend_pixel(row[sx], color, a);
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_rounded_outline(int x, int y, int w, int h, int radius,
				int thickness, uint32_t color)
{
	int py, px;
	bool use_lut;

	if (w <= 0 || h <= 0 || thickness <= 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_rounded_outline(x, y, w, h, radius,
					       thickness, color, 255);
		shell_display_damage(x, y, w, h);
		return;
	}
	if (radius * 2 > w)
		radius = w / 2;
	if (radius * 2 > h)
		radius = h / 2;
	color |= 0xff000000U;
	use_lut = (g_corner_luts_ready && radius >= 1 && radius <= MAX_PRECOMP_RADIUS &&
		   thickness >= 1 && thickness <= MAX_PRECOMP_THICK);

	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		bool in_y_mid = (py >= radius && py < h - radius);

		if (in_y_mid) {
			/* Only left [0, thickness) and right [w - thickness, w) vertical borders */
			for (px = 0; px < thickness; px++) {
				int sx = x + px;
				if (sx >= 0 && sx < SHELL_SCREEN_W)
					row[sx] = color;
			}
			for (px = w - thickness; px < w; px++) {
				int sx = x + px;
				if (sx >= 0 && sx < SHELL_SCREEN_W)
					row[sx] = color;
			}
			continue;
		}

		/* Top or bottom straight horizontal border segment [radius, w - radius) */
		if (py < thickness || py >= h - thickness) {
			int sx0 = x + radius;
			int sx1 = x + w - radius;
			if (sx0 < 0)
				sx0 = 0;
			if (sx1 > SHELL_SCREEN_W)
				sx1 = SHELL_SCREEN_W;
			if (sx1 > sx0)
				fill_span_u32(row + sx0, color, sx1 - sx0);
		}

		/* Left and right rounded corners */
		int cy = (py < radius) ? py : (h - 1 - py);
		int corner_side;

		for (corner_side = 0; corner_side < 2; corner_side++) {
			int px_start = (corner_side == 0) ? 0 : (w - radius);
			int px_end   = (corner_side == 0) ? radius : w;

			for (px = px_start; px < px_end; px++) {
				int sx = x + px;
				if (sx < 0 || sx >= SHELL_SCREEN_W)
					continue;
				int cx = (px < radius) ? px : (w - 1 - px);
				uint32_t a;

				if (use_lut) {
					a = g_corner_stroke_lut[radius][thickness][cy][cx];
				} else {
					float wf = (float)w, hf = (float)h;
					float rf = (float)radius, tf = (float)thickness;
					int sub_x, sub_y;
					float acc = 0.0f;

					for (sub_y = 0; sub_y < 2; sub_y++) {
						float fy = (float)py + 0.25f + 0.5f * (float)sub_y;
						for (sub_x = 0; sub_x < 2; sub_x++) {
							float fx = (float)px + 0.25f + 0.5f * (float)sub_x;
							float d = sdf_rounded_rect_dist(fx, fy, wf, hf, rf);
							float outer_c = 0.5f - d;
							float inner_c = 0.5f + d + tf;

							if (outer_c > 1.0f)
								outer_c = 1.0f;
							else if (outer_c < 0.0f)
								outer_c = 0.0f;
							if (inner_c > 1.0f)
								inner_c = 1.0f;
							else if (inner_c < 0.0f)
								inner_c = 0.0f;
							acc += outer_c * inner_c;
						}
					}
					a = (uint32_t)(acc * 63.75f + 0.5f);
				}

				if (a >= 255)
					row[sx] = color;
				else if (a > 0)
					row[sx] = blend_pixel(row[sx], color, a);
			}
		}
	}
	shell_display_damage(x, y, w, h);
}

/*
 * Outer focus glow ring: box-shadow: 0 0 0 <spread>px rgba(245, 158, 11, 0.28)
 */
void shell_draw_focus_glow(int x, int y, int w, int h, int radius,
			   int spread, uint32_t glow_color, uint8_t max_alpha)
{
	int gx = x - spread;
	int gy = y - spread;
	int gw = w + 2 * spread;
	int gh = h + 2 * spread;
	int gr = radius + spread;
	int py, px;

	if (spread <= 0 || max_alpha == 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_rounded_rect(gx, gy, gw, gh, gr,
					    glow_color, max_alpha);
		shell_display_damage(gx, gy, gw, gh);
		return;
	}

	for (py = 0; py < gh; py++) {
		int sy = gy + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		bool in_inner_y_mid = (py >= spread + radius && py < gh - spread - radius);

		if (in_inner_y_mid) {
			/* Only left [0, spread) and right [gw - spread, gw) glow bands */
			for (px = 0; px < spread; px++) {
				int sx = gx + px;
				if (sx >= 0 && sx < SHELL_SCREEN_W)
					row[sx] = blend_pixel(row[sx], glow_color, max_alpha);
			}
			for (px = gw - spread; px < gw; px++) {
				int sx = gx + px;
				if (sx >= 0 && sx < SHELL_SCREEN_W)
					row[sx] = blend_pixel(row[sx], glow_color, max_alpha);
			}
			continue;
		}

		for (px = 0; px < gw; px++) {
			int sx = gx + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			/* Skip interior of the element itself */
			if (px >= spread + radius && px < gw - spread - radius &&
			    py >= spread && py < gh - spread) {
				px = gw - spread - radius - 1;
				continue;
			}

			uint32_t outer_a = rounded_rect_cov255(px, py, gw, gh, gr);
			uint32_t inner_a = 0;

			if (px >= spread && px < gw - spread &&
			    py >= spread && py < gh - spread) {
				inner_a = rounded_rect_cov255(px - spread,
							      py - spread,
							      w, h, radius);
			}
			if (outer_a > inner_a) {
				uint32_t cov = outer_a - inner_a;
				uint32_t a = (cov * max_alpha + 127U) >> 8;

				row[sx] = blend_pixel(row[sx], glow_color, a);
			}
		}
	}
	shell_display_damage(gx, gy, gw, gh);
}

/*
 * Subtle drop shadow beneath elevated focused cards: 0 10px 22px rgba(0,0,0,0.22)
 * Skips the opaque card interior [x, x+w) x [y, y+h) that the card/modal body
 * immediately overwrites, cutting shadow pixel count by ~95%.
 */
void shell_draw_card_shadow(int x, int y, int w, int h, int radius)
{
	int s, py, px;

	if (shell_gpu_active()) {
		for (s = 8; s >= 2; s -= 2) {
			shell_gpu_draw_rounded_rect(x - s / 2, y + 4 + s,
						    w + s, h,
						    radius + s / 2,
						    0x06080cU, 14);
		}
		shell_display_damage(x - 4, y + 6, w + 8, h + 10);
		return;
	}

	for (s = 8; s >= 2; s -= 2) {
		int sx0 = x - s / 2;
		int sy0 = y + 4 + s;
		int sw = w + s;
		int sh = h;
		int sr = radius + s / 2;

		for (py = 0; py < sh; py++) {
			int sy = sy0 + py;

			if (sy < 0 || sy >= SHELL_SCREEN_H)
				continue;
			uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
			bool in_card_y_mid = (sy >= y + radius && sy < y + h - radius);

			for (px = 0; px < sw; px++) {
				int sx = sx0 + px;

				if (sx < 0 || sx >= SHELL_SCREEN_W)
					continue;
				/* Fast skip of opaque card interior */
				if (in_card_y_mid && sx >= x && sx < x + w) {
					px = (x + w - 1) - sx0;
					continue;
				}
				if (sy >= y && sy < y + h &&
				    sx >= x + radius && sx < x + w - radius) {
					px = (x + w - radius - 1) - sx0;
					continue;
				}

				uint32_t cov = rounded_rect_cov255(px, py, sw, sh, sr);
				if (cov == 0)
					continue;
				uint32_t a = (cov * 14U + 127U) >> 8;
				if (a > 0)
					row[sx] = blend_pixel(row[sx], 0xff06080cU, a);
			}
		}
	}
	shell_display_damage(x - 4, y + 6, w + 8, h + 6);
}

/*
 * Bottom gradient scrim on Slot 3 media cards (.media-body):
 *   linear-gradient(to top, rgba(6,8,12,0.96) 0%, rgba(6,8,12,0.84) 58%,
 *                   rgba(6,8,12,0.32) 84%, transparent 100%)
 * Clipped to the bottom rounded corners of the card.
 */
void shell_draw_card_bottom_scrim(int card_x, int card_y, int card_w, int card_h,
				  int radius, int scrim_h)
{
	int y_start, py, px;

	if (scrim_h <= 0 || scrim_h > card_h)
		scrim_h = card_h;
	y_start = card_h - scrim_h;

	if (shell_gpu_active()) {
		shell_gpu_draw_card_bottom_scrim(card_x, card_y, card_w, card_h,
						 radius, scrim_h);
		shell_display_damage(card_x, card_y + y_start, card_w, scrim_h);
		return;
	}

	{
		static const int8_t bayer4x4[16] = {
			-7,  1, -5,  3,
			 5, -3,  7, -1,
			-4,  4, -6,  2,
			 8,  0,  6, -2
		};

		for (py = y_start; py < card_h; py++) {
			int sy = card_y + py;
			int ly = py - y_start;

			if (sy < 0 || sy >= SHELL_SCREEN_H)
				continue;
			uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;

			/* Distance from bottom: 0.0 at bottom -> 1.0 at top of scrim */
			float dist_from_bottom = (float)(card_h - 1 - py) / (float)(scrim_h > 1 ? scrim_h - 1 : 1);
			float alpha_f;

			if (dist_from_bottom <= 0.58f) {
				float t = dist_from_bottom / 0.58f;
				alpha_f = 0.96f + (0.84f - 0.96f) * t;
			} else if (dist_from_bottom <= 0.84f) {
				float t = (dist_from_bottom - 0.58f) / (0.84f - 0.58f);
				alpha_f = 0.84f + (0.32f - 0.84f) * t;
			} else {
				float t = (dist_from_bottom - 0.84f) / (1.0f - 0.84f);
				alpha_f = 0.32f * (1.0f - t);
			}

			if (alpha_f <= 0.0f)
				continue;

			for (px = 0; px < card_w; px++) {
				int sx = card_x + px;
				float cov = 1.0f;
				float dither = (float)bayer4x4[(ly & 3) * 4 + (px & 3)] * (1.5f / 16.0f);
				int a;

				if (sx < 0 || sx >= SHELL_SCREEN_W)
					continue;
				if (py >= card_h - radius &&
				    (px < radius || px >= card_w - radius)) {
					cov = (float)rounded_rect_cov255(px, py, card_w, card_h, radius) *
					      (1.0f / 255.0f);
					if (cov <= 0.0f)
						continue;
				}
				a = (int)(alpha_f * cov * 255.0f + dither + 0.5f);
				if (a > 255)
					a = 255;
				if (a > 0)
					row[sx] = blend_pixel(row[sx], 0xff06080cU, (uint32_t)a);
			}
		}
	}
	shell_display_damage(card_x, card_y + y_start, card_w, scrim_h);
}

/*
 * Fullscreen overlay top scrim (.overlay-top-bar):
 *   linear-gradient(to bottom, rgba(6,8,12,0.88) 0%, rgba(6,8,12,0.45) 65%, transparent 100%)
 */
void shell_draw_top_scrim(int w, int h)
{
	static const int8_t bayer4x4[16] = {
		-7,  1, -5,  3,
		 5, -3,  7, -1,
		-4,  4, -6,  2,
		 8,  0,  6, -2
	};
	int py, px;
	int cw = (w > SHELL_SCREEN_W) ? SHELL_SCREEN_W : w;

	if (cw <= 0 || h <= 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_top_scrim(cw, h);
		shell_display_damage(0, 0, w, h);
		return;
	}
	for (py = 0; py < h && py < SHELL_SCREEN_H; py++) {
		uint32_t *row = disp.backbuf + py * SHELL_SCREEN_W;
		float t = (float)py / (float)(h > 1 ? h - 1 : 1);
		float alpha_f = (t <= 0.65f) ?
			(0.88f + (0.45f - 0.88f) * (t / 0.65f)) :
			(0.45f * (1.0f - (t - 0.65f) / 0.35f));

		if (alpha_f <= 0.0f)
			continue;
		for (px = 0; px < cw; px++) {
			float dither = (float)bayer4x4[(py & 3) * 4 + (px & 3)] * (1.5f / 16.0f);
			int a = (int)(alpha_f * 255.0f + dither + 0.5f);

			if (a > 255)
				a = 255;
			if (a > 0)
				row[px] = blend_pixel(row[px], 0xff06080cU, (uint32_t)a);
		}
	}
	shell_display_damage(0, 0, w, h);
}

/*
 * Fullscreen overlay bottom scrim (.overlay-bottom-scrim):
 *   linear-gradient(to top, rgba(6,8,12,0.96) 0%, rgba(6,8,12,0.82) 55%,
 *                   rgba(6,8,12,0.35) 82%, transparent 100%)
 */
void shell_draw_bottom_scrim(int y0, int w, int h)
{
	static const int8_t bayer4x4[16] = {
		-7,  1, -5,  3,
		 5, -3,  7, -1,
		-4,  4, -6,  2,
		 8,  0,  6, -2
	};
	int py, px;
	int cw = (w > SHELL_SCREEN_W) ? SHELL_SCREEN_W : w;

	if (cw <= 0 || h <= 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_bottom_scrim(y0, cw, h);
		shell_display_damage(0, y0, w, h);
		return;
	}
	for (py = 0; py < h; py++) {
		int sy = y0 + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		float dist = (float)(h - 1 - py) / (float)(h > 1 ? h - 1 : 1);
		float alpha_f;

		if (dist <= 0.55f)
			alpha_f = 0.96f + (0.82f - 0.96f) * (dist / 0.55f);
		else if (dist <= 0.82f)
			alpha_f = 0.82f + (0.35f - 0.82f) * ((dist - 0.55f) / 0.27f);
		else
			alpha_f = 0.35f * (1.0f - (dist - 0.82f) / 0.18f);

		if (alpha_f <= 0.0f)
			continue;
		for (px = 0; px < cw; px++) {
			float dither = (float)bayer4x4[(py & 3) * 4 + (px & 3)] * (1.5f / 16.0f);
			int a = (int)(alpha_f * 255.0f + dither + 0.5f);

			if (a > 255)
				a = 255;
			if (a > 0)
				row[px] = blend_pixel(row[px], 0xff06080cU, (uint32_t)a);
		}
	}
	shell_display_damage(0, y0, w, h);
}

/*
 * Stage-1 progressive skeleton shimmer bar (.skeleton-line)
 */
void shell_draw_shimmer_bar(int x, int y, int w, int h, int radius,
			    uint32_t base_color, uint32_t highlight_color, int phase_pct)
{
	int py, px;
	int center_x, band_w;

	if (w <= 0 || h <= 0)
		return;
	if (shell_gpu_active()) {
		shell_gpu_draw_shimmer_bar(x, y, w, h, radius,
					   base_color, highlight_color,
					   phase_pct);
		shell_display_damage(x, y, w, h);
		return;
	}

	center_x = (w * phase_pct) / 100;
	band_w = w / 3 > 1 ? w / 3 : 1;

	base_color |= 0xff000000U;
	highlight_color |= 0xff000000U;

	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;

		for (px = 0; px < w; px++) {
			int sx = x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t cov = rounded_rect_cov255(px, py, w, h, radius);
			if (cov == 0)
				continue;

			int dist = abs(px - center_x);
			uint32_t bar_c = base_color;

			if (dist < band_w) {
				uint32_t t = (uint32_t)((band_w - dist) * 180 / band_w);
				bar_c = blend_pixel(base_color, highlight_color, t);
			}
			if (cov >= 255)
				row[sx] = bar_c;
			else
				row[sx] = blend_pixel(row[sx], bar_c, cov);
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_bitmap_xrgb(int x, int y, int w, int h, const uint32_t *pixels)
{
	int py, px;

	if (!pixels || w <= 0 || h <= 0)
		return;
	if (shell_gpu_active() &&
	    shell_gpu_draw_bitmap_rect(x, y, 0, 0, w, h, pixels, w, 255)) {
		shell_display_damage(x, y, w, h);
		return;
	}
	if (!disp.backbuf)
		return;
	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		const uint32_t *src = pixels + py * w;

		if (x >= 0 && x + w <= SHELL_SCREEN_W) {
			copy_span_u32(row + x, src, w);
		} else {
			for (px = 0; px < w; px++) {
				int sx = x + px;

				if (sx >= 0 && sx < SHELL_SCREEN_W)
					row[sx] = src[px] | 0xff000000U;
			}
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_rounded_bitmap_stride(int x, int y, int w, int h, int stride,
				      int radius, const uint32_t *pixels,
				      bool zoom_slightly)
{
	int py, px;

	(void)zoom_slightly;
	if (!pixels || w <= 0 || h <= 0 || stride < w)
		return;
	if (shell_gpu_active() &&
	    shell_gpu_draw_rounded_bitmap(x, y, w, h, radius, pixels, stride)) {
		shell_display_damage(x, y, w, h);
		return;
	}
	if (!disp.backbuf)
		return;
	if (radius * 2 > w)
		radius = w / 2;
	if (radius * 2 > h)
		radius = h / 2;

	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		bool is_corner_row = (py < radius || py >= h - radius);
		const uint32_t *row0 = pixels + py * stride;

		if (!is_corner_row && x >= 0 && x + w <= SHELL_SCREEN_W) {
			copy_span_u32(row + x, row0, w);
			continue;
		}
		if (is_corner_row && x >= 0 && x + w <= SHELL_SCREEN_W) {
			for (px = 0; px < radius; px++) {
				uint32_t a = rounded_rect_cov255(px, py, w, h, radius);
				if (a >= 255)
					row[x + px] = row0[px] | 0xff000000U;
				else if (a > 0)
					row[x + px] = blend_pixel(row[x + px], row0[px], a);
			}
			copy_span_u32(row + x + radius, row0 + radius, w - 2 * radius);
			for (px = w - radius; px < w; px++) {
				uint32_t a = rounded_rect_cov255(px, py, w, h, radius);
				if (a >= 255)
					row[x + px] = row0[px] | 0xff000000U;
				else if (a > 0)
					row[x + px] = blend_pixel(row[x + px], row0[px], a);
			}
			continue;
		}

		for (px = 0; px < w; px++) {
			int sx = x + px;
			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t a = rounded_rect_cov255(px, py, w, h, radius);
			if (a >= 255)
				row[sx] = row0[px] | 0xff000000U;
			else if (a > 0)
				row[sx] = blend_pixel(row[sx], row0[px], a);
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_rounded_bitmap(int x, int y, int w, int h, int radius,
			       const uint32_t *pixels, bool zoom_slightly)
{
	shell_draw_rounded_bitmap_stride(x, y, w, h, w, radius, pixels, zoom_slightly);
}

void shell_draw_circle_alpha(int cx, int cy, int r, uint32_t color, uint8_t alpha)
{
	int d = r * 2;

	shell_draw_rounded_rect_alpha(cx - r, cy - r, d, d, r, color, alpha);
}

void shell_draw_symbol(enum shell_symbol_id sym, int x, int y, int size, uint32_t fg)
{
	static struct {
		bool valid;
		enum shell_symbol_id sym;
		int size;
		uint16_t u;
		uint16_t v;
	} sym_cache[64];
	const uint8_t *mask = shell_get_symbol_mask(sym, size);
	int py, px, i;

	if (!mask || size <= 0)
		return;

	if (shell_gpu_active()) {
		int slot = -1;

		for (i = 0; i < 64; i++) {
			if (sym_cache[i].valid &&
			    sym_cache[i].sym == sym &&
			    sym_cache[i].size == size) {
				slot = i;
				break;
			}
			if (!sym_cache[i].valid && slot < 0)
				slot = i;
		}
		if (slot >= 0 && !sym_cache[slot].valid) {
			if (shell_gpu_upload_glyph((uint16_t)size, (uint16_t)size,
						   mask, &sym_cache[slot].u,
						   &sym_cache[slot].v)) {
				sym_cache[slot].valid = true;
				sym_cache[slot].sym = sym;
				sym_cache[slot].size = size;
			} else {
				slot = -1;
			}
		}
		if (slot >= 0 && sym_cache[slot].valid) {
			shell_gpu_draw_glyph(x, y, size, size,
					     sym_cache[slot].u, sym_cache[slot].v,
					     fg, 255);
			shell_display_damage(x, y, size, size);
			return;
		}
	}

	if (!disp.backbuf)
		return;
	for (py = 0; py < size; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;

		for (px = 0; px < size; px++) {
			int sx = x + px;
			uint8_t a = mask[py * size + px];

			if (a && sx >= 0 && sx < SHELL_SCREEN_W)
				row[sx] = blend_pixel(row[sx], fg, a);
		}
	}
	shell_display_damage(x, y, size, size);
}
