// SPDX-License-Identifier: GPL-2.0-only
/*
 * Velliangiri 1280x720 TV Shell - Hardware Media Pipeline & Specimen Artwork
 *
 * Provides:
 *   1. Built-in flat-colour specimen thumbnails (248x140) and full-screen
 *      viewer scenes (1280x720) matching research/design/design-system.html.
 *   2. Hardware V4L2 JPEG decode (/dev/video2, s5l8947x-jpeg) + M2 Scaler
 *      (/dev/video0, s5l8947x-m2sc) for user photos in /mnt/data/linux/media.
 */

#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/videodev2.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tvshell.h"

void shell_media_render_sample_thumb(uint32_t *dst, int art_id)
{
	int x, y;
	const int w = SHELL_THUMB_W;
	const int h = SHELL_THUMB_H;

	if (!dst)
		return;

	switch (art_id & 3) {
	case 0: /* Garden time-lapse (#A7C08F + #5F7B4B band) */
		for (y = 0; y < h; y++) {
			uint32_t c = (y >= h - 62) ? 0xff5f7b4bU : 0xffa7c08fU;

			for (x = 0; x < w; x++)
				dst[y * w + x] = c;
		}
		break;

	case 1: /* Kids' concert (#2E2440 + #D9B44A spotlight ellipse) */
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				int dx = x - 124;
				int dy = y - 30;
				/* Ellipse: rx=40, ry=70 -> (dx/40)^2 + (dy/70)^2 <= 1 */
				if (dx * dx * 4900 + dy * dy * 1600 <= 1600 * 4900)
					dst[y * w + x] = 0xffd9b44aU;
				else
					dst[y * w + x] = 0xff2e2440U;
			}
		}
		break;

	case 2: /* Beach at sunset (#E9B872 + #3E6E8E sea + #F7E3A1 sun) */
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				int dx = x - 155;
				int dy = y - 62;

				if (y >= h - 48)
					dst[y * w + x] = 0xff3e6e8eU;
				else if (dx * dx + dy * dy <= 24 * 24)
					dst[y * w + x] = 0xfff7e3a1U;
				else
					dst[y * w + x] = 0xffe9b872U;
			}
		}
		break;

	case 3: /* Velliangiri hills (flat mountain silhouettes + amber sun) */
	default:
		for (y = 0; y < h; y++) {
			for (x = 0; x < w; x++) {
				int dx = x - 180;
				int dy = y - 42;
				int ridge1 = 68 + abs(x - 95) * 45 / 100;
				int ridge2 = 54 + abs(x - 165) * 50 / 100;

				if (y >= ridge1)
					dst[y * w + x] = 0xff1e293bU;
				else if (y >= ridge2)
					dst[y * w + x] = 0xff334155U;
				else if (dx * dx + dy * dy <= 20 * 20)
					dst[y * w + x] = 0xfff59e0bU;
				else
					dst[y * w + x] = 0xffcbd5e1U;
			}
		}
		break;
	}
}

void shell_media_render_sample_viewer(uint32_t *dst, int art_id, uint32_t ground)
{
	int x, y;
	const int w = SHELL_SCREEN_W;
	const int h = SHELL_SCREEN_H;
	/* Fit a 4:3 photo (960x720) letterboxed on the 1280x720 theme ground */
	const int vx0 = 160;
	const int vw = 960;

	if (!dst)
		return;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			if (x < vx0 || x >= vx0 + vw) {
				dst[y * w + x] = ground | 0xff000000U;
				continue;
			}
			int lx = x - vx0;

			if ((art_id & 3) == 2) {
				/* Beach at sunset (matches design-system.html viewer) */
				int dx = lx - (vw * 58 / 100);
				int dy = y - (h * 36 / 100);

				if (y >= h * 66 / 100)
					dst[y * w + x] = 0xff3e6e8eU;
				else if (dx * dx + dy * dy <= 70 * 70)
					dst[y * w + x] = 0xfff7e3a1U;
				else
					dst[y * w + x] = 0xffe9b872U;
			} else if ((art_id & 3) == 3) {
				/* Velliangiri hills full-screen photo */
				int dx = lx - 660;
				int dy = y - 210;
				int ridge1 = 360 + abs(lx - 360) * 45 / 100;
				int ridge2 = 280 + abs(lx - 620) * 48 / 100;

				if (y >= ridge1)
					dst[y * w + x] = 0xff1e293bU;
				else if (y >= ridge2)
					dst[y * w + x] = 0xff334155U;
				else if (dx * dx + dy * dy <= 76 * 76)
					dst[y * w + x] = 0xfff59e0bU;
				else
					dst[y * w + x] = 0xffcbd5e1U;
			} else if ((art_id & 3) == 1) {
				int dx = lx - 480;
				int dy = y - 160;

				if (dx * dx * 4 + dy * dy <= 280 * 280)
					dst[y * w + x] = 0xffd9b44aU;
				else
					dst[y * w + x] = 0xff2e2440U;
			} else {
				dst[y * w + x] = (y >= h * 56 / 100) ? 0xff5f7b4bU : 0xffa7c08fU;
			}
		}
	}
}

/*
 * Hardware V4L2 JPEG (/dev/video2) + M2 Scaler (/dev/video0) pipeline.
 * Decodes baseline JPEG into NV12 and scales/composes into dst (w x h XRGB8888).
 */
static int hw_jpeg_scale(const char *jpeg_path, uint32_t *dst,
			 uint32_t out_w, uint32_t out_h,
			 bool letterbox, uint32_t ground)
{
#ifdef __linux__
	FILE *fp;
	long fsize;
	uint8_t *jbuf = NULL;
	int jfd = -1, sfd = -1, ret = -1;
	void *j_src = MAP_FAILED, *j_dst = MAP_FAILED;
	void *s_src = MAP_FAILED, *s_dst = MAP_FAILED;
	size_t j_src_len = 0, j_dst_len = 0, s_src_len = 0, s_dst_len = 0;

	fp = fopen(jpeg_path, "rb");
	if (!fp)
		return -1;
	if (fseek(fp, 0, SEEK_END) || (fsize = ftell(fp)) <= 16 ||
	    fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return -1;
	}
	jbuf = malloc((size_t)fsize);
	if (!jbuf || fread(jbuf, 1, (size_t)fsize, fp) != (size_t)fsize) {
		free(jbuf);
		fclose(fp);
		return -1;
	}
	fclose(fp);

	/* Parse SOF0 for dimensions and verify baseline 4:2:0 / 4:2:2 / 4:0:0 */
	uint32_t iw = 0, ih = 0;
	size_t pos = 2;

	if (jbuf[0] != 0xff || jbuf[1] != 0xd8) {
		free(jbuf);
		return -1;
	}
	while (pos + 4 <= (size_t)fsize) {
		if (jbuf[pos] != 0xff) {
			pos++;
			continue;
		}
		while (pos < (size_t)fsize && jbuf[pos] == 0xff)
			pos++;
		if (pos >= (size_t)fsize)
			break;
		uint8_t marker = jbuf[pos++];

		if (marker == 0xd9 || marker == 0xda)
			break;
		if (pos + 2 > (size_t)fsize)
			break;
		uint16_t seg_len = (uint16_t)((jbuf[pos] << 8) | jbuf[pos + 1]);

		if (marker == 0xc0 && pos + 7 <= (size_t)fsize) {
			ih = (uint32_t)((jbuf[pos + 3] << 8) | jbuf[pos + 4]);
			iw = (uint32_t)((jbuf[pos + 5] << 8) | jbuf[pos + 6]);
			break;
		}
		if (marker == 0xc2) {
			/* Progressive JPEG not supported by hardware engine */
			free(jbuf);
			return -1;
		}
		pos += seg_len;
	}

	if (iw < 16 || ih < 16 || iw > 4096 || ih > 4096) {
		free(jbuf);
		return -1;
	}

	jfd = open("/dev/video2", O_RDWR | O_CLOEXEC);
	sfd = open("/dev/video0", O_RDWR | O_CLOEXEC);
	if (jfd < 0 || sfd < 0)
		goto out;

	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = iw;
	fmt.fmt.pix.height = ih;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
	fmt.fmt.pix.sizeimage = (uint32_t)fsize;
	if (ioctl(jfd, VIDIOC_S_FMT, &fmt) < 0)
		goto out;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = iw;
	fmt.fmt.pix.height = ih;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	if (ioctl(jfd, VIDIOC_S_FMT, &fmt) < 0)
		goto out;
	uint32_t dec_w = fmt.fmt.pix.width;
	uint32_t dec_h = fmt.fmt.pix.height;

	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	if (ioctl(jfd, VIDIOC_REQBUFS, &req) < 0)
		goto out;

	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	if (ioctl(jfd, VIDIOC_QUERYBUF, &buf) < 0)
		goto out;
	j_src_len = buf.length;
	j_src = mmap(NULL, j_src_len, PROT_READ | PROT_WRITE, MAP_SHARED, jfd, buf.m.offset);
	if (j_src == MAP_FAILED)
		goto out;

	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(jfd, VIDIOC_REQBUFS, &req) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(jfd, VIDIOC_QUERYBUF, &buf) < 0)
		goto out;
	j_dst_len = buf.length;
	j_dst = mmap(NULL, j_dst_len, PROT_READ | PROT_WRITE, MAP_SHARED, jfd, buf.m.offset);
	if (j_dst == MAP_FAILED)
		goto out;

	memcpy(j_src, jbuf, (size_t)fsize);
	memset(&buf, 0, sizeof(buf));
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.bytesused = (uint32_t)fsize;
	if (ioctl(jfd, VIDIOC_QBUF, &buf) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.bytesused = 0;
	if (ioctl(jfd, VIDIOC_QBUF, &buf) < 0)
		goto out;

	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(jfd, VIDIOC_STREAMON, &type) < 0)
		goto out;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(jfd, VIDIOC_STREAMON, &type) < 0)
		goto out;

	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(jfd, VIDIOC_DQBUF, &buf) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(jfd, VIDIOC_DQBUF, &buf) < 0)
		goto out;

	/* Scale NV12 -> XRGB32 on /dev/video0 (s5l8947x-m2sc) */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = dec_w;
	fmt.fmt.pix.height = dec_h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	if (ioctl(sfd, VIDIOC_S_FMT, &fmt) < 0)
		goto out;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = out_w;
	fmt.fmt.pix.height = out_h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_XRGB32;
	if (ioctl(sfd, VIDIOC_S_FMT, &fmt) < 0)
		goto out;

	if (letterbox) {
		struct v4l2_selection sel = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
			.target = V4L2_SEL_TGT_COMPOSE,
		};
		uint32_t cw = out_w, ch = out_h;

		if (iw * out_h > ih * out_w)
			ch = (ih * out_w) / iw;
		else
			cw = (iw * out_h) / ih;
		sel.r.left = (int)(out_w - cw) / 2;
		sel.r.top = (int)(out_h - ch) / 2;
		sel.r.width = cw;
		sel.r.height = ch;
		(void)ioctl(sfd, VIDIOC_S_SELECTION, &sel);
	}

	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(sfd, VIDIOC_REQBUFS, &req) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(sfd, VIDIOC_QUERYBUF, &buf) < 0)
		goto out;
	s_src_len = buf.length;
	s_src = mmap(NULL, s_src_len, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, buf.m.offset);
	if (s_src == MAP_FAILED)
		goto out;

	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(sfd, VIDIOC_REQBUFS, &req) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(sfd, VIDIOC_QUERYBUF, &buf) < 0)
		goto out;
	s_dst_len = buf.length;
	s_dst = mmap(NULL, s_dst_len, PROT_READ | PROT_WRITE, MAP_SHARED, sfd, buf.m.offset);
	if (s_dst == MAP_FAILED)
		goto out;

	memcpy(s_src, j_dst, s_src_len < j_dst_len ? s_src_len : j_dst_len);
	if (letterbox) {
		uint32_t *p = s_dst;
		size_t n = out_w * out_h, i;

		for (i = 0; i < n; i++)
			p[i] = ground | 0xff000000U;
	}

	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	buf.bytesused = (uint32_t)s_src_len;
	if (ioctl(sfd, VIDIOC_QBUF, &buf) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.bytesused = 0;
	if (ioctl(sfd, VIDIOC_QBUF, &buf) < 0)
		goto out;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(sfd, VIDIOC_STREAMON, &type) < 0)
		goto out;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(sfd, VIDIOC_STREAMON, &type) < 0)
		goto out;

	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (ioctl(sfd, VIDIOC_DQBUF, &buf) < 0)
		goto out;
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(sfd, VIDIOC_DQBUF, &buf) < 0)
		goto out;

	memcpy(dst, s_dst, (size_t)out_w * out_h * sizeof(uint32_t));
	ret = 0;

out:
	if (s_dst != MAP_FAILED)
		munmap(s_dst, s_dst_len);
	if (s_src != MAP_FAILED)
		munmap(s_src, s_src_len);
	if (j_dst != MAP_FAILED)
		munmap(j_dst, j_dst_len);
	if (j_src != MAP_FAILED)
		munmap(j_src, j_src_len);
	if (sfd >= 0)
		close(sfd);
	if (jfd >= 0)
		close(jfd);
	free(jbuf);
	return ret;
#else
	(void)jpeg_path;
	(void)dst;
	(void)out_w;
	(void)out_h;
	(void)letterbox;
	(void)ground;
	return -1;
#endif
}

int shell_media_load_jpeg_thumb(const char *jpeg_path, uint32_t *dst_248x140)
{
	return hw_jpeg_scale(jpeg_path, dst_248x140,
			     SHELL_THUMB_W, SHELL_THUMB_H, false, 0);
}

int shell_media_load_jpeg_viewer(const char *jpeg_path, uint32_t *dst_1280x720,
				 uint32_t ground)
{
	return hw_jpeg_scale(jpeg_path, dst_1280x720,
			     SHELL_SCREEN_W, SHELL_SCREEN_H, true, ground);
}
