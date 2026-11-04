// SPDX-License-Identifier: GPL-2.0-only
/*
 * Giri on Velliangiri OS — Hardware/Software Media Decoder, Internet
 * Placeholder & Live Article Image Loader, and On-Device Gallery Scanner
 *
 * Implements:
 *   1. S5L8947X Hardware V4L2 M2M JPEG (/dev/video2) + M2SC (/dev/video0)
 *      pipeline for < 3ms 720p JPEG decoding on target hardware
 *   2. Self-contained pure-C baseline JPEG (SOF0 4:2:0 / 4:2:2 / 4:4:4) and
 *      PPM (P6) decoder + bilinear object-fit:cover scaler for host & fallback
 *   3. High-resolution internet placeholder photographs (1280x720) with
 *      non-blocking background fetching for live article image URLs
 *   4. On-device gallery scanner (/mnt/data/linux/media, /usr/share/tvshell/media,
 *      build/dl/gallery) with 720p multi-frame video playback support
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifdef __linux__
#include <linux/videodev2.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tvshell.h"
#include "shell_assets.h"

#ifdef HAVE_DOOM_POSTER_ASSET
static inline uint32_t sample_doom_poster_bilinear(float sx, float sy)
{
	if (sx < 0.0f)
		sx = 0.0f;
	if (sx > 319.0f)
		sx = 319.0f;
	if (sy < 0.0f)
		sy = 0.0f;
	if (sy > 199.0f)
		sy = 199.0f;

	int x0 = (int)sx;
	int y0 = (int)sy;
	int x1 = (x0 + 1 < 320) ? (x0 + 1) : x0;
	int y1 = (y0 + 1 < 200) ? (y0 + 1) : y0;
	float fx = sx - (float)x0;
	float fy = sy - (float)y0;

	uint32_t c00 = doom_poster_320x200[y0 * 320 + x0];
	uint32_t c10 = doom_poster_320x200[y0 * 320 + x1];
	uint32_t c01 = doom_poster_320x200[y1 * 320 + x0];
	uint32_t c11 = doom_poster_320x200[y1 * 320 + x1];

	float w00 = (1.0f - fx) * (1.0f - fy);
	float w10 = fx * (1.0f - fy);
	float w01 = (1.0f - fx) * fy;
	float w11 = fx * fy;

	uint32_t r = (uint32_t)(((c00 >> 16) & 0xffU) * w00 +
				((c10 >> 16) & 0xffU) * w10 +
				((c01 >> 16) & 0xffU) * w01 +
				((c11 >> 16) & 0xffU) * w11 + 0.5f);
	uint32_t g = (uint32_t)(((c00 >> 8) & 0xffU) * w00 +
				((c10 >> 8) & 0xffU) * w10 +
				((c01 >> 8) & 0xffU) * w01 +
				((c11 >> 8) & 0xffU) * w11 + 0.5f);
	uint32_t b = (uint32_t)((c00 & 0xffU) * w00 +
				(c10 & 0xffU) * w10 +
				(c01 & 0xffU) * w01 +
				(c11 & 0xffU) * w11 + 0.5f);

	return 0xff000000U | (r << 16) | (g << 8) | b;
}
#endif

/* =========================================================================
 * 1. Bilinear Object-Fit: Cover Scaler
 * ========================================================================= */

static uint32_t *scale_cover_bilinear(const uint32_t *src, int src_w, int src_h,
				      int dst_w, int dst_h)
{
	if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		return NULL;

	uint32_t *dst = malloc((size_t)dst_w * dst_h * sizeof(uint32_t));
	if (!dst)
		return NULL;

	if (src_w == dst_w && src_h == dst_h) {
		memcpy(dst, src, (size_t)dst_w * dst_h * sizeof(uint32_t));
		return dst;
	}

	float scale_x = (float)dst_w / (float)src_w;
	float scale_y = (float)dst_h / (float)src_h;
	float scale = (scale_x > scale_y) ? scale_x : scale_y;
	float crop_w = (float)dst_w / scale;
	float crop_h = (float)dst_h / scale;
	float off_x = ((float)src_w - crop_w) * 0.5f;
	float off_y = ((float)src_h - crop_h) * 0.5f;
	int y, x;

	for (y = 0; y < dst_h; y++) {
		float sy = off_y + ((float)y + 0.5f) * (crop_h / (float)dst_h) - 0.5f;
		if (sy < 0.0f)
			sy = 0.0f;
		if (sy > (float)(src_h - 1))
			sy = (float)(src_h - 1);
		int y0 = (int)sy;
		int y1 = (y0 + 1 < src_h) ? (y0 + 1) : y0;
		uint32_t fy = (uint32_t)((sy - (float)y0) * 256.0f);
		uint32_t ify = 256U - fy;
		const uint32_t *row0 = src + (size_t)y0 * src_w;
		const uint32_t *row1 = src + (size_t)y1 * src_w;
		uint32_t *drow = dst + (size_t)y * dst_w;

		for (x = 0; x < dst_w; x++) {
			float sx = off_x + ((float)x + 0.5f) * (crop_w / (float)dst_w) - 0.5f;
			if (sx < 0.0f)
				sx = 0.0f;
			if (sx > (float)(src_w - 1))
				sx = (float)(src_w - 1);
			int x0 = (int)sx;
			int x1 = (x0 + 1 < src_w) ? (x0 + 1) : x0;
			uint32_t fx = (uint32_t)((sx - (float)x0) * 256.0f);
			uint32_t ifx = 256U - fx;

			uint32_t w00 = (ifx * ify) >> 8;
			uint32_t w10 = (fx * ify) >> 8;
			uint32_t w01 = (ifx * fy) >> 8;
			uint32_t w11 = (fx * fy) >> 8;

			uint32_t c00 = row0[x0];
			uint32_t c10 = row0[x1];
			uint32_t c01 = row1[x0];
			uint32_t c11 = row1[x1];

			uint32_t r = (((c00 >> 16) & 0xffU) * w00 +
				      ((c10 >> 16) & 0xffU) * w10 +
				      ((c01 >> 16) & 0xffU) * w01 +
				      ((c11 >> 16) & 0xffU) * w11) >> 8;
			uint32_t g = (((c00 >> 8) & 0xffU) * w00 +
				      ((c10 >> 8) & 0xffU) * w10 +
				      ((c01 >> 8) & 0xffU) * w01 +
				      ((c11 >> 8) & 0xffU) * w11) >> 8;
			uint32_t b = ((c00 & 0xffU) * w00 +
				      (c10 & 0xffU) * w10 +
				      (c01 & 0xffU) * w01 +
				      (c11 & 0xffU) * w11) >> 8;

			drow[x] = 0xff000000U | (r << 16) | (g << 8) | b;
		}
	}

	return dst;
}

/* =========================================================================
 * 2. S5L8947X Hardware V4L2 M2M JPEG (/dev/video2) + M2SC (/dev/video0)
 * ========================================================================= */

#ifdef __linux__
static bool parse_jpeg_sof0_dims(const uint8_t *buf, size_t len, int *w, int *h)
{
	size_t i = 2;

	if (len < 4 || buf[0] != 0xff || buf[1] != 0xd8)
		return false;

	while (i + 9 < len) {
		if (buf[i] != 0xff) {
			i++;
			continue;
		}
		while (i < len && buf[i] == 0xff)
			i++;
		if (i >= len)
			break;
		uint8_t m = buf[i++];
		if (m == 0x00 || m == 0xd8 || m == 0xd9 || (m >= 0xd0 && m <= 0xd7))
			continue;
		if (i + 2 > len)
			break;
		size_t seglen = ((size_t)buf[i] << 8) | buf[i + 1];
		if (seglen < 2 || i + seglen > len)
			break;
		if (m == 0xc0 && seglen >= 7) {
			*h = ((int)buf[i + 3] << 8) | buf[i + 4];
			*w = ((int)buf[i + 5] << 8) | buf[i + 6];
			return (*w >= 16 && *h >= 16);
		}
		i += seglen;
	}
	return false;
}

static int xioctl_v4l2(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

struct v4l2_slot {
	void *ptr;
	size_t len;
};

static int m2m_run_one(const char *dev, uint32_t out_pixfmt,
		       uint32_t out_w, uint32_t out_h,
		       const void *in_data, size_t in_len, size_t in_alloc,
		       uint32_t cap_pixfmt, uint32_t cap_w, uint32_t cap_h,
		       void *out_data, size_t out_max)
{
	struct v4l2_slot out_s = { 0 }, cap_s = { 0 };
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer b;
	enum v4l2_buf_type type;
	int fd, rc = -1;

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = out_w;
	fmt.fmt.pix.height = out_h;
	fmt.fmt.pix.pixelformat = out_pixfmt;
	fmt.fmt.pix.sizeimage = (uint32_t)in_alloc;
	if (xioctl_v4l2(fd, VIDIOC_S_FMT, &fmt) < 0)
		goto out;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = cap_w;
	fmt.fmt.pix.height = cap_h;
	fmt.fmt.pix.pixelformat = cap_pixfmt;
	if (xioctl_v4l2(fd, VIDIOC_S_FMT, &fmt) < 0)
		goto out;

	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(fd, VIDIOC_REQBUFS, &req) < 0)
		goto out;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	if (xioctl_v4l2(fd, VIDIOC_QUERYBUF, &b) < 0)
		goto out;
	out_s.len = b.length;
	out_s.ptr = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
	if (out_s.ptr == MAP_FAILED) {
		out_s.ptr = NULL;
		goto out;
	}

	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(fd, VIDIOC_REQBUFS, &req) < 0)
		goto out;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	if (xioctl_v4l2(fd, VIDIOC_QUERYBUF, &b) < 0)
		goto out;
	cap_s.len = b.length;
	cap_s.ptr = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
	if (cap_s.ptr == MAP_FAILED) {
		cap_s.ptr = NULL;
		goto out;
	}

	if (in_len > out_s.len)
		goto out;
	memcpy(out_s.ptr, in_data, in_len);

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	b.bytesused = (uint32_t)in_len;
	if (xioctl_v4l2(fd, VIDIOC_QBUF, &b) < 0)
		goto out;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	if (xioctl_v4l2(fd, VIDIOC_QBUF, &b) < 0)
		goto out;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl_v4l2(fd, VIDIOC_STREAMON, &type) < 0)
		goto out;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl_v4l2(fd, VIDIOC_STREAMON, &type) < 0)
		goto out;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(fd, VIDIOC_DQBUF, &b) < 0)
		goto stop;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(fd, VIDIOC_DQBUF, &b) < 0 || (b.flags & V4L2_BUF_FLAG_ERROR))
		goto stop;

	size_t n = b.bytesused < out_max ? b.bytesused : out_max;
	memcpy(out_data, cap_s.ptr, n);
	rc = (int)n;

stop:
	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	xioctl_v4l2(fd, VIDIOC_STREAMOFF, &type);
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl_v4l2(fd, VIDIOC_STREAMOFF, &type);
out:
	if (out_s.ptr)
		munmap(out_s.ptr, out_s.len);
	if (cap_s.ptr)
		munmap(cap_s.ptr, cap_s.len);
	close(fd);
	return rc;
}

static uint32_t *hw_jpeg_decode_raw(const uint8_t *jpg, size_t jpg_len,
				    int *out_w, int *out_h)
{
	int src_w = 0, src_h = 0;

	if (!parse_jpeg_sof0_dims(jpg, jpg_len, &src_w, &src_h))
		return NULL;
	if (src_w > 4096 || src_h > 4096)
		return NULL;

	uint32_t aw = ((uint32_t)src_w + 15U) & ~15U;
	uint32_t ah = ((uint32_t)src_h + 15U) & ~15U;
	size_t nv12_sz = (size_t)aw * ah * 3 / 2;
	uint8_t *nv12 = malloc(nv12_sz);
	if (!nv12)
		return NULL;

	if (m2m_run_one("/dev/video2", V4L2_PIX_FMT_JPEG, aw, ah,
			jpg, jpg_len, jpg_len + 4096,
			V4L2_PIX_FMT_NV12, aw, ah, nv12, nv12_sz) < 0) {
		free(nv12);
		return NULL;
	}

	int dst_w = (src_w == 1280 && src_h == 720) ? 1280 : (src_w & ~1);
	int dst_h = (src_w == 1280 && src_h == 720) ? 720 : (src_h & ~1);
	if (dst_w > 1280 || dst_h > 720) {
		dst_w = 1280;
		dst_h = 720;
	}
	size_t xrgb_sz = (size_t)dst_w * dst_h * sizeof(uint32_t);
	uint32_t *xrgb = malloc(xrgb_sz);
	if (!xrgb) {
		free(nv12);
		return NULL;
	}

	if (m2m_run_one("/dev/video0", V4L2_PIX_FMT_NV12, (uint32_t)(src_w & ~1), (uint32_t)(src_h & ~1),
			nv12, nv12_sz, nv12_sz,
			V4L2_PIX_FMT_XRGB32, (uint32_t)dst_w, (uint32_t)dst_h,
			xrgb, xrgb_sz) < 0) {
		free(nv12);
		free(xrgb);
		return NULL;
	}
	free(nv12);

	size_t total = (size_t)dst_w * dst_h;
	for (size_t i = 0; i < total; i++)
		xrgb[i] |= 0xff000000U;

	*out_w = dst_w;
	*out_h = dst_h;
	return xrgb;
}
#endif

/* =========================================================================
 * 3. Self-Contained Pure-C Baseline JPEG (SOF0) & PPM (P6) Decoder
 * ========================================================================= */

static const uint8_t jpeg_zigzag[64] = {
	 0,  1,  8, 16,  9,  2,  3, 10,
	17, 24, 32, 25, 18, 11,  4,  5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13,  6,  7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

struct huff_tbl {
	int32_t maxcode[17];
	int32_t valptr[17];
	uint8_t vals[256];
	bool valid;
};

struct jpeg_comp {
	uint8_t id;
	uint8_t h_samp;
	uint8_t v_samp;
	uint8_t tq;
	uint8_t td;
	uint8_t ta;
	int dc_pred;
	int16_t blocks[4][64];
};

struct jpeg_dec {
	const uint8_t *data;
	size_t len;
	size_t pos;
	uint32_t bit_buf;
	int bit_cnt;
	bool marker_hit;
	uint8_t hit_marker;
	int width;
	int height;
	int ncomp;
	int max_h;
	int max_v;
	int restart_interval;
	uint16_t qtbl[4][64];
	struct huff_tbl dc_huff[4];
	struct huff_tbl ac_huff[4];
	struct jpeg_comp comp[3];
};

static inline uint8_t clamp_u8_int(int v)
{
	if (v < 0)
		return 0;
	if (v > 255)
		return 255;
	return (uint8_t)v;
}

static void idct_8x8(const int16_t *in, uint8_t *out)
{
	/* Fast 1D float/fixed separable IDCT across rows then columns */
	static bool cos_init;
	static float c_tbl[8][8];
	float tmp[64];
	int u, v, x, y;

	if (!cos_init) {
		for (u = 0; u < 8; u++) {
			float cu = (u == 0) ? (1.0f / sqrtf(2.0f)) : 1.0f;
			for (x = 0; x < 8; x++) {
				c_tbl[u][x] = 0.5f * cu *
					      cosf(((2.0f * (float)x + 1.0f) * (float)u * (float)M_PI) / 16.0f);
			}
		}
		cos_init = true;
	}

	for (y = 0; y < 8; y++) {
		const int16_t *row = in + y * 8;
		bool all_ac_zero = (row[1] == 0 && row[2] == 0 && row[3] == 0 &&
				    row[4] == 0 && row[5] == 0 && row[6] == 0 && row[7] == 0);
		if (all_ac_zero) {
			float dc = (float)row[0] * c_tbl[0][0];
			for (x = 0; x < 8; x++)
				tmp[y * 8 + x] = dc;
			continue;
		}
		for (x = 0; x < 8; x++) {
			float s = 0.0f;
			for (u = 0; u < 8; u++)
				s += (float)row[u] * c_tbl[u][x];
			tmp[y * 8 + x] = s;
		}
	}

	for (x = 0; x < 8; x++) {
		for (y = 0; y < 8; y++) {
			float s = 0.0f;
			for (v = 0; v < 8; v++)
				s += tmp[v * 8 + x] * c_tbl[v][y];
			out[y * 8 + x] = clamp_u8_int((int)(s + 128.5f));
		}
	}
}

static void build_huff_tbl(struct huff_tbl *ht, const uint8_t *bits, const uint8_t *vals)
{
	int code = 0;
	int k = 0;
	int i;

	memset(ht, 0, sizeof(*ht));
	for (i = 1; i <= 16; i++) {
		int cnt = bits[i - 1];
		if (cnt == 0) {
			ht->maxcode[i] = -1;
		} else {
			ht->valptr[i] = k - code;
			k += cnt;
			code += cnt;
			ht->maxcode[i] = code - 1;
		}
		code <<= 1;
	}
	if (k > 256)
		k = 256;
	memcpy(ht->vals, vals, (size_t)k);
	ht->valid = true;
}

static int jpeg_next_bit(struct jpeg_dec *jd)
{
	if (jd->bit_cnt == 0) {
		if (jd->marker_hit || jd->pos >= jd->len) {
			jd->bit_buf = 0;
			jd->bit_cnt = 8;
		} else {
			uint8_t b = jd->data[jd->pos++];
			if (b == 0xff) {
				while (jd->pos < jd->len && jd->data[jd->pos] == 0xff)
					jd->pos++;
				if (jd->pos < jd->len) {
					uint8_t m = jd->data[jd->pos++];
					if (m != 0x00) {
						jd->marker_hit = true;
						jd->hit_marker = m;
						b = 0;
					}
				}
			}
			jd->bit_buf = b;
			jd->bit_cnt = 8;
		}
	}
	int bit = (jd->bit_buf >> 7) & 1;
	jd->bit_buf = (jd->bit_buf << 1) & 0xffU;
	jd->bit_cnt--;
	return bit;
}

static int jpeg_read_bits(struct jpeg_dec *jd, int n)
{
	int val = 0;
	int i;

	for (i = 0; i < n; i++)
		val = (val << 1) | jpeg_next_bit(jd);
	return val;
}

static int jpeg_huff_decode(struct jpeg_dec *jd, const struct huff_tbl *ht)
{
	int code = 0;
	int len;

	if (!ht->valid)
		return 0;
	for (len = 1; len <= 16; len++) {
		code = (code << 1) | jpeg_next_bit(jd);
		if (code <= ht->maxcode[len]) {
			int idx = code + ht->valptr[len];
			if (idx >= 0 && idx < 256)
				return ht->vals[idx];
			return 0;
		}
	}
	return 0;
}

static int jpeg_extend(int v, int t)
{
	if (t <= 0)
		return 0;
	int vt = 1 << (t - 1);
	if (v < vt)
		v -= (1 << t) - 1;
	return v;
}

static void jpeg_decode_block(struct jpeg_dec *jd, int comp_idx, int16_t *out_blk)
{
	struct jpeg_comp *c = &jd->comp[comp_idx];
	const uint16_t *qt = jd->qtbl[c->tq & 3];
	const struct huff_tbl *dc_ht = &jd->dc_huff[c->td & 3];
	const struct huff_tbl *ac_ht = &jd->ac_huff[c->ta & 3];
	int k = 1;

	memset(out_blk, 0, 64 * sizeof(int16_t));

	int t = jpeg_huff_decode(jd, dc_ht);
	int diff = (t > 0) ? jpeg_extend(jpeg_read_bits(jd, t), t) : 0;
	c->dc_pred += diff;
	out_blk[0] = (int16_t)(c->dc_pred * (int)qt[0]);

	while (k < 64) {
		int rs = jpeg_huff_decode(jd, ac_ht);
		int s = rs & 0x0f;
		int r = (rs >> 4) & 0x0f;

		if (s == 0) {
			if (r == 15) {
				k += 16;
				continue;
			}
			break;
		}
		k += r;
		if (k >= 64)
			break;
		int ac = jpeg_extend(jpeg_read_bits(jd, s), s);
		out_blk[jpeg_zigzag[k]] = (int16_t)(ac * (int)qt[k]);
		k++;
	}
}

static uint32_t *sw_jpeg_decode_raw(const uint8_t *buf, size_t len,
				    int *out_w, int *out_h)
{
	struct jpeg_dec *jd;
	int i;

	if (!buf || len < 4 || buf[0] != 0xff || buf[1] != 0xd8)
		return NULL;

	jd = calloc(1, sizeof(*jd));
	if (!jd)
		return NULL;
	jd->data = buf;
	jd->len = len;
	jd->pos = 2;

	bool saw_sof = false, saw_sos = false;
	while (jd->pos + 2 <= jd->len && !saw_sos) {
		if (buf[jd->pos] != 0xff) {
			jd->pos++;
			continue;
		}
		while (jd->pos < jd->len && buf[jd->pos] == 0xff)
			jd->pos++;
		if (jd->pos >= jd->len)
			break;
		uint8_t m = buf[jd->pos++];
		if (m == 0xd8 || m == 0x00)
			continue;
		if (m == 0xd9)
			break;
		if (jd->pos + 2 > jd->len)
			break;
		size_t seglen = ((size_t)buf[jd->pos] << 8) | buf[jd->pos + 1];
		if (seglen < 2 || jd->pos + seglen > jd->len)
			break;
		const uint8_t *seg = buf + jd->pos + 2;
		size_t paylen = seglen - 2;

		if (m == 0xdb) {
			/* DQT */
			size_t off = 0;
			while (off + 65 <= paylen) {
				uint8_t pq_tq = seg[off++];
				int pq = (pq_tq >> 4) & 0x0f;
				int tq = pq_tq & 0x03;
				if (pq == 0) {
					for (i = 0; i < 64; i++)
						jd->qtbl[tq][i] = seg[off + i];
					off += 64;
				} else {
					if (off + 128 > paylen)
						break;
					for (i = 0; i < 64; i++)
						jd->qtbl[tq][i] = ((uint16_t)seg[off + i * 2] << 8) |
								  seg[off + i * 2 + 1];
					off += 128;
				}
			}
		} else if (m == 0xc0) {
			/* Baseline SOF0 */
			if (paylen < 6)
				break;
			jd->height = ((int)seg[1] << 8) | seg[2];
			jd->width = ((int)seg[3] << 8) | seg[4];
			jd->ncomp = seg[5];
			if ((jd->ncomp != 1 && jd->ncomp != 3) ||
			    jd->width <= 0 || jd->height <= 0 ||
			    jd->width > 4096 || jd->height > 4096 ||
			    paylen < (size_t)(6 + jd->ncomp * 3))
				break;
			jd->max_h = 1;
			jd->max_v = 1;
			for (i = 0; i < jd->ncomp; i++) {
				jd->comp[i].id = seg[6 + i * 3];
				jd->comp[i].h_samp = (seg[6 + i * 3 + 1] >> 4) & 0x0f;
				jd->comp[i].v_samp = seg[6 + i * 3 + 1] & 0x0f;
				jd->comp[i].tq = seg[6 + i * 3 + 2] & 0x03;
				if (jd->comp[i].h_samp > jd->max_h)
					jd->max_h = jd->comp[i].h_samp;
				if (jd->comp[i].v_samp > jd->max_v)
					jd->max_v = jd->comp[i].v_samp;
			}
			saw_sof = true;
		} else if (m == 0xc4) {
			/* DHT */
			size_t off = 0;
			while (off + 17 <= paylen) {
				uint8_t tc_th = seg[off++];
				int tc = (tc_th >> 4) & 0x0f;
				int th = tc_th & 0x03;
				const uint8_t *bits = seg + off;
				off += 16;
				int total = 0;
				for (i = 0; i < 16; i++)
					total += bits[i];
				if (total > 256 || off + (size_t)total > paylen)
					break;
				if (tc == 0)
					build_huff_tbl(&jd->dc_huff[th], bits, seg + off);
				else
					build_huff_tbl(&jd->ac_huff[th], bits, seg + off);
				off += (size_t)total;
			}
		} else if (m == 0xdd) {
			/* DRI */
			if (paylen >= 2)
				jd->restart_interval = ((int)seg[0] << 8) | seg[1];
		} else if (m == 0xda) {
			/* SOS */
			if (!saw_sof || paylen < (size_t)(1 + jd->ncomp * 2))
				break;
			int ns = seg[0];
			for (i = 0; i < ns && i < jd->ncomp; i++) {
				uint8_t cid = seg[1 + i * 2];
				uint8_t td_ta = seg[1 + i * 2 + 1];
				int c;
				for (c = 0; c < jd->ncomp; c++) {
					if (jd->comp[c].id == cid) {
						jd->comp[c].td = (td_ta >> 4) & 0x03;
						jd->comp[c].ta = td_ta & 0x03;
					}
				}
			}
			jd->pos += seglen;
			saw_sos = true;
			break;
		}
		jd->pos += seglen;
	}

	if (!saw_sof || !saw_sos) {
		free(jd);
		return NULL;
	}

	uint32_t *pixels = malloc((size_t)jd->width * jd->height * sizeof(uint32_t));
	if (!pixels) {
		free(jd);
		return NULL;
	}

	int mcu_w = jd->max_h * 8;
	int mcu_h = jd->max_v * 8;
	int mcus_x = (jd->width + mcu_w - 1) / mcu_w;
	int mcus_y = (jd->height + mcu_h - 1) / mcu_h;
	int mcu_cnt = 0;
	int mx, my;

	uint8_t y_plane[4][64];
	uint8_t cb_plane[64];
	uint8_t cr_plane[64];

	for (my = 0; my < mcus_y; my++) {
		for (mx = 0; mx < mcus_x; mx++) {
			if (jd->restart_interval > 0 && mcu_cnt > 0 &&
			    (mcu_cnt % jd->restart_interval) == 0) {
				jd->bit_cnt = 0;
				if (jd->marker_hit && jd->hit_marker >= 0xd0 && jd->hit_marker <= 0xd7) {
					jd->marker_hit = false;
				} else {
					while (jd->pos + 1 < jd->len) {
						if (jd->data[jd->pos] == 0xff &&
						    jd->data[jd->pos + 1] >= 0xd0 &&
						    jd->data[jd->pos + 1] <= 0xd7) {
							jd->pos += 2;
							break;
						}
						jd->pos++;
					}
				}
				jd->comp[0].dc_pred = 0;
				jd->comp[1].dc_pred = 0;
				jd->comp[2].dc_pred = 0;
			}
			mcu_cnt++;

			int y_blocks = jd->comp[0].h_samp * jd->comp[0].v_samp;
			if (y_blocks > 4)
				y_blocks = 4;
			int b;
			for (b = 0; b < y_blocks; b++) {
				jpeg_decode_block(jd, 0, jd->comp[0].blocks[b]);
				idct_8x8(jd->comp[0].blocks[b], y_plane[b]);
			}

			if (jd->ncomp == 3) {
				jpeg_decode_block(jd, 1, jd->comp[1].blocks[0]);
				idct_8x8(jd->comp[1].blocks[0], cb_plane);
				jpeg_decode_block(jd, 2, jd->comp[2].blocks[0]);
				idct_8x8(jd->comp[2].blocks[0], cr_plane);
			} else {
				memset(cb_plane, 128, 64);
				memset(cr_plane, 128, 64);
			}

			int py, px;
			for (py = 0; py < mcu_h; py++) {
				int dst_y = my * mcu_h + py;
				if (dst_y >= jd->height)
					break;
				uint32_t *drow = pixels + (size_t)dst_y * jd->width;

				for (px = 0; px < mcu_w; px++) {
					int dst_x = mx * mcu_w + px;
					if (dst_x >= jd->width)
						break;

					int bx = px >> 3;
					int by = py >> 3;
					int b_idx = by * jd->comp[0].h_samp + bx;
					if (b_idx >= y_blocks)
						b_idx = 0;
					int y_val = y_plane[b_idx][(py & 7) * 8 + (px & 7)];

					int cx = (px * 8) / mcu_w;
					int cy = (py * 8) / mcu_h;
					int cb = (int)cb_plane[cy * 8 + cx] - 128;
					int cr = (int)cr_plane[cy * 8 + cx] - 128;

					int r = y_val + ((359 * cr + 128) >> 8);
					int g = y_val - ((88 * cb + 183 * cr + 128) >> 8);
					int bl = y_val + ((454 * cb + 128) >> 8);

					drow[dst_x] = 0xff000000U |
						      ((uint32_t)clamp_u8_int(r) << 16) |
						      ((uint32_t)clamp_u8_int(g) << 8) |
						      (uint32_t)clamp_u8_int(bl);
				}
			}
		}
	}

	*out_w = jd->width;
	*out_h = jd->height;
	free(jd);
	return pixels;
}

static uint32_t *load_ppm_raw(const uint8_t *buf, size_t len, int *out_w, int *out_h)
{
	if (!buf || len < 16 || buf[0] != 'P' || buf[1] != '6')
		return NULL;

	size_t pos = 2;
	int vals[3] = { 0, 0, 0 };
	int vi = 0;

	while (pos < len && vi < 3) {
		while (pos < len && isspace(buf[pos]))
			pos++;
		if (pos < len && buf[pos] == '#') {
			while (pos < len && buf[pos] != '\n')
				pos++;
			continue;
		}
		if (pos >= len || !isdigit(buf[pos]))
			return NULL;
		int v = 0;
		while (pos < len && isdigit(buf[pos]))
			v = v * 10 + (buf[pos++] - '0');
		vals[vi++] = v;
	}
	if (vi < 3 || vals[0] <= 0 || vals[1] <= 0 || vals[2] != 255 || pos >= len)
		return NULL;
	pos++; /* single whitespace after maxval */

	int w = vals[0], h = vals[1];
	size_t rgb_bytes = (size_t)w * h * 3;
	if (pos + rgb_bytes > len)
		return NULL;

	uint32_t *pixels = malloc((size_t)w * h * sizeof(uint32_t));
	if (!pixels)
		return NULL;

	const uint8_t *src = buf + pos;
	size_t total = (size_t)w * h;
	for (size_t i = 0; i < total; i++) {
		pixels[i] = 0xff000000U |
			    ((uint32_t)src[i * 3 + 0] << 16) |
			    ((uint32_t)src[i * 3 + 1] << 8) |
			    (uint32_t)src[i * 3 + 2];
	}
	*out_w = w;
	*out_h = h;
	return pixels;
}

/* Decode a JPEG or PPM file into a raw XRGB8888 surface (hardware V4L2 first, C fallback) */
static uint32_t *decode_image_file_raw(const char *path, int *out_w, int *out_h)
{
	FILE *fp;
	long flen;
	uint8_t *fbuf;
	uint32_t *raw = NULL;

	if (!path || !path[0])
		return NULL;
	fp = fopen(path, "rb");
	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	flen = ftell(fp);
	if (flen <= 16 || flen > 16 * 1024 * 1024) {
		fclose(fp);
		return NULL;
	}
	rewind(fp);
	fbuf = malloc((size_t)flen);
	if (!fbuf) {
		fclose(fp);
		return NULL;
	}
	if (fread(fbuf, 1, (size_t)flen, fp) != (size_t)flen) {
		free(fbuf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);

	if (fbuf[0] == 'P' && fbuf[1] == '6') {
		raw = load_ppm_raw(fbuf, (size_t)flen, out_w, out_h);
		free(fbuf);
		return raw;
	}

#ifdef __linux__
	raw = hw_jpeg_decode_raw(fbuf, (size_t)flen, out_w, out_h);
	if (raw) {
		free(fbuf);
		return raw;
	}
#endif
	raw = sw_jpeg_decode_raw(fbuf, (size_t)flen, out_w, out_h);
	free(fbuf);
	return raw;
}

/* =========================================================================
 * 4. Procedural Fallback Rasterizer & Original 1993 DOOM Poster
 * ========================================================================= */

static const int bayer4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

static inline uint32_t lerp_rgb_dither(uint32_t c0, uint32_t c1, float t, int x, int y)
{
	if (t <= 0.0f)
		return 0xff000000U | c0;
	if (t >= 1.0f)
		return 0xff000000U | c1;

	float d = ((float)bayer4[y & 3][x & 3] - 7.5f) * (1.0f / 255.0f);
	float td = t + d;

	if (td < 0.0f)
		td = 0.0f;
	if (td > 1.0f)
		td = 1.0f;

	float r0 = (float)((c0 >> 16) & 0xff);
	float g0 = (float)((c0 >> 8) & 0xff);
	float b0 = (float)(c0 & 0xff);
	float r1 = (float)((c1 >> 16) & 0xff);
	float g1 = (float)((c1 >> 8) & 0xff);
	float b1 = (float)(c1 & 0xff);

	uint32_t r = (uint32_t)(r0 + (r1 - r0) * td + 0.5f);
	uint32_t g = (uint32_t)(g0 + (g1 - g0) * td + 0.5f);
	uint32_t b = (uint32_t)(b0 + (b1 - b0) * td + 0.5f);

	return 0xff000000U | (r << 16) | (g << 8) | b;
}

static inline uint32_t blend_rgb(uint32_t dst, uint32_t src, float alpha)
{
	if (alpha <= 0.0f)
		return dst;
	if (alpha >= 1.0f)
		return 0xff000000U | src;

	uint32_t a = (uint32_t)(alpha * 256.0f + 0.5f);
	if (a > 256)
		a = 256;
	uint32_t inv = 256 - a;

	uint32_t r = (((dst >> 16) & 0xff) * inv + ((src >> 16) & 0xff) * a) >> 8;
	uint32_t g = (((dst >> 8) & 0xff) * inv + ((src >> 8) & 0xff) * a) >> 8;
	uint32_t b = ((dst & 0xff) * inv + (src & 0xff) * a) >> 8;

	return 0xff000000U | (r << 16) | (g << 8) | b;
}

static uint32_t grad_3stop(uint32_t c0, float mid, uint32_t c1, uint32_t c2,
			   float t, int x, int y)
{
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (t <= mid)
		return lerp_rgb_dither(c0, c1, t / mid, x, y);
	return lerp_rgb_dither(c1, c2, (t - mid) / (1.0f - mid), x, y);
}

static float ridge_cov(float ux, float uy, float aa_px,
		       const float *xs, const float *ys, int n)
{
	int i;

	for (i = 0; i < n - 1; i++) {
		if (ux >= xs[i] && ux <= xs[i + 1]) {
			float t = (ux - xs[i]) / (xs[i + 1] - xs[i]);
			float ry = ys[i] + (ys[i + 1] - ys[i]) * t;
			float d = uy - ry;

			if (d <= -aa_px)
				return 0.0f;
			if (d >= aa_px)
				return 1.0f;
			return (d + aa_px) / (2.0f * aa_px);
		}
	}
	return (uy >= ys[n - 1]) ? 1.0f : 0.0f;
}

static float circle_cov(float ux, float uy, float cx, float cy, float r, float aa_px)
{
	float dist = hypotf(ux - cx, uy - cy);
	float d = r - dist;

	if (d <= -aa_px)
		return 0.0f;
	if (d >= aa_px)
		return 1.0f;
	return (d + aa_px) / (2.0f * aa_px);
}

static float ring_cov(float ux, float uy, float cx, float cy,
		      float r, float half_w, float aa_px)
{
	float dist = hypotf(ux - cx, uy - cy);
	float d = half_w - fabsf(dist - r);

	if (d <= -aa_px)
		return 0.0f;
	if (d >= aa_px)
		return 1.0f;
	return (d + aa_px) / (2.0f * aa_px);
}

static float round_rect_cov_f(float ux, float uy, float rx0, float ry0,
			      float rw, float rh, float rad, float aa_px)
{
	float cx = rx0 + rw * 0.5f;
	float cy = ry0 + rh * 0.5f;
	float qx = fabsf(ux - cx) - (rw * 0.5f - rad);
	float qy = fabsf(uy - cy) - (rh * 0.5f - rad);
	float out_d = hypotf(qx > 0.0f ? qx : 0.0f, qy > 0.0f ? qy : 0.0f);
	float in_d = fminf(fmaxf(qx, qy), 0.0f);
	float sdf = out_d + in_d - rad;
	float d = -sdf;

	if (d <= -aa_px)
		return 0.0f;
	if (d >= aa_px)
		return 1.0f;
	return (d + aa_px) / (2.0f * aa_px);
}

static uint32_t *rasterize_preset_surface(enum giri_preset_art preset, int out_w, int out_h)
{
	uint32_t *buf = malloc((size_t)out_w * out_h * sizeof(uint32_t));
	float sx = (float)out_w / 640.0f;
	float sy = (float)out_h / 360.0f;
	float scale = sx > sy ? sx : sy;
	float aa = 0.75f / scale;
	int py, px;

	if (!buf)
		return NULL;

	static const float dawn_r1_x[] = { 0, 140, 270, 410, 560, 640 };
	static const float dawn_r1_y[] = { 250, 165, 220, 130, 210, 180 };
	static const float dawn_r2_x[] = { 0, 190, 340, 490, 640 };
	static const float dawn_r2_y[] = { 285, 195, 255, 175, 245 };
	static const float monsoon_rx[] = { 0, 160, 310, 480, 640 };
	static const float monsoon_ry[] = { 260, 180, 235, 145, 230 };
#ifndef HAVE_DOOM_POSTER_ASSET
	static const float doom_rx[] = { 0, 120, 220, 320, 430, 540, 640 };
	static const float doom_ry[] = { 250, 180, 230, 140, 230, 175, 240 };
#endif

	for (py = 0; py < out_h; py++) {
		float uy = 180.0f + ((float)py + 0.5f - (float)out_h * 0.5f) / scale;
		uint32_t *row = buf + py * out_w;

		for (px = 0; px < out_w; px++) {
			float ux = 320.0f + ((float)px + 0.5f - (float)out_w * 0.5f) / scale;
			uint32_t c = 0xff12151cU;

			switch (preset) {
			case GIRI_ART_DAWN_RIDGE:
			default: {
				c = grad_3stop(0xff1e293bU, 0.55f, 0xffb45309U, 0xfffde68aU,
					       uy / 360.0f, px, py);
				float sun = circle_cov(ux, uy, 460.0f, 140.0f, 46.0f, aa);
				if (sun > 0.0f)
					c = blend_rgb(c, 0xfffef08aU, sun * 0.90f);
				float r1 = ridge_cov(ux, uy, aa, dawn_r1_x, dawn_r1_y, 6);
				if (r1 > 0.0f)
					c = blend_rgb(c, 0xff334155U, r1);
				float r2 = ridge_cov(ux, uy, aa, dawn_r2_x, dawn_r2_y, 5);
				if (r2 > 0.0f)
					c = blend_rgb(c, 0xff0f172aU, r2);
				break;
			}
			case GIRI_ART_WEATHER_SKY: {
				float diag_t = (ux / 640.0f + uy / 360.0f) * 0.5f;
				c = lerp_rgb_dither(0xff0284c7U, 0xff1e293bU, diag_t, px, py);
				float sun = circle_cov(ux, uy, 210.0f, 145.0f, 54.0f, aa);
				if (sun > 0.0f)
					c = blend_rgb(c, 0xfffbbf24U, sun);
				float cl = fmaxf(
					fmaxf(circle_cov(ux, uy, 276.0f, 159.0f, 46.0f, aa),
					      circle_cov(ux, uy, 355.0f, 145.0f, 58.0f, aa)),
					fmaxf(circle_cov(ux, uy, 422.0f, 163.0f, 42.0f, aa),
					      round_rect_cov_f(ux, uy, 230.0f, 152.0f, 234.0f, 53.0f, 26.0f, aa))
				);
				if (uy > 205.0f + aa)
					cl = 0.0f;
				else if (uy > 205.0f - aa)
					cl *= (205.0f + aa - uy) / (2.0f * aa);
				if (cl > 0.0f)
					c = blend_rgb(c, 0xfff8fafcU, cl * 0.92f);
				break;
			}
			case GIRI_ART_MONSOON_VIDEO: {
				float diag_t = (ux / 640.0f + uy / 360.0f) * 0.5f;
				c = grad_3stop(0xff090d16U, 0.60f, 0xff1e293bU, 0xff475569U,
					       diag_t, px, py);
				float r1 = ridge_cov(ux, uy, aa, monsoon_rx, monsoon_ry, 5);
				if (r1 > 0.0f)
					c = blend_rgb(c, 0xff0f172aU, r1);
				break;
			}
			case GIRI_ART_WATERFALL_VIDEO: {
				c = 0xff0f172aU;
				if (uy >= 120.0f) {
					float top_y = 120.0f + (ux / 250.0f) * 20.0f;
					float right_x = 250.0f - ((uy - 140.0f) / 220.0f) * 20.0f;
					if (uy >= top_y && ux <= right_x)
						c = 0xff1e293bU;
				}
				if (uy >= 110.0f) {
					float top_y = 130.0f - ((ux - 390.0f) / 250.0f) * 20.0f;
					float left_x = 390.0f + ((uy - 130.0f) / 230.0f) * 20.0f;
					if (uy >= top_y && ux >= left_x)
						c = 0xff1e293bU;
				}
				float wf = round_rect_cov_f(ux, uy, 260.0f, 110.0f, 120.0f, 250.0f, 1.0f, aa);
				if (wf > 0.0f)
					c = blend_rgb(c, 0xff38bdf8U, wf * 0.75f);
				break;
			}
			case GIRI_ART_SHOLA_MIST: {
				c = grad_3stop(0xff0f172aU, 0.60f, 0xff1e3a2fU, 0xff34d399U,
					       uy / 360.0f, px, py);
				float w1_y = 205.0f - 32.0f * sinf(ux * (float)M_PI / 320.0f);
				float d1 = uy - w1_y;
				if (d1 > -aa) {
					float cov = d1 >= aa ? 1.0f : (d1 + aa) / (2.0f * aa);
					c = blend_rgb(c, 0xff064e3bU, cov * 0.85f);
				}
				if (uy >= 225.0f && uy <= 260.0f)
					c = blend_rgb(c, 0xffe2e8f0U, 0.22f);
				float w2_y = 260.0f - 26.0f * sinf(ux * (float)M_PI / 440.0f);
				float d2 = uy - w2_y;
				if (d2 > -aa) {
					float cov = d2 >= aa ? 1.0f : (d2 + aa) / (2.0f * aa);
					c = blend_rgb(c, 0xff022c22U, cov);
				}
				break;
			}
			case GIRI_ART_DOOM_GAME: {
#ifdef HAVE_DOOM_POSTER_ASSET
				if (out_w < 640) {
					float poster_h = (float)out_h * 0.80f;
					float sx_p = 10.0f + ((float)px / (float)(out_w - 1)) * 300.0f;

					if ((float)py <= poster_h) {
						float sy_p = ((float)py / poster_h) * 189.0f;
						c = sample_doom_poster_bilinear(sx_p, sy_p);
					} else {
						float fade = ((float)py - poster_h) / ((float)out_h - poster_h);
						uint32_t edge_c = sample_doom_poster_bilinear(sx_p, 188.0f);
						c = blend_rgb(edge_c, 0xff090d16U, fade);
					}
				} else {
					float sx_p = ((float)px / (float)(out_w - 1)) * 319.0f;
					float sy_p = ((float)py / (float)(out_h - 1)) * 189.0f;
					c = sample_doom_poster_bilinear(sx_p, sy_p);
				}
#else
				c = grad_3stop(0xff450a0aU, 0.60f, 0xff991b1bU, 0xff090d16U,
					       uy / 360.0f, px, py);
				float r = ridge_cov(ux, uy, aa, doom_rx, doom_ry, 7);
				if (r > 0.0f)
					c = blend_rgb(c, 0xff18181bU, r);
#endif
				break;
			}
			case GIRI_ART_APP_CHESS: {
				c = 0xff1e293bU;
				if (ux >= 160.0f && ux < 480.0f && uy >= 60.0f && uy < 300.0f) {
					int col = (int)((ux - 160.0f) / 80.0f);
					int r_idx = (int)((uy - 60.0f) / 80.0f);
					if (((col + r_idx) & 1) == 0)
						c = 0xff334155U;
				}
				float disc = circle_cov(ux, uy, 320.0f, 180.0f, 46.0f, aa);
				if (disc > 0.0f)
					c = blend_rgb(c, 0xffd97706U, disc);
				break;
			}
			case GIRI_ART_APP_RADIO: {
				c = 0xff0f172aU;
				float w1 = ring_cov(ux, uy, 320.0f, 180.0f, 110.0f, 2.0f, aa);
				if (w1 > 0.0f)
					c = blend_rgb(c, 0xfff59e0bU, w1 * 0.35f);
				float w2 = ring_cov(ux, uy, 320.0f, 180.0f, 70.0f, 2.5f, aa);
				if (w2 > 0.0f)
					c = blend_rgb(c, 0xfff59e0bU, w2 * 0.65f);
				float core = circle_cov(ux, uy, 320.0f, 180.0f, 24.0f, aa);
				if (core > 0.0f)
					c = blend_rgb(c, 0xffd97706U, core);
				break;
			}
			}

			row[px] = c;
		}
	}

	return buf;
}

/* =========================================================================
 * 5. High-Res Internet Placeholders, LRU Texture Cache & Async URL Fetcher
 * ========================================================================= */

static const char *const preset_placeholder_names[GIRI_ART_COUNT] = {
	[GIRI_ART_DAWN_RIDGE]      = "dawn_ridge.jpg",
	[GIRI_ART_WEATHER_SKY]     = "weather_sky.jpg",
	[GIRI_ART_MONSOON_VIDEO]   = "monsoon_video.jpg",
	[GIRI_ART_WATERFALL_VIDEO] = "waterfall_video.jpg",
	[GIRI_ART_SHOLA_MIST]      = "shola_mist.jpg",
	[GIRI_ART_DOOM_GAME]       = NULL, /* Always uses original 1993 DOOM poster */
	[GIRI_ART_APP_CHESS]       = "app_chess.jpg",
	[GIRI_ART_APP_RADIO]       = "app_radio.jpg",
};

struct art_cache_slot {
	uint32_t *card_274;
	uint32_t *card_275;
	uint32_t *fs_720p;
};

#define URI_CACHE_SLOTS		16
#define MAX_ASYNC_FETCHES	4

struct uri_cache_entry {
	char path[256];
	uint32_t *card_274;
	uint32_t *card_275;
	uint32_t *fs_720p;
	uint64_t last_used;
};

struct async_fetch_slot {
	pid_t pid;
	char url[256];
	char cache_path[256];
	bool done;
	bool failed;
};

static struct art_cache_slot g_art_cache[GIRI_ART_COUNT];
static struct uri_cache_entry g_uri_cache[URI_CACHE_SLOTS];
static struct async_fetch_slot g_fetches[MAX_ASYNC_FETCHES];
static uint64_t g_cache_clock = 1;

/* Video playback frame buffer (1280x720) */
static char g_vid_frame_path[512];
static uint32_t *g_vid_frame_buf;

static bool resolve_placeholder_path(const char *name, char *out, size_t out_sz)
{
	static const char *const dirs[] = {
		"/mnt/data/linux/placeholders",
		"/usr/local/share/tvshell/placeholders",
		"/usr/share/tvshell/placeholders",
		"build/dl/placeholders",
		NULL
	};
	struct stat st;
	int i;

	if (!name || !name[0])
		return false;

	for (i = 0; dirs[i]; i++) {
		snprintf(out, out_sz, "%s/%s", dirs[i], name);
		if (stat(out, &st) == 0 && st.st_size > 128)
			return true;
	}
	return false;
}

static bool resolve_media_path(const char *uri, char *out, size_t out_sz)
{
	struct stat st;

	if (!uri || !uri[0])
		return false;

	/* Direct absolute or relative path */
	if (stat(uri, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 128) {
		snprintf(out, out_sz, "%s", uri);
		return true;
	}

	/* Placeholder shorthand ("placeholder:news_world.jpg" or bare filename) */
	const char *pname = uri;
	if (strncmp(uri, "placeholder:", 12) == 0)
		pname = uri + 12;

	if (strchr(pname, '/') == NULL) {
		if (resolve_placeholder_path(pname, out, out_sz))
			return true;

		static const char *const mdirs[] = {
			"/mnt/data/linux/media",
			"/usr/local/share/tvshell/media",
			"/usr/share/tvshell/media",
			"build/dl/gallery",
			NULL
		};
		int i;
		for (i = 0; mdirs[i]; i++) {
			snprintf(out, out_sz, "%s/%s", mdirs[i], pname);
			if (stat(out, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 128)
				return true;
		}
	}
	return false;
}

static uint32_t fnv1a_hash(const char *str)
{
	uint32_t h = 2166136261U;

	while (str && *str) {
		h ^= (uint8_t)*str++;
		h *= 16777619U;
	}
	return h;
}

/* Non-blocking background fetcher for live http/https article image URLs */
static bool check_or_start_url_fetch(const char *url, char *local_out, size_t out_sz)
{
	struct stat st;
	uint32_t h = fnv1a_hash(url);
	int i;

	(void)mkdir("/tmp/tvshell-cache", 0755);
	snprintf(local_out, out_sz, "/tmp/tvshell-cache/art_%08x.jpg", h);

	if (stat(local_out, &st) == 0 && st.st_size > 256)
		return true;

	for (i = 0; i < MAX_ASYNC_FETCHES; i++) {
		if (strcmp(g_fetches[i].url, url) == 0) {
			if (g_fetches[i].done && !g_fetches[i].failed &&
			    stat(local_out, &st) == 0 && st.st_size > 256)
				return true;
			return false;
		}
	}

	/* Find an empty slot to launch background wget/curl */
	for (i = 0; i < MAX_ASYNC_FETCHES; i++) {
		if (g_fetches[i].pid == 0 && g_fetches[i].url[0] == '\0') {
			pid_t pid = fork();
			if (pid == 0) {
				char tmp_path[280];
				snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", local_out);
				int devnull = open("/dev/null", O_RDWR);
				if (devnull >= 0) {
					dup2(devnull, STDOUT_FILENO);
					dup2(devnull, STDERR_FILENO);
					if (devnull > 2)
						close(devnull);
				}
				/* Try wget first (BusyBox / Alpine), then curl */
				char cmd[1536];
				snprintf(cmd, sizeof(cmd),
					 "(wget -q -T 8 -O '%s' '%s' || curl -fsSL --max-time 8 -o '%s' '%s') && mv '%s' '%s'",
					 tmp_path, url, tmp_path, url, tmp_path, local_out);
				execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
				_exit(1);
			} else if (pid > 0) {
				g_fetches[i].pid = pid;
				snprintf(g_fetches[i].url, sizeof(g_fetches[i].url), "%s", url);
				snprintf(g_fetches[i].cache_path, sizeof(g_fetches[i].cache_path), "%s", local_out);
				g_fetches[i].done = false;
				g_fetches[i].failed = false;
			}
			break;
		}
	}
	return false;
}

bool shell_media_poll_async(void)
{
	bool updated = false;
	int i;

	for (i = 0; i < MAX_ASYNC_FETCHES; i++) {
		if (g_fetches[i].pid > 0) {
			int status = 0;
			pid_t w = waitpid(g_fetches[i].pid, &status, WNOHANG);
			if (w == g_fetches[i].pid) {
				struct stat st;
				g_fetches[i].pid = 0;
				g_fetches[i].done = true;
				if (WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
				    stat(g_fetches[i].cache_path, &st) == 0 && st.st_size > 256) {
					updated = true;
				} else {
					g_fetches[i].failed = true;
				}
			} else if (w < 0 && errno != EINTR) {
				g_fetches[i].pid = 0;
				g_fetches[i].done = true;
				g_fetches[i].failed = true;
			}
		}
	}
	return updated;
}

static struct uri_cache_entry *get_uri_cache_entry(const char *resolved_path, bool need_fs)
{
	int i, victim = 0;
	uint64_t oldest = UINT64_MAX;

	for (i = 0; i < URI_CACHE_SLOTS; i++) {
		if (strcmp(g_uri_cache[i].path, resolved_path) == 0) {
			g_uri_cache[i].last_used = ++g_cache_clock;
			if (need_fs && !g_uri_cache[i].fs_720p) {
				int rw = 0, rh = 0;
				uint32_t *raw = decode_image_file_raw(resolved_path, &rw, &rh);
				if (raw) {
					if (rw == SHELL_SCREEN_W && rh == SHELL_SCREEN_H) {
						g_uri_cache[i].fs_720p = raw;
					} else {
						g_uri_cache[i].fs_720p = scale_cover_bilinear(raw, rw, rh,
											      SHELL_SCREEN_W,
											      SHELL_SCREEN_H);
						free(raw);
					}
				}
			}
			return &g_uri_cache[i];
		}
		if (g_uri_cache[i].last_used < oldest) {
			oldest = g_uri_cache[i].last_used;
			victim = i;
		}
	}

	int rw = 0, rh = 0;
	uint32_t *raw = decode_image_file_raw(resolved_path, &rw, &rh);
	if (!raw)
		return NULL;

	struct uri_cache_entry *e = &g_uri_cache[victim];
	free(e->card_274);
	free(e->card_275);
	free(e->fs_720p);
	memset(e, 0, sizeof(*e));

	snprintf(e->path, sizeof(e->path), "%s", resolved_path);
	e->last_used = ++g_cache_clock;
	e->card_274 = scale_cover_bilinear(raw, rw, rh, 274, 312);
	e->card_275 = scale_cover_bilinear(raw, rw, rh, 275, 312);
	if (rw == SHELL_SCREEN_W && rh == SHELL_SCREEN_H) {
		e->fs_720p = raw;
	} else {
		if (need_fs)
			e->fs_720p = scale_cover_bilinear(raw, rw, rh, SHELL_SCREEN_W, SHELL_SCREEN_H);
		free(raw);
	}
	return e;
}

void shell_media_init(void)
{
	int i;

	memset(g_art_cache, 0, sizeof(g_art_cache));
	memset(g_uri_cache, 0, sizeof(g_uri_cache));
	memset(g_fetches, 0, sizeof(g_fetches));

	for (i = 0; i < GIRI_ART_COUNT; i++) {
		char ph_path[256];
		const char *ph_name = preset_placeholder_names[i];

		if (ph_name && resolve_placeholder_path(ph_name, ph_path, sizeof(ph_path))) {
			int rw = 0, rh = 0;
			uint32_t *raw = decode_image_file_raw(ph_path, &rw, &rh);
			if (raw) {
				g_art_cache[i].card_274 = scale_cover_bilinear(raw, rw, rh, 274, 312);
				g_art_cache[i].card_275 = scale_cover_bilinear(raw, rw, rh, 275, 312);
				if (rw == SHELL_SCREEN_W && rh == SHELL_SCREEN_H) {
					g_art_cache[i].fs_720p = raw;
				} else {
					g_art_cache[i].fs_720p = scale_cover_bilinear(raw, rw, rh,
										      SHELL_SCREEN_W,
										      SHELL_SCREEN_H);
					free(raw);
				}
				continue;
			}
		}

		g_art_cache[i].card_274 = rasterize_preset_surface((enum giri_preset_art)i, 274, 312);
		g_art_cache[i].card_275 = rasterize_preset_surface((enum giri_preset_art)i, 275, 312);
		g_art_cache[i].fs_720p = NULL;
	}
}

void shell_media_free(void)
{
	int i;

	for (i = 0; i < GIRI_ART_COUNT; i++) {
		free(g_art_cache[i].card_274);
		free(g_art_cache[i].card_275);
		free(g_art_cache[i].fs_720p);
		memset(&g_art_cache[i], 0, sizeof(g_art_cache[i]));
	}
	for (i = 0; i < URI_CACHE_SLOTS; i++) {
		free(g_uri_cache[i].card_274);
		free(g_uri_cache[i].card_275);
		free(g_uri_cache[i].fs_720p);
		memset(&g_uri_cache[i], 0, sizeof(g_uri_cache[i]));
	}
	free(g_vid_frame_buf);
	g_vid_frame_buf = NULL;
	g_vid_frame_path[0] = '\0';
}

static bool resolve_card_image_path(const struct giri_media_card *card,
				    char *out_path, size_t out_sz)
{
	if (!card)
		return false;

	if (card->uri[0]) {
		if (strncmp(card->uri, "http://", 7) == 0 ||
		    strncmp(card->uri, "https://", 8) == 0) {
			if (check_or_start_url_fetch(card->uri, out_path, out_sz))
				return true;
		} else if (resolve_media_path(card->uri, out_path, out_sz)) {
			return true;
		}
	}

	if (card->kind == GIRI_CARD_LOCAL_PHOTO || card->kind == GIRI_CARD_LOCAL_VIDEO) {
		if (card->detail_text[0] && resolve_media_path(card->detail_text, out_path, out_sz))
			return true;
	}

	return false;
}

const uint32_t *shell_media_get_card_art(const struct giri_media_card *card,
					 int w, int h)
{
	char resolved[256];
	enum giri_preset_art preset = GIRI_ART_DAWN_RIDGE;

	(void)h;
	if (card) {
		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;

		if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
			struct uri_cache_entry *e = get_uri_cache_entry(resolved, false);
			if (e) {
				if (w == 275 && e->card_275)
					return e->card_275;
				if (e->card_274)
					return e->card_274;
			}
		}
	}

	if (w == 275 && g_art_cache[preset].card_275)
		return g_art_cache[preset].card_275;
	return g_art_cache[preset].card_274;
}

const uint32_t *shell_media_get_fullscreen_art(const struct giri_media_card *card)
{
	char resolved[256];
	enum giri_preset_art preset = GIRI_ART_DAWN_RIDGE;

	if (card) {
		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;

		if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
			struct uri_cache_entry *e = get_uri_cache_entry(resolved, true);
			if (e && e->fs_720p)
				return e->fs_720p;
		}
	}

	if (!g_art_cache[preset].fs_720p) {
		g_art_cache[preset].fs_720p = rasterize_preset_surface(preset,
								       SHELL_SCREEN_W,
								       SHELL_SCREEN_H);
	}
	return g_art_cache[preset].fs_720p;
}

const uint32_t *shell_media_get_video_frame(const struct giri_media_card *card,
					    int frame_idx)
{
	if (!card)
		return NULL;

	const char *src_uri = card->uri[0] ? card->uri : card->detail_text;
	const char *bn = strrchr(src_uri, '/');
	bn = bn ? (bn + 1) : src_uri;

	if (bn && bn[0]) {
		static const char *const fdirs[] = {
			"/mnt/data/linux/media/.frames",
			"/usr/local/share/tvshell/media/.frames",
			"/usr/share/tvshell/media/.frames",
			"build/dl/gallery/.frames",
			NULL
		};
		int fidx = frame_idx % 15;
		if (fidx < 0)
			fidx = 0;
		int i;

		for (i = 0; fdirs[i]; i++) {
			char cand[512];
			struct stat st;

			snprintf(cand, sizeof(cand), "%s/%.128s.f%02d.jpg", fdirs[i], bn, fidx);
			if (stat(cand, &st) == 0 && st.st_size > 128) {
				if (strcmp(g_vid_frame_path, cand) == 0 && g_vid_frame_buf)
					return g_vid_frame_buf;

				int rw = 0, rh = 0;
				uint32_t *raw = decode_image_file_raw(cand, &rw, &rh);
				if (raw) {
					free(g_vid_frame_buf);
					if (rw == SHELL_SCREEN_W && rh == SHELL_SCREEN_H) {
						g_vid_frame_buf = raw;
					} else {
						g_vid_frame_buf = scale_cover_bilinear(raw, rw, rh,
										       SHELL_SCREEN_W,
										       SHELL_SCREEN_H);
						free(raw);
					}
					snprintf(g_vid_frame_path, sizeof(g_vid_frame_path), "%s", cand);
					return g_vid_frame_buf;
				}
			}
		}
	}

	return shell_media_get_fullscreen_art(card);
}

/* =========================================================================
 * 6. On-Device Gallery Media Scanner & Metadata Catalog
 * ========================================================================= */

struct gallery_meta {
	const char *filename;
	int priority;
	const char *title;
	const char *subtitle;
	const char *detail;
};

static const struct gallery_meta gallery_catalog[] = {
	{ "IMG_6452.jpeg",  1, "Seventh Hill Ascent",     "Velliangiri Summit Trail · 1,778m",
	  "Pilgrims and trekkers ascending the granite boulder trail of the seventh hill in the Velliangiri Mountains." },
	{ "IMG_6455.jpeg",  2, "Shola Cloud Canopy",      "Velliangiri High Ridge · Mist",
	  "Sweeping view of montane shola forests and valley clouds drifting across the Velliangiri mountain range." },
	{ "IMG_6397.jpeg",  3, "Adiyogi Foothills",       "Poondi Valley · Western Ghats",
	  "Portrait at the base of the Velliangiri foothills with the 112-foot Adiyogi statue and mist-clad ridges behind." },
	{ "GX016305.mov",   4, "Lagoon Speedboat Clip",   "Turquoise Reef · 720p Video",
	  "Widescreen 720p video cruising across crystal-clear turquoise lagoon waters aboard a coastal boat." },
	{ "IMG_6402.jpeg",  5, "Poondi Trek Start",       "Bamboo Walking Sticks · Trailhead",
	  "Trekking group gathered at the Poondi temple trailhead with traditional bamboo staffs before climbing the seven hills." },
	{ "IMG_6418.jpeg",  6, "Above the Cloud Bank",    "High Meadow · Velliangiri Hills",
	  "Golden alpine grasslands and white orographic clouds below the upper ridges of Velliangiri." },
	{ "IMG_8624.mov",   7, "Castle Courtyard Band",   "Brass Ensemble · 720p Video",
	  "Live brass band concert performing in a historic stone castle courtyard, captured in 1280x720 HD video." },
	{ "IMG_9481.jpeg",  8, "Natural Coral Arch",      "Andaman Coastline · Limestone Bridge",
	  "Sunlight framing a natural coastal rock arch and tide pools against a deep blue sky." },
	{ "_MG_9451.jpeg",  9, "Flyboard Water Jet",      "Coastal Lagoon · Action Photo",
	  "High-speed telephoto shot of hydroflight flyboarding above tropical green coastline waters." },
	{ "DSC04988.jpeg", 10, "Radhanagar Beach",        "Family Portrait · White Sand Shore",
	  "Family photograph along the pristine white sand shoreline and emerald hills." },
	{ "GOPR6262.jpeg", 11, "Reef Snorkeling Dive",    "Clear Lagoon Waters · Action Cam",
	  "Wetsuit snorkeling session in calm shallow turquoise reef waters." },
	{ "IMG_8734.mov",  12, "Tower Bridge by Night",   "Thames Bascule Lift · 720p Video",
	  "Nighttime 720p video of London's illuminated Tower Bridge with its twin bascules raised over the River Thames." },
	{ "IMG_6395.jpeg", 13, "Foothills Walkway",       "Velliangiri Horizon · Group Photo",
	  "Friends walking along the grand promenade beneath the Velliangiri mountain range." },
	{ "IMG_6414.jpeg", 14, "Dawn on the Ridge",       "Morning Trek · Mountain Trail",
	  "Early morning golden light along the steep mountain path with fellow trekkers ascending behind." },
	{ "IMG_9403.jpeg", 15, "Banyan Canopy Climb",     "Tropical Forest · Island Trail",
	  "Resting high on the gnarled branch of a coastal tropical tree canopy." },
	{ "DSC04995.jpeg", 16, "Ocean Horizon",           "Coastal Breeze · Beach Walk",
	  "Scenic ocean waves rolling onto a quiet tropical beach." },
	{ "_MG_9470.jpeg", 17, "Jet Ski Spray",           "Coastal Waters · Action Shot",
	  "High-speed water sports across sparkling blue bay waters." },
	{ "_MG_9486.jpeg", 18, "Lagoon Hoverflight",      "Water Sports · Telephoto",
	  "Rising above the bay on twin hydro-jets against a backdrop of forested hills." },
	{ "GOPR6315.jpeg", 19, "Coral Reef Swim",         "Snorkeling · Underwater Cam",
	  "Exploring shallow tropical reef waters with diving masks." },
	{ "GOPR6337.jpeg", 20, "Emerald Bay Float",       "Island Excursion · Action Cam",
	  "Crystal-clear turquoise waters along the palm-lined island shore." },
	{ "GOPR6373.jpeg", 21, "Shoreline Dive Team",     "Lagoon Expedition · Widescreen",
	  "Group diving and snorkeling session in warm coastal waters." },
	{ "IMG_9368.jpeg", 22, "Departure Terminal",      "Family Travel · Morning Flight",
	  "Family photo inside the airport terminal at the start of the trip." },
	{ "IMG_9370.jpeg", 23, "Window Seat Boarding",    "Cabin View · Travel",
	  "Settling into the aircraft cabin before takeoff." },
	{ "IMG_9376.jpeg", 24, "Island Pier Arrival",     "Harbor Jetty · Coastal Walk",
	  "Strolling along the sunlit harbor pier above clear blue water." },
	{ "IMG_9411.jpeg", 25, "Forest Trail Banyan",     "Ancient Roots · Island Woods",
	  "Towering buttress roots and lush tropical foliage along the forest walk." },
	{ "IMG_9412.jpeg", 26, "Tide Pool Shore",         "Rocky Coast · Low Tide",
	  "Exploring weathered coastal rock formations and shallow tide pools." },
	{ "IMG_9455.jpeg", 27, "Sunset Beach Silhouette", "Golden Hour · Evening Shore",
	  "Warm sunset colors reflecting across the wet sand at dusk." },
	{ "IMG_9505.jpeg", 28, "Coastal Cliff Vista",     "Turquoise Cove · Panorama",
	  "Overlooking a secluded turquoise cove framed by rugged cliffs." },
	{ NULL, 999, NULL, NULL, NULL }
};

static const struct gallery_meta *lookup_gallery_meta(const char *bn)
{
	int i;

	for (i = 0; gallery_catalog[i].filename; i++) {
		if (strcasecmp(gallery_catalog[i].filename, bn) == 0)
			return &gallery_catalog[i];
	}
	return NULL;
}

static bool has_ext(const char *name, const char *const *exts)
{
	const char *dot = strrchr(name, '.');
	int i;

	if (!dot)
		return false;
	for (i = 0; exts[i]; i++) {
		if (strcasecmp(dot, exts[i]) == 0)
			return true;
	}
	return false;
}

static int cmp_media_items(const void *a, const void *b)
{
	const struct shell_media_item *ia = (const struct shell_media_item *)a;
	const struct shell_media_item *ib = (const struct shell_media_item *)b;
	const char *bna = strrchr(ia->path, '/');
	const char *bnb = strrchr(ib->path, '/');
	bna = bna ? (bna + 1) : ia->path;
	bnb = bnb ? (bnb + 1) : ib->path;

	const struct gallery_meta *ma = lookup_gallery_meta(bna);
	const struct gallery_meta *mb = lookup_gallery_meta(bnb);
	int pa = ma ? ma->priority : 500;
	int pb = mb ? mb->priority : 500;

	if (pa != pb)
		return pa - pb;
	return strcasecmp(bna, bnb);
}

int shell_media_scan_device(struct shell_media_item *items, int max_items, bool video_only)
{
	static const char *const scan_dirs[] = {
		"/mnt/data/linux/media",
		"/usr/local/share/tvshell/media",
		"/usr/share/tvshell/media",
		"build/dl/gallery",
		"/media",
		"/root/media",
		NULL
	};
	static const char *const img_exts[] = { ".jpg", ".jpeg", ".png", ".ppm", NULL };
	static const char *const vid_exts[] = { ".mp4", ".mkv", ".webm", ".mov", NULL };
	int count = 0;
	int d, i;

	if (!items || max_items <= 0)
		return 0;

	for (d = 0; scan_dirs[d] && count < max_items; d++) {
		DIR *dir = opendir(scan_dirs[d]);
		struct dirent *ent;

		if (!dir)
			continue;
		while ((ent = readdir(dir)) != NULL && count < max_items) {
			if (ent->d_name[0] == '.')
				continue;

			bool is_vid = has_ext(ent->d_name, vid_exts);
			bool is_img = !video_only && has_ext(ent->d_name, img_exts);
			struct stat st;
			char full[512];

			if (!is_vid && !is_img)
				continue;

			/* Deduplicate by basename across fallback directories */
			bool dup = false;
			for (i = 0; i < count; i++) {
				const char *ebn = strrchr(items[i].path, '/');
				ebn = ebn ? (ebn + 1) : items[i].path;
				if (strcasecmp(ebn, ent->d_name) == 0) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;

			snprintf(full, sizeof(full), "%s/%s", scan_dirs[d], ent->d_name);
			if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
				continue;

			struct shell_media_item *it = &items[count++];
			const struct gallery_meta *meta = lookup_gallery_meta(ent->d_name);

			memset(it, 0, sizeof(*it));
			snprintf(it->path, sizeof(it->path), "%.*s",
				 (int)sizeof(it->path) - 1, full);

			if (meta) {
				shell_font_fit_clean_phrase(meta->title, it->title,
							    sizeof(it->title), SHELL_LIMIT_CARD_TITLE);
				shell_font_fit_clean_phrase(meta->subtitle, it->subtitle,
							    sizeof(it->subtitle), SHELL_LIMIT_CARD_SUB);
				snprintf(it->detail, sizeof(it->detail), "%s", meta->detail);
			} else {
				shell_font_fit_clean_phrase(ent->d_name, it->title,
							    sizeof(it->title), SHELL_LIMIT_CARD_TITLE);
				snprintf(it->subtitle, sizeof(it->subtitle),
					 "%s · %.0f KB on device",
					 is_vid ? "Local Video" : "Local Photo",
					 (double)st.st_size / 1024.0);
				snprintf(it->detail, sizeof(it->detail),
					 "On-device media file (%.120s) stored locally on Velliangiri OS.",
					 ent->d_name);
			}
			snprintf(it->badge, sizeof(it->badge), "%s",
				 is_vid ? "DEVICE VIDEO" : "DEVICE PHOTO");
			snprintf(it->meta_pill, sizeof(it->meta_pill), "%s",
				 is_vid ? "720P" : "16:9 HD");
			it->is_video = is_vid;
		}
		closedir(dir);
	}

	if (count > 1)
		qsort(items, (size_t)count, sizeof(items[0]), cmp_media_items);

	return count;
}
