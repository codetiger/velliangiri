// SPDX-License-Identifier: GPL-2.0-only
/*
 * fbtest - framebuffer write throughput and sync smoke test
 *
 * Fills the visible framebuffer with a moving pattern and reports the
 * sustained rate, so the cost of presenting a frame at a given mode can be
 * compared (for example 1280x720@60 against 1920x1080@30). With -v it waits
 * for vertical sync between frames and says whether the driver supports it;
 * without it the loop measures the raw write cost.
 *
 * The DRM framebuffer is 32 bits per pixel (XRGB8888), which is what a game
 * writing directly to /dev/fb0 has to produce.
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double now_sec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-d /dev/fb0] [-s seconds] [-v] [-q]\n"
		"  -d DEV  framebuffer device (default /dev/fb0)\n"
		"  -s SEC  measurement time in seconds (default 5)\n"
		"  -v      wait for vertical sync between frames (exit 3 if the\n"
		"          driver accepts the call but does not block)\n"
		"  -q      print the mode and draw one frame, then exit\n",
		argv0);
}

/*
 * One full-screen frame: a horizontal shade ramp that scrolls with the frame
 * number, plus a moving block so motion is visible on the display.
 */
static void draw_frame(uint32_t *base, unsigned int w, unsigned int h,
		       unsigned int pitch, unsigned int frame)
{
	unsigned int y, x;

	for (y = 0; y < h; y++) {
		uint32_t *row = base + (size_t)y * pitch;
		unsigned int v = ((y + frame) & 63) * 4;
		uint32_t c = 0xff000000U | (v << 16) | ((255U - v) << 8) |
			     ((v * 3U) & 0xffU);

		for (x = 0; x < w; x++)
			row[x] = c;
	}

	if (w > 64 && h > 64) {
		unsigned int bx = (frame * 4) % (w - 64);
		unsigned int by = (h - 64) / 2;

		for (y = by; y < by + 64; y++) {
			uint32_t *row = base + (size_t)y * pitch;

			for (x = bx; x < bx + 64; x++)
				row[x] = 0xffffffffU;
		}
	}
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/fb0";
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	unsigned int seconds = 5;
	int wait_vsync = 0, quick = 0;
	unsigned int w, h, pitch, frame = 0;
	size_t map_len, frame_bytes;
	uint32_t *fb;
	double start, elapsed, fps;
	double wait_total = 0.0;
	unsigned int wait_count = 0;
	int vsync_ok = -1;
	int fd, opt, status = 0;

	while ((opt = getopt(argc, argv, "d:s:vqh")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		case 's':
			seconds = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'v':
			wait_vsync = 1;
			break;
		case 'q':
			quick = 1;
			break;
		default:
			usage(argv[0]);
			return opt == 'h' ? 0 : 2;
		}
	}

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "fbtest: %s: %s\n", dev, strerror(errno));
		return 1;
	}

	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &var) < 0) {
		fprintf(stderr, "fbtest: %s: not a framebuffer\n", dev);
		close(fd);
		return 1;
	}

	if (var.bits_per_pixel != 32) {
		fprintf(stderr, "fbtest: %s is %u bpp, expected 32\n",
			dev, var.bits_per_pixel);
		close(fd);
		return 1;
	}

	w = var.xres;
	h = var.yres;
	pitch = fix.line_length / 4;
	map_len = (size_t)fix.line_length * var.yres_virtual;

	fb = mmap(NULL, map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		fprintf(stderr, "fbtest: mmap: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	frame_bytes = (size_t)fix.line_length * h;
	printf("fbtest: %s %ux%u %u bpp, stride %u, %.1f MiB per frame\n",
	       dev, w, h, var.bits_per_pixel, fix.line_length,
	       (double)frame_bytes / (1024.0 * 1024.0));
	if (var.yres_virtual > var.yres)
		printf("fbtest: %u virtual rows (panning may be possible)\n",
		       var.yres_virtual);
	else
		printf("fbtest: single buffer, no panning\n");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (quick) {
		draw_frame(fb, w, h, pitch, 0);
		printf("fbtest: drew one %ux%u frame\n", w, h);
		munmap(fb, map_len);
		close(fd);
		return 0;
	}

	if (seconds == 0)
		seconds = 1;

	start = now_sec();
	for (;;) {
		if (wait_vsync && vsync_ok != 0) {
			int zero = 0;
			double w0 = now_sec();

			if (ioctl(fd, FBIO_WAITFORVSYNC, &zero) < 0) {
				if (vsync_ok < 0) {
					printf("fbtest: vertical sync not supported (%s)\n",
					       strerror(errno));
					wait_vsync = 0;
				}
				vsync_ok = 0;
			} else {
				vsync_ok = 1;
				wait_total += now_sec() - w0;
				wait_count++;
			}
		}

		draw_frame(fb, w, h, pitch, frame++);

		elapsed = now_sec() - start;
		if (stop || elapsed >= (double)seconds)
			break;
	}

	elapsed = now_sec() - start;
	if (elapsed <= 0)
		elapsed = 1e-6;
	fps = (double)frame / elapsed;

	if (wait_vsync && vsync_ok == 1) {
		double avg_wait = wait_count ? wait_total / wait_count : 0.0;

		/*
		 * The generic framebuffer ioctl returns success even when the
		 * CRTC has no vblank, so judge it by the time spent in it: a
		 * blocking call costs a noticeable part of a frame.
		 */
		if (avg_wait < 0.0005) {
			printf("fbtest: vertical sync accepted but not blocking "
			       "(%.3f ms per call: no vblank interrupt)\n",
			       avg_wait * 1000.0);
			status = 3;
		} else {
			printf("fbtest: vertical sync blocking, %.2f ms per call\n",
			       avg_wait * 1000.0);
		}
	}

	printf("fbtest: %u frames in %.2f s, %.1f fps, %.0f MB/s, %.2f ms/frame\n",
	       frame, elapsed, fps,
	       (double)frame * (double)frame_bytes / elapsed / 1e6,
	       1000.0 / fps);
	if (!wait_vsync)
		printf("fbtest: write cost only; add -v to measure frame pacing\n");

	munmap(fb, map_len);
	close(fd);
	return status;
}
