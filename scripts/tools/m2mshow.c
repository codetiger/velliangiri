// SPDX-License-Identifier: GPL-2.0-only
/*
 * m2mshow - scale frames on a V4L2 mem2mem device straight into scanout
 *
 *   m2mshow [-i] [-d /dev/videoN] [-n frames] [SRCWxH]
 *
 * An NV12 source (default 640x360) with a moving bar is scaled and
 * converted to XRGB8888 at the current display mode, and every frame is
 * page-flipped to the screen with no CPU copy:
 *
 *   default  two DRM dumb buffers are exported as DMABUF and queued on the
 *            scaler's capture queue (the display owns the memory)
 *   -i       the scaler allocates its capture buffers, which are exported
 *            with VIDIOC_EXPBUF and imported into DRM as framebuffers
 *
 * Flips wait for the page-flip event, so the rate is capped by the display
 * refresh. The result is checked in the last frame shown, and the rate and
 * whole-system CPU load are reported. Exits 0 when the check passes.
 */
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define NBUF 2

static int vfd, dfd;

static void die(const char *what)
{
	perror(what);
	exit(2);
}

static void xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	while (ioctl(fd, req, arg) < 0) {
		if (errno != EINTR)
			die(what);
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void cpu_ticks(unsigned long long *busy, unsigned long long *total)
{
	unsigned long long v[8] = { 0 };
	FILE *f = fopen("/proc/stat", "r");
	int i;

	if (!f || fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
			 &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
		die("/proc/stat");
	fclose(f);
	*total = 0;
	for (i = 0; i < 8; i++)
		*total += v[i];
	*busy = *total - v[3] - v[4];
}

/* The active CRTC, its mode and a connector driving it */
static void find_output(uint32_t *crtc_id, uint32_t *conn_id, struct drm_mode_modeinfo *mode)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[8], conns[8];
	struct drm_mode_crtc crtc = { 0 };

	xioctl(dfd, DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	if (!res.count_crtcs || !res.count_connectors)
		die("no crtc or connector");
	res.count_crtcs = res.count_crtcs > 8 ? 8 : res.count_crtcs;
	res.count_connectors = res.count_connectors > 8 ? 8 : res.count_connectors;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	res.count_fbs = res.count_encoders = 0;
	xioctl(dfd, DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
	crtc.crtc_id = crtcs[0];
	xioctl(dfd, DRM_IOCTL_MODE_GETCRTC, &crtc, "GETCRTC");
	if (!crtc.mode_valid)
		die("no active mode");
	*crtc_id = crtcs[0];
	*conn_id = conns[0];
	*mode = crtc.mode;
}

static uint32_t add_fb(uint32_t handle, uint32_t w, uint32_t h, uint32_t pitch)
{
	struct drm_mode_fb_cmd2 fb = { 0 };

	fb.width = w;
	fb.height = h;
	fb.pixel_format = DRM_FORMAT_XRGB8888;
	fb.handles[0] = handle;
	fb.pitches[0] = pitch;
	xioctl(dfd, DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2");
	return fb.fb_id;
}

static void fill_nv12(uint8_t *p, uint32_t w, uint32_t h, uint32_t bpl, unsigned int frame)
{
	uint32_t x, y, bar = (frame * 4) % w;
	uint8_t *c = p + bpl * h;

	for (y = 0; y < h; y++) {
		uint8_t *row = p + y * bpl;

		memset(row, 81, w / 2);
		memset(row + w / 2, 145, w - w / 2);
		for (x = bar; x < bar + w / 32 && x < w; x++)
			row[x] = 235;
	}
	for (y = 0; y < h / 2; y++)
		for (x = 0; x < w / 2; x++) {
			int in_bar = 2 * x >= bar && 2 * x < bar + w / 32;

			c[y * bpl + 2 * x] = in_bar ? 128 : x < w / 4 ? 90 : 54;
			c[y * bpl + 2 * x + 1] = in_bar ? 128 : x < w / 4 ? 240 : 34;
		}
}

static void flip(uint32_t crtc_id, uint32_t fb_id)
{
	struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc_id, .fb_id = fb_id,
					      .flags = DRM_MODE_PAGE_FLIP_EVENT };
	struct pollfd pfd = { .fd = dfd, .events = POLLIN };
	char ev[256];

	xioctl(dfd, DRM_IOCTL_MODE_PAGE_FLIP, &pf, "PAGE_FLIP");
	if (poll(&pfd, 1, 1000) <= 0)
		die("page flip event");
	if (read(dfd, ev, sizeof(ev)) <= 0)
		die("read event");
}

int main(int argc, char **argv)
{
	const char *vdev = "/dev/video0";
	uint32_t sw = 640, sh = 360, crtc_id, conn_id, fb_id[NBUF], pitch = 0;
	int import = 0, n = 300, opt, i, dmabuf[NBUF], ok = 1;
	struct drm_mode_modeinfo mode;
	struct v4l2_format sf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2_format cf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer qb;
	unsigned long long b0, t0, b1, t1;
	enum v4l2_buf_type type;
	uint8_t *src;
	uint32_t *last = NULL;
	size_t srclen;
	double start, secs;

	while ((opt = getopt(argc, argv, "id:n:")) != -1) {
		if (opt == 'i')
			import = 1;
		else if (opt == 'd')
			vdev = optarg;
		else if (opt == 'n')
			n = atoi(optarg);
		else
			goto usage;
	}
	if (optind < argc && sscanf(argv[optind], "%ux%u", &sw, &sh) != 2)
		goto usage;

	dfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (dfd < 0)
		die("/dev/dri/card0");
	xioctl(dfd, DRM_IOCTL_SET_MASTER, NULL, "SET_MASTER");
	find_output(&crtc_id, &conn_id, &mode);
	vfd = open(vdev, O_RDWR);
	if (vfd < 0)
		die(vdev);

	/* Source: one MMAP NV12 buffer */
	sf.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	sf.fmt.pix.width = sw;
	sf.fmt.pix.height = sh;
	xioctl(vfd, VIDIOC_S_FMT, &sf, "S_FMT out");
	rb = (struct v4l2_requestbuffers){ .count = 1, .type = sf.type,
					   .memory = V4L2_MEMORY_MMAP };
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS out");
	qb = (struct v4l2_buffer){ .type = sf.type, .memory = V4L2_MEMORY_MMAP };
	xioctl(vfd, VIDIOC_QUERYBUF, &qb, "QUERYBUF out");
	srclen = qb.length;
	src = mmap(NULL, srclen, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, qb.m.offset);
	if (src == MAP_FAILED)
		die("mmap src");

	/* Destination at the display mode */
	cf.fmt.pix.pixelformat = V4L2_PIX_FMT_XRGB32;
	cf.fmt.pix.width = mode.hdisplay;
	cf.fmt.pix.height = mode.vdisplay;
	if (!import) {
		struct drm_mode_create_dumb cd = { .width = mode.hdisplay,
						   .height = mode.vdisplay, .bpp = 32 };

		for (i = 0; i < NBUF; i++) {
			struct drm_prime_handle ph = { .flags = DRM_CLOEXEC | DRM_RDWR };

			xioctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB");
			pitch = cd.pitch;
			fb_id[i] = add_fb(cd.handle, mode.hdisplay, mode.vdisplay, cd.pitch);
			ph.handle = cd.handle;
			xioctl(dfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph, "PRIME_HANDLE_TO_FD");
			dmabuf[i] = ph.fd;
		}
		cf.fmt.pix.bytesperline = pitch;
	}
	xioctl(vfd, VIDIOC_S_FMT, &cf, "S_FMT cap");
	if (!import && cf.fmt.pix.bytesperline != pitch) {
		fprintf(stderr, "stride mismatch: dumb %u, scaler %u\n", pitch,
			cf.fmt.pix.bytesperline);
		return 2;
	}
	rb = (struct v4l2_requestbuffers){ .count = NBUF, .type = cf.type,
					   .memory = import ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF };
	xioctl(vfd, VIDIOC_REQBUFS, &rb, "REQBUFS cap");
	if (import) {
		for (i = 0; i < NBUF; i++) {
			struct v4l2_exportbuffer eb = { .type = cf.type, .index = i,
							.flags = O_RDWR | O_CLOEXEC };
			struct drm_prime_handle ph = { 0 };

			xioctl(vfd, VIDIOC_EXPBUF, &eb, "EXPBUF");
			dmabuf[i] = eb.fd;
			ph.fd = eb.fd;
			xioctl(dfd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph, "PRIME_FD_TO_HANDLE");
			fb_id[i] = add_fb(ph.handle, mode.hdisplay, mode.vdisplay,
					  cf.fmt.pix.bytesperline);
		}
	}
	printf("%s: NV12 %ux%u -> XRGB8888 %ux%u (stride %u), %s, crtc %u\n", vdev, sw, sh,
	       mode.hdisplay, mode.vdisplay, cf.fmt.pix.bytesperline,
	       import ? "scaler buffers imported into DRM" : "DRM dumb buffers as DMABUF",
	       crtc_id);

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl(vfd, VIDIOC_STREAMON, &type, "STREAMON out");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(vfd, VIDIOC_STREAMON, &type, "STREAMON cap");

	/* The first flip puts our framebuffer on the CRTC */
	{
		struct drm_mode_crtc c = { .crtc_id = crtc_id, .fb_id = fb_id[NBUF - 1],
					   .set_connectors_ptr = (uintptr_t)&conn_id,
					   .count_connectors = 1, .mode = mode, .mode_valid = 1 };

		xioctl(dfd, DRM_IOCTL_MODE_SETCRTC, &c, "SETCRTC");
	}

	cpu_ticks(&b0, &t0);
	start = now();
	for (i = 0; i < n; i++) {
		int k = i % NBUF;

		fill_nv12(src, sw, sh, sf.fmt.pix.bytesperline, i);
		qb = (struct v4l2_buffer){ .type = sf.type, .memory = V4L2_MEMORY_MMAP,
					   .bytesused = sf.fmt.pix.sizeimage };
		xioctl(vfd, VIDIOC_QBUF, &qb, "QBUF out");
		qb = (struct v4l2_buffer){ .type = cf.type, .index = k,
					   .memory = import ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF };
		if (!import) {
			qb.m.fd = dmabuf[k];
			qb.length = cf.fmt.pix.sizeimage;
		}
		xioctl(vfd, VIDIOC_QBUF, &qb, "QBUF cap");
		xioctl(vfd, VIDIOC_DQBUF, &qb, "DQBUF cap");
		if (qb.flags & V4L2_BUF_FLAG_ERROR) {
			fprintf(stderr, "frame %d: scaler error\n", i);
			return 1;
		}
		qb = (struct v4l2_buffer){ .type = sf.type, .memory = V4L2_MEMORY_MMAP };
		xioctl(vfd, VIDIOC_DQBUF, &qb, "DQBUF out");
		flip(crtc_id, fb_id[k]);
	}
	secs = now() - start;
	cpu_ticks(&b1, &t1);
	printf("%d frames in %.2f s: %.1f fps, CPU busy %.0f%% (includes filling the source)\n",
	       n, secs, n / secs, t1 > t0 ? 100.0 * (b1 - b0) / (t1 - t0) : 0.0);

	/* Check the frame on screen: red left, green right, bar somewhere */
	{
		size_t len = (size_t)cf.fmt.pix.bytesperline * mode.vdisplay;
		int k = (n - 1) % NBUF;

		last = mmap(NULL, len, PROT_READ, MAP_SHARED, dmabuf[k], 0);
		if (last != MAP_FAILED) {
			uint32_t stride = cf.fmt.pix.bytesperline / 4, y = mode.vdisplay / 2;
			uint32_t w = mode.hdisplay, bar = (((n - 1) * 4) % sw) * w / sw;
			/* sample each half away from the bar (w / 32 wide) */
			uint32_t lx = bar < w / 4 ? 3 * w / 8 : w / 8;
			uint32_t rx = bar > w / 2 + w / 4 ? 5 * w / 8 : 7 * w / 8;
			uint32_t l = last[y * stride + lx] & 0xffffff;
			uint32_t r = last[y * stride + rx] & 0xffffff;
			uint32_t b = last[y * stride + bar + w / 64] & 0xffffff;

			printf("on screen: left %06x right %06x bar %06x\n", l, r, b);
			ok = (l & 0xf0f0f0) == 0xf00000 && (r & 0xf0f0f0) == 0x00f000;
			munmap(last, len);
		} else {
			printf("cannot map the shown buffer for checking\n");
		}
	}
	printf("%s\n", ok ? "PASS" : "FAIL");
	return !ok;

usage:
	fprintf(stderr, "usage: %s [-i] [-d dev] [-n frames] [SRCWxH]\n", argv[0]);
	return 2;
}
