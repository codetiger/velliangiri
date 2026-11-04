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

/* Minimal DRM/KMS UAPI definitions so static builds need no libdrm */
#define DRM_IOCTL_BASE			'd'
#define DRM_IO(nr)			_IO(DRM_IOCTL_BASE, nr)
#define DRM_IOWR(nr, type)		_IOWR(DRM_IOCTL_BASE, nr, type)

#define DRM_IOCTL_SET_MASTER		DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER		DRM_IO(0x1f)
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

static const uint8_t bayer4x4[16] = {
	 0,  8,  2, 10,
	12,  4, 14,  6,
	 3, 11,  1,  9,
	15,  7, 13,  5,
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
	uint32_t dumb_handle;
	struct drm_mode_modeinfo mode_720p30;
	bool have_mode;

	uint32_t *scanout;
	size_t scanout_bytes;
	uint32_t scanout_pitch_px;
	uint32_t scanout_w;
	uint32_t scanout_h;

	uint32_t *backbuf;
	int dmg_x0, dmg_y0, dmg_x1, dmg_y1;
} disp = {
	.drm_fd = -1,
	.fb_fd = -1,
	.tty_fd = -1,
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
	uint32_t inv_a, rl, gl, bl;

	if (a255 == 0)
		return dst;
	if (a255 >= 255)
		return src | 0xff000000U;

	inv_a = 255U - a255;
	rl = (shell_srgb_to_linear[(dst >> 16) & 0xffU] * inv_a +
	      shell_srgb_to_linear[(src >> 16) & 0xffU] * a255 + 127U) / 255U;
	gl = (shell_srgb_to_linear[(dst >> 8) & 0xffU] * inv_a +
	      shell_srgb_to_linear[(src >> 8) & 0xffU] * a255 + 127U) / 255U;
	bl = (shell_srgb_to_linear[dst & 0xffU] * inv_a +
	      shell_srgb_to_linear[src & 0xffU] * a255 + 127U) / 255U;
	if (rl > 4095U)
		rl = 4095U;
	if (gl > 4095U)
		gl = 4095U;
	if (bl > 4095U)
		bl = 4095U;
	return 0xff000000U |
	       ((uint32_t)shell_lin_to_srgb[rl] << 16) |
	       ((uint32_t)shell_lin_to_srgb[gl] << 8) |
	       (uint32_t)shell_lin_to_srgb[bl];
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
	for (i = 0; i < conn.count_modes; i++) {
		if (modes[i].hdisplay == 1280 && modes[i].vdisplay == 720) {
			disp.mode_720p30 = modes[i];
			disp.have_mode = true;
			return true;
		}
	}
	disp.mode_720p30 = modes[0];
	disp.have_mode = true;
	return true;
}

static int setup_drm_scanout(void)
{
	struct drm_mode_create_dumb creq = { 0 };
	struct drm_mode_fb_cmd fbcmd = { 0 };
	struct drm_mode_map_dumb mreq = { 0 };
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

	creq.width = SHELL_SCREEN_W;
	creq.height = SHELL_SCREEN_H;
	creq.bpp = 32;
	if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		goto err;

	disp.dumb_handle = creq.handle;
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
		goto err_dumb;
	disp.fb_id = fbcmd.fb_id;

	mreq.handle = creq.handle;
	if (ioctl(disp.drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
		goto err_fb;

	disp.scanout = mmap(NULL, disp.scanout_bytes, PROT_READ | PROT_WRITE,
			    MAP_SHARED, disp.drm_fd, (off_t)mreq.offset);
	if (disp.scanout == MAP_FAILED) {
		disp.scanout = NULL;
		goto err_fb;
	}

	crtc.crtc_id = disp.crtc_id;
	crtc.fb_id = disp.fb_id;
	crtc.set_connectors_ptr = (uintptr_t)&disp.conn_id;
	crtc.count_connectors = 1;
	crtc.mode = disp.mode_720p30;
	crtc.mode_valid = 1;
	(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_SETCRTC, &crtc);

	disp.use_drm = true;
	return 0;

err_fb:
	(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_RMFB, &disp.fb_id);
err_dumb:
	{
		struct drm_mode_destroy_dumb dreq = { .handle = disp.dumb_handle };

		(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	}
err:
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
	disp.headless = headless;
	disp.backbuf = calloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H, sizeof(uint32_t));
	if (!disp.backbuf)
		return -ENOMEM;

	disp.dmg_x0 = 0;
	disp.dmg_y0 = 0;
	disp.dmg_x1 = SHELL_SCREEN_W;
	disp.dmg_y1 = SHELL_SCREEN_H;

	if (headless)
		return 0;

	ensure_720p30_and_gamma();

	disp.tty_fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (disp.tty_fd >= 0) {
		(void)write(disp.tty_fd, "\033[?25l", 6);
#ifdef __linux__
		(void)ioctl(disp.tty_fd, KDSETMODE, KD_GRAPHICS);
#endif
	}

	if (setup_fbdev_scanout() == 0)
		return 0;
	if (setup_drm_scanout() == 0)
		return 0;

	fprintf(stderr, "tvshell: no /dev/fb0 or /dev/dri/card*, running headless\n");
	disp.headless = true;
	return 0;
}

void shell_display_close(void)
{
	if (disp.tty_fd >= 0) {
#ifdef __linux__
		(void)ioctl(disp.tty_fd, KDSETMODE, KD_TEXT);
#endif
		(void)write(disp.tty_fd, "\033[?25h", 6);
		close(disp.tty_fd);
		disp.tty_fd = -1;
	}
	if (disp.scanout) {
		munmap(disp.scanout, disp.scanout_bytes);
		disp.scanout = NULL;
	}
	if (disp.use_drm && disp.drm_fd >= 0) {
		struct drm_mode_destroy_dumb dreq = { .handle = disp.dumb_handle };

		(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_RMFB, &disp.fb_id);
		(void)ioctl(disp.drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		(void)ioctl(disp.drm_fd, DRM_IOCTL_DROP_MASTER, 0);
		close(disp.drm_fd);
		disp.drm_fd = -1;
	}
	if (disp.fb_fd >= 0) {
		close(disp.fb_fd);
		disp.fb_fd = -1;
	}
	free(disp.backbuf);
	disp.backbuf = NULL;
}

uint32_t *shell_display_backbuffer(void)
{
	return disp.backbuf;
}

void shell_display_damage(int x, int y, int w, int h)
{
	int x1, y1;

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

	if (disp.dmg_x0 >= disp.dmg_x1 || disp.dmg_y0 >= disp.dmg_y1) {
		disp.dmg_x0 = x;
		disp.dmg_y0 = y;
		disp.dmg_x1 = x1;
		disp.dmg_y1 = y1;
	} else {
		if (x < disp.dmg_x0)
			disp.dmg_x0 = x;
		if (y < disp.dmg_y0)
			disp.dmg_y0 = y;
		if (x1 > disp.dmg_x1)
			disp.dmg_x1 = x1;
		if (y1 > disp.dmg_y1)
			disp.dmg_y1 = y1;
	}
}

void shell_display_damage_all(void)
{
	disp.dmg_x0 = 0;
	disp.dmg_y0 = 0;
	disp.dmg_x1 = SHELL_SCREEN_W;
	disp.dmg_y1 = SHELL_SCREEN_H;
}

void shell_display_present(void)
{
	int y, x0, y0, x1, y1, copy_w;

	if (disp.dmg_x0 >= disp.dmg_x1 || disp.dmg_y0 >= disp.dmg_y1)
		return;
	x0 = disp.dmg_x0;
	y0 = disp.dmg_y0;
	x1 = disp.dmg_x1;
	y1 = disp.dmg_y1;
	disp.dmg_x0 = disp.dmg_y0 = disp.dmg_x1 = disp.dmg_y1 = 0;

	if (disp.headless || !disp.scanout)
		return;

	if (x1 > (int)disp.scanout_w)
		x1 = (int)disp.scanout_w;
	if (y1 > (int)disp.scanout_h)
		y1 = (int)disp.scanout_h;
	copy_w = x1 - x0;
	if (copy_w <= 0)
		return;

	for (y = y0; y < y1; y++) {
		const uint32_t *src = disp.backbuf + y * SHELL_SCREEN_W + x0;
		uint32_t *dst = disp.scanout + (size_t)y * disp.scanout_pitch_px + x0;

		memcpy(dst, src, (size_t)copy_w * sizeof(uint32_t));
	}
}

void shell_display_release(void)
{
	if (disp.use_drm && disp.drm_fd >= 0)
		(void)ioctl(disp.drm_fd, DRM_IOCTL_DROP_MASTER, 0);
}

int shell_display_acquire(void)
{
	ensure_720p30_and_gamma();

	if (disp.use_drm && disp.drm_fd >= 0) {
		struct drm_mode_crtc crtc = { 0 };

		(void)ioctl(disp.drm_fd, DRM_IOCTL_SET_MASTER, 0);
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
	FILE *f = fopen(path, "wb");
	int i, total = SHELL_SCREEN_W * SHELL_SCREEN_H;

	if (!f)
		return -errno;
	fprintf(f, "P6\n%d %d\n255\n", SHELL_SCREEN_W, SHELL_SCREEN_H);
	for (i = 0; i < total; i++) {
		uint32_t c = disp.backbuf[i];
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
	int x0, y0, x1, y1, py, px;

	if (w <= 0 || h <= 0)
		return;
	x0 = x < 0 ? 0 : x;
	y0 = y < 0 ? 0 : y;
	x1 = (x + w > SHELL_SCREEN_W) ? SHELL_SCREEN_W : (x + w);
	y1 = (y + h > SHELL_SCREEN_H) ? SHELL_SCREEN_H : (y + h);
	if (x0 >= x1 || y0 >= y1)
		return;

	color |= 0xff000000U;
	for (py = y0; py < y1; py++) {
		uint32_t *row = disp.backbuf + py * SHELL_SCREEN_W;

		for (px = x0; px < x1; px++)
			row[px] = color;
	}
	shell_display_damage(x0, y0, x1 - x0, y1 - y0);
}

void shell_draw_rect_alpha(int x, int y, int w, int h, uint32_t color, uint8_t alpha)
{
	int x0, y0, x1, y1, py, px;

	if (w <= 0 || h <= 0 || alpha == 0)
		return;
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

		for (px = x0; px < x1; px++)
			row[px] = blend_pixel(row[px], color, alpha);
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
	int sx, sy;
	float acc = 0.0f;
	float wf = (float)w, hf = (float)h, rf = (float)r;

	if (r <= 0)
		return 255;
	if (lx < 0 || ly < 0 || lx >= w || ly >= h)
		return 0;
	if (lx >= r && lx < w - r)
		return 255;
	if (ly >= r && ly < h - r)
		return 255;

	/* 2x2 subpixel rotated-grid SDF evaluation for ultra-smooth corners */
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

		for (px = 0; px < w; px++) {
			int sx = x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t cov = rounded_rect_cov255(px, py, w, h, radius);
			uint32_t a = (cov * alpha + 127U) / 255U;

			if (a == 255)
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
	float wf, hf, rf, tf;

	if (w <= 0 || h <= 0 || thickness <= 0)
		return;
	if (radius * 2 > w)
		radius = w / 2;
	if (radius * 2 > h)
		radius = h / 2;
	wf = (float)w;
	hf = (float)h;
	rf = (float)radius;
	tf = (float)thickness;
	color |= 0xff000000U;

	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		bool in_y_mid = (py >= radius && py < h - radius);

		for (px = 0; px < w; px++) {
			int sx = x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;

			/* Fast-path interior hollow region */
			if (in_y_mid && px >= thickness && px < w - thickness)
				continue;
			if (px >= radius && px < w - radius &&
			    py >= thickness && py < h - thickness)
				continue;

			/* Fast-path straight horizontal/vertical border segments */
			if ((in_y_mid && (px < thickness || px >= w - thickness)) ||
			    (px >= radius && px < w - radius &&
			     (py < thickness || py >= h - thickness))) {
				row[sx] = color;
				continue;
			}

			/* Exact perpendicular Euclidean SDF stroke at rounded corners */
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
			uint32_t a = (uint32_t)(acc * 63.75f + 0.5f);

			if (a >= 255)
				row[sx] = color;
			else if (a > 0)
				row[sx] = blend_pixel(row[sx], color, a);
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

	for (py = 0; py < gh; py++) {
		int sy = gy + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;

		for (px = 0; px < gw; px++) {
			int sx = gx + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			/* Skip interior of the element itself */
			if (px >= spread + radius && px < gw - spread - radius &&
			    py >= spread && py < gh - spread)
				continue;
			if (py >= spread + radius && py < gh - spread - radius &&
			    px >= spread && px < gw - spread)
				continue;

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
				uint32_t a = (cov * max_alpha + 127U) / 255U;

				row[sx] = blend_pixel(row[sx], glow_color, a);
			}
		}
	}
	shell_display_damage(gx, gy, gw, gh);
}

/*
 * Subtle drop shadow beneath elevated focused cards: 0 10px 22px rgba(0,0,0,0.22)
 */
void shell_draw_card_shadow(int x, int y, int w, int h, int radius)
{
	int s;

	for (s = 8; s >= 2; s -= 2) {
		shell_draw_rounded_rect_alpha(x - s / 2, y + 4 + s,
					      w + s, h, radius + s / 2,
					      0xff06080cU, 14);
	}
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

	for (py = y_start; py < card_h; py++) {
		int sy = card_y + py;

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

		for (px = 0; px < card_w; px++) {
			int sx = card_x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t cov = rounded_rect_cov255(px, py, card_w, card_h, radius);
			if (cov == 0)
				continue;

			int dither = (int)bayer4x4[(sy & 3) * 4 + (sx & 3)] - 7;
			int a255 = (int)(alpha_f * 255.0f) + (dither >> 2);
			if (a255 < 0)
				a255 = 0;
			if (a255 > 255)
				a255 = 255;
			a255 = (int)((cov * (uint32_t)a255 + 127U) / 255U);
			if (a255 > 0)
				row[sx] = blend_pixel(row[sx], 0xff06080cU, (uint32_t)a255);
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
	int py, px;

	for (py = 0; py < h && py < SHELL_SCREEN_H; py++) {
		uint32_t *row = disp.backbuf + py * SHELL_SCREEN_W;
		float t = (float)py / (float)(h > 1 ? h - 1 : 1);
		float alpha_f = (t <= 0.65f) ?
			(0.88f + (0.45f - 0.88f) * (t / 0.65f)) :
			(0.45f * (1.0f - (t - 0.65f) / 0.35f));

		for (px = 0; px < w && px < SHELL_SCREEN_W; px++) {
			int dither = (int)bayer4x4[(py & 3) * 4 + (px & 3)] - 7;
			int a = (int)(alpha_f * 255.0f) + (dither >> 2);

			if (a > 0) {
				if (a > 255)
					a = 255;
				row[px] = blend_pixel(row[px], 0xff06080cU, (uint32_t)a);
			}
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
	int py, px;

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

		for (px = 0; px < w && px < SHELL_SCREEN_W; px++) {
			int dither = (int)bayer4x4[(sy & 3) * 4 + (px & 3)] - 7;
			int a = (int)(alpha_f * 255.0f) + (dither >> 2);

			if (a > 0) {
				if (a > 255)
					a = 255;
				row[px] = blend_pixel(row[px], 0xff06080cU, (uint32_t)a);
			}
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
	int center_x = (w * phase_pct) / 100;
	int band_w = w / 3 > 1 ? w / 3 : 1;

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
	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		const uint32_t *src = pixels + py * w;

		for (px = 0; px < w; px++) {
			int sx = x + px;

			if (sx >= 0 && sx < SHELL_SCREEN_W)
				row[sx] = src[px] | 0xff000000U;
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_rounded_bitmap(int x, int y, int w, int h, int radius,
			       const uint32_t *pixels, bool zoom_slightly)
{
	int py, px;

	if (!pixels || w <= 0 || h <= 0)
		return;
	for (py = 0; py < h; py++) {
		int sy = y + py;

		if (sy < 0 || sy >= SHELL_SCREEN_H)
			continue;
		uint32_t *row = disp.backbuf + sy * SHELL_SCREEN_W;
		int y_q8 = py << 8;

		if (zoom_slightly) {
			/* Smooth bilinear 1.03x center zoom on focused card artwork */
			y_q8 = (h << 7) + (((py * 2 - h) * 124) );
			if (y_q8 < 0)
				y_q8 = 0;
			if (y_q8 > ((h - 1) << 8))
				y_q8 = (h - 1) << 8;
		}
		int y0 = y_q8 >> 8;
		int y1 = (y0 + 1 < h) ? (y0 + 1) : y0;
		uint32_t fy = (uint32_t)(y_q8 & 0xff);
		const uint32_t *row0 = pixels + y0 * w;
		const uint32_t *row1 = pixels + y1 * w;

		for (px = 0; px < w; px++) {
			int sx = x + px;

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t sample;

			if (!zoom_slightly) {
				sample = row0[px] | 0xff000000U;
			} else {
				int x_q8 = (w << 7) + (((px * 2 - w) * 124));
				if (x_q8 < 0)
					x_q8 = 0;
				if (x_q8 > ((w - 1) << 8))
					x_q8 = (w - 1) << 8;
				int x0 = x_q8 >> 8;
				int x1 = (x0 + 1 < w) ? (x0 + 1) : x0;
				uint32_t fx = (uint32_t)(x_q8 & 0xff);
				uint32_t c00 = row0[x0], c10 = row0[x1];
				uint32_t c01 = row1[x0], c11 = row1[x1];
				uint32_t w00 = (256U - fx) * (256U - fy);
				uint32_t w10 = fx * (256U - fy);
				uint32_t w01 = (256U - fx) * fy;
				uint32_t w11 = fx * fy;
				uint32_t r = (((c00 >> 16) & 0xffU) * w00 +
					      ((c10 >> 16) & 0xffU) * w10 +
					      ((c01 >> 16) & 0xffU) * w01 +
					      ((c11 >> 16) & 0xffU) * w11 + 32768U) >> 16;
				uint32_t g = (((c00 >> 8) & 0xffU) * w00 +
					      ((c10 >> 8) & 0xffU) * w10 +
					      ((c01 >> 8) & 0xffU) * w01 +
					      ((c11 >> 8) & 0xffU) * w11 + 32768U) >> 16;
				uint32_t b = ((c00 & 0xffU) * w00 +
					      (c10 & 0xffU) * w10 +
					      (c01 & 0xffU) * w01 +
					      (c11 & 0xffU) * w11 + 32768U) >> 16;
				sample = 0xff000000U | (r << 16) | (g << 8) | b;
			}
			uint32_t a = rounded_rect_cov255(px, py, w, h, radius);

			if (a == 255)
				row[sx] = sample;
			else if (a > 0)
				row[sx] = blend_pixel(row[sx], sample, a);
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_circle_alpha(int cx, int cy, int r, uint32_t color, uint8_t alpha)
{
	int d = r * 2;

	shell_draw_rounded_rect_alpha(cx - r, cy - r, d, d, r, color, alpha);
}

void shell_draw_symbol(enum shell_symbol_id sym, int x, int y, int size, uint32_t fg)
{
	const uint8_t *mask = shell_get_symbol_mask(sym, size);
	int py, px;

	if (!mask)
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
