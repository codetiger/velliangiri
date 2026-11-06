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
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif
#ifdef __linux__
#include <linux/videodev2.h>
#include <sound/asound.h>
#endif
#include <math.h>
#include <pthread.h>
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
#include "shell_gpu.h"

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

#ifdef HAVE_DOOM_POSTER_PALETTE
	uint32_t c00 = doom_poster_palette[doom_poster_idx_320x200[y0 * 320 + x0]];
	uint32_t c10 = doom_poster_palette[doom_poster_idx_320x200[y0 * 320 + x1]];
	uint32_t c01 = doom_poster_palette[doom_poster_idx_320x200[y1 * 320 + x0]];
	uint32_t c11 = doom_poster_palette[doom_poster_idx_320x200[y1 * 320 + x1]];
#else
	uint32_t c00 = doom_poster_320x200[y0 * 320 + x0];
	uint32_t c10 = doom_poster_320x200[y0 * 320 + x1];
	uint32_t c01 = doom_poster_320x200[y1 * 320 + x0];
	uint32_t c11 = doom_poster_320x200[y1 * 320 + x1];
#endif

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

	uint16_t *x0_lut = malloc((size_t)dst_w * sizeof(uint16_t));
	uint16_t *x1_lut = malloc((size_t)dst_w * sizeof(uint16_t));
	uint16_t *fx_lut = malloc((size_t)dst_w * sizeof(uint16_t));
	if (!x0_lut || !x1_lut || !fx_lut) {
		free(x0_lut);
		free(x1_lut);
		free(fx_lut);
		free(dst);
		return NULL;
	}

	float scale_x = (float)dst_w / (float)src_w;
	float scale_y = (float)dst_h / (float)src_h;
	float scale = (scale_x > scale_y) ? scale_x : scale_y;
	float crop_w = (float)dst_w / scale;
	float crop_h = (float)dst_h / scale;
	float off_x = ((float)src_w - crop_w) * 0.5f;
	float off_y = ((float)src_h - crop_h) * 0.5f;
	float step_x = crop_w / (float)dst_w;
	float step_y = crop_h / (float)dst_h;
	int y, x;

	for (x = 0; x < dst_w; x++) {
		float sx = off_x + ((float)x + 0.5f) * step_x - 0.5f;
		if (sx < 0.0f)
			sx = 0.0f;
		if (sx > (float)(src_w - 1))
			sx = (float)(src_w - 1);
		int x0 = (int)sx;
		int x1 = (x0 + 1 < src_w) ? (x0 + 1) : x0;
		x0_lut[x] = (uint16_t)x0;
		x1_lut[x] = (uint16_t)x1;
		fx_lut[x] = (uint16_t)((sx - (float)x0) * 256.0f);
	}

	for (y = 0; y < dst_h; y++) {
		float sy = off_y + ((float)y + 0.5f) * step_y - 0.5f;
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
			uint32_t fx = fx_lut[x];
			uint32_t ifx = 256U - fx;

			uint32_t w00 = (ifx * ify) >> 8;
			uint32_t w10 = (fx * ify) >> 8;
			uint32_t w01 = (ifx * fy) >> 8;
			uint32_t w11 = 256U - w00 - w10 - w01;

			uint32_t c00 = row0[x0_lut[x]];
			uint32_t c10 = row0[x1_lut[x]];
			uint32_t c01 = row1[x0_lut[x]];
			uint32_t c11 = row1[x1_lut[x]];

			uint32_t rb = (((c00 & 0x00ff00ffU) * w00 +
					(c10 & 0x00ff00ffU) * w10 +
					(c01 & 0x00ff00ffU) * w01 +
					(c11 & 0x00ff00ffU) * w11 +
					0x00800080U) >> 8) & 0x00ff00ffU;
			uint32_t g  = (((c00 & 0x0000ff00U) * w00 +
					(c10 & 0x0000ff00U) * w10 +
					(c01 & 0x0000ff00U) * w01 +
					(c11 & 0x0000ff00U) * w11 +
					0x00008000U) >> 8) & 0x0000ff00U;

			drow[x] = 0xff000000U | rb | g;
		}
	}

	free(x0_lut);
	free(x1_lut);
	free(fx_lut);
	return dst;
}

/* =========================================================================
 * 2. S5L8947X Hardware V4L2 M2M JPEG (/dev/video2) + Dual M2SC (/dev/video0,1)
 *
 * Persistent zero-copy V4L2 M2M hardware pipeline:
 *   - Keeps s5l8947x-jpeg open with STREAMON and an exported NV12 DMABUF
 *     (VIDIOC_EXPBUF), avoiding per-image open/REQBUFS/mmap/close overhead.
 *   - Keeps two persistent s5l8947x-m2sc contexts open (275x312 card scaler on
 *     scaler0 and 1280x720 fullscreen/video scaler on scaler1) that import the
 *     JPEG decoder's NV12 DMABUF directly (V4L2_MEMORY_DMABUF) with zero CPU
 *     copy of the intermediate NV12 frame.
 *   - Offloads centered object-fit:cover cropping (VIDIOC_S_SELECTION /
 *     V4L2_SEL_TGT_CROP), 8-tap/4-tap polyphase scaling, and full-range BT.601
 *     YCbCr->XRGB8888 colour conversion entirely to the M2SC hardware engines.
 * ========================================================================= */

/* =========================================================================
 * 1c. QuickTime (.mov / ISO-BMFF Motion-JPEG) Container Reader
 *
 * Parses the standard QuickTime `moov` -> `trak` -> `mdia` -> `minf` -> `stbl`
 * sample tables (`mdhd`, `hdlr`, `stts`, `stsc`, `stsz`, `stco`) directly from
 * `.mov` files so that:
 *   - Frame 0 is extracted on the fly for card/poster thumbnails
 *   - All video frames are streamed directly from the `.mov` into the
 *     s5l8947x-jpeg + s5l8947x-m2sc hardware pipeline at 24 FPS with zero
 *     sidecar frame JPEG files.
 * ========================================================================= */

#define QT_MOV_MAX_FRAMES	1024

struct qt_mov_sample {
	uint32_t offset;
	uint32_t size;
};

struct qt_mov_audio_chunk {
	uint32_t offset;
	uint32_t pcm_frames;
	uint32_t start_frame;
};

static struct {
	int fd;
	char path[512];
	uint32_t fps;
	uint32_t frame_count;
	uint32_t duration_sec;
	struct qt_mov_sample samples[QT_MOV_MAX_FRAMES];
	uint32_t audio_rate;
	uint32_t audio_chunk_count;
	uint32_t audio_total_frames;
	struct qt_mov_audio_chunk audio_chunks[QT_MOV_MAX_FRAMES];
} g_qt_mov = {
	.fd = -1,
};

static uint32_t rd_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void qt_mov_close(void)
{
	if (g_qt_mov.fd >= 0) {
		close(g_qt_mov.fd);
		g_qt_mov.fd = -1;
	}
	g_qt_mov.path[0] = '\0';
	g_qt_mov.fps = 0;
	g_qt_mov.frame_count = 0;
	g_qt_mov.duration_sec = 0;
	g_qt_mov.audio_rate = 0;
	g_qt_mov.audio_chunk_count = 0;
	g_qt_mov.audio_total_frames = 0;
}

struct qt_stbl_parse {
	bool is_video;
	bool is_audio;
	uint32_t timescale;
	uint32_t duration_ts;
	uint32_t sample_delta;
	const uint8_t *stsc;
	size_t stsc_len;
	const uint8_t *stsz;
	size_t stsz_len;
	const uint8_t *stco;
	size_t stco_len;
};

static void qt_walk_atoms(const uint8_t *buf, size_t len,
			  struct qt_stbl_parse *cur,
			  struct qt_stbl_parse *vid_out,
			  struct qt_stbl_parse *aud_out)
{
	size_t off = 0;

	while (off + 8 <= len) {
		uint32_t sz = rd_be32(buf + off);
		const uint8_t *typ = buf + off + 4;
		size_t hdr = 8;
		size_t body_len;

		if (sz == 1) {
			if (off + 16 > len)
				break;
			sz = rd_be32(buf + off + 12);
			hdr = 16;
		} else if (sz == 0) {
			sz = (uint32_t)(len - off);
		}
		if (sz < hdr || off + sz > len)
			break;
		body_len = sz - hdr;
		const uint8_t *body = buf + off + hdr;

		if (!memcmp(typ, "moov", 4)) {
			qt_walk_atoms(body, body_len, cur, vid_out, aud_out);
		} else if (!memcmp(typ, "trak", 4)) {
			struct qt_stbl_parse sub;
			memset(&sub, 0, sizeof(sub));
			qt_walk_atoms(body, body_len, &sub, vid_out, aud_out);
			if (sub.is_video && sub.stsz && sub.stco)
				*vid_out = sub;
			else if (sub.is_audio && sub.stsz && sub.stco)
				*aud_out = sub;
		} else if (!memcmp(typ, "mdia", 4) || !memcmp(typ, "minf", 4) ||
			   !memcmp(typ, "stbl", 4)) {
			qt_walk_atoms(body, body_len, cur, vid_out, aud_out);
		} else if (!memcmp(typ, "hdlr", 4) && body_len >= 12) {
			if (!memcmp(body + 8, "vide", 4))
				cur->is_video = true;
			else if (!memcmp(body + 8, "soun", 4))
				cur->is_audio = true;
		} else if (!memcmp(typ, "mdhd", 4) && body_len >= 20) {
			uint8_t ver = body[0];
			if (ver == 0 && body_len >= 20) {
				cur->timescale = rd_be32(body + 12);
				cur->duration_ts = rd_be32(body + 16);
			} else if (ver == 1 && body_len >= 28) {
				cur->timescale = rd_be32(body + 20);
				cur->duration_ts = rd_be32(body + 28);
			}
		} else if (!memcmp(typ, "stts", 4) && body_len >= 16) {
			uint32_t cnt = rd_be32(body + 4);
			if (cnt >= 1)
				cur->sample_delta = rd_be32(body + 12);
		} else if (!memcmp(typ, "stsc", 4) && body_len >= 8) {
			cur->stsc = body;
			cur->stsc_len = body_len;
		} else if (!memcmp(typ, "stsz", 4) && body_len >= 12) {
			cur->stsz = body;
			cur->stsz_len = body_len;
		} else if (!memcmp(typ, "stco", 4) && body_len >= 8) {
			cur->stco = body;
			cur->stco_len = body_len;
		}
		off += sz;
	}
}

static bool qt_mov_open_path(const char *path)
{
	struct stat st;
	int fd;
	uint8_t hdr[16];
	off_t fpos = 0;
	uint8_t *moov_buf = NULL;
	size_t moov_len = 0;
	struct qt_stbl_parse cur_sp, sp, aud_sp;
	uint32_t fixed_sz, sample_cnt, chunk_cnt, stsc_cnt;
	uint32_t s_idx = 0, c_idx;

	if (!path || !path[0])
		return false;
	if (g_qt_mov.fd >= 0 && strcmp(g_qt_mov.path, path) == 0 &&
	    g_qt_mov.frame_count > 0)
		return true;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	if (fstat(fd, &st) < 0 || st.st_size < 32) {
		close(fd);
		return false;
	}

	while (fpos + 8 <= st.st_size) {
		uint64_t asz;
		size_t ahdr = 8;

		if (pread(fd, hdr, 16, fpos) < 8)
			break;
		asz = rd_be32(hdr);
		if (asz == 1) {
			asz = ((uint64_t)rd_be32(hdr + 8) << 32) | rd_be32(hdr + 12);
			ahdr = 16;
		} else if (asz == 0) {
			asz = (uint64_t)(st.st_size - fpos);
		}
		if (asz < ahdr || fpos + (off_t)asz > st.st_size)
			break;
		if (!memcmp(hdr + 4, "moov", 4)) {
			moov_len = (size_t)(asz - ahdr);
			if (moov_len > 4 * 1024 * 1024)
				break;
			moov_buf = malloc(moov_len);
			if (!moov_buf)
				break;
			if (pread(fd, moov_buf, moov_len, fpos + (off_t)ahdr) !=
			    (ssize_t)moov_len) {
				free(moov_buf);
				moov_buf = NULL;
			}
			break;
		}
		fpos += (off_t)asz;
	}

	if (!moov_buf) {
		close(fd);
		return false;
	}

	memset(&cur_sp, 0, sizeof(cur_sp));
	memset(&sp, 0, sizeof(sp));
	memset(&aud_sp, 0, sizeof(aud_sp));
	qt_walk_atoms(moov_buf, moov_len, &cur_sp, &sp, &aud_sp);
	if (!sp.is_video || !sp.stsz || !sp.stco || sp.stsz_len < 12 || sp.stco_len < 8) {
		free(moov_buf);
		close(fd);
		return false;
	}

	fixed_sz = rd_be32(sp.stsz + 4);
	sample_cnt = rd_be32(sp.stsz + 8);
	if (sample_cnt == 0) {
		free(moov_buf);
		close(fd);
		return false;
	}
	if (sample_cnt > QT_MOV_MAX_FRAMES)
		sample_cnt = QT_MOV_MAX_FRAMES;
	if (fixed_sz == 0 && sp.stsz_len < 12 + (size_t)sample_cnt * 4) {
		free(moov_buf);
		close(fd);
		return false;
	}

	chunk_cnt = rd_be32(sp.stco + 4);
	if (chunk_cnt == 0 || sp.stco_len < 8 + (size_t)chunk_cnt * 4) {
		free(moov_buf);
		close(fd);
		return false;
	}
	stsc_cnt = (sp.stsc && sp.stsc_len >= 8) ? rd_be32(sp.stsc + 4) : 0;

	qt_mov_close();
	for (c_idx = 0; c_idx < chunk_cnt && s_idx < sample_cnt; c_idx++) {
		uint32_t chunk_no = c_idx + 1;
		uint32_t spc = 1;
		uint32_t cur_off = rd_be32(sp.stco + 8 + c_idx * 4);
		uint32_t j;

		for (j = 0; j < stsc_cnt; j++) {
			if (8 + (j + 1) * 12 > sp.stsc_len)
				break;
			uint32_t first_chunk = rd_be32(sp.stsc + 8 + j * 12);
			uint32_t n_spc = rd_be32(sp.stsc + 8 + j * 12 + 4);
			if (chunk_no >= first_chunk)
				spc = n_spc;
			else
				break;
		}
		for (j = 0; j < spc && s_idx < sample_cnt; j++) {
			uint32_t ssz = fixed_sz ? fixed_sz :
				       rd_be32(sp.stsz + 12 + s_idx * 4);
			g_qt_mov.samples[s_idx].offset = cur_off;
			g_qt_mov.samples[s_idx].size = ssz;
			cur_off += ssz;
			s_idx++;
		}
	}

	/* Parse S16_LE stereo PCM audio track ("soun") if present */
	if (aud_sp.is_audio && aud_sp.stco && aud_sp.stco_len >= 8) {
		uint32_t a_chunks = rd_be32(aud_sp.stco + 4);
		uint32_t a_stsc_cnt = (aud_sp.stsc && aud_sp.stsc_len >= 8) ?
				      rd_be32(aud_sp.stsc + 4) : 0;
		uint32_t total_pcm = 0;
		uint32_t ac;

		if (a_chunks > QT_MOV_MAX_FRAMES)
			a_chunks = QT_MOV_MAX_FRAMES;
		for (ac = 0; ac < a_chunks; ac++) {
			uint32_t chunk_no = ac + 1;
			uint32_t spc = 1024;
			uint32_t j;

			if (8 + (size_t)(ac + 1) * 4 > aud_sp.stco_len)
				break;
			for (j = 0; j < a_stsc_cnt; j++) {
				if (8 + (j + 1) * 12 > aud_sp.stsc_len)
					break;
				uint32_t first_chunk = rd_be32(aud_sp.stsc + 8 + j * 12);
				uint32_t n_spc = rd_be32(aud_sp.stsc + 8 + j * 12 + 4);
				if (chunk_no >= first_chunk)
					spc = n_spc;
				else
					break;
			}
			g_qt_mov.audio_chunks[ac].offset = rd_be32(aud_sp.stco + 8 + ac * 4);
			g_qt_mov.audio_chunks[ac].pcm_frames = spc;
			g_qt_mov.audio_chunks[ac].start_frame = total_pcm;
			total_pcm += spc;
		}
		g_qt_mov.audio_chunk_count = ac;
		g_qt_mov.audio_total_frames = total_pcm;
		g_qt_mov.audio_rate = (aud_sp.timescale >= 8000 && aud_sp.timescale <= 96000) ?
				      aud_sp.timescale : 48000;
	}

	free(moov_buf);
	if (s_idx == 0) {
		close(fd);
		return false;
	}

	g_qt_mov.fd = fd;
	snprintf(g_qt_mov.path, sizeof(g_qt_mov.path), "%s", path);
	g_qt_mov.frame_count = s_idx;
	if (sp.timescale > 0 && sp.sample_delta > 0) {
		uint32_t fps = (sp.timescale + sp.sample_delta / 2) / sp.sample_delta;
		g_qt_mov.fps = (fps >= 1 && fps <= 120) ? fps : 24;
	} else {
		g_qt_mov.fps = 24;
	}
	if (sp.timescale > 0 && sp.duration_ts > 0) {
		g_qt_mov.duration_sec = (sp.duration_ts + sp.timescale / 2) / sp.timescale;
	} else {
		g_qt_mov.duration_sec = g_qt_mov.frame_count / g_qt_mov.fps;
	}
	if (g_qt_mov.duration_sec == 0)
		g_qt_mov.duration_sec = 1;
	return true;
}

/*
 * Reads either a standalone JPEG file or Frame 0 of a QuickTime .mov container
 * into @dst_buf (up to @max_len bytes). Returns the JPEG byte length on success,
 * or 0 on failure.
 */
static size_t read_jpeg_or_mov_frame0(const char *path, uint8_t *dst_buf, size_t max_len)
{
	struct stat st;
	int fd;
	uint8_t magic[4];

	if (!path || !path[0] || !dst_buf || max_len < 64)
		return 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) < 0 || st.st_size < 16 ||
	    read(fd, magic, 4) != 4) {
		close(fd);
		return 0;
	}

	/* Standalone JPEG (starts with FF D8) */
	if (magic[0] == 0xff && magic[1] == 0xd8) {
		ssize_t n;
		if ((size_t)st.st_size > max_len) {
			close(fd);
			return 0;
		}
		n = pread(fd, dst_buf, (size_t)st.st_size, 0);
		close(fd);
		return (n == st.st_size) ? (size_t)n : 0;
	}
	close(fd);

	/* QuickTime .mov container: read sample 0 */
	if (qt_mov_open_path(path) && g_qt_mov.frame_count > 0) {
		uint32_t sz = g_qt_mov.samples[0].size;
		uint32_t off = g_qt_mov.samples[0].offset;
		if (sz > 16 && sz <= max_len) {
			ssize_t n = pread(g_qt_mov.fd, dst_buf, sz, (off_t)off);
			if (n == (ssize_t)sz && dst_buf[0] == 0xff && dst_buf[1] == 0xd8)
				return (size_t)n;
		}
	}
	return 0;
}

#ifdef __linux__
#define HW_JPG_MAX_BS		(1536 * 1024)
#define HW_NV12_BUF_W		1280
#define HW_NV12_BUF_H		960

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
		if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
			if (m != 0xc0 || seglen < 7)
				return false;
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

struct hw_m2sc_ctx {
	int fd;
	uint32_t dst_w;
	uint32_t dst_h;
	uint32_t out_sizeimage;
	uint32_t cap_sizeimage;
	void *cap_ptr;
	size_t cap_len;
	struct v4l2_rect last_crop;
	bool cap_dmabuf;
	bool streaming;
};

static struct {
	bool probed;
	bool ready;
	char jpg_dev[32];
	char m2sc_dev[2][32];
	int m2sc_count;

	int jpg_fd;
	void *jpg_out_ptr;
	size_t jpg_out_len;
	int nv12_dmabuf_fd;
	struct v4l2_pix_format nv12_pix;
	bool jpg_streaming;

	char last_jpg_path[512];
	int last_src_w;
	int last_src_h;

	struct hw_m2sc_ctx sc_card;	/* 550 x 624 (2x SS -> 275 x 312) on scaler0 */
	struct hw_m2sc_ctx sc_tile;	/* 550 x 328 (2x SS -> 275 x 164) on scaler0 */
	struct hw_m2sc_ctx sc_fs;	/* 1280 x 720 XRGB32 on scaler1 (MMAP) */
	struct hw_m2sc_ctx sc_vid;	/* 1280 x 720 XRGB32 on scaler1 (DRM DMABUF) */
} g_hw_pipe = {
	.jpg_fd = -1,
	.nv12_dmabuf_fd = -1,
	.sc_card = { .fd = -1 },
	.sc_tile = { .fd = -1 },
	.sc_fs = { .fd = -1 },
	.sc_vid = { .fd = -1 },
};

static void discover_v4l2_devices(void)
{
	int i;

	if (g_hw_pipe.probed)
		return;
	g_hw_pipe.probed = true;

	for (i = 0; i < 8; i++) {
		char path[32];
		struct v4l2_capability cap;
		int fd;

		snprintf(path, sizeof(path), "/dev/video%d", i);
		fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		memset(&cap, 0, sizeof(cap));
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
			if (!g_hw_pipe.jpg_dev[0] &&
			    strcmp((const char *)cap.driver, "s5l8947x-jpeg") == 0) {
				snprintf(g_hw_pipe.jpg_dev, sizeof(g_hw_pipe.jpg_dev),
					 "%s", path);
			} else if (g_hw_pipe.m2sc_count < 2 &&
				   strcmp((const char *)cap.driver, "s5l8947x-m2sc") == 0) {
				snprintf(g_hw_pipe.m2sc_dev[g_hw_pipe.m2sc_count],
					 sizeof(g_hw_pipe.m2sc_dev[0]), "%s", path);
				g_hw_pipe.m2sc_count++;
			}
		}
		close(fd);
	}
}

static void hw_m2sc_ctx_close(struct hw_m2sc_ctx *sc)
{
	if (!sc)
		return;
	if (sc->fd >= 0 && sc->streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		(void)xioctl_v4l2(sc->fd, VIDIOC_STREAMOFF, &type);
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		(void)xioctl_v4l2(sc->fd, VIDIOC_STREAMOFF, &type);
		sc->streaming = false;
	}
	if (sc->cap_ptr) {
		munmap(sc->cap_ptr, sc->cap_len);
		sc->cap_ptr = NULL;
		sc->cap_len = 0;
	}
	if (sc->fd >= 0) {
		close(sc->fd);
		sc->fd = -1;
	}
	memset(&sc->last_crop, 0, sizeof(sc->last_crop));
}

static void hw_pipe_close(void)
{
	hw_m2sc_ctx_close(&g_hw_pipe.sc_card);
	hw_m2sc_ctx_close(&g_hw_pipe.sc_tile);
	hw_m2sc_ctx_close(&g_hw_pipe.sc_fs);
	hw_m2sc_ctx_close(&g_hw_pipe.sc_vid);

	if (g_hw_pipe.jpg_fd >= 0 && g_hw_pipe.jpg_streaming) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		(void)xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_STREAMOFF, &type);
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		(void)xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_STREAMOFF, &type);
		g_hw_pipe.jpg_streaming = false;
	}
	if (g_hw_pipe.nv12_dmabuf_fd >= 0) {
		close(g_hw_pipe.nv12_dmabuf_fd);
		g_hw_pipe.nv12_dmabuf_fd = -1;
	}
	if (g_hw_pipe.jpg_out_ptr) {
		munmap(g_hw_pipe.jpg_out_ptr, g_hw_pipe.jpg_out_len);
		g_hw_pipe.jpg_out_ptr = NULL;
		g_hw_pipe.jpg_out_len = 0;
	}
	if (g_hw_pipe.jpg_fd >= 0) {
		close(g_hw_pipe.jpg_fd);
		g_hw_pipe.jpg_fd = -1;
	}
	g_hw_pipe.last_jpg_path[0] = '\0';
	g_hw_pipe.last_src_w = 0;
	g_hw_pipe.last_src_h = 0;
	g_hw_pipe.ready = false;
}

static bool hw_m2sc_ctx_init(struct hw_m2sc_ctx *sc, const char *dev,
			     uint32_t dst_w, uint32_t dst_h, bool cap_dmabuf)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer b;
	enum v4l2_buf_type type;

	memset(sc, 0, sizeof(*sc));
	sc->fd = open(dev, O_RDWR | O_CLOEXEC);
	if (sc->fd < 0)
		return false;
	sc->cap_dmabuf = cap_dmabuf;

	/* OUTPUT: zero-copy NV12 DMABUF exported by s5l8947x-jpeg */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix = g_hw_pipe.nv12_pix;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_JPEG;
	fmt.fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
	fmt.fmt.pix.quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt.fmt.pix.xfer_func = V4L2_XFER_FUNC_SRGB;
	if (xioctl_v4l2(sc->fd, VIDIOC_S_FMT, &fmt) < 0 ||
	    fmt.fmt.pix.bytesperline != g_hw_pipe.nv12_pix.bytesperline)
		goto err;
	sc->out_sizeimage = fmt.fmt.pix.sizeimage;

	/* CAPTURE: scaled & colour-converted XRGB8888 surface */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = dst_w;
	fmt.fmt.pix.height = dst_h;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_XRGB32;
	fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (xioctl_v4l2(sc->fd, VIDIOC_S_FMT, &fmt) < 0)
		goto err;
	sc->dst_w = fmt.fmt.pix.width;
	sc->dst_h = fmt.fmt.pix.height;
	sc->cap_sizeimage = fmt.fmt.pix.sizeimage;

	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_DMABUF;
	if (xioctl_v4l2(sc->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
		goto err;

	memset(&req, 0, sizeof(req));
	req.count = cap_dmabuf ? 3 : 1;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = cap_dmabuf ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(sc->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
		goto err;

	if (!cap_dmabuf) {
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = 0;
		if (xioctl_v4l2(sc->fd, VIDIOC_QUERYBUF, &b) < 0)
			goto err;
		sc->cap_len = b.length;
		sc->cap_ptr = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED,
				   sc->fd, b.m.offset);
		if (sc->cap_ptr == MAP_FAILED) {
			sc->cap_ptr = NULL;
			goto err;
		}
	}

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl_v4l2(sc->fd, VIDIOC_STREAMON, &type) < 0)
		goto err;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl_v4l2(sc->fd, VIDIOC_STREAMON, &type) < 0)
		goto err;
	sc->streaming = true;
	return true;

err:
	hw_m2sc_ctx_close(sc);
	return false;
}

static bool hw_pipe_init(void)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer b;
	struct v4l2_exportbuffer eb;
	enum v4l2_buf_type type;
	const char *fs_dev;

	if (g_hw_pipe.ready)
		return true;

	discover_v4l2_devices();
	if (!g_hw_pipe.jpg_dev[0] || g_hw_pipe.m2sc_count <= 0)
		return false;

	g_hw_pipe.jpg_fd = open(g_hw_pipe.jpg_dev, O_RDWR | O_CLOEXEC);
	if (g_hw_pipe.jpg_fd < 0)
		return false;

	/* JPEG OUTPUT (compressed bitstream) */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = HW_NV12_BUF_W;
	fmt.fmt.pix.height = HW_NV12_BUF_H;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
	fmt.fmt.pix.sizeimage = HW_JPG_MAX_BS;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_S_FMT, &fmt) < 0)
		goto err;

	/* JPEG CAPTURE (NV12 DMA buffer exported as DMABUF) */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width = HW_NV12_BUF_W;
	fmt.fmt.pix.height = HW_NV12_BUF_H;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_S_FMT, &fmt) < 0)
		goto err;
	g_hw_pipe.nv12_pix = fmt.fmt.pix;

	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
		goto err;

	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = 0;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_QUERYBUF, &b) < 0)
		goto err;
	g_hw_pipe.jpg_out_len = b.length;
	g_hw_pipe.jpg_out_ptr = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
				     MAP_SHARED, g_hw_pipe.jpg_fd, b.m.offset);
	if (g_hw_pipe.jpg_out_ptr == MAP_FAILED) {
		g_hw_pipe.jpg_out_ptr = NULL;
		goto err;
	}

	memset(&req, 0, sizeof(req));
	req.count = 1;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1)
		goto err;

	memset(&eb, 0, sizeof(eb));
	eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	eb.index = 0;
	eb.flags = O_RDWR | O_CLOEXEC;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_EXPBUF, &eb) < 0)
		goto err;
	g_hw_pipe.nv12_dmabuf_fd = eb.fd;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_STREAMON, &type) < 0)
		goto err;
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_STREAMON, &type) < 0)
		goto err;
	g_hw_pipe.jpg_streaming = true;

	fs_dev = g_hw_pipe.m2sc_dev[g_hw_pipe.m2sc_count > 1 ? 1 : 0];
	if (!hw_m2sc_ctx_init(&g_hw_pipe.sc_card, g_hw_pipe.m2sc_dev[0], 550, 624, false))
		goto err;
	if (!hw_m2sc_ctx_init(&g_hw_pipe.sc_tile, g_hw_pipe.m2sc_dev[0], 550, 328, false))
		goto err;
	if (!hw_m2sc_ctx_init(&g_hw_pipe.sc_fs, fs_dev, SHELL_SCREEN_W, SHELL_SCREEN_H, false))
		goto err;
	(void)hw_m2sc_ctx_init(&g_hw_pipe.sc_vid, fs_dev, SHELL_SCREEN_W, SHELL_SCREEN_H, true);

	g_hw_pipe.ready = true;
	return true;

err:
	hw_pipe_close();
	return false;
}

/* Compute centered object-fit:cover source crop rectangle (even-aligned for NV12) */
static void compute_cover_crop_rect(int src_w, int src_h, int dst_w, int dst_h,
				    struct v4l2_rect *r)
{
	int sw = src_w & ~1;
	int sh = src_h & ~1;
	int cw, ch;

	if (sw < 16)
		sw = 16;
	if (sh < 16)
		sh = 16;

	if ((int64_t)sw * dst_h >= (int64_t)sh * dst_w) {
		ch = sh;
		cw = (int)(((int64_t)sh * dst_w) / dst_h) & ~1;
	} else {
		cw = sw;
		ch = (int)(((int64_t)sw * dst_h) / dst_w) & ~1;
	}
	if (cw < 16)
		cw = 16;
	if (ch < 16)
		ch = 16;
	if (cw > sw)
		cw = sw;
	if (ch > sh)
		ch = sh;

	/* Respect M2SC hardware polyphase step limits (< 8x H, <= 7x V) */
	if (cw >= 8 * dst_w)
		cw = (8 * dst_w - 2) & ~1;
	if (ch > 7 * dst_h)
		ch = (7 * dst_h) & ~1;

	r->left = ((sw - cw) / 2) & ~1;
	r->top = ((sh - ch) / 2) & ~1;
	r->width = cw;
	r->height = ch;
}

static bool hw_m2sc_queue_job(struct hw_m2sc_ctx *sc, const struct v4l2_rect *crop)
{
	struct v4l2_buffer ob, cb;

	if (!sc || sc->fd < 0 || !sc->streaming)
		return false;

	if (crop->left != sc->last_crop.left ||
	    crop->top != sc->last_crop.top ||
	    crop->width != sc->last_crop.width ||
	    crop->height != sc->last_crop.height) {
		struct v4l2_selection sel = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.target = V4L2_SEL_TGT_CROP,
			.r = *crop,
		};
		if (xioctl_v4l2(sc->fd, VIDIOC_S_SELECTION, &sel) < 0)
			return false;
		sc->last_crop = *crop;
	}

	memset(&ob, 0, sizeof(ob));
	ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ob.memory = V4L2_MEMORY_DMABUF;
	ob.index = 0;
	ob.m.fd = g_hw_pipe.nv12_dmabuf_fd;
	ob.bytesused = sc->out_sizeimage;
	ob.length = sc->out_sizeimage;
	if (xioctl_v4l2(sc->fd, VIDIOC_QBUF, &ob) < 0)
		return false;

	memset(&cb, 0, sizeof(cb));
	cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cb.memory = V4L2_MEMORY_MMAP;
	cb.index = 0;
	if (xioctl_v4l2(sc->fd, VIDIOC_QBUF, &cb) < 0) {
		(void)xioctl_v4l2(sc->fd, VIDIOC_DQBUF, &ob);
		return false;
	}
	return true;
}

static bool hw_m2sc_wait_job(struct hw_m2sc_ctx *sc, uint32_t *dst_pixels)
{
	struct v4l2_buffer ob, cb;
	bool ok = true;

	memset(&cb, 0, sizeof(cb));
	cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cb.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(sc->fd, VIDIOC_DQBUF, &cb) < 0 ||
	    (cb.flags & V4L2_BUF_FLAG_ERROR))
		ok = false;

	memset(&ob, 0, sizeof(ob));
	ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ob.memory = V4L2_MEMORY_DMABUF;
	if (xioctl_v4l2(sc->fd, VIDIOC_DQBUF, &ob) < 0)
		ok = false;

	if (ok && dst_pixels) {
		size_t bytes = (size_t)sc->dst_w * sc->dst_h * sizeof(uint32_t);
		if (bytes <= sc->cap_len)
			memcpy(dst_pixels, sc->cap_ptr, bytes);
		else
			ok = false;
	}
	return ok;
}

static void downsample_2x2_xrgb_stride(const uint32_t *src, int dst_w, int dst_h,
				       uint32_t *dst, int dst_stride)
{
	int src_stride = dst_w * 2;
	int y, x;
#ifdef __ARM_NEON
	uint8x8_t alpha_mask = vreinterpret_u8_u32(vdup_n_u32(0xff000000U));
#endif

	for (y = 0; y < dst_h; y++) {
		const uint32_t *r0 = src + (size_t)(y * 2) * src_stride;
		const uint32_t *r1 = r0 + src_stride;
		uint32_t *out = dst + (size_t)y * dst_stride;

		x = 0;
#ifdef __ARM_NEON
		for (; x + 2 <= dst_w; x += 2) {
			uint8x16_t row0 = vld1q_u8((const uint8_t *)(r0 + x * 2));
			uint8x16_t row1 = vld1q_u8((const uint8_t *)(r1 + x * 2));
			uint16x8_t sum01 = vaddl_u8(vget_low_u8(row0), vget_low_u8(row1));
			uint16x8_t sum23 = vaddl_u8(vget_high_u8(row0), vget_high_u8(row1));
			uint16x4_t p0 = vadd_u16(vget_low_u16(sum01), vget_high_u16(sum01));
			uint16x4_t p1 = vadd_u16(vget_low_u16(sum23), vget_high_u16(sum23));
			uint8x8_t avg = vrshrn_n_u16(vcombine_u16(p0, p1), 2);

			vst1_u8((uint8_t *)(out + x), vorr_u8(avg, alpha_mask));
		}
#endif
		for (; x < dst_w; x++) {
			uint32_t p00 = r0[x * 2 + 0];
			uint32_t p01 = r0[x * 2 + 1];
			uint32_t p10 = r1[x * 2 + 0];
			uint32_t p11 = r1[x * 2 + 1];
			uint32_t rb = ((p00 & 0x00ff00ffU) + (p01 & 0x00ff00ffU) +
				       (p10 & 0x00ff00ffU) + (p11 & 0x00ff00ffU) +
				       0x00020002U) >> 2;
			uint32_t g  = ((p00 & 0x0000ff00U) + (p01 & 0x0000ff00U) +
				       (p10 & 0x0000ff00U) + (p11 & 0x0000ff00U) +
				       0x00000200U) >> 2;

			out[x] = 0xff000000U | (rb & 0x00ff00ffU) | (g & 0x0000ff00U);
		}
	}
}

/*
 * Run a zero-copy M2SC job that DMAs the scaled 1280x720 XRGB32 output
 * directly into the caller's DRM scanout DMABUF fd (zero CPU copies!).
 */
static bool hw_m2sc_dmabuf_run(struct hw_m2sc_ctx *sc,
			       const struct v4l2_rect *crop,
			       int buf_idx, int dst_dmabuf_fd)
{
	struct v4l2_buffer ob, cb;
	bool ok = true;

	if (!sc || sc->fd < 0 || !sc->streaming || !sc->cap_dmabuf ||
	    buf_idx < 0 || buf_idx > 2 || dst_dmabuf_fd < 0)
		return false;

	if (crop->left != sc->last_crop.left ||
	    crop->top != sc->last_crop.top ||
	    crop->width != sc->last_crop.width ||
	    crop->height != sc->last_crop.height) {
		struct v4l2_selection sel = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
			.target = V4L2_SEL_TGT_CROP,
			.r = *crop,
		};
		if (xioctl_v4l2(sc->fd, VIDIOC_S_SELECTION, &sel) < 0)
			return false;
		sc->last_crop = *crop;
	}

	memset(&ob, 0, sizeof(ob));
	ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ob.memory = V4L2_MEMORY_DMABUF;
	ob.index = 0;
	ob.m.fd = g_hw_pipe.nv12_dmabuf_fd;
	ob.bytesused = sc->out_sizeimage;
	ob.length = sc->out_sizeimage;
	if (xioctl_v4l2(sc->fd, VIDIOC_QBUF, &ob) < 0)
		return false;

	memset(&cb, 0, sizeof(cb));
	cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cb.memory = V4L2_MEMORY_DMABUF;
	cb.index = (uint32_t)buf_idx;
	cb.m.fd = dst_dmabuf_fd;
	cb.bytesused = sc->cap_sizeimage;
	cb.length = sc->cap_sizeimage;
	if (xioctl_v4l2(sc->fd, VIDIOC_QBUF, &cb) < 0) {
		(void)xioctl_v4l2(sc->fd, VIDIOC_DQBUF, &ob);
		return false;
	}

	memset(&cb, 0, sizeof(cb));
	cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cb.memory = V4L2_MEMORY_DMABUF;
	if (xioctl_v4l2(sc->fd, VIDIOC_DQBUF, &cb) < 0 ||
	    (cb.flags & V4L2_BUF_FLAG_ERROR))
		ok = false;

	memset(&ob, 0, sizeof(ob));
	ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ob.memory = V4L2_MEMORY_DMABUF;
	if (xioctl_v4l2(sc->fd, VIDIOC_DQBUF, &ob) < 0)
		ok = false;

	return ok;
}

/*
 * Zero-copy M2SC0 -> SGX Atlas 1 (1680x960 DMABUF) sub-rectangle scaling via
 * V4L2_SEL_TGT_CROP (OUTPUT) + V4L2_SEL_TGT_COMPOSE (CAPTURE).
 */
struct atlas_slot_owner {
	uint32_t **owner_ptr;
	uint32_t *slot_ptr;
	uint64_t last_used;
};

static struct atlas_slot_owner g_atlas_card_slots[10];
static struct atlas_slot_owner g_atlas_tile_slots[10];
static struct atlas_slot_owner g_atlas_fs_slots[2];
static uint64_t g_atlas_clock = 1;

static int alloc_atlas1_slot(bool is_tile, uint32_t **owner_ptr,
			     uint32_t **out_slot_ptr)
{
	struct atlas_slot_owner *tbl = is_tile ? g_atlas_tile_slots : g_atlas_card_slots;
	int i, victim = 0;
	uint64_t oldest = UINT64_MAX;
	uint32_t *sptr;

	for (i = 0; i < 10; i++) {
		if (tbl[i].owner_ptr == owner_ptr) {
			victim = i;
			break;
		}
		if (!tbl[i].owner_ptr) {
			victim = i;
			break;
		}
		if (tbl[i].last_used < oldest) {
			oldest = tbl[i].last_used;
			victim = i;
		}
	}

	sptr = shell_gpu_atlas1_slot_ptr(is_tile, victim, NULL, NULL,
					 NULL, NULL, NULL);
	if (!sptr)
		return -1;

	if (tbl[victim].owner_ptr && tbl[victim].owner_ptr != owner_ptr)
		*tbl[victim].owner_ptr = NULL;

	tbl[victim].owner_ptr = owner_ptr;
	tbl[victim].slot_ptr = sptr;
	tbl[victim].last_used = ++g_atlas_clock;
	*out_slot_ptr = sptr;
	return victim;
}

static void touch_atlas1_slot(const uint32_t *ptr)
{
	int i;

	if (!ptr)
		return;
	for (i = 0; i < 10; i++) {
		if (g_atlas_card_slots[i].slot_ptr == ptr) {
			g_atlas_card_slots[i].last_used = ++g_atlas_clock;
			return;
		}
		if (g_atlas_tile_slots[i].slot_ptr == ptr) {
			g_atlas_tile_slots[i].last_used = ++g_atlas_clock;
			return;
		}
	}
}

static void promote_to_atlas1(bool is_tile, uint32_t **owner_ptr)
{
	uint32_t *heap_buf, *sptr = NULL;
	int h = is_tile ? 164 : 312;
	int row;

	if (!owner_ptr || !*owner_ptr || !shell_gpu_active())
		return;
	heap_buf = *owner_ptr;
	if (alloc_atlas1_slot(is_tile, owner_ptr, &sptr) < 0 || !sptr)
		return;
	for (row = 0; row < h; row++) {
		memcpy(sptr + row * 2048, heap_buf + row * 275,
		       275 * sizeof(uint32_t));
	}
	free(heap_buf);
	*owner_ptr = sptr;
}

/* Run s5l8947x-jpeg on the bitstream already placed in g_hw_pipe.jpg_out_ptr */
static bool hw_jpeg_run_bitstream(size_t jpg_len, int *out_w, int *out_h)
{
	struct v4l2_buffer ob, cb;
	int src_w = 0, src_h = 0;
	bool ok = true;

	if (!parse_jpeg_sof0_dims(g_hw_pipe.jpg_out_ptr, jpg_len, &src_w, &src_h))
		return false;
	if (src_w > (int)g_hw_pipe.nv12_pix.width ||
	    src_h > (int)g_hw_pipe.nv12_pix.height)
		return false;

	memset(&ob, 0, sizeof(ob));
	ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ob.memory = V4L2_MEMORY_MMAP;
	ob.index = 0;
	ob.bytesused = (uint32_t)jpg_len;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_QBUF, &ob) < 0)
		return false;

	memset(&cb, 0, sizeof(cb));
	cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cb.memory = V4L2_MEMORY_MMAP;
	cb.index = 0;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_QBUF, &cb) < 0) {
		(void)xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_DQBUF, &ob);
		return false;
	}

	memset(&cb, 0, sizeof(cb));
	cb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	cb.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_DQBUF, &cb) < 0 ||
	    (cb.flags & V4L2_BUF_FLAG_ERROR))
		ok = false;

	memset(&ob, 0, sizeof(ob));
	ob.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	ob.memory = V4L2_MEMORY_MMAP;
	if (xioctl_v4l2(g_hw_pipe.jpg_fd, VIDIOC_DQBUF, &ob) < 0 ||
	    (ob.flags & V4L2_BUF_FLAG_ERROR))
		ok = false;

	if (!ok)
		return false;

	*out_w = src_w;
	*out_h = src_h;
	return true;
}

/*
 * Decode a baseline JPEG file on s5l8947x-jpeg directly into the persistent
 * NV12 DMABUF, then crop & polyphase-scale on s5l8947x-m2sc (scaler0 for
 * 2x-supersampled 275x312 cards and 275x164 gallery tiles, scaler1 for
 * 1280x720 fullscreen/video).
 */
static bool hw_decode_and_scale_jpeg_file(const char *path,
					  uint32_t **out_card_275,
					  uint32_t **out_tile_275x164,
					  uint32_t **out_fs_720p,
					  uint32_t *fs_inplace_buf)
{
	int src_w = 0, src_h = 0;
	bool need_card = (out_card_275 != NULL);
	bool need_tile = (out_tile_275x164 != NULL);
	bool need_fs = (out_fs_720p != NULL || fs_inplace_buf != NULL);

	if (!path || !path[0] || (!need_card && !need_tile && !need_fs))
		return false;
	if (!hw_pipe_init())
		return false;

	/* Reuse the already-decoded NV12 DMABUF if the same file was just decoded */
	if (g_hw_pipe.last_jpg_path[0] &&
	    strcmp(g_hw_pipe.last_jpg_path, path) == 0 &&
	    g_hw_pipe.last_src_w >= 16 && g_hw_pipe.last_src_h >= 16) {
		src_w = g_hw_pipe.last_src_w;
		src_h = g_hw_pipe.last_src_h;
	} else {
		ssize_t n = read_jpeg_or_mov_frame0(path, g_hw_pipe.jpg_out_ptr,
						    g_hw_pipe.jpg_out_len);
		if (n <= 16) {
			g_hw_pipe.last_jpg_path[0] = '\0';
			return false;
		}
		if (!hw_jpeg_run_bitstream((size_t)n, &src_w, &src_h)) {
			g_hw_pipe.last_jpg_path[0] = '\0';
			return false;
		}
		snprintf(g_hw_pipe.last_jpg_path, sizeof(g_hw_pipe.last_jpg_path),
			 "%s", path);
		g_hw_pipe.last_src_w = src_w;
		g_hw_pipe.last_src_h = src_h;
	}

	uint32_t *card_buf = NULL;
	uint32_t *tile_buf = NULL;
	uint32_t *fs_buf = fs_inplace_buf;
	int card_stride = 275;
	int tile_stride = 275;
	bool card_in_atlas = false;
	bool tile_in_atlas = false;
	bool fs_in_atlas = false;

	if (need_card) {
		if (shell_gpu_active() &&
		    alloc_atlas1_slot(false, out_card_275, &card_buf) >= 0) {
			card_stride = 2048;
			card_in_atlas = true;
		} else {
			card_buf = malloc(275 * 312 * sizeof(uint32_t));
			if (!card_buf)
				return false;
		}
	}
	if (need_tile) {
		if (shell_gpu_active() &&
		    alloc_atlas1_slot(true, out_tile_275x164, &tile_buf) >= 0) {
			tile_stride = 2048;
			tile_in_atlas = true;
		} else {
			tile_buf = malloc(275 * 164 * sizeof(uint32_t));
			if (!tile_buf) {
				if (!card_in_atlas)
					free(card_buf);
				return false;
			}
		}
	}

	struct v4l2_rect card_crop, tile_crop, fs_crop;
	bool card_queued = false, fs_queued = false;
	bool ok = true;

	if (need_card) {
		compute_cover_crop_rect(src_w, src_h, 550, 624, &card_crop);
		card_queued = hw_m2sc_queue_job(&g_hw_pipe.sc_card, &card_crop);
		if (!card_queued)
			ok = false;
	}
	if (ok && need_fs) {
		compute_cover_crop_rect(src_w, src_h, SHELL_SCREEN_W, SHELL_SCREEN_H, &fs_crop);
		if (out_fs_720p && shell_gpu_active() && g_hw_pipe.sc_vid.streaming) {
			uint32_t *fs_ptr = NULL;
			int fs_fd = shell_gpu_fs_atlas_dmabuf_fd(0, &fs_ptr, NULL);

			if (fs_fd >= 0 && fs_ptr &&
			    hw_m2sc_dmabuf_run(&g_hw_pipe.sc_vid, &fs_crop, 0, fs_fd)) {
				if (g_atlas_fs_slots[0].owner_ptr &&
				    g_atlas_fs_slots[0].owner_ptr != out_fs_720p)
					*g_atlas_fs_slots[0].owner_ptr = NULL;
				g_atlas_fs_slots[0].owner_ptr = out_fs_720p;
				g_atlas_fs_slots[0].slot_ptr = fs_ptr;
				g_atlas_fs_slots[0].last_used = ++g_atlas_clock;
				fs_buf = fs_ptr;
				fs_in_atlas = true;
			}
		}
		if (!fs_in_atlas) {
			if (out_fs_720p) {
				fs_buf = malloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H * sizeof(uint32_t));
				if (!fs_buf)
					ok = false;
			}
			if (ok) {
				fs_queued = hw_m2sc_queue_job(&g_hw_pipe.sc_fs, &fs_crop);
				if (!fs_queued)
					ok = false;
			}
		}
	}

	if (card_queued) {
		if (hw_m2sc_wait_job(&g_hw_pipe.sc_card, NULL) && g_hw_pipe.sc_card.cap_ptr)
			downsample_2x2_xrgb_stride((const uint32_t *)g_hw_pipe.sc_card.cap_ptr,
						   275, 312, card_buf, card_stride);
		else
			ok = false;
	}

	if (ok && need_tile) {
		compute_cover_crop_rect(src_w, src_h, 550, 328, &tile_crop);
		if (hw_m2sc_queue_job(&g_hw_pipe.sc_tile, &tile_crop) &&
		    hw_m2sc_wait_job(&g_hw_pipe.sc_tile, NULL) &&
		    g_hw_pipe.sc_tile.cap_ptr)
			downsample_2x2_xrgb_stride((const uint32_t *)g_hw_pipe.sc_tile.cap_ptr,
						   275, 164, tile_buf, tile_stride);
		else
			ok = false;
	}

	if (fs_queued && !hw_m2sc_wait_job(&g_hw_pipe.sc_fs, fs_buf))
		ok = false;

	if (!ok) {
		if (!card_in_atlas)
			free(card_buf);
		if (!tile_in_atlas)
			free(tile_buf);
		if (out_fs_720p && !fs_in_atlas)
			free(fs_buf);
		return false;
	}

	if (out_card_275)
		*out_card_275 = card_buf;
	if (out_tile_275x164)
		*out_tile_275x164 = tile_buf;
	if (out_fs_720p)
		*out_fs_720p = fs_buf;
	return true;
}

static uint32_t *hw_jpeg_decode_raw(const uint8_t *jpg, size_t jpg_len,
				    int *out_w, int *out_h)
{
	int src_w = 0, src_h = 0;

	if (!hw_pipe_init() || jpg_len > g_hw_pipe.jpg_out_len)
		return NULL;

	memcpy(g_hw_pipe.jpg_out_ptr, jpg, jpg_len);
	g_hw_pipe.last_jpg_path[0] = '\0';
	if (!hw_jpeg_run_bitstream(jpg_len, &src_w, &src_h))
		return NULL;

	uint32_t *fs_buf = malloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H * sizeof(uint32_t));
	if (!fs_buf)
		return NULL;

	struct v4l2_rect fs_crop;
	compute_cover_crop_rect(src_w, src_h, SHELL_SCREEN_W, SHELL_SCREEN_H, &fs_crop);
	if (!hw_m2sc_queue_job(&g_hw_pipe.sc_fs, &fs_crop) ||
	    !hw_m2sc_wait_job(&g_hw_pipe.sc_fs, fs_buf)) {
		free(fs_buf);
		return NULL;
	}

	*out_w = SHELL_SCREEN_W;
	*out_h = SHELL_SCREEN_H;
	return fs_buf;
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
	uint8_t fast_sym[256];
	uint8_t fast_len[256];
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

static void idct_8x8(const int16_t *in, uint8_t *out, bool has_ac)
{
	/* Fast fixed-point Q11 separable IDCT across rows then columns */
	static bool cos_init;
	static int32_t c_tbl[8][8];
	int32_t tmp[64];
	int u, x, y;

	if (!cos_init) {
		for (u = 0; u < 8; u++) {
			float cu = (u == 0) ? (1.0f / sqrtf(2.0f)) : 1.0f;
			for (x = 0; x < 8; x++) {
				float c = 0.5f * cu *
					  cosf(((2.0f * (float)x + 1.0f) * (float)u * (float)M_PI) / 16.0f);
				c_tbl[u][x] = (int32_t)lrintf(c * 2048.0f);
			}
		}
		cos_init = true;
	}

	if (!has_ac) {
		uint8_t dc_u8 = clamp_u8_int(((in[0] + 4) >> 3) + 128);
		memset(out, dc_u8, 64);
		return;
	}

	for (y = 0; y < 8; y++) {
		const int16_t *row = in + y * 8;
		if ((row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7]) == 0) {
			int32_t dc = ((int32_t)row[0] * c_tbl[0][0]) >> 6;
			for (x = 0; x < 8; x++)
				tmp[y * 8 + x] = dc;
			continue;
		}
		for (x = 0; x < 8; x++) {
			int32_t s = (int32_t)row[0] * c_tbl[0][x] +
				    (int32_t)row[1] * c_tbl[1][x] +
				    (int32_t)row[2] * c_tbl[2][x] +
				    (int32_t)row[3] * c_tbl[3][x] +
				    (int32_t)row[4] * c_tbl[4][x] +
				    (int32_t)row[5] * c_tbl[5][x] +
				    (int32_t)row[6] * c_tbl[6][x] +
				    (int32_t)row[7] * c_tbl[7][x];
			tmp[y * 8 + x] = s >> 6;
		}
	}

#ifdef __ARM_NEON
	{
		int32x4_t t0_lo = vld1q_s32(&tmp[0 * 8 + 0]), t0_hi = vld1q_s32(&tmp[0 * 8 + 4]);
		int32x4_t t1_lo = vld1q_s32(&tmp[1 * 8 + 0]), t1_hi = vld1q_s32(&tmp[1 * 8 + 4]);
		int32x4_t t2_lo = vld1q_s32(&tmp[2 * 8 + 0]), t2_hi = vld1q_s32(&tmp[2 * 8 + 4]);
		int32x4_t t3_lo = vld1q_s32(&tmp[3 * 8 + 0]), t3_hi = vld1q_s32(&tmp[3 * 8 + 4]);
		int32x4_t t4_lo = vld1q_s32(&tmp[4 * 8 + 0]), t4_hi = vld1q_s32(&tmp[4 * 8 + 4]);
		int32x4_t t5_lo = vld1q_s32(&tmp[5 * 8 + 0]), t5_hi = vld1q_s32(&tmp[5 * 8 + 4]);
		int32x4_t t6_lo = vld1q_s32(&tmp[6 * 8 + 0]), t6_hi = vld1q_s32(&tmp[6 * 8 + 4]);
		int32x4_t t7_lo = vld1q_s32(&tmp[7 * 8 + 0]), t7_hi = vld1q_s32(&tmp[7 * 8 + 4]);
		int16x8_t bias_128 = vdupq_n_s16(128);

		for (y = 0; y < 8; y++) {
			int32x4_t s_lo = vdupq_n_s32(32768);
			int32x4_t s_hi = vdupq_n_s32(32768);

			s_lo = vmlaq_n_s32(s_lo, t0_lo, c_tbl[0][y]);
			s_hi = vmlaq_n_s32(s_hi, t0_hi, c_tbl[0][y]);
			s_lo = vmlaq_n_s32(s_lo, t1_lo, c_tbl[1][y]);
			s_hi = vmlaq_n_s32(s_hi, t1_hi, c_tbl[1][y]);
			s_lo = vmlaq_n_s32(s_lo, t2_lo, c_tbl[2][y]);
			s_hi = vmlaq_n_s32(s_hi, t2_hi, c_tbl[2][y]);
			s_lo = vmlaq_n_s32(s_lo, t3_lo, c_tbl[3][y]);
			s_hi = vmlaq_n_s32(s_hi, t3_hi, c_tbl[3][y]);
			s_lo = vmlaq_n_s32(s_lo, t4_lo, c_tbl[4][y]);
			s_hi = vmlaq_n_s32(s_hi, t4_hi, c_tbl[4][y]);
			s_lo = vmlaq_n_s32(s_lo, t5_lo, c_tbl[5][y]);
			s_hi = vmlaq_n_s32(s_hi, t5_hi, c_tbl[5][y]);
			s_lo = vmlaq_n_s32(s_lo, t6_lo, c_tbl[6][y]);
			s_hi = vmlaq_n_s32(s_hi, t6_hi, c_tbl[6][y]);
			s_lo = vmlaq_n_s32(s_lo, t7_lo, c_tbl[7][y]);
			s_hi = vmlaq_n_s32(s_hi, t7_hi, c_tbl[7][y]);

			int16x8_t v16 = vcombine_s16(vshrn_n_s32(s_lo, 16), vshrn_n_s32(s_hi, 16));
			uint8x8_t u8 = vqmovun_s16(vaddq_s16(v16, bias_128));
			vst1_u8(out + y * 8, u8);
		}
		return;
	}
#endif

	for (x = 0; x < 8; x++) {
		if ((tmp[1 * 8 + x] | tmp[2 * 8 + x] | tmp[3 * 8 + x] |
		     tmp[4 * 8 + x] | tmp[5 * 8 + x] | tmp[6 * 8 + x] | tmp[7 * 8 + x]) == 0) {
			uint8_t dc_u8 = clamp_u8_int(((tmp[0 * 8 + x] * c_tbl[0][0] + 32768) >> 16) + 128);
			for (y = 0; y < 8; y++)
				out[y * 8 + x] = dc_u8;
			continue;
		}
		for (y = 0; y < 8; y++) {
			int32_t s = tmp[0 * 8 + x] * c_tbl[0][y] +
				    tmp[1 * 8 + x] * c_tbl[1][y] +
				    tmp[2 * 8 + x] * c_tbl[2][y] +
				    tmp[3 * 8 + x] * c_tbl[3][y] +
				    tmp[4 * 8 + x] * c_tbl[4][y] +
				    tmp[5 * 8 + x] * c_tbl[5][y] +
				    tmp[6 * 8 + x] * c_tbl[6][y] +
				    tmp[7 * 8 + x] * c_tbl[7][y];
			out[y * 8 + x] = clamp_u8_int(((s + 32768) >> 16) + 128);
		}
	}
}

static void build_huff_tbl(struct huff_tbl *ht, const uint8_t *bits, const uint8_t *vals)
{
	int code = 0;
	int k = 0;
	int i, j, p;

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

	/* Build 8-bit fast lookahead table for 1-step Huffman decode */
	code = 0;
	k = 0;
	for (i = 1; i <= 8; i++) {
		int cnt = bits[i - 1];
		for (j = 0; j < cnt; j++) {
			uint8_t sym = ht->vals[k < 256 ? k : 0];
			int pad_bits = 8 - i;
			int base_prefix = code << pad_bits;
			int num_prefixes = 1 << pad_bits;
			for (p = 0; p < num_prefixes; p++) {
				ht->fast_sym[base_prefix + p] = sym;
				ht->fast_len[base_prefix + p] = (uint8_t)i;
			}
			k++;
			code++;
		}
		code <<= 1;
	}
	ht->valid = true;
}

static inline void jpeg_fill_bits(struct jpeg_dec *jd)
{
	while (jd->bit_cnt <= 24) {
		uint8_t b = 0;

		if (!jd->marker_hit && jd->pos < jd->len) {
			b = jd->data[jd->pos++];
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
		}
		jd->bit_buf = (jd->bit_buf << 8) | b;
		jd->bit_cnt += 8;
	}
}

static inline int jpeg_read_bits(struct jpeg_dec *jd, int n)
{
	if (n <= 0)
		return 0;
	if (jd->bit_cnt < n)
		jpeg_fill_bits(jd);
	jd->bit_cnt -= n;
	return (int)((jd->bit_buf >> jd->bit_cnt) & ((1U << n) - 1U));
}

static inline int jpeg_huff_decode(struct jpeg_dec *jd, const struct huff_tbl *ht)
{
	int code = 0;
	int len;

	if (!ht->valid)
		return 0;
	if (jd->bit_cnt < 16)
		jpeg_fill_bits(jd);

	uint32_t top8 = (jd->bit_buf >> (jd->bit_cnt - 8)) & 0xffU;
	uint8_t flen = ht->fast_len[top8];
	if (flen > 0) {
		jd->bit_cnt -= flen;
		return ht->fast_sym[top8];
	}

	code = (int)top8;
	jd->bit_cnt -= 8;
	for (len = 9; len <= 16; len++) {
		jd->bit_cnt--;
		code = (code << 1) | (int)((jd->bit_buf >> jd->bit_cnt) & 1U);
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

static bool jpeg_decode_block(struct jpeg_dec *jd, int comp_idx, int16_t *out_blk)
{
	struct jpeg_comp *c = &jd->comp[comp_idx];
	const uint16_t *qt = jd->qtbl[c->tq & 3];
	const struct huff_tbl *dc_ht = &jd->dc_huff[c->td & 3];
	const struct huff_tbl *ac_ht = &jd->ac_huff[c->ta & 3];
	int k = 1;
	bool has_ac = false;

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
		has_ac = true;
		k++;
	}
	return has_ac;
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
				for (c = 0; c < jd->ncomp && c < 3; c++) {
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
				bool has_ac = jpeg_decode_block(jd, 0, jd->comp[0].blocks[b]);
				idct_8x8(jd->comp[0].blocks[b], y_plane[b], has_ac);
			}

			if (jd->ncomp == 3) {
				bool cb_ac = jpeg_decode_block(jd, 1, jd->comp[1].blocks[0]);
				idct_8x8(jd->comp[1].blocks[0], cb_plane, cb_ac);
				bool cr_ac = jpeg_decode_block(jd, 2, jd->comp[2].blocks[0]);
				idct_8x8(jd->comp[2].blocks[0], cr_plane, cr_ac);
			} else {
				memset(cb_plane, 128, 64);
				memset(cr_plane, 128, 64);
			}

			int py, px;
			bool is_420 = (jd->ncomp == 3 && jd->max_h == 2 && jd->max_v == 2 &&
				       jd->comp[0].h_samp == 2 && jd->comp[0].v_samp == 2);

			if (is_420 && mx * 16 + 16 <= jd->width && my * 16 + 16 <= jd->height) {
#ifdef __ARM_NEON
				uint8x8_t u8_128 = vdup_n_u8(128);
				int16x8_t s16_64 = vdupq_n_s16(64);
				uint8x8_t ff_v   = vdup_n_u8(0xffU);
#endif
				for (py = 0; py < 16; py++) {
					uint32_t *drow = pixels + (size_t)(my * 16 + py) * jd->width + mx * 16;
					const uint8_t *y_row0 = y_plane[(py >> 3) * 2 + 0] + (py & 7) * 8;
					const uint8_t *y_row1 = y_plane[(py >> 3) * 2 + 1] + (py & 7) * 8;
					const uint8_t *cb_row = cb_plane + (py >> 1) * 8;
					const uint8_t *cr_row = cr_plane + (py >> 1) * 8;

#ifdef __ARM_NEON
					int16x8_t cb = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(cb_row), u8_128));
					int16x8_t cr = vreinterpretq_s16_u16(vsubl_u8(vld1_u8(cr_row), u8_128));
					int16x8_t r_off = vshrq_n_s16(vaddq_s16(vmulq_n_s16(cr, 179), s16_64), 7);
					int16x8_t g_off = vshrq_n_s16(vaddq_s16(vaddq_s16(vmulq_n_s16(cb, 44),
											  vmulq_n_s16(cr, 91)), s16_64), 7);
					int16x8_t b_off = vshrq_n_s16(vaddq_s16(vmulq_n_s16(cb, 227), s16_64), 7);

					int16x8x2_t r_dup = vzipq_s16(r_off, r_off);
					int16x8x2_t g_dup = vzipq_s16(g_off, g_off);
					int16x8x2_t b_dup = vzipq_s16(b_off, b_off);

					int16x8_t y0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(y_row0)));
					int16x8_t y1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(y_row1)));
					uint8x8x4_t p0, p1;

					p0.val[0] = vqmovun_s16(vaddq_s16(y0, b_dup.val[0]));
					p0.val[1] = vqmovun_s16(vsubq_s16(y0, g_dup.val[0]));
					p0.val[2] = vqmovun_s16(vaddq_s16(y0, r_dup.val[0]));
					p0.val[3] = ff_v;
					vst4_u8((uint8_t *)drow, p0);

					p1.val[0] = vqmovun_s16(vaddq_s16(y1, b_dup.val[1]));
					p1.val[1] = vqmovun_s16(vsubq_s16(y1, g_dup.val[1]));
					p1.val[2] = vqmovun_s16(vaddq_s16(y1, r_dup.val[1]));
					p1.val[3] = ff_v;
					vst4_u8((uint8_t *)(drow + 8), p1);
#else
					for (px = 0; px < 16; px++) {
						int y_val = (px < 8) ? y_row0[px] : y_row1[px - 8];
						int cb = (int)cb_row[px >> 1] - 128;
						int cr = (int)cr_row[px >> 1] - 128;
						int r = y_val + ((359 * cr + 128) >> 8);
						int g = y_val - ((88 * cb + 183 * cr + 128) >> 8);
						int bl = y_val + ((454 * cb + 128) >> 8);

						drow[px] = 0xff000000U |
							   ((uint32_t)clamp_u8_int(r) << 16) |
							   ((uint32_t)clamp_u8_int(g) << 8) |
							   (uint32_t)clamp_u8_int(bl);
					}
#endif
				}
				continue;
			}

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

/* Decode a JPEG, QuickTime .mov (frame 0), or PPM file into a raw XRGB8888 surface */
static uint32_t *decode_image_file_raw(const char *path, int *out_w, int *out_h)
{
	FILE *fp;
	long flen;
	uint8_t *fbuf;
	uint32_t *raw = NULL;

	if (!path || !path[0])
		return NULL;

	if (qt_mov_open_path(path)) {
		struct qt_mov_sample s0 = g_qt_mov.samples[0];
		if (s0.size <= 16 || s0.size > 4 * 1024 * 1024)
			return NULL;
		fbuf = malloc(s0.size);
		if (!fbuf)
			return NULL;
		if (pread(g_qt_mov.fd, fbuf, s0.size, (off_t)s0.offset) != (ssize_t)s0.size) {
			free(fbuf);
			return NULL;
		}
#ifdef __linux__
		raw = hw_jpeg_decode_raw(fbuf, s0.size, out_w, out_h);
		if (raw) {
			free(fbuf);
			return raw;
		}
#endif
		raw = sw_jpeg_decode_raw(fbuf, s0.size, out_w, out_h);
		free(fbuf);
		return raw;
	}

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
	uint32_t *card_275;
	uint32_t *tile_275x164;
	uint32_t *fs_720p;
};

#define URI_CACHE_SLOTS		40
#define MAX_ASYNC_FETCHES	8

struct uri_cache_entry {
	char path[256];
	uint32_t *card_275;
	uint32_t *tile_275x164;
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
			"/mnt/data/linux/media/photos",
			"/mnt/data/linux/media/videos",
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

/*
 * "commons:<phrase>" card images: search Wikimedia Commons over HTTPS and
 * download a 960 px JPEG thumbnail (Commons serves these as baseline JPEG,
 * which both JPEG decoders handle). Runs in a forked child of a process
 * that has other threads, so it sticks to static buffers, fork/exec and
 * plain syscalls.
 */
#define COMMONS_UA	"Velliangiri-tvshell/1.0 (https://github.com/codetiger/velliangiri)"
#define COMMONS_API	"https://commons.wikimedia.org/w/api.php?action=query&format=json" \
			"&generator=search&gsrnamespace=6&gsrlimit=10&prop=imageinfo" \
			"&iiprop=url%7Cmime%7Csize%7Cextmetadata" \
			"&iiextmetadatafilter=LicenseShortName&iiurlwidth=960&gsrsearch="

static int run_wget(const char *url, const char *out, int timeout_s)
{
	char tmo[8];
	int status = 0;
	pid_t pid;

	snprintf(tmo, sizeof(tmo), "%d", timeout_s);
	pid = fork();
	if (pid == 0) {
		char *argv[] = { "wget", "-q", "-T", tmo, "-U", COMMONS_UA,
				 "-O", (char *)out, (char *)url, NULL };
		execvp("wget", argv);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Percent-encode q (+ " filetype:bitmap") onto dst. */
static void commons_encode_query(const char *q, char *dst, size_t sz)
{
	static const char hex[] = "0123456789ABCDEF";
	const char *suffix = " filetype:bitmap";
	size_t o = 0;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		const unsigned char *p = (const unsigned char *)(pass ? suffix : q);

		for (; *p && o + 4 < sz; p++) {
			if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
			    (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.') {
				dst[o++] = (char)*p;
			} else {
				dst[o++] = '%';
				dst[o++] = hex[*p >> 4];
				dst[o++] = hex[*p & 15];
			}
		}
	}
	dst[o] = '\0';
}

/* Copy the JSON string value following key inside [seg, end) ("\/" -> "/"). */
static bool seg_get_str(const char *seg, const char *end, const char *key,
			char *dst, size_t sz)
{
	const char *p = seg;
	size_t klen = strlen(key), o = 0;

	while ((p = strstr(p, key)) && p < end) {
		p += klen;
		if (*p != ':' || p[1] != '"')
			continue;
		for (p += 2; *p && *p != '"' && p < end && o + 1 < sz; p++) {
			if (*p == '\\' && p[1]) {
				p++;
				if (*p == 'u')	/* keep it simple: drop \uXXXX */
					{ p += 4; continue; }
			}
			dst[o++] = *p;
		}
		dst[o] = '\0';
		return o > 0;
	}
	return false;
}

static long seg_get_int(const char *seg, const char *end, const char *key)
{
	const char *p = strstr(seg, key);

	if (!p || p >= end)
		return -1;
	p += strlen(key);
	return *p == ':' ? strtol(p + 1, NULL, 10) : -1;
}

static int commons_fetch_child(const char *query, const char *out_jpg)
{
	static char json[256 * 1024], url[1024], enc[256], best[768];
	static char lic[64], credit[128], json_path[300], credit_path[300];
	const char *p;
	long best_idx = 1L << 30;
	bool best_wide = false;
	ssize_t n;
	int fd;

	commons_encode_query(query, enc, sizeof(enc));
	snprintf(url, sizeof(url), "%s%s", COMMONS_API, enc);
	snprintf(json_path, sizeof(json_path), "%s.json", out_jpg);
	if (run_wget(url, json_path, 12) < 0)
		return -1;
	fd = open(json_path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, json, sizeof(json) - 1);
	close(fd);
	unlink(json_path);
	if (n <= 0)
		return -1;
	json[n] = '\0';

	/* page objects: ..."index":N,"imageinfo":[{ ... }] */
	best[0] = lic[0] = '\0';
	for (p = json; (p = strstr(p, "\"index\":")); ) {
		long idx = strtol(p + 8, NULL, 10);
		const char *seg = strstr(p, "\"imageinfo\":[{");
		const char *end = seg ? strstr(seg, "}]") : NULL;
		char mime[32], thumb[768], l[64];
		long w, h;
		bool wide;

		if (!seg || !end) {
			p += 8;
			continue;
		}
		p = end;
		if (!seg_get_str(seg, end, "\"mime\"", mime, sizeof(mime)) ||
		    strcmp(mime, "image/jpeg") ||
		    !seg_get_str(seg, end, "\"thumburl\"", thumb, sizeof(thumb)))
			continue;
		w = seg_get_int(seg, end, "\"width\"");
		h = seg_get_int(seg, end, "\"height\"");
		wide = w > 0 && h > 0 && w * 10 >= h * 12;
		/* prefer landscape, then search rank */
		if ((wide && !best_wide) || (wide == best_wide && idx < best_idx)) {
			best_idx = idx;
			best_wide = wide;
			snprintf(best, sizeof(best), "%s", thumb);
			l[0] = '\0';
			if (seg_get_str(seg, end, "\"LicenseShortName\":{\"value\"", l, sizeof(l)))
				snprintf(lic, sizeof(lic), "%s", l);
			else
				lic[0] = '\0';
		}
	}
	if (!best[0] || run_wget(best, out_jpg, 12) < 0)
		return -1;

	snprintf(credit_path, sizeof(credit_path), "%.250s.credit", out_jpg);
	snprintf(credit, sizeof(credit), "%s%sWikimedia Commons", lic, lic[0] ? " · " : "");
	fd = open(credit_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		ssize_t wr = write(fd, credit, strlen(credit));

		(void)wr;
		close(fd);
	}
	return 0;
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

	/* Find an empty slot, else recycle a finished one (slots used to be
	 * single-use, so after the first few downloads nothing loaded) */
	for (i = 0; i < MAX_ASYNC_FETCHES; i++)
		if (g_fetches[i].pid == 0 && g_fetches[i].url[0] == '\0')
			break;
	if (i == MAX_ASYNC_FETCHES) {
		for (i = 0; i < MAX_ASYNC_FETCHES; i++)
			if (g_fetches[i].pid == 0 && g_fetches[i].done)
				break;
		if (i < MAX_ASYNC_FETCHES)
			memset(&g_fetches[i], 0, sizeof(g_fetches[i]));
	}
	for (; i < MAX_ASYNC_FETCHES; i++) {
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
				if (strncmp(url, "commons:", 8) == 0) {
					if (commons_fetch_child(url + 8, tmp_path) == 0 &&
					    rename(tmp_path, local_out) == 0) {
						char c_tmp[300], c_out[300];

						snprintf(c_tmp, sizeof(c_tmp), "%.250s.credit", tmp_path);
						snprintf(c_out, sizeof(c_out), "%.250s.credit", local_out);
						(void)rename(c_tmp, c_out);
						_exit(0);
					}
					unlink(tmp_path);
					_exit(1);
				}
				/* Try wget first (BusyBox / Alpine), then curl without invoking /bin/sh */
				int status = 0;
				pid_t dl_pid = fork();
				if (dl_pid == 0) {
					char *wget_argv[] = {
						"wget", "-q", "-T", "8",
						"-O", tmp_path, (char *)url, NULL
					};
					execvp("wget", wget_argv);
					_exit(127);
				}
				if (dl_pid < 0 || waitpid(dl_pid, &status, 0) < 0 ||
				    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
					dl_pid = fork();
					if (dl_pid == 0) {
						char *curl_argv[] = {
							"curl", "-fsSL", "--max-time", "8",
							"-o", tmp_path, (char *)url, NULL
						};
						execvp("curl", curl_argv);
						_exit(127);
					}
					if (dl_pid < 0 || waitpid(dl_pid, &status, 0) < 0 ||
					    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
						_exit(1);
				}
				if (rename(tmp_path, local_out) == 0)
					_exit(0);
				unlink(tmp_path);
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

/* Licence/credit line for a downloaded Commons card image, if known. */
bool shell_media_get_card_credit(const struct giri_media_card *card, char *buf, size_t sz)
{
	char path[300];
	ssize_t n;
	int fd;

	if (!card || strncmp(card->uri, "commons:", 8) || sz < 2)
		return false;
	snprintf(path, sizeof(path), "/tmp/tvshell-cache/art_%08x.jpg.credit",
		 fnv1a_hash(card->uri));
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	n = read(fd, buf, sz - 1);
	close(fd);
	if (n <= 0)
		return false;
	buf[n] = '\0';
	return true;
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

static struct uri_cache_entry *find_uri_cache_entry(const char *resolved_path)
{
	int i;

	for (i = 0; i < URI_CACHE_SLOTS; i++) {
		if (g_uri_cache[i].path[0] &&
		    strcmp(g_uri_cache[i].path, resolved_path) == 0) {
			g_uri_cache[i].last_used = ++g_cache_clock;
#ifdef __linux__
			touch_atlas1_slot(g_uri_cache[i].card_275);
			touch_atlas1_slot(g_uri_cache[i].tile_275x164);
#endif
			return &g_uri_cache[i];
		}
	}
	return NULL;
}

static void free_media_ptr(uint32_t **p)
{
	if (!p || !*p)
		return;
#ifdef __linux__
	{
		int i;
		for (i = 0; i < 10; i++) {
			if (g_atlas_card_slots[i].owner_ptr == p) {
				g_atlas_card_slots[i].owner_ptr = NULL;
				*p = NULL;
				return;
			}
			if (g_atlas_tile_slots[i].owner_ptr == p) {
				g_atlas_tile_slots[i].owner_ptr = NULL;
				*p = NULL;
				return;
			}
		}
		for (i = 0; i < 2; i++) {
			if (g_atlas_fs_slots[i].owner_ptr == p) {
				g_atlas_fs_slots[i].owner_ptr = NULL;
				*p = NULL;
				return;
			}
		}
	}
#endif
	free(*p);
	*p = NULL;
}

static void evict_excess_fs_720p(const struct uri_cache_entry *keep)
{
	while (1) {
		int i, active_fs = 0, oldest_idx = -1;
		uint64_t oldest_clk = UINT64_MAX;

		for (i = 0; i < URI_CACHE_SLOTS; i++) {
			if (&g_uri_cache[i] == keep || !g_uri_cache[i].fs_720p)
				continue;
			active_fs++;
			if (g_uri_cache[i].last_used < oldest_clk) {
				oldest_clk = g_uri_cache[i].last_used;
				oldest_idx = i;
			}
		}
		if (active_fs < 2 || oldest_idx < 0)
			break;
		free_media_ptr(&g_uri_cache[oldest_idx].fs_720p);
	}
}

static struct uri_cache_entry *get_uri_cache_entry(const char *resolved_path,
						   bool need_card,
						   bool need_tile,
						   bool need_fs)
{
	int i, victim = 0;
	uint64_t oldest = UINT64_MAX;
	struct uri_cache_entry *hit = find_uri_cache_entry(resolved_path);

	if (hit) {
		bool miss_card = need_card && !hit->card_275;
		bool miss_tile = need_tile && !hit->tile_275x164;
		bool miss_fs   = need_fs   && !hit->fs_720p;

		if (!miss_card && !miss_tile && !miss_fs)
			return hit;
		if (miss_fs)
			evict_excess_fs_720p(hit);
#ifdef __linux__
		if (hw_decode_and_scale_jpeg_file(resolved_path,
						  miss_card ? &hit->card_275 : NULL,
						  miss_tile ? &hit->tile_275x164 : NULL,
						  miss_fs   ? &hit->fs_720p : NULL,
						  NULL))
			return hit;
#endif
		int rw = 0, rh = 0;
		uint32_t *raw = decode_image_file_raw(resolved_path, &rw, &rh);
		if (raw) {
			if (miss_card) {
				hit->card_275 = scale_cover_bilinear(raw, rw, rh, 275, 312);
				promote_to_atlas1(false, &hit->card_275);
			}
			if (miss_tile) {
				hit->tile_275x164 = scale_cover_bilinear(raw, rw, rh, 275, 164);
				promote_to_atlas1(true, &hit->tile_275x164);
			}
			if (miss_fs) {
				if (rw == SHELL_SCREEN_W && rh == SHELL_SCREEN_H) {
					hit->fs_720p = raw;
					raw = NULL;
				} else {
					hit->fs_720p = scale_cover_bilinear(raw, rw, rh,
									    SHELL_SCREEN_W,
									    SHELL_SCREEN_H);
				}
			}
			free(raw);
		}
		return hit;
	}

	for (i = 0; i < URI_CACHE_SLOTS; i++) {
		if (g_uri_cache[i].last_used < oldest) {
			oldest = g_uri_cache[i].last_used;
			victim = i;
		}
	}

	if (need_fs)
		evict_excess_fs_720p(&g_uri_cache[victim]);

	struct uri_cache_entry *e = &g_uri_cache[victim];
	free_media_ptr(&e->card_275);
	free_media_ptr(&e->tile_275x164);
	free_media_ptr(&e->fs_720p);
	memset(e, 0, sizeof(*e));

#ifdef __linux__
	if (hw_decode_and_scale_jpeg_file(resolved_path,
					  need_card ? &e->card_275 : NULL,
					  need_tile ? &e->tile_275x164 : NULL,
					  need_fs   ? &e->fs_720p : NULL,
					  NULL)) {
		snprintf(e->path, sizeof(e->path), "%s", resolved_path);
		e->last_used = ++g_cache_clock;
		return e;
	}
#endif

	int rw = 0, rh = 0;
	uint32_t *raw = decode_image_file_raw(resolved_path, &rw, &rh);
	if (!raw)
		return NULL;

	snprintf(e->path, sizeof(e->path), "%s", resolved_path);
	e->last_used = ++g_cache_clock;
	if (need_card) {
		e->card_275 = scale_cover_bilinear(raw, rw, rh, 275, 312);
		promote_to_atlas1(false, &e->card_275);
	}
	if (need_tile) {
		e->tile_275x164 = scale_cover_bilinear(raw, rw, rh, 275, 164);
		promote_to_atlas1(true, &e->tile_275x164);
	}
	if (need_fs) {
		if (rw == SHELL_SCREEN_W && rh == SHELL_SCREEN_H) {
			e->fs_720p = raw;
			raw = NULL;
		} else {
			e->fs_720p = scale_cover_bilinear(raw, rw, rh, SHELL_SCREEN_W, SHELL_SCREEN_H);
		}
	}
	free(raw);
	return e;
}

static const uint32_t *ensure_preset_art_loaded(enum giri_preset_art preset, bool want_tile)
{
	char ph_path[256];
	const char *ph_name;
	bool need_card = !want_tile && !g_art_cache[preset < GIRI_ART_COUNT && preset >= 0 ? preset : 0].card_275;
	bool need_tile = want_tile && !g_art_cache[preset < GIRI_ART_COUNT && preset >= 0 ? preset : 0].tile_275x164;

	if (preset < 0 || preset >= GIRI_ART_COUNT)
		preset = GIRI_ART_DAWN_RIDGE;
	if (!need_card && !need_tile) {
		const uint32_t *cached = want_tile ? g_art_cache[preset].tile_275x164 : g_art_cache[preset].card_275;
#ifdef __linux__
		touch_atlas1_slot(cached);
#endif
		return cached;
	}

	ph_name = preset_placeholder_names[preset];
	if (ph_name && resolve_placeholder_path(ph_name, ph_path, sizeof(ph_path))) {
#ifdef __linux__
		if (hw_decode_and_scale_jpeg_file(ph_path,
						  need_card ? &g_art_cache[preset].card_275 : NULL,
						  need_tile ? &g_art_cache[preset].tile_275x164 : NULL,
						  NULL, NULL))
			return want_tile ? g_art_cache[preset].tile_275x164 : g_art_cache[preset].card_275;
#endif
		int rw = 0, rh = 0;
		uint32_t *raw = decode_image_file_raw(ph_path, &rw, &rh);
		if (raw) {
			if (need_card) {
				g_art_cache[preset].card_275 = scale_cover_bilinear(raw, rw, rh, 275, 312);
				promote_to_atlas1(false, &g_art_cache[preset].card_275);
			}
			if (need_tile) {
				g_art_cache[preset].tile_275x164 = scale_cover_bilinear(raw, rw, rh, 275, 164);
				promote_to_atlas1(true, &g_art_cache[preset].tile_275x164);
			}
			free(raw);
			return want_tile ? g_art_cache[preset].tile_275x164 : g_art_cache[preset].card_275;
		}
	}

	if (need_card) {
		g_art_cache[preset].card_275 = rasterize_preset_surface(preset, 275, 312);
		promote_to_atlas1(false, &g_art_cache[preset].card_275);
	}
	if (need_tile) {
		g_art_cache[preset].tile_275x164 = rasterize_preset_surface(preset, 275, 164);
		promote_to_atlas1(true, &g_art_cache[preset].tile_275x164);
	}
	return want_tile ? g_art_cache[preset].tile_275x164 : g_art_cache[preset].card_275;
}

static bool qt_mov_open_for_card(const struct giri_media_card *card)
{
	char resolved[512];

	if (!card)
		return false;
	if (card->uri[0] && resolve_media_path(card->uri, resolved, sizeof(resolved))) {
		if (qt_mov_open_path(resolved))
			return true;
	}
	if (card->detail_text[0] &&
	    resolve_media_path(card->detail_text, resolved, sizeof(resolved))) {
		if (qt_mov_open_path(resolved))
			return true;
	}
	return false;
}

/* =========================================================================
 * 5b. S5L8947X Direct Kernel ALSA Audio Engine (/dev/snd/pcmC0D0p HDMI I2S
 *     & /dev/snd/pcmC0D1p Optical S/PDIF) for Synchronized .mov Playback
 * ========================================================================= */

#define VID_PCM_PERIOD_FRAMES	1024
#define VID_PCM_BUFFER_FRAMES	4096
#define VID_OPT_PERIOD_FRAMES	1000
#define VID_OPT_BUFFER_FRAMES	4000

static struct {
	pthread_mutex_t lock;
	pthread_t thread;
	volatile bool running;
	volatile bool paused;
	volatile bool hw_active;
	int aud_fd;
	uint32_t rate;
	uint32_t chunk_count;
	uint32_t total_frames;
	uint32_t pcm_pos;
	uint32_t cur_chunk;
	uint32_t primed_frames;
	uint32_t synth_phase;
	struct qt_mov_audio_chunk chunks[QT_MOV_MAX_FRAMES];
} g_vid_audio = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.aud_fd = -1,
	.rate = 48000,
};

#ifdef __linux__
static void alsa_hw_mask_set(struct snd_pcm_hw_params *p, int param, unsigned int bit)
{
	struct snd_mask *mask = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(mask, 0, sizeof(*mask));
	mask->bits[bit / 32] |= 1U << (bit % 32);
}

static void alsa_hw_interval_set(struct snd_pcm_hw_params *p, int param, unsigned int val)
{
	struct snd_interval *iv =
		&p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	memset(iv, 0, sizeof(*iv));
	iv->min = val;
	iv->max = val;
	iv->integer = 1;
}

static int alsa_pcm_configure(int fd, unsigned int rate,
			      unsigned int period_frames,
			      unsigned int buffer_frames)
{
	struct snd_pcm_hw_params hw;
	int i;

	memset(&hw, 0, sizeof(hw));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&hw.masks[i], 0xff, sizeof(hw.masks[i]));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
		hw.intervals[i].max = ~0U;

	alsa_hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	alsa_hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	alsa_hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	alsa_hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	alsa_hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
	alsa_hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_frames);
	alsa_hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, buffer_frames);
	hw.rmask = ~0U;

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0)
		return -1;
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0)
		return -1;
	return 0;
}

static void vid_audio_fill_period_locked(int16_t *dst_stereo, uint32_t frames)
{
	if (g_vid_audio.aud_fd >= 0 && g_vid_audio.chunk_count > 0 &&
	    g_vid_audio.total_frames > 0) {
		while (frames > 0) {
			uint32_t pos = g_vid_audio.pcm_pos % g_vid_audio.total_frames;
			uint32_t cnt = g_vid_audio.chunk_count;
			uint32_t idx = g_vid_audio.cur_chunk % cnt;
			uint32_t step;
			const struct qt_mov_audio_chunk *ch = &g_vid_audio.chunks[idx];

			for (step = 0; step < cnt; step++) {
				ch = &g_vid_audio.chunks[idx];
				if (pos >= ch->start_frame &&
				    pos < ch->start_frame + ch->pcm_frames) {
					g_vid_audio.cur_chunk = idx;
					break;
				}
				idx = (idx + 1) % cnt;
			}

			uint32_t in_off = (pos >= ch->start_frame &&
					   pos < ch->start_frame + ch->pcm_frames) ?
					  (pos - ch->start_frame) : 0;
			uint32_t avail = (ch->pcm_frames > in_off) ?
					 (ch->pcm_frames - in_off) : 1;
			uint32_t take = frames < avail ? frames : avail;
			off_t byte_off = (off_t)ch->offset + (off_t)in_off * 4;

			if (pread(g_vid_audio.aud_fd, dst_stereo, take * 4, byte_off) !=
			    (ssize_t)(take * 4)) {
				memset(dst_stereo, 0, take * 4);
			}
			dst_stereo += take * 2;
			frames -= take;
			g_vid_audio.pcm_pos = (pos + take) % g_vid_audio.total_frames;
			if (in_off + take >= ch->pcm_frames)
				g_vid_audio.cur_chunk = (g_vid_audio.cur_chunk + 1) % cnt;
		}
		return;
	}

	/*
	 * Gentle stereo ambient harmonic chord for non-.mov video stream cards.
	 * Uses a precomputed 1024-entry Q12 sine LUT and 32-bit fixed-point
	 * phase accumulators instead of 7 per-sample sinf() calls (eliminating
	 * 336,000 floating-point transcendental evaluations/sec at 48 kHz).
	 */
	{
		static int16_t sin_lut[1024];
		static bool lut_ready;
		uint32_t i;
		uint32_t rate = g_vid_audio.rate > 0 ? g_vid_audio.rate : 48000;
		uint64_t scale = 4294967296ULL;
		uint32_t inc_env   = (uint32_t)((scale * 127) / ((uint64_t)rate * 1000));
		uint32_t inc_l0    = (uint32_t)((scale * 22000) / ((uint64_t)rate * 100));
		uint32_t inc_l1    = (uint32_t)((scale * 32963) / ((uint64_t)rate * 100));
		uint32_t inc_l2    = (uint32_t)((scale * 44000) / ((uint64_t)rate * 100));
		uint32_t inc_r0    = (uint32_t)((scale * 22050) / ((uint64_t)rate * 100));
		uint32_t inc_r1    = (uint32_t)((scale * 33000) / ((uint64_t)rate * 100));
		uint32_t inc_r2    = (uint32_t)((scale * 55437) / ((uint64_t)rate * 100));

		if (!lut_ready) {
			for (i = 0; i < 1024; i++) {
				sin_lut[i] = (int16_t)(sinf((float)i * (2.0f * 3.14159265f / 1024.0f)) *
						       4096.0f);
			}
			lut_ready = true;
		}

		for (i = 0; i < frames; i++) {
			uint32_t p = g_vid_audio.synth_phase++;
			int32_t s_env = sin_lut[(uint32_t)(p * inc_env) >> 22];
			int32_t env_q12 = 2253 + ((1434 * s_env) >> 12);
			int32_t l_q12 = (45 * (int32_t)sin_lut[(uint32_t)(p * inc_l0) >> 22] +
					 30 * (int32_t)sin_lut[(uint32_t)(p * inc_l1) >> 22] +
					 20 * (int32_t)sin_lut[(uint32_t)(p * inc_l2) >> 22]) / 100;
			int32_t r_q12 = (45 * (int32_t)sin_lut[(uint32_t)(p * inc_r0) >> 22] +
					 30 * (int32_t)sin_lut[(uint32_t)(p * inc_r1) >> 22] +
					 20 * (int32_t)sin_lut[(uint32_t)(p * inc_r2) >> 22]) / 100;

			dst_stereo[i * 2]     = (int16_t)(((int64_t)l_q12 * env_q12 * 2800LL) >> 24);
			dst_stereo[i * 2 + 1] = (int16_t)(((int64_t)r_q12 * env_q12 * 2800LL) >> 24);
		}
		if (g_vid_audio.total_frames > 0)
			g_vid_audio.pcm_pos = (g_vid_audio.pcm_pos + frames) %
					      g_vid_audio.total_frames;
	}
}

static void *alsa_video_audio_worker(void *arg)
{
	int16_t period_buf[VID_PCM_PERIOD_FRAMES * 2];
	int16_t opt_buf[VID_OPT_PERIOD_FRAMES * 2];
	int hdmi_fd = -1;
	int opt_fd = -1;
	unsigned int rate = g_vid_audio.rate ? g_vid_audio.rate : 48000;

	(void)arg;
	hdmi_fd = open("/dev/snd/pcmC0D0p", O_WRONLY | O_CLOEXEC);
	if (hdmi_fd >= 0) {
		if (alsa_pcm_configure(hdmi_fd, rate,
				       VID_PCM_PERIOD_FRAMES,
				       VID_PCM_BUFFER_FRAMES) < 0) {
			if (alsa_pcm_configure(hdmi_fd, 48000,
					       VID_PCM_PERIOD_FRAMES,
					       VID_PCM_BUFFER_FRAMES) == 0) {
				g_vid_audio.rate = 48000;
			} else {
				close(hdmi_fd);
				hdmi_fd = -1;
			}
		}
	}

	opt_fd = open("/dev/snd/pcmC0D1p", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (opt_fd >= 0) {
		if (alsa_pcm_configure(opt_fd, 46875,
				       VID_OPT_PERIOD_FRAMES,
				       VID_OPT_BUFFER_FRAMES) < 0) {
			close(opt_fd);
			opt_fd = -1;
		}
	}

	if (hdmi_fd < 0 && opt_fd < 0) {
		g_vid_audio.hw_active = false;
		return NULL;
	}
	g_vid_audio.hw_active = true;

	while (g_vid_audio.running) {
		if (g_vid_audio.paused) {
			usleep(10000);
			continue;
		}

		pthread_mutex_lock(&g_vid_audio.lock);
		vid_audio_fill_period_locked(period_buf, VID_PCM_PERIOD_FRAMES);
		g_vid_audio.primed_frames += VID_PCM_PERIOD_FRAMES;
		pthread_mutex_unlock(&g_vid_audio.lock);

		if (hdmi_fd >= 0) {
			struct snd_xferi xfer = {
				.buf = period_buf,
				.frames = VID_PCM_PERIOD_FRAMES,
			};
			while (g_vid_audio.running && !g_vid_audio.paused &&
			       ioctl(hdmi_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EPIPE) {
					(void)ioctl(hdmi_fd, SNDRV_PCM_IOCTL_PREPARE);
					continue;
				}
				usleep(5000);
				break;
			}
		}

		if (opt_fd >= 0) {
			int i;
			struct snd_xferi xfer_opt = {
				.buf = opt_buf,
				.frames = VID_OPT_PERIOD_FRAMES,
			};
			for (i = 0; i < VID_OPT_PERIOD_FRAMES; i++) {
				uint32_t pos_q10 = ((uint32_t)i * (VID_PCM_PERIOD_FRAMES << 10)) /
						   VID_OPT_PERIOD_FRAMES;
				uint32_t s0 = pos_q10 >> 10;
				uint32_t s1 = (s0 + 1 < VID_PCM_PERIOD_FRAMES) ? (s0 + 1) : s0;
				int32_t frac = (int32_t)(pos_q10 & 0x3ffU);
				int32_t l0 = period_buf[s0 * 2],     l1 = period_buf[s1 * 2];
				int32_t r0 = period_buf[s0 * 2 + 1], r1 = period_buf[s1 * 2 + 1];

				opt_buf[i * 2]     = (int16_t)(l0 + (((l1 - l0) * frac) >> 10));
				opt_buf[i * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * frac) >> 10));
			}
			if (ioctl(opt_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer_opt) < 0 &&
			    errno == EPIPE) {
				(void)ioctl(opt_fd, SNDRV_PCM_IOCTL_PREPARE);
				(void)ioctl(opt_fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer_opt);
			}
			if (hdmi_fd < 0)
				usleep(21333);
		}
	}

	g_vid_audio.hw_active = false;
	if (hdmi_fd >= 0)
		close(hdmi_fd);
	if (opt_fd >= 0)
		close(opt_fd);
	return NULL;
}
#endif

void shell_media_audio_stop(void)
{
#ifdef __linux__
	if (g_vid_audio.running) {
		g_vid_audio.running = false;
		pthread_join(g_vid_audio.thread, NULL);
	}
#endif
	pthread_mutex_lock(&g_vid_audio.lock);
	if (g_vid_audio.aud_fd >= 0) {
		close(g_vid_audio.aud_fd);
		g_vid_audio.aud_fd = -1;
	}
	g_vid_audio.hw_active = false;
	g_vid_audio.chunk_count = 0;
	g_vid_audio.total_frames = 0;
	g_vid_audio.pcm_pos = 0;
	g_vid_audio.cur_chunk = 0;
	g_vid_audio.primed_frames = 0;
	pthread_mutex_unlock(&g_vid_audio.lock);
}

void shell_media_audio_start(const struct giri_media_card *card)
{
	shell_media_audio_stop();
	if (!card)
		return;

	(void)qt_mov_open_for_card(card);

	pthread_mutex_lock(&g_vid_audio.lock);
	if (g_qt_mov.fd >= 0 && g_qt_mov.audio_chunk_count > 0 &&
	    g_qt_mov.audio_total_frames > 0) {
		g_vid_audio.aud_fd = open(g_qt_mov.path, O_RDONLY | O_CLOEXEC);
		g_vid_audio.rate = g_qt_mov.audio_rate ? g_qt_mov.audio_rate : 48000;
		g_vid_audio.chunk_count = g_qt_mov.audio_chunk_count;
		g_vid_audio.total_frames = g_qt_mov.audio_total_frames;
		memcpy(g_vid_audio.chunks, g_qt_mov.audio_chunks,
		       (size_t)g_vid_audio.chunk_count * sizeof(struct qt_mov_audio_chunk));
	} else {
		g_vid_audio.aud_fd = -1;
		g_vid_audio.rate = 48000;
		g_vid_audio.chunk_count = 0;
		g_vid_audio.total_frames = 48000 * 15;
	}
	g_vid_audio.pcm_pos = 0;
	g_vid_audio.cur_chunk = 0;
	g_vid_audio.primed_frames = 0;
	g_vid_audio.synth_phase = 0;
	g_vid_audio.paused = false;
	g_vid_audio.running = true;
	pthread_mutex_unlock(&g_vid_audio.lock);

#ifdef __linux__
	if (pthread_create(&g_vid_audio.thread, NULL, alsa_video_audio_worker, NULL) != 0) {
		g_vid_audio.running = false;
		if (g_vid_audio.aud_fd >= 0) {
			close(g_vid_audio.aud_fd);
			g_vid_audio.aud_fd = -1;
		}
	}
#endif
}

void shell_media_audio_set_paused(bool paused)
{
	pthread_mutex_lock(&g_vid_audio.lock);
	g_vid_audio.paused = paused;
	pthread_mutex_unlock(&g_vid_audio.lock);
}

void shell_media_audio_seek_sec(int elapsed_sec)
{
	if (elapsed_sec < 0)
		elapsed_sec = 0;
	pthread_mutex_lock(&g_vid_audio.lock);
	if (g_vid_audio.total_frames > 0 && g_vid_audio.rate > 0) {
		g_vid_audio.pcm_pos = (uint32_t)(((uint64_t)elapsed_sec * g_vid_audio.rate) %
						 g_vid_audio.total_frames);
		g_vid_audio.cur_chunk = 0;
		g_vid_audio.primed_frames = 0;
	}
	pthread_mutex_unlock(&g_vid_audio.lock);
}

int shell_media_audio_get_frame_idx(int fps, int total_frames)
{
	int fidx = -1;

	if (!g_vid_audio.running || !g_vid_audio.hw_active ||
	    fps <= 0 || total_frames <= 0)
		return -1;

	pthread_mutex_lock(&g_vid_audio.lock);
	if (g_vid_audio.total_frames > 0 && g_vid_audio.rate > 0) {
		uint32_t tot = g_vid_audio.total_frames;
		uint32_t lat = g_vid_audio.primed_frames < 3072 ?
			       g_vid_audio.primed_frames : 3072;
		uint32_t audible = (g_vid_audio.pcm_pos + tot - (lat % tot)) % tot;
		fidx = (int)((((uint64_t)audible * (uint64_t)fps) /
			      (uint64_t)g_vid_audio.rate) % (uint64_t)total_frames);
	}
	pthread_mutex_unlock(&g_vid_audio.lock);
	return fidx;
}

void shell_media_init(void)
{
	memset(g_art_cache, 0, sizeof(g_art_cache));
	memset(g_uri_cache, 0, sizeof(g_uri_cache));
	memset(g_fetches, 0, sizeof(g_fetches));

	/* Pre-warm only the 4 Home Screen card presets; others load on demand */
	(void)ensure_preset_art_loaded(GIRI_ART_DAWN_RIDGE, false);
	(void)ensure_preset_art_loaded(GIRI_ART_WEATHER_SKY, false);
	(void)ensure_preset_art_loaded(GIRI_ART_MONSOON_VIDEO, false);
	(void)ensure_preset_art_loaded(GIRI_ART_DOOM_GAME, false);
}

void shell_media_release_hw(void)
{
	shell_media_audio_stop();
	qt_mov_close();
#ifdef __linux__
	hw_pipe_close();
#endif
}

void shell_media_free(void)
{
	int i;

	shell_media_audio_stop();
	qt_mov_close();
#ifdef __linux__
	hw_pipe_close();
#endif
	for (i = 0; i < GIRI_ART_COUNT; i++) {
		free_media_ptr(&g_art_cache[i].card_275);
		free_media_ptr(&g_art_cache[i].tile_275x164);
		free_media_ptr(&g_art_cache[i].fs_720p);
		memset(&g_art_cache[i], 0, sizeof(g_art_cache[i]));
	}
	for (i = 0; i < URI_CACHE_SLOTS; i++) {
		free_media_ptr(&g_uri_cache[i].card_275);
		free_media_ptr(&g_uri_cache[i].tile_275x164);
		free_media_ptr(&g_uri_cache[i].fs_720p);
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
		    strncmp(card->uri, "https://", 8) == 0 ||
		    strncmp(card->uri, "commons:", 8) == 0) {
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
	bool want_tile = (h > 0 && h <= 200);

	(void)w;
	if (card) {
		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;

		if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
			struct uri_cache_entry *e = get_uri_cache_entry(resolved,
									!want_tile,
									want_tile,
									false);
			if (e) {
				if (want_tile && e->tile_275x164)
					return e->tile_275x164;
				if (e->card_275)
					return e->card_275;
			}
		}
	}

	return ensure_preset_art_loaded(preset, want_tile);
}

/*
 * Non-blocking card art lookup: returns cached URI artwork immediately if
 * already decoded in g_uri_cache, or falls back to the instant preset surface
 * without stalling the UI thread on synchronous JPEG decoding.
 */
const uint32_t *shell_media_get_card_art_fast(const struct giri_media_card *card,
					      int w, int h, bool *is_final_out)
{
	char resolved[256];
	enum giri_preset_art preset = GIRI_ART_DAWN_RIDGE;
	bool want_tile = (h > 0 && h <= 200);

	(void)w;
	if (is_final_out)
		*is_final_out = true;

	if (card) {
		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;

		if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
			struct uri_cache_entry *e = find_uri_cache_entry(resolved);
			if (e) {
				if (want_tile && e->tile_275x164)
					return e->tile_275x164;
				if (!want_tile && e->card_275)
					return e->card_275;
			}
			if (is_final_out)
				*is_final_out = false;
		}
	}

	(void)ensure_preset_art_loaded(preset, want_tile);
	if (!g_art_cache[preset].card_275 && !want_tile && preset != GIRI_ART_DOOM_GAME)
		g_art_cache[preset].card_275 = rasterize_preset_surface(preset, 275, 312);
	if (want_tile && !g_art_cache[preset].tile_275x164 && preset != GIRI_ART_DOOM_GAME)
		g_art_cache[preset].tile_275x164 = rasterize_preset_surface(preset, 275, 164);
	if (want_tile && g_art_cache[preset].tile_275x164)
		return g_art_cache[preset].tile_275x164;
	return g_art_cache[preset].card_275;
}

/*
 * Incrementally decode and cache at most ONE uncached gallery tile image.
 */
bool shell_media_preload_card_one(const struct giri_media_card *card)
{
	char resolved[256];

	if (!card)
		return false;
	if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
		struct uri_cache_entry *e = find_uri_cache_entry(resolved);

		if (!e || !e->tile_275x164) {
			(void)get_uri_cache_entry(resolved, false, true, false);
			return true;
		}
	}
	return false;
}

bool shell_media_preload_next_card(const struct giri_screen *scr)
{
	int i;

	if (!scr)
		return false;

	for (i = 0; i < SHELL_CARD_COUNT; i++) {
		char resolved[256];
		const struct giri_media_card *card = &scr->media_cards[i];

		if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
			struct uri_cache_entry *e = find_uri_cache_entry(resolved);

			if (!e || !e->card_275) {
				(void)get_uri_cache_entry(resolved, true, false, false);
				return true;
			}
		}
	}
	return false;
}

const uint32_t *shell_media_get_fullscreen_art(const struct giri_media_card *card)
{
	char resolved[256];
	enum giri_preset_art preset = GIRI_ART_DAWN_RIDGE;

	if (card) {
		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;

		if (resolve_card_image_path(card, resolved, sizeof(resolved))) {
			struct uri_cache_entry *e = get_uri_cache_entry(resolved,
									false,
									false,
									true);
			if (e && e->fs_720p)
				return e->fs_720p;
		}
	}

	(void)ensure_preset_art_loaded(preset, false);
	if (!g_art_cache[preset].fs_720p) {
		char ph_path[256];
		const char *ph_name = preset_placeholder_names[preset];

#ifdef __linux__
		if (ph_name && resolve_placeholder_path(ph_name, ph_path, sizeof(ph_path)))
			(void)hw_decode_and_scale_jpeg_file(ph_path, NULL, NULL,
							    &g_art_cache[preset].fs_720p, NULL);
#else
		(void)ph_path;
		(void)ph_name;
#endif
		if (!g_art_cache[preset].fs_720p) {
			g_art_cache[preset].fs_720p = rasterize_preset_surface(preset,
									       SHELL_SCREEN_W,
									       SHELL_SCREEN_H);
		}
	}
	return g_art_cache[preset].fs_720p;
}

void shell_media_get_video_info(const struct giri_media_card *card,
				int *out_fps, int *out_frame_count,
				int *out_duration_sec)
{
	int fps = 24, nframes = 360, dur = 15;

	if (qt_mov_open_for_card(card)) {
		fps = (int)g_qt_mov.fps;
		nframes = (int)g_qt_mov.frame_count;
		dur = (int)g_qt_mov.duration_sec;
	}
	if (out_fps)
		*out_fps = fps;
	if (out_frame_count)
		*out_frame_count = nframes;
	if (out_duration_sec)
		*out_duration_sec = dur;
}

#ifdef __linux__
static void compute_pan_zoom_crop(int src_w, int src_h, int frame_idx,
				  struct v4l2_rect *anim_r)
{
	struct v4l2_rect base_r;
	int step, phase, dw, dh;

	compute_cover_crop_rect(src_w, src_h, SHELL_SCREEN_W, SHELL_SCREEN_H, &base_r);

	/* Smooth 24 FPS triangle wave 0..180..0 across the 360-frame (15s) cycle */
	step = frame_idx % 360;
	if (step < 0)
		step += 360;
	phase = (step <= 180) ? step : (360 - step);

	dw = ((base_r.width * 6) / 100) & ~1;
	dh = ((base_r.height * 6) / 100) & ~1;
	anim_r->width = (base_r.width - (dw * phase) / 180) & ~1;
	anim_r->height = (base_r.height - (dh * phase) / 180) & ~1;
	if (anim_r->width < 64 || anim_r->height < 64) {
		*anim_r = base_r;
		return;
	}
	anim_r->left = (base_r.left + ((base_r.width - anim_r->width) * phase) / 360) & ~1;
	anim_r->top = (base_r.top + ((base_r.height - anim_r->height) / 2)) & ~1;
}

static bool hw_decode_jpeg_file_to_nv12(const char *path, int *out_w, int *out_h)
{
	size_t bslen;
	int src_w = 0, src_h = 0;

	if (!path || !path[0] || !hw_pipe_init())
		return false;

	if (g_hw_pipe.last_jpg_path[0] &&
	    strcmp(g_hw_pipe.last_jpg_path, path) == 0 &&
	    g_hw_pipe.last_src_w >= 16 && g_hw_pipe.last_src_h >= 16) {
		*out_w = g_hw_pipe.last_src_w;
		*out_h = g_hw_pipe.last_src_h;
		return true;
	}

	bslen = read_jpeg_or_mov_frame0(path, g_hw_pipe.jpg_out_ptr, g_hw_pipe.jpg_out_len);
	if (bslen <= 16 || !hw_jpeg_run_bitstream(bslen, &src_w, &src_h)) {
		g_hw_pipe.last_jpg_path[0] = '\0';
		return false;
	}
	snprintf(g_hw_pipe.last_jpg_path, sizeof(g_hw_pipe.last_jpg_path), "%s", path);
	g_hw_pipe.last_src_w = src_w;
	g_hw_pipe.last_src_h = src_h;
	*out_w = src_w;
	*out_h = src_h;
	return true;
}

static bool hw_decode_mov_frame_to_nv12(int frame_idx, int *out_w, int *out_h)
{
	uint32_t fidx;
	struct qt_mov_sample ent;
	char tag[544];
	ssize_t n;
	int src_w = 0, src_h = 0;

	if (g_qt_mov.fd < 0 || g_qt_mov.frame_count == 0 || !hw_pipe_init())
		return false;

	fidx = (uint32_t)(((frame_idx % (int)g_qt_mov.frame_count) + (int)g_qt_mov.frame_count) %
			  (int)g_qt_mov.frame_count);
	ent = g_qt_mov.samples[fidx];
	if (ent.size <= 16 || ent.size > g_hw_pipe.jpg_out_len)
		return false;

	snprintf(tag, sizeof(tag), "%.490s#%u", g_qt_mov.path, fidx);
	if (strcmp(g_hw_pipe.last_jpg_path, tag) == 0 &&
	    g_hw_pipe.last_src_w >= 16 && g_hw_pipe.last_src_h >= 16) {
		*out_w = g_hw_pipe.last_src_w;
		*out_h = g_hw_pipe.last_src_h;
		return true;
	}

	n = pread(g_qt_mov.fd, g_hw_pipe.jpg_out_ptr, ent.size, (off_t)ent.offset);
	if (n != (ssize_t)ent.size || !hw_jpeg_run_bitstream(ent.size, &src_w, &src_h)) {
		g_hw_pipe.last_jpg_path[0] = '\0';
		return false;
	}
	snprintf(g_hw_pipe.last_jpg_path, sizeof(g_hw_pipe.last_jpg_path), "%.510s", tag);
	g_hw_pipe.last_src_w = src_w;
	g_hw_pipe.last_src_h = src_h;
	*out_w = src_w;
	*out_h = src_h;
	return true;
}
#endif

/*
 * Zero-copy hardware video frame renderer:
 *   1. Decodes the target frame on s5l8947x-jpeg (/dev/video2) into NV12 DMABUF
 *   2. Crops, scales, and colour-converts on s5l8947x-m2sc (/dev/video1)
 *      directly into the back DRM scanout DMABUF (@dst_dmabuf_fd)
 *   3. Zero CPU reads from uncached memory and zero full-screen CPU copies!
 */
bool shell_media_render_video_frame_dmabuf(const struct giri_media_card *card,
					   int frame_idx, int buf_idx,
					   int dst_dmabuf_fd)
{
#ifdef __linux__
	struct v4l2_rect crop;
	int src_w = 0, src_h = 0;

	if (!card || buf_idx < 0 || buf_idx > 2 || dst_dmabuf_fd < 0 ||
	    !hw_pipe_init() || g_hw_pipe.sc_vid.fd < 0)
		return false;

	/* 1. Continuous 24 FPS QuickTime .mov container */
	if (qt_mov_open_for_card(card) &&
	    hw_decode_mov_frame_to_nv12(frame_idx, &src_w, &src_h)) {
		compute_cover_crop_rect(src_w, src_h, SHELL_SCREEN_W, SHELL_SCREEN_H, &crop);
		return hw_m2sc_dmabuf_run(&g_hw_pipe.sc_vid, &crop, buf_idx, dst_dmabuf_fd);
	}

	/* 2. Hardware M2SC pan/zoom video synthesizer for still-backed video cards */
	{
		char resolved[256] = "";
		enum giri_preset_art preset = GIRI_ART_DAWN_RIDGE;

		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;
		if (!resolve_card_image_path(card, resolved, sizeof(resolved)) &&
		    preset_placeholder_names[preset]) {
			(void)resolve_placeholder_path(preset_placeholder_names[preset],
						       resolved, sizeof(resolved));
		}
		if (resolved[0] && hw_decode_jpeg_file_to_nv12(resolved, &src_w, &src_h)) {
			compute_pan_zoom_crop(src_w, src_h, frame_idx, &crop);
			return hw_m2sc_dmabuf_run(&g_hw_pipe.sc_vid, &crop, buf_idx, dst_dmabuf_fd);
		}
	}
#else
	(void)card;
	(void)frame_idx;
	(void)buf_idx;
	(void)dst_dmabuf_fd;
#endif
	return false;
}

const uint32_t *shell_media_get_video_frame(const struct giri_media_card *card,
					    int frame_idx)
{
	if (!card)
		return NULL;

#ifdef __linux__
	/* Fast hardware V4L2 path (used by GPU mode, --bench, and non-DRM fallback) */
	if (hw_pipe_init()) {
		struct v4l2_rect crop;
		int src_w = 0, src_h = 0;

		if (qt_mov_open_for_card(card) &&
		    hw_decode_mov_frame_to_nv12(frame_idx, &src_w, &src_h)) {
			compute_cover_crop_rect(src_w, src_h, SHELL_SCREEN_W, SHELL_SCREEN_H, &crop);
			if (shell_gpu_active() && g_hw_pipe.sc_vid.streaming) {
				uint32_t *fs_ptr = NULL;
				int fs_fd = shell_gpu_fs_atlas_dmabuf_fd(0, &fs_ptr, NULL);

				if (fs_fd >= 0 && fs_ptr &&
				    hw_m2sc_dmabuf_run(&g_hw_pipe.sc_vid, &crop, 0, fs_fd)) {
					if (g_atlas_fs_slots[0].owner_ptr) {
						*g_atlas_fs_slots[0].owner_ptr = NULL;
						g_atlas_fs_slots[0].owner_ptr = NULL;
					}
					return fs_ptr;
				}
			}
			if (hw_m2sc_queue_job(&g_hw_pipe.sc_fs, &crop) &&
			    hw_m2sc_wait_job(&g_hw_pipe.sc_fs, NULL))
				return (const uint32_t *)g_hw_pipe.sc_fs.cap_ptr;
		}
	}
#endif

	if (!g_vid_frame_buf) {
		g_vid_frame_buf = malloc((size_t)SHELL_SCREEN_W * SHELL_SCREEN_H *
					 sizeof(uint32_t));
	}

	/* Software fallback for QuickTime .mov on host */
	if (qt_mov_open_for_card(card)) {
		uint32_t fidx = (uint32_t)(((frame_idx % (int)g_qt_mov.frame_count) +
					    (int)g_qt_mov.frame_count) % (int)g_qt_mov.frame_count);
		struct qt_mov_sample ent = g_qt_mov.samples[fidx];
		uint8_t *jbuf = malloc(ent.size);

		if (jbuf) {
			if (pread(g_qt_mov.fd, jbuf, ent.size, (off_t)ent.offset) == (ssize_t)ent.size) {
				int rw = 0, rh = 0;
				uint32_t *raw = sw_jpeg_decode_raw(jbuf, ent.size, &rw, &rh);
				free(jbuf);
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
					return g_vid_frame_buf;
				}
			} else {
				free(jbuf);
			}
		}
	}

#ifdef __linux__
	{
		char resolved[256] = "";
		enum giri_preset_art preset = GIRI_ART_DAWN_RIDGE;
		int src_w = 0, src_h = 0;
		struct v4l2_rect anim_r;

		if (card->preset_art >= 0 && card->preset_art < GIRI_ART_COUNT)
			preset = card->preset_art;
		if (!resolve_card_image_path(card, resolved, sizeof(resolved)) &&
		    preset_placeholder_names[preset]) {
			(void)resolve_placeholder_path(preset_placeholder_names[preset],
						       resolved, sizeof(resolved));
		}
		if (resolved[0] && hw_decode_jpeg_file_to_nv12(resolved, &src_w, &src_h)) {
			compute_pan_zoom_crop(src_w, src_h, frame_idx, &anim_r);
			if (shell_gpu_active() && g_hw_pipe.sc_vid.streaming) {
				uint32_t *fs_ptr = NULL;
				int fs_fd = shell_gpu_fs_atlas_dmabuf_fd(0, &fs_ptr, NULL);

				if (fs_fd >= 0 && fs_ptr &&
				    hw_m2sc_dmabuf_run(&g_hw_pipe.sc_vid, &anim_r, 0, fs_fd)) {
					if (g_atlas_fs_slots[0].owner_ptr) {
						*g_atlas_fs_slots[0].owner_ptr = NULL;
						g_atlas_fs_slots[0].owner_ptr = NULL;
					}
					return fs_ptr;
				}
			}
			if (hw_m2sc_queue_job(&g_hw_pipe.sc_fs, &anim_r) &&
			    hw_m2sc_wait_job(&g_hw_pipe.sc_fs, NULL))
				return (const uint32_t *)g_hw_pipe.sc_fs.cap_ptr;
		}
	}
#endif

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
	{ "GX016305.mov",   1, "Lagoon Speedboat Clip",   "Turquoise Reef · 720p Video",
	  "Widescreen 720p video cruising across crystal-clear turquoise lagoon waters aboard a coastal boat." },
	{ "IMG_8624.mov",   2, "Castle Courtyard Band",   "Brass Ensemble · 720p Video",
	  "Live brass band concert performing in a historic stone castle courtyard, captured in 1280x720 HD video." },
	{ "IMG_8734.mov",   3, "Tower Bridge by Night",   "Thames Bascule Lift · 720p Video",
	  "Nighttime 720p video of London's illuminated Tower Bridge with its twin bascules raised over the River Thames." },
	{ "IMG_6452.jpeg",  4, "Seventh Hill Ascent",     "Velliangiri Summit Trail · 1,778m",
	  "Pilgrims and trekkers ascending the granite boulder trail of the seventh hill in the Velliangiri Mountains." },
	{ "IMG_6455.jpeg",  5, "Shola Cloud Canopy",      "Velliangiri High Ridge · Mist",
	  "Sweeping view of montane shola forests and valley clouds drifting across the Velliangiri mountain range." },
	{ "IMG_6397.jpeg",  6, "Adiyogi Foothills",       "Poondi Valley · Western Ghats",
	  "Portrait at the base of the Velliangiri foothills with the 112-foot Adiyogi statue and mist-clad ridges behind." },
	{ "IMG_6402.jpeg",  7, "Poondi Trek Start",       "Bamboo Walking Sticks · Trailhead",
	  "Trekking group gathered at the Poondi temple trailhead with traditional bamboo staffs before climbing the seven hills." },
	{ "IMG_6418.jpeg",  8, "Above the Cloud Bank",    "High Meadow · Velliangiri Hills",
	  "Golden alpine grasslands and white orographic clouds below the upper ridges of Velliangiri." },
	{ "IMG_9481.jpeg",  9, "Natural Coral Arch",      "Andaman Coastline · Limestone Bridge",
	  "Sunlight framing a natural coastal rock arch and tide pools against a deep blue sky." },
	{ "_MG_9451.jpeg", 10, "Flyboard Water Jet",      "Coastal Lagoon · Action Photo",
	  "High-speed telephoto shot of hydroflight flyboarding above tropical green coastline waters." },
	{ "DSC04988.jpeg", 11, "Radhanagar Beach",        "Family Portrait · White Sand Shore",
	  "Family photograph along the pristine white sand shoreline and emerald hills." },
	{ "GOPR6262.jpeg", 12, "Reef Snorkeling Dive",    "Clear Lagoon Waters · Action Cam",
	  "Wetsuit snorkeling session in calm shallow turquoise reef waters." },
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
		"/mnt/data/linux/media/photos",
		"/mnt/data/linux/media/videos",
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
