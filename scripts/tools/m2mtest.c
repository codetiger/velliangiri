// SPDX-License-Identifier: GPL-2.0-only
/*
 * m2mtest - drive a V4L2 memory-to-memory scaler and measure it
 *
 *   m2mtest [-d /dev/videoN] [-n frames] [-c X,Y,WxH] [-z] SRCFMT SRCWxH DSTFMT DSTWxH
 *
 * Formats: nv12, nv16, yuyv, xrgb, rgb565. The source is filled with a
 * known pattern (red left half and green right half for YCbCr sources,
 * four colour quadrants for RGB sources), one frame is converted and, for
 * an xrgb destination, checked at the centre of each region. Then the
 * frame is converted N times and the rate, the time per frame and the
 * whole-system CPU load during the run are reported. Exits 0 when the
 * check passes.
 *
 * With -c the result is composed into that rectangle of the destination
 * (V4L2_SEL_TGT_COMPOSE), and the check also requires the pixels around the
 * rectangle to be left untouched.
 *
 * With -z the source is a one-pixel checkerboard of black and white
 * instead (xrgb or nv12 sources). A downscale that averages properly
 * turns it into an even mid grey; one that skips pixels leaves bands or
 * moire. The check then requires every destination pixel (xrgb) to be
 * within 24 of the mean grey level, ignoring the outermost row and column
 * on each side, where the filter runs off the frame. It only makes sense
 * for downscales.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct buf {
	void *p;
	size_t len;
};

static int fd;
static struct v4l2_rect comp;	/* destination rectangle, width 0 = whole frame */

static void die(const char *what)
{
	perror(what);
	exit(2);
}

static void xioctl(unsigned long req, void *arg, const char *what)
{
	while (ioctl(fd, req, arg) < 0) {
		if (errno != EINTR)
			die(what);
	}
}

static uint32_t parse_fmt(const char *s)
{
	if (!strcmp(s, "nv12"))
		return V4L2_PIX_FMT_NV12;
	if (!strcmp(s, "nv16"))
		return V4L2_PIX_FMT_NV16;
	if (!strcmp(s, "yuyv"))
		return V4L2_PIX_FMT_YUYV;
	if (!strcmp(s, "xrgb"))
		return V4L2_PIX_FMT_XRGB32;
	if (!strcmp(s, "rgb565"))
		return V4L2_PIX_FMT_RGB565;
	fprintf(stderr, "unknown format %s\n", s);
	exit(2);
}

static void parse_size(const char *s, uint32_t *w, uint32_t *h)
{
	if (sscanf(s, "%ux%u", w, h) != 2) {
		fprintf(stderr, "bad size %s\n", s);
		exit(2);
	}
}

static void setup(enum v4l2_buf_type type, uint32_t fourcc, uint32_t w, uint32_t h,
		  struct v4l2_pix_format *out, struct buf *b)
{
	struct v4l2_format f = { .type = type };
	struct v4l2_requestbuffers rb = { .count = 1, .type = type, .memory = V4L2_MEMORY_MMAP };
	struct v4l2_buffer qb = { .type = type, .memory = V4L2_MEMORY_MMAP };

	f.fmt.pix.pixelformat = fourcc;
	f.fmt.pix.width = w;
	f.fmt.pix.height = h;
	xioctl(VIDIOC_S_FMT, &f, "S_FMT");
	*out = f.fmt.pix;
	xioctl(VIDIOC_REQBUFS, &rb, "REQBUFS");
	xioctl(VIDIOC_QUERYBUF, &qb, "QUERYBUF");
	b->len = qb.length;
	b->p = mmap(NULL, qb.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, qb.m.offset);
	if (b->p == MAP_FAILED)
		die("mmap");
}

static int checker;

static void fill_checker(const struct v4l2_pix_format *f, uint8_t *p)
{
	uint32_t x, y, bpl = f->bytesperline;

	for (y = 0; y < f->height; y++)
		for (x = 0; x < f->width; x++) {
			int on = (x ^ y) & 1;

			if (f->pixelformat == V4L2_PIX_FMT_XRGB32)
				((uint32_t *)(p + y * bpl))[x] = on ? 0xffffff : 0;
			else
				p[y * bpl + x] = on ? 235 : 16;
		}
	if (f->pixelformat == V4L2_PIX_FMT_NV12)
		memset(p + bpl * f->height, 128, bpl * f->height / 2);
}

/* Spread of the green channel inside the composed rectangle */
static int check_checker(const struct v4l2_pix_format *d, const uint8_t *p)
{
	uint32_t x, y, lo = 255, hi = 0, bpl = d->bytesperline;
	uint64_t sum = 0;
	uint32_t mean, bad = 0;

	if (d->pixelformat != V4L2_PIX_FMT_XRGB32)
		return -1;
	for (y = comp.top; y < comp.top + comp.height; y++)
		for (x = comp.left; x < comp.left + comp.width; x++) {
			uint32_t g = (((const uint32_t *)(p + y * bpl))[x] >> 8) & 0xff;

			sum += g;
			lo = g < lo ? g : lo;
			hi = g > hi ? g : hi;
		}
	mean = sum / ((uint64_t)comp.width * comp.height);
	for (y = comp.top; y < comp.top + comp.height; y++)
		for (x = comp.left; x < comp.left + comp.width; x++) {
			int g = (((const uint32_t *)(p + y * bpl))[x] >> 8) & 0xff;

			if (x == (uint32_t)comp.left || x == comp.left + comp.width - 1 ||
			    y == (uint32_t)comp.top || y == comp.top + comp.height - 1)
				continue;
			if (g - (int)mean > 24 || (int)mean - g > 24)
				bad++;
		}
	printf("  checkerboard: grey mean %u, min %u, max %u, %u inner pixels off by more than 24\n",
	       mean, lo, hi, bad);
	return !bad;
}

static void fill(const struct v4l2_pix_format *f, uint8_t *p)
{
	if (checker) {
		fill_checker(f, p);
		return;
	}
	uint32_t x, y, bpl = f->bytesperline;

	switch (f->pixelformat) {
	case V4L2_PIX_FMT_NV12:
	case V4L2_PIX_FMT_NV16: {
		uint32_t ch = f->pixelformat == V4L2_PIX_FMT_NV12 ? f->height / 2 : f->height;
		uint8_t *c = p + bpl * f->height;

		for (y = 0; y < f->height; y++)
			for (x = 0; x < f->width; x++)
				p[y * bpl + x] = x < f->width / 2 ? 81 : 145;
		for (y = 0; y < ch; y++)
			for (x = 0; x < f->width / 2; x++) {
				c[y * bpl + 2 * x] = x < f->width / 4 ? 90 : 54;
				c[y * bpl + 2 * x + 1] = x < f->width / 4 ? 240 : 34;
			}
		break;
	}
	case V4L2_PIX_FMT_YUYV:
		for (y = 0; y < f->height; y++)
			for (x = 0; x < f->width; x += 2) {
				uint8_t *q = p + y * bpl + 2 * x;
				int left = x < f->width / 2;

				q[0] = q[2] = left ? 81 : 145;
				q[1] = left ? 90 : 54;
				q[3] = left ? 240 : 34;
			}
		break;
	case V4L2_PIX_FMT_XRGB32: {
		static const uint32_t quad[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };

		for (y = 0; y < f->height; y++)
			for (x = 0; x < f->width; x++)
				((uint32_t *)(p + y * bpl))[x] =
					quad[(x >= f->width / 2) | (y >= f->height / 2) << 1];
		break;
	}
	default:
		memset(p, 0x80, f->sizeimage);
	}
}

static int near(uint32_t a, uint32_t b)
{
	int i;

	for (i = 0; i < 24; i += 8) {
		int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff);

		if (d > 6 || d < -6)
			return 0;
	}
	return 1;
}

/* Check region centres of an xrgb result against the source pattern */
static int check(const struct v4l2_pix_format *s, const struct v4l2_pix_format *d,
		 const uint8_t *p)
{
	int yuv = s->pixelformat != V4L2_PIX_FMT_XRGB32 && s->pixelformat != V4L2_PIX_FMT_RGB565;
	uint32_t x0 = comp.left, y0 = comp.top, w = comp.width, h = comp.height;
	uint32_t bpl = d->bytesperline;
	static const uint32_t quad[4] = { 0xff0000, 0x00ff00, 0x0000ff, 0xffffff };
	uint32_t pts[4][2] = { { x0 + w / 4, y0 + h / 4 }, { x0 + 3 * w / 4, y0 + h / 4 },
			       { x0 + w / 4, y0 + 3 * h / 4 }, { x0 + 3 * w / 4, y0 + 3 * h / 4 } };
	uint32_t x, y, outside = 0;
	int i, ok = 1;

	if (d->pixelformat != V4L2_PIX_FMT_XRGB32 || s->pixelformat == V4L2_PIX_FMT_RGB565)
		return -1;
	for (y = 0; y < d->height; y++)
		for (x = 0; x < d->width; x++)
			if ((x < x0 || x >= x0 + w || y < y0 || y >= y0 + h) &&
			    ((const uint32_t *)(p + y * bpl))[x])
				outside++;
	printf("  %u pixels written outside %ux%u+%u+%u\n", outside, w, h, x0, y0);
	ok &= !outside;
	for (i = 0; i < 4; i++) {
		uint32_t v = ((const uint32_t *)(p + pts[i][1] * bpl))[pts[i][0]] & 0xffffff;
		uint32_t want = yuv ? (i & 1 ? 0x00ff00 : 0xff0000) : quad[i];

		printf("  (%u,%u) %06x want %06x\n", pts[i][0], pts[i][1], v, want);
		ok &= near(v, want);
	}
	return ok;
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

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t src_size;

static void one_frame(void)
{
	struct v4l2_buffer b = { .memory = V4L2_MEMORY_MMAP };

	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.bytesused = src_size;
	xioctl(VIDIOC_QBUF, &b, "QBUF out");
	b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				  .memory = V4L2_MEMORY_MMAP };
	xioctl(VIDIOC_QBUF, &b, "QBUF cap");
	b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
				  .memory = V4L2_MEMORY_MMAP };
	xioctl(VIDIOC_DQBUF, &b, "DQBUF cap");
	if (b.flags & V4L2_BUF_FLAG_ERROR) {
		fprintf(stderr, "job failed (buffer error)\n");
		exit(1);
	}
	b = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
				  .memory = V4L2_MEMORY_MMAP };
	xioctl(VIDIOC_DQBUF, &b, "DQBUF out");
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/video0";
	struct v4l2_pix_format s, d;
	struct buf sb, db;
	uint32_t sw, sh, dw, dh, sf, df;
	unsigned long long b0, t0, b1, t1;
	int n = 100, opt, i, ok;
	enum v4l2_buf_type type;
	double start, secs;

	while ((opt = getopt(argc, argv, "d:n:c:z")) != -1) {
		if (opt == 'd')
			dev = optarg;
		else if (opt == 'n')
			n = atoi(optarg);
		else if (opt == 'c' && sscanf(optarg, "%d,%d,%ux%u", &comp.left, &comp.top,
					      &comp.width, &comp.height) == 4)
			;
		else if (opt == 'z')
			checker = 1;
		else
			goto usage;
	}
	if (argc - optind != 4)
		goto usage;
	sf = parse_fmt(argv[optind]);
	parse_size(argv[optind + 1], &sw, &sh);
	df = parse_fmt(argv[optind + 2]);
	parse_size(argv[optind + 3], &dw, &dh);

	fd = open(dev, O_RDWR);
	if (fd < 0)
		die(dev);
	setup(V4L2_BUF_TYPE_VIDEO_OUTPUT, sf, sw, sh, &s, &sb);
	setup(V4L2_BUF_TYPE_VIDEO_CAPTURE, df, dw, dh, &d, &db);
	if (comp.width) {
		struct v4l2_selection sel = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					      .target = V4L2_SEL_TGT_COMPOSE, .r = comp };

		xioctl(VIDIOC_S_SELECTION, &sel, "S_SELECTION compose");
		comp = sel.r;
	} else {
		comp = (struct v4l2_rect){ 0, 0, d.width, d.height };
	}
	printf("%s: %.4s %ux%u (stride %u) -> %.4s %ux%u (stride %u)\n", dev,
	       (char *)&s.pixelformat, s.width, s.height, s.bytesperline,
	       (char *)&d.pixelformat, d.width, d.height, d.bytesperline);
	src_size = s.sizeimage;
	fill(&s, sb.p);
	memset(db.p, 0, db.len);

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl(VIDIOC_STREAMON, &type, "STREAMON out");
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(VIDIOC_STREAMON, &type, "STREAMON cap");

	one_frame();
	ok = checker ? check_checker(&d, db.p) : check(&s, &d, db.p);

	cpu_ticks(&b0, &t0);
	start = now();
	for (i = 0; i < n; i++)
		one_frame();
	secs = now() - start;
	cpu_ticks(&b1, &t1);
	printf("%d frames in %.3f s: %.1f fps, %.2f ms/frame, CPU busy %.0f%%\n", n, secs,
	       n / secs, 1000 * secs / n,
	       t1 > t0 ? 100.0 * (b1 - b0) / (t1 - t0) : 0.0);
	if (ok >= 0)
		printf("%s\n", ok ? "PASS" : "FAIL");
	return ok == 0;

usage:
	fprintf(stderr, "usage: %s [-d dev] [-n frames] [-c X,Y,WxH] [-z] SRCFMT WxH DSTFMT WxH\n",
		argv[0]);
	return 2;
}
