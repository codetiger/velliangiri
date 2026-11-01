// SPDX-License-Identifier: GPL-2.0-only
/*
 * jpegtest - decode a JPEG file on the hardware JPEG decoder
 *
 *   jpegtest [-n runs] [-o out.nv12] [-s seconds] file.jpg
 *
 * Reads the image size from the frame header, decodes the file to NV12
 * through the V4L2 JPEG decoder and reports the time per decode. With -o
 * the NV12 result is written out (plane 0 then plane 1, stride printed).
 * With -s the decoded frame is handed to the M2 scaler as a DMABUF and
 * scaled straight into a DRM framebuffer at the display mode, shown for
 * the given number of seconds (stretched to fill the screen). No pixel is
 * copied by the CPU on that path.
 */
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* Open the first /dev/videoN whose driver name matches */
static int open_video(const char *driver)
{
	char path[32];
	int i, fd;

	for (i = 0; i < 16; i++) {
		struct v4l2_capability cap;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR);
		if (fd < 0)
			continue;
		if (!ioctl(fd, VIDIOC_QUERYCAP, &cap) && !strcmp((char *)cap.driver, driver))
			return fd;
		close(fd);
	}
	fprintf(stderr, "no %s device\n", driver);
	exit(2);
}

/* Frame header: width, height, and whether the coding is baseline/extended */
static int parse_sof(const uint8_t *p, size_t n, uint32_t *w, uint32_t *h, int *progressive)
{
	size_t i = 2;

	if (n < 4 || p[0] != 0xff || p[1] != 0xd8)
		return -1;
	while (i + 4 <= n) {
		uint8_t m;
		uint32_t len;

		if (p[i] != 0xff) {
			i++;
			continue;
		}
		m = p[i + 1];
		if (m == 0xff || m == 0x01 || (m >= 0xd0 && m <= 0xd8)) {
			i += m == 0xff ? 1 : 2;
			continue;
		}
		len = p[i + 2] << 8 | p[i + 3];
		if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
			if (i + 9 > n)
				return -1;
			*h = p[i + 5] << 8 | p[i + 6];
			*w = p[i + 7] << 8 | p[i + 8];
			*progressive = m == 0xc2 || m == 0xc6 || m == 0xca || m == 0xce;
			return 0;
		}
		i += 2 + len;
	}
	return -1;
}

static void find_output(int dfd, uint32_t *crtc_id, uint32_t *conn_id,
			struct drm_mode_modeinfo *mode)
{
	struct drm_mode_card_res res = { 0 };
	uint32_t crtcs[8], conns[8];
	struct drm_mode_crtc crtc = { 0 };

	xioctl(dfd, DRM_IOCTL_MODE_GETRESOURCES, &res, "GETRESOURCES");
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

/* One-shot m2m job: queue one buffer on each side and wait for both */
static void m2m_run(int fd, struct v4l2_buffer *out, struct v4l2_buffer *cap, const char *who)
{
	struct v4l2_buffer b;

	xioctl(fd, VIDIOC_QBUF, out, "QBUF out");
	xioctl(fd, VIDIOC_QBUF, cap, "QBUF cap");
	b = (struct v4l2_buffer){ .type = cap->type, .memory = cap->memory };
	xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF cap");
	if (b.flags & V4L2_BUF_FLAG_ERROR) {
		fprintf(stderr, "%s failed (see dmesg)\n", who);
		exit(1);
	}
	b = (struct v4l2_buffer){ .type = out->type, .memory = out->memory };
	xioctl(fd, VIDIOC_DQBUF, &b, "DQBUF out");
}

static void show(int jfd, const struct v4l2_pix_format *dec, int secs)
{
	struct v4l2_format sf = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2_format cf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_exportbuffer eb = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .flags = O_RDWR };
	struct drm_mode_create_dumb cd = { .bpp = 32 };
	struct drm_prime_handle ph = { .flags = DRM_CLOEXEC | DRM_RDWR };
	struct drm_mode_fb_cmd2 fb = { .pixel_format = DRM_FORMAT_XRGB8888 };
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer ob, cb;
	struct drm_mode_modeinfo mode;
	uint32_t crtc_id, conn_id;
	enum v4l2_buf_type type;
	int sfd, dfd;
	double t;

	dfd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (dfd < 0)
		die("/dev/dri/card0");
	xioctl(dfd, DRM_IOCTL_SET_MASTER, NULL, "SET_MASTER");
	find_output(dfd, &crtc_id, &conn_id, &mode);
	cd.width = mode.hdisplay;
	cd.height = mode.vdisplay;
	xioctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &cd, "CREATE_DUMB");
	fb.width = mode.hdisplay;
	fb.height = mode.vdisplay;
	fb.handles[0] = cd.handle;
	fb.pitches[0] = cd.pitch;
	xioctl(dfd, DRM_IOCTL_MODE_ADDFB2, &fb, "ADDFB2");
	ph.handle = cd.handle;
	xioctl(dfd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph, "PRIME_HANDLE_TO_FD");
	xioctl(jfd, VIDIOC_EXPBUF, &eb, "EXPBUF");

	sfd = open_video("s5l8947x-m2sc");
	sf.fmt.pix = *dec;
	xioctl(sfd, VIDIOC_S_FMT, &sf, "scaler S_FMT out");
	if (sf.fmt.pix.bytesperline != dec->bytesperline) {
		fprintf(stderr, "scaler stride %u != decoder stride %u\n",
			sf.fmt.pix.bytesperline, dec->bytesperline);
		exit(2);
	}
	cf.fmt.pix.pixelformat = V4L2_PIX_FMT_XRGB32;
	cf.fmt.pix.width = mode.hdisplay;
	cf.fmt.pix.height = mode.vdisplay;
	cf.fmt.pix.bytesperline = cd.pitch;
	xioctl(sfd, VIDIOC_S_FMT, &cf, "scaler S_FMT cap");
	rb = (struct v4l2_requestbuffers){ .count = 1, .type = sf.type,
					   .memory = V4L2_MEMORY_DMABUF };
	xioctl(sfd, VIDIOC_REQBUFS, &rb, "scaler REQBUFS out");
	rb.type = cf.type;
	xioctl(sfd, VIDIOC_REQBUFS, &rb, "scaler REQBUFS cap");
	type = sf.type;
	xioctl(sfd, VIDIOC_STREAMON, &type, "scaler STREAMON out");
	type = cf.type;
	xioctl(sfd, VIDIOC_STREAMON, &type, "scaler STREAMON cap");

	ob = (struct v4l2_buffer){ .type = sf.type, .memory = V4L2_MEMORY_DMABUF,
				   .bytesused = sf.fmt.pix.sizeimage, .length = sf.fmt.pix.sizeimage };
	ob.m.fd = eb.fd;
	cb = (struct v4l2_buffer){ .type = cf.type, .memory = V4L2_MEMORY_DMABUF,
				   .length = cf.fmt.pix.sizeimage };
	cb.m.fd = ph.fd;
	t = now();
	m2m_run(sfd, &ob, &cb, "scale");
	printf("scaled %ux%u NV12 -> %ux%u XRGB8888 in %.2f ms (zero-copy)\n", dec->width,
	       dec->height, mode.hdisplay, mode.vdisplay, 1000 * (now() - t));
	{
		struct drm_mode_crtc c = { .crtc_id = crtc_id, .fb_id = fb.fb_id,
					   .set_connectors_ptr = (uintptr_t)&conn_id,
					   .count_connectors = 1, .mode = mode, .mode_valid = 1 };

		xioctl(dfd, DRM_IOCTL_MODE_SETCRTC, &c, "SETCRTC");
	}
	sleep(secs);
}

int main(int argc, char **argv)
{
	struct v4l2_format of = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	struct v4l2_format cf = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers rb;
	struct v4l2_buffer qb, ob, cb;
	const char *outfile = NULL;
	int runs = 1, secs = 0, opt, fd, jfd, progressive, i;
	uint32_t w, h;
	uint8_t *file, *in, *out;
	enum v4l2_buf_type type;
	struct stat st;
	double t, best = 1e9;

	while ((opt = getopt(argc, argv, "n:o:s:")) != -1) {
		if (opt == 'n')
			runs = atoi(optarg);
		else if (opt == 'o')
			outfile = optarg;
		else if (opt == 's')
			secs = atoi(optarg);
		else
			goto usage;
	}
	if (argc - optind != 1)
		goto usage;

	fd = open(argv[optind], O_RDONLY);
	if (fd < 0 || fstat(fd, &st))
		die(argv[optind]);
	file = malloc(st.st_size);
	if (!file || read(fd, file, st.st_size) != st.st_size)
		die("read");
	close(fd);
	if (parse_sof(file, st.st_size, &w, &h, &progressive)) {
		fprintf(stderr, "no JPEG frame header\n");
		return 2;
	}
	printf("%s: %ux%u, %ld bytes%s\n", argv[optind], w, h, (long)st.st_size,
	       progressive ? ", progressive (not supported by the hardware)" : "");

	jfd = open_video("s5l8947x-jpeg");
	of.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
	of.fmt.pix.width = w;
	of.fmt.pix.height = h;
	of.fmt.pix.sizeimage = st.st_size + 4096;
	xioctl(jfd, VIDIOC_S_FMT, &of, "S_FMT out");
	cf.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	cf.fmt.pix.width = w;
	cf.fmt.pix.height = h;
	xioctl(jfd, VIDIOC_S_FMT, &cf, "S_FMT cap");

	rb = (struct v4l2_requestbuffers){ .count = 1, .type = of.type,
					   .memory = V4L2_MEMORY_MMAP };
	xioctl(jfd, VIDIOC_REQBUFS, &rb, "REQBUFS out");
	rb.type = cf.type;
	xioctl(jfd, VIDIOC_REQBUFS, &rb, "REQBUFS cap");
	qb = (struct v4l2_buffer){ .type = of.type, .memory = V4L2_MEMORY_MMAP };
	xioctl(jfd, VIDIOC_QUERYBUF, &qb, "QUERYBUF out");
	in = mmap(NULL, qb.length, PROT_READ | PROT_WRITE, MAP_SHARED, jfd, qb.m.offset);
	qb = (struct v4l2_buffer){ .type = cf.type, .memory = V4L2_MEMORY_MMAP };
	xioctl(jfd, VIDIOC_QUERYBUF, &qb, "QUERYBUF cap");
	out = mmap(NULL, qb.length, PROT_READ | PROT_WRITE, MAP_SHARED, jfd, qb.m.offset);
	if (in == MAP_FAILED || out == MAP_FAILED)
		die("mmap");
	memcpy(in, file, st.st_size);

	type = of.type;
	xioctl(jfd, VIDIOC_STREAMON, &type, "STREAMON out");
	type = cf.type;
	xioctl(jfd, VIDIOC_STREAMON, &type, "STREAMON cap");
	for (i = 0; i < runs; i++) {
		ob = (struct v4l2_buffer){ .type = of.type, .memory = V4L2_MEMORY_MMAP,
					   .bytesused = st.st_size };
		cb = (struct v4l2_buffer){ .type = cf.type, .memory = V4L2_MEMORY_MMAP };
		t = now();
		m2m_run(jfd, &ob, &cb, "decode");
		t = now() - t;
		if (t < best)
			best = t;
	}
	printf("decoded to NV12 %ux%u (stride %u) in %.2f ms (best of %d)\n", cf.fmt.pix.width,
	       cf.fmt.pix.height, cf.fmt.pix.bytesperline, 1000 * best, runs);

	if (outfile) {
		fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || write(fd, out, cf.fmt.pix.sizeimage) != (ssize_t)cf.fmt.pix.sizeimage)
			die(outfile);
		close(fd);
		printf("wrote %s (%u bytes)\n", outfile, cf.fmt.pix.sizeimage);
	}
	if (secs)
		show(jfd, &cf.fmt.pix, secs);
	return 0;

usage:
	fprintf(stderr, "usage: %s [-n runs] [-o out.nv12] [-s seconds] file.jpg\n", argv[0]);
	return 2;
}
