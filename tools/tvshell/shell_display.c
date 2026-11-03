// SPDX-License-Identifier: GPL-2.0-only
/*
 * Velliangiri 1280x720 TV Shell - Display Surface & Damaged-Rectangle Renderer
 *
 * Uses a cached 1280x720 XRGB8888 backbuffer and copies only damaged
 * rectangles to the scanout mapping (/dev/dri/card0 dumb buffer or /dev/fb0).
 * Supports clean display release and 1280x720@30 mode restoration when
 * launching external applications such as Doom.
 */

#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/fb.h>
#include <linux/kd.h>
#endif
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

/* Nearest 8-bit sRGB value of a 16-bit linear value */
static inline uint32_t linear_to_srgb8(uint32_t lin)
{
	uint32_t lo = 0, hi = 255;

	while (lo < hi) {
		uint32_t mid = (lo + hi + 1) >> 1;

		if (shell_srgb_to_linear[mid] <= lin)
			lo = mid;
		else
			hi = mid - 1;
	}
	if (lo < 255 &&
	    lin - shell_srgb_to_linear[lo] > shell_srgb_to_linear[lo + 1] - lin)
		lo++;
	return lo;
}

static inline uint32_t blend_pixel_linear(uint32_t dst, uint32_t src, uint32_t a255)
{
	uint32_t out = 0xff000000U;
	uint32_t a = a255 + (a255 >> 7); /* 0..255 -> 0..256 */
	int sh;

	if (a == 0)
		return dst;
	if (a >= 256)
		return src | 0xff000000U;

	for (sh = 0; sh < 24; sh += 8) {
		uint32_t d = shell_srgb_to_linear[(dst >> sh) & 0xffU];
		uint32_t s = shell_srgb_to_linear[(src >> sh) & 0xffU];
		uint32_t v = (d * (256 - a) + s * a) >> 8;

		out |= linear_to_srgb8(v) << sh;
	}
	return out;
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

	/* Prefer 1280x720@30, then any 1280x720, then preferred mode */
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

	/* Bypass the hardware gamma LUT (100 = OUT_GAMMA_EN 0) */
	ensure_720p30_and_gamma();

	/* Silence the fbcon cursor on tty0 */
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

/* --- Drawing primitives --- */

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

/*
 * 4x4 subpixel coverage for a rounded rectangle of size (w, h) and corner
 * radius r at local coordinates (lx, ly).
 */
static inline uint32_t rounded_rect_cov255(int lx, int ly, int w, int h, int r)
{
	int cx, cy, sx, sy, inside = 0;
	int r4;

	if (r <= 0)
		return 255;
	if (lx >= r && lx < w - r)
		return 255;
	if (ly >= r && ly < h - r)
		return 255;

	cx = (lx < r) ? (r * 4) : ((w - r) * 4);
	cy = (ly < r) ? (r * 4) : ((h - r) * 4);
	r4 = r * 4;

	for (sy = 0; sy < 4; sy++) {
		int dy = (ly * 4 + sy) * 2 + 1 - cy * 2;

		for (sx = 0; sx < 4; sx++) {
			int dx = (lx * 4 + sx) * 2 + 1 - cx * 2;

			if (dx * dx + dy * dy <= (r4 * 2) * (r4 * 2))
				inside++;
		}
	}
	return (uint32_t)((inside * 255 + 8) >> 4);
}

void shell_draw_rounded_rect(int x, int y, int w, int h, int radius, uint32_t color)
{
	int py, px;

	if (radius <= 0) {
		shell_draw_rect(x, y, w, h, color);
		return;
	}
	if (w <= 0 || h <= 0)
		return;
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
			uint32_t a = rounded_rect_cov255(px, py, w, h, radius);

			if (a == 255)
				row[sx] = color;
			else if (a > 0)
				row[sx] = blend_pixel_linear(row[sx], color, a);
		}
	}
	shell_display_damage(x, y, w, h);
}

void shell_draw_rounded_outline(int x, int y, int w, int h, int radius,
				int thickness, uint32_t color)
{
	int py, px;
	int inner_r = radius > thickness ? radius - thickness : 0;
	int inner_w = w - 2 * thickness;
	int inner_h = h - 2 * thickness;

	if (w <= 0 || h <= 0 || thickness <= 0)
		return;
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
			uint32_t outer_a = rounded_rect_cov255(px, py, w, h, radius);
			uint32_t inner_a = 0;

			if (px >= thickness && px < w - thickness &&
			    py >= thickness && py < h - thickness) {
				inner_a = rounded_rect_cov255(px - thickness,
							      py - thickness,
							      inner_w, inner_h,
							      inner_r);
			}
			if (outer_a > inner_a) {
				uint32_t a = outer_a - inner_a;

				row[sx] = blend_pixel_linear(row[sx], color, a);
			}
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
			       const uint32_t *pixels)
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

			if (sx < 0 || sx >= SHELL_SCREEN_W)
				continue;
			uint32_t a = rounded_rect_cov255(px, py, w, h, radius);

			if (a == 255)
				row[sx] = src[px] | 0xff000000U;
			else if (a > 0)
				row[sx] = blend_pixel_linear(row[sx], src[px], a);
		}
	}
	shell_display_damage(x, y, w, h);
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
				row[sx] = blend_pixel_linear(row[sx], fg, a);
		}
	}
	shell_display_damage(x, y, size, size);
}
