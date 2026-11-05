/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * shell_gpu.c - Direct-UAPI PowerVR SGX543MP1 TBDR 2D UI Compositor
 *
 * Drives the SGX543 TA/3D hardware directly through /dev/dri/renderD128
 * using the clean-room template frame (sgx_template.c / sgx_rt.c) and custom
 * 3-instruction USSE pixel shaders:
 *   - PROG_UI_BLEND:        o0.rgb = pa0.rgb * pa0.a + o0.rgb * (1 - pa0.a)
 *   - PROG_MASK_ALPHA:      o0.rgb = o0.rgb, o0.a = pa0.a
 *   - PROG_CARD_DST_ALPHA:  o0.rgb = pa0.rgb * o0.a + o0.rgb * (1 - o0.a)
 */

#include <drm/drm.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "shell_gpu.h"
#include "../../include/uapi/drm/apple_sgx_drm.h"
#include "sgx_rt.h"
#include "sgx_template.h"

/* Template frame layout inside glb, code, vdm, and ext BOs */
#define BUILT_PDS		0x0000u
#define BUILT_PIX_PDS		0x0240u		/* 6 * 0x40-byte pixel PDS blocks */
#define BUILT_STATE		0x0400u
#define BUILT_WHITE		0x0c00u
#define BUILT_IDX		0x1000u

#define MAX_QUADS		8192u
#define MAX_INDICES		(MAX_QUADS * 6u)
#define MAX_VERTS		(MAX_QUADS * 4u)
#define MAX_DRAWS		256u

#define GL_SIZE			(BUILT_IDX + MAX_INDICES * sizeof(uint16_t))
#define PROG_SLOT		0x80u
#define PROGS_SIZE		0x200u
#define CODE_SIZE		0x4000u
#define VDM_SIZE		0x4000u

/* Offsets inside the 1.5 MiB ext BO */
#define EXT_VB			0x000000u	/* 8192 * 4 * 32 = 1 MiB */
#define EXT_STATE_FULL		0x100000u	/* 6 * 0x80 bytes */
#define EXT_STATE_PROG		0x100400u	/* 6 * 0x40 bytes */
#define EXT_FETCH		0x101000u	/* 256 * 0x80 bytes */
#define EXT_SIZE		0x120000u

#define STATE_WORDS		21u
#define PACK_VTX_FLOATS		8u		/* r, g, b, a, u, v, ndc_x, ndc_y */

/* Atlas dimensions (power-of-two for Atlas 0 & Atlas 1 for exact UV float representation) */
#define ATLAS0_W		1024
#define ATLAS0_H		1024
#define ATLAS1_W		2048		/* 5 cols * 280 + 280 staging col (POT) */
#define ATLAS1_H		1024		/* 2*312 cards + 2*164 tiles (POT) */
#define ATLAS_FS_W		1280
#define ATLAS_FS_H		720
#define MAX_RADIUS		32
#define MAX_STROKE_THICK	3
#define CARD_SCRIM_W		275
#define CARD_SCRIM_H		148
#define CARD_SCRIM_R		14
#define TOP_SCRIM_W		320
#define TOP_SCRIM_H		96
#define BOT_SCRIM_W		320
#define BOT_SCRIM_H		288

/* USSE instruction encodings (verified against sgx_usse.c & sgx_compiler.c) */
#define USSE_PHAS		0xfa44070000000000ull
#define USSE_EMIT_PIXEL		0xfb24000003200082ull
#define USSE_SOP2M_MOD		0x90807982a0000140ull	/* pa0 = texel * vtx_rgba */

/*
 * USSE_SOP2 | USSE_END (dest = o0, src1 = pa0, src2 = o0):
 *   - BLEND_UI:       o0.rgb = pa0.rgb * pa0.a + o0.rgb * (1 - pa0.a), o0.a = pa0.a
 *   - MASK_ALPHA:     o0.rgb = pa0.rgb * 0     + o0.rgb * 1,           o0.a = pa0.a
 *   - CARD_DST_ALPHA: o0.rgb = pa0.rgb * o0.a  + o0.rgb * (1 - o0.a),  o0.a = pa0.a
 */
#define USSE_SOP2_BLEND_UI	0x808488dd90000000ull
#define USSE_SOP2_MASK_ALPHA	0x8084880190000000ull
#define USSE_SOP2_CARD_DSTA	0x8084892190000000ull

static const uint64_t usse_dummy_load[3] = {
	0x488b0281a00c0000ull,
	0xe9a30084a0000000ull,
	0xf920000000000000ull,
};

enum gpu_state_id {
	GPU_STATE_UI = 0,		/* Atlas 0, PROG_UI_BLEND, nearest */
	GPU_STATE_MASK = 1,		/* Atlas 0, PROG_MASK_ALPHA, nearest */
	GPU_STATE_CARD_MASKED = 2,	/* Atlas 1, PROG_CARD_DST_ALPHA, bilinear */
	GPU_STATE_CARD_OPAQUE = 3,	/* Atlas 1, PROG_UI_BLEND, bilinear */
	GPU_STATE_FS0 = 4,		/* Atlas 2, PROG_UI_BLEND, nearest */
	GPU_STATE_FS1 = 5,		/* Atlas 3, PROG_UI_BLEND, nearest */
	GPU_STATE_COUNT = 6
};

struct sgx_bo {
	uint32_t handle;
	uint32_t va;
	size_t size;
	int dmabuf_fd;
	void *map;
};

struct gpu_draw_pkt {
	enum gpu_state_id state;
	uint32_t first_quad;
	uint32_t nquads;
};

static struct {
	bool active;
	int sgxfd;
	int width;
	int height;
	uint32_t code_base;
	uint32_t pb_va;

	/* Core SGX template & heap BOs */
	struct sgx_bo tprog;
	struct sgx_bo code;
	struct sgx_bo glb;
	struct sgx_bo vdm;
	struct sgx_bo ext;
	struct sgx_bo rtb;

	/* Texture Atlases */
	struct sgx_bo atlas0;		/* 1024x1024 UI primitives & FreeType glyphs */
	struct sgx_bo atlas1;		/* 2048x1024 M2SC0 photo cards, tiles & cached icons */
	struct sgx_bo atlas_fs[2];	/* 1280x720 M2SC1 fullscreen photos */

	/* Imported (or headless) scanout render targets */
	int num_targets;
	bool headless_targets;
	struct sgx_bo targets[3];

	/* Template & RT structures */
	struct sgx_tmpl tmpl;
	struct sgx_rt rt;
	uint32_t rt_va[SGX_RT_NBUF];
	uint32_t blk_va;
	uint32_t cmd[0x200 / 4];
	uint32_t tail[5];
	uint32_t base_fetch[32];
	uint32_t state_prog_va[GPU_STATE_COUNT];

	/* Pre-baked Atlas 0 coordinates */
	uint16_t fill_corner_u[MAX_RADIUS + 1];
	uint16_t fill_corner_v[MAX_RADIUS + 1];
	uint16_t outl_corner_u[MAX_STROKE_THICK + 1][MAX_RADIUS + 1];
	uint16_t outl_corner_v[MAX_STROKE_THICK + 1][MAX_RADIUS + 1];
	uint16_t card_scrim_u;
	uint16_t card_scrim_v;
	uint16_t top_scrim_u;
	uint16_t top_scrim_v;
	uint16_t bot_scrim_u;
	uint16_t bot_scrim_v;

	/* Atlas 0 shelf allocator state */
	uint16_t shelf_x;
	uint16_t shelf_y;
	uint16_t shelf_h;

	/* Persistent Atlas 1 static bitmap cache (column x=1682) */
	struct {
		const uint32_t *pixels;
		int w;
		int h;
		int stride;
		int sy;
	} static_bmps[16];
	int static_bmp_count;
	int static_bmp_y;

	/* Per-frame geometry & command batching */
	struct gpu_draw_pkt draws[MAX_DRAWS];
	uint32_t ndraws;
	uint32_t nquads;
	int staging_y;
	bool frame_cleared;
} g_gpu = {
	.sgxfd = -1,
};

static uint64_t usse_limm_r(unsigned int reg, uint32_t imm)
{
	return 0xfca0000000000000ull | (uint64_t)(reg & 0x7f) << 21 |
	       (uint64_t)((imm >> 26) & 0x3f) << 44 |
	       (uint64_t)((imm >> 21) & 0x1f) << 36 | (imm & 0x1fffff);
}

static uint32_t doutu(const struct sgx_tmpl *t, uint32_t va)
{
	return ((va - t->code_base) / 8) << 4 | 3;
}

static uint32_t vdm4(uint32_t tag, uint32_t va)
{
	return tag << 28 | ((va >> 4) & 0x0fffffff);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int get_param(int fd, uint32_t param, uint32_t *out_val)
{
	struct drm_apple_sgx_get_param p = { .param = param };

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GET_PARAM, &p) < 0)
		return -1;
	*out_val = (uint32_t)p.value;
	return 0;
}

static int bo_alloc(int fd, size_t size, uint32_t flags, struct sgx_bo *b)
{
	struct drm_apple_sgx_gem_create gc = { .size = size, .flags = flags };
	struct drm_apple_sgx_gem_mmap_offset gm = { 0 };

	memset(b, 0, sizeof(*b));
	b->dmabuf_fd = -1;

	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &gc) < 0)
		return -1;

	gm.handle = gc.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &gm) < 0) {
		struct drm_gem_close cl = { .handle = gc.handle };
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl);
		return -1;
	}

	b->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, gm.offset);
	if (b->map == MAP_FAILED) {
		struct drm_gem_close cl = { .handle = gc.handle };
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl);
		b->map = NULL;
		return -1;
	}

	b->handle = gc.handle;
	b->va = gc.va;
	b->size = size;
	memset(b->map, 0, size);
	return 0;
}

static int bo_export_dmabuf(int fd, struct sgx_bo *b)
{
	struct drm_prime_handle prime = {
		.handle = b->handle,
		.flags = DRM_CLOEXEC | DRM_RDWR,
	};

	if (ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0)
		return -1;
	b->dmabuf_fd = prime.fd;
	return 0;
}

static int bo_import_dmabuf(int fd, int dmabuf_fd, size_t size, struct sgx_bo *b)
{
	struct drm_prime_handle prime = { .fd = dmabuf_fd };
	struct drm_apple_sgx_gem_info info = { 0 };

	memset(b, 0, sizeof(*b));
	b->dmabuf_fd = -1;

	if (ioctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) < 0)
		return -1;

	info.handle = prime.handle;
	if (ioctl(fd, DRM_IOCTL_APPLE_SGX_GEM_INFO, &info) < 0) {
		struct drm_gem_close cl = { .handle = prime.handle };
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl);
		return -1;
	}

	b->handle = prime.handle;
	b->va = info.va;
	b->size = size;
	return 0;
}

static void bo_free(int fd, struct sgx_bo *b)
{
	if (b->dmabuf_fd >= 0) {
		close(b->dmabuf_fd);
		b->dmabuf_fd = -1;
	}
	if (b->map && b->map != MAP_FAILED) {
		munmap(b->map, b->size);
		b->map = NULL;
	}
	if (b->handle && fd >= 0) {
		struct drm_gem_close cl = { .handle = b->handle };
		ioctl(fd, DRM_IOCTL_GEM_CLOSE, &cl);
		b->handle = 0;
	}
}

static void state_size(uint32_t *full, unsigned int w, unsigned int h)
{
	float hw = w / 2.0f, hh = h / 2.0f;

	full[7] = (full[7] & ~0xfffu) | ((w + 31) / 32 - 1);
	full[8] = (full[8] & ~0xfffu) | ((h + 31) / 32 - 1);
	memcpy(&full[9], &hw, 4);
	memcpy(&full[10], &hw, 4);
	memcpy(&full[11], &hh, 4);
	memcpy(&full[12], &hh, 4);
}

/* Allocate a rectangle in Atlas 0 with 1px padding to prevent texel bleed */
static bool atlas0_alloc(uint16_t w, uint16_t h, uint16_t *out_u, uint16_t *out_v)
{
	uint16_t pw = w + 2;
	uint16_t ph = h + 2;

	if (pw > ATLAS0_W || ph > ATLAS0_H)
		return false;

	if (g_gpu.shelf_x + pw > ATLAS0_W) {
		g_gpu.shelf_y += g_gpu.shelf_h;
		g_gpu.shelf_x = 0;
		g_gpu.shelf_h = 0;
	}
	if (g_gpu.shelf_y + ph > ATLAS0_H)
		return false;

	*out_u = g_gpu.shelf_x + 1;
	*out_v = g_gpu.shelf_y + 1;
	g_gpu.shelf_x += pw;
	if (ph > g_gpu.shelf_h)
		g_gpu.shelf_h = ph;
	return true;
}

static const int8_t gpu_bayer4x4[16] = {
	-7,  1, -5,  3,
	 5, -3,  7, -1,
	-4,  4, -6,  2,
	 8,  0,  6, -2
};

/*
 * Pre-bake solid patch, 1:1 2x2-supersampled SDF filled corners (r=1..32),
 * 1:1 2x2-supersampled SDF outline corners (thickness=1..3, r=1..32), and
 * 1:1 4x4 Bayer-dithered scrim gradients reusing shell_display.c's shared
 * subpixel coverage LUTs.
 */
static void init_atlas0_primitives(void)
{
	uint32_t *px = (uint32_t *)g_gpu.atlas0.map;
	int r, t, lx, ly;

	memset(px, 0, g_gpu.atlas0.size);

	/* Reserve (0, 0, 10, 10) for the 8x8 solid white patch at (1, 1 .. 8, 8) */
	g_gpu.shelf_x = 10;
	g_gpu.shelf_y = 0;
	g_gpu.shelf_h = 10;
	for (ly = 0; ly < 10; ly++) {
		for (lx = 0; lx < 10; lx++)
			px[ly * ATLAS0_W + lx] = 0xFFFFFFFFu;
	}

	/* 1:1 filled rounded-rect corners for r = 1..32 */
	for (r = 1; r <= MAX_RADIUS; r++) {
		uint16_t u0 = 0, v0 = 0;

		if (!atlas0_alloc((uint16_t)(2 * r), (uint16_t)(2 * r), &u0, &v0))
			break;
		g_gpu.fill_corner_u[r] = u0;
		g_gpu.fill_corner_v[r] = v0;

		for (ly = 0; ly < 2 * r; ly++) {
			int cy = (ly < r) ? ly : (2 * r - 1 - ly);

			for (lx = 0; lx < 2 * r; lx++) {
				int cx = (lx < r) ? lx : (2 * r - 1 - lx);
				uint8_t a = shell_sdf_corner_cov(r, cy, cx);

				px[(v0 + ly) * ATLAS0_W + (u0 + lx)] =
					((uint32_t)a << 24) | 0x00FFFFFFu;
			}
		}
	}

	/* 1:1 anti-aliased outline corners for thickness t = 1..3 and r = 1..32 */
	for (t = 1; t <= MAX_STROKE_THICK; t++) {
		for (r = 1; r <= MAX_RADIUS; r++) {
			uint16_t u0 = 0, v0 = 0;

			if (!atlas0_alloc((uint16_t)(2 * r), (uint16_t)(2 * r), &u0, &v0))
				break;
			g_gpu.outl_corner_u[t][r] = u0;
			g_gpu.outl_corner_v[t][r] = v0;

			for (ly = 0; ly < 2 * r; ly++) {
				int cy = (ly < r) ? ly : (2 * r - 1 - ly);

				for (lx = 0; lx < 2 * r; lx++) {
					int cx = (lx < r) ? lx : (2 * r - 1 - lx);
					uint8_t a = shell_sdf_corner_stroke(r, t, cy, cx);

					px[(v0 + ly) * ATLAS0_W + (u0 + lx)] =
						((uint32_t)a << 24) | 0x00FFFFFFu;
				}
			}
		}
	}

	/* 1:1 4x4 Bayer-dithered card bottom scrim (275x148, r=14) */
	if (atlas0_alloc(CARD_SCRIM_W, CARD_SCRIM_H,
			 &g_gpu.card_scrim_u, &g_gpu.card_scrim_v)) {
		uint16_t u0 = g_gpu.card_scrim_u;
		uint16_t v0 = g_gpu.card_scrim_v;

		for (ly = 0; ly < CARD_SCRIM_H; ly++) {
			float dist = (float)(CARD_SCRIM_H - 1 - ly) /
				     (float)(CARD_SCRIM_H - 1);
			float alpha_f;

			if (dist <= 0.58f) {
				float u = dist / 0.58f;
				alpha_f = 0.96f + (0.84f - 0.96f) * u;
			} else if (dist <= 0.84f) {
				float u = (dist - 0.58f) / (0.84f - 0.58f);
				alpha_f = 0.84f + (0.32f - 0.84f) * u;
			} else {
				float u = (dist - 0.84f) / (1.0f - 0.84f);
				alpha_f = 0.32f * (1.0f - u);
			}

			for (lx = 0; lx < CARD_SCRIM_W; lx++) {
				float cov = 1.0f;
				float dither = (float)gpu_bayer4x4[(ly & 3) * 4 + (lx & 3)] *
					       (1.5f / 16.0f);
				int a;

				if (ly >= CARD_SCRIM_H - CARD_SCRIM_R &&
				    (lx < CARD_SCRIM_R || lx >= CARD_SCRIM_W - CARD_SCRIM_R)) {
					int cy = CARD_SCRIM_H - 1 - ly;
					int cx = (lx < CARD_SCRIM_R) ? lx : (CARD_SCRIM_W - 1 - lx);
					uint32_t c_tex = px[(g_gpu.fill_corner_v[CARD_SCRIM_R] + cy) *
							    ATLAS0_W +
							    (g_gpu.fill_corner_u[CARD_SCRIM_R] + cx)];
					cov = (float)(c_tex >> 24) * (1.0f / 255.0f);
				}

				if (alpha_f <= 0.0f || cov <= 0.0f) {
					a = 0;
				} else {
					a = (int)(alpha_f * cov * 255.0f + dither + 0.5f);
					if (a < 0)
						a = 0;
					else if (a > 255)
						a = 255;
				}
				px[(v0 + ly) * ATLAS0_W + (u0 + lx)] =
					((uint32_t)a << 24) | 0x00FFFFFFu;
			}
		}
	}

	/* 1:1 4x4 Bayer-dithered fullscreen top scrim (320x96 tile) */
	if (atlas0_alloc(TOP_SCRIM_W, TOP_SCRIM_H,
			 &g_gpu.top_scrim_u, &g_gpu.top_scrim_v)) {
		uint16_t u0 = g_gpu.top_scrim_u;
		uint16_t v0 = g_gpu.top_scrim_v;

		for (ly = 0; ly < TOP_SCRIM_H; ly++) {
			float u = (float)ly / (float)(TOP_SCRIM_H - 1);
			float alpha_f = (u <= 0.65f) ?
				(0.88f + (0.45f - 0.88f) * (u / 0.65f)) :
				(0.45f * (1.0f - (u - 0.65f) / 0.35f));

			for (lx = 0; lx < TOP_SCRIM_W; lx++) {
				float dither = (float)gpu_bayer4x4[(ly & 3) * 4 + (lx & 3)] *
					       (1.5f / 16.0f);
				int a = (alpha_f <= 0.0f) ? 0 :
					(int)(alpha_f * 255.0f + dither + 0.5f);

				if (a < 0)
					a = 0;
				else if (a > 255)
					a = 255;
				px[(v0 + ly) * ATLAS0_W + (u0 + lx)] =
					((uint32_t)a << 24) | 0x00FFFFFFu;
			}
		}
	}

	/* 1:1 4x4 Bayer-dithered fullscreen bottom scrim (320x288 tile) */
	if (atlas0_alloc(BOT_SCRIM_W, BOT_SCRIM_H,
			 &g_gpu.bot_scrim_u, &g_gpu.bot_scrim_v)) {
		uint16_t u0 = g_gpu.bot_scrim_u;
		uint16_t v0 = g_gpu.bot_scrim_v;

		for (ly = 0; ly < BOT_SCRIM_H; ly++) {
			float dist = (float)(BOT_SCRIM_H - 1 - ly) / (float)(BOT_SCRIM_H - 1);
			float alpha_f;

			if (dist <= 0.55f)
				alpha_f = 0.96f + (0.82f - 0.96f) * (dist / 0.55f);
			else if (dist <= 0.82f)
				alpha_f = 0.82f + (0.35f - 0.82f) * ((dist - 0.55f) / 0.27f);
			else
				alpha_f = 0.35f * (1.0f - (dist - 0.82f) / 0.18f);

			for (lx = 0; lx < BOT_SCRIM_W; lx++) {
				float dither = (float)gpu_bayer4x4[(ly & 3) * 4 + (lx & 3)] *
					       (1.5f / 16.0f);
				int a = (alpha_f <= 0.0f) ? 0 :
					(int)(alpha_f * 255.0f + dither + 0.5f);

				if (a < 0)
					a = 0;
				else if (a > 255)
					a = 255;
				px[(v0 + ly) * ATLAS0_W + (u0 + lx)] =
					((uint32_t)a << 24) | 0x00FFFFFFu;
			}
		}
	}
}

static void write_eot_program(unsigned int slot, uint32_t dst_va)
{
	uint64_t eot[11];
	uint32_t w = (uint32_t)g_gpu.width;
	uint32_t h = (uint32_t)g_gpu.height;

	eot[0] = USSE_PHAS;
	memcpy(&eot[1], usse_dummy_load, sizeof(usse_dummy_load));
	eot[4] = usse_limm_r(0, 0x00110000);		/* linear BGRA8 */
	eot[5] = usse_limm_r(1, dst_va);
	eot[6] = usse_limm_r(2, w / 2 - 1);		/* stride in pixels / 2 - 1 */
	eot[7] = usse_limm_r(3, 0);
	eot[8] = usse_limm_r(4, 0);
	eot[9] = usse_limm_r(5, (h - 1) << 12 | (w - 1));
	eot[10] = USSE_EMIT_PIXEL;

	memcpy((uint8_t *)g_gpu.code.map + PROGS_SIZE + slot * PROG_SLOT,
	       eot, sizeof(eot));
}

bool shell_gpu_init(int drm_fd, int num_scanout_bufs,
		    const int *scanout_dmabuf_fds, int width, int height)
{
	const char *event_path = "/lib/firmware/s5l8947x/gl-event.pds";
	const char *render_node = "/dev/dri/renderD128";
	uint32_t event[SGX_TMPL_EVENT_WORDS];
	uint32_t pds[SGX_TMPL_PDS_SIZE / 4];
	uint32_t state[SGX_TMPL_STATE_SIZE / 4];
	uint32_t blk[SGX_TMPL_BLOCK_SIZE / 4];
	uint32_t uapi_ver = 0, ncores = 1, ukernel_up = 0, rt_size = 0;
	FILE *fp;
	int i;

	if (g_gpu.active)
		return true;

	fp = fopen(event_path, "rb");
	if (!fp)
		return false;
	if (fread(event, 1, sizeof(event), fp) != sizeof(event)) {
		fclose(fp);
		return false;
	}
	fclose(fp);

	g_gpu.sgxfd = open(render_node, O_RDWR | O_CLOEXEC);
	if (g_gpu.sgxfd < 0)
		return false;

	if (get_param(g_gpu.sgxfd, APPLE_SGX_PARAM_UAPI_VERSION, &uapi_ver) < 0 ||
	    uapi_ver < 3 ||
	    get_param(g_gpu.sgxfd, APPLE_SGX_PARAM_UKERNEL_UP, &ukernel_up) < 0 ||
	    !ukernel_up ||
	    get_param(g_gpu.sgxfd, APPLE_SGX_PARAM_CODE_BASE, &g_gpu.code_base) < 0 ||
	    get_param(g_gpu.sgxfd, APPLE_SGX_PARAM_PB_VA, &g_gpu.pb_va) < 0) {
		close(g_gpu.sgxfd);
		g_gpu.sgxfd = -1;
		return false;
	}
	get_param(g_gpu.sgxfd, APPLE_SGX_PARAM_NUM_CORES, &ncores);

	g_gpu.width = width;
	g_gpu.height = height;
	if (!sgx_rt_layout(&g_gpu.rt, (unsigned int)width, (unsigned int)height,
			   ncores ? ncores : 1)) {
		close(g_gpu.sgxfd);
		g_gpu.sgxfd = -1;
		return false;
	}

	/* Allocate core template, shader, vertex, and RT heap BOs */
	for (i = 0; i < SGX_RT_NBUF; i++)
		rt_size += (g_gpu.rt.size[i] + 4095u) & ~4095u;
	rt_size += SGX_TMPL_BLOCK_SIZE;

	if (bo_alloc(g_gpu.sgxfd, 0x1000, APPLE_SGX_BO_USSE_CODE, &g_gpu.tprog) < 0 ||
	    bo_alloc(g_gpu.sgxfd, CODE_SIZE, APPLE_SGX_BO_USSE_CODE, &g_gpu.code) < 0 ||
	    bo_alloc(g_gpu.sgxfd, GL_SIZE, 0, &g_gpu.glb) < 0 ||
	    bo_alloc(g_gpu.sgxfd, VDM_SIZE, 0, &g_gpu.vdm) < 0 ||
	    bo_alloc(g_gpu.sgxfd, EXT_SIZE, 0, &g_gpu.ext) < 0 ||
	    bo_alloc(g_gpu.sgxfd, rt_size, APPLE_SGX_BO_TA_HEAP, &g_gpu.rtb) < 0)
		goto err_cleanup;

	/*
	 * Allocate texture atlases:
	 * - Atlas 0 (1024x1024 UI primitives & glyphs): shmem (0 CMA)
	 * - Atlas 1 (1680x960 card/tile slots): shmem (0 CMA)
	 * - Atlas 2 (1280x720 fullscreen DMA target): contiguous DMABUF for M2SC1
	 * - Atlas 3 (1280x720 fullscreen staging): shmem (0 CMA)
	 */
	if (bo_alloc(g_gpu.sgxfd, (size_t)ATLAS0_W * ATLAS0_H * 4, 0,
		     &g_gpu.atlas0) < 0 ||
	    bo_alloc(g_gpu.sgxfd, (size_t)ATLAS1_W * ATLAS1_H * 4, 0,
		     &g_gpu.atlas1) < 0 ||
	    (bo_alloc(g_gpu.sgxfd, (size_t)ATLAS_FS_W * ATLAS_FS_H * 4,
		      APPLE_SGX_BO_CONTIGUOUS, &g_gpu.atlas_fs[0]) < 0 &&
	     bo_alloc(g_gpu.sgxfd, (size_t)ATLAS_FS_W * ATLAS_FS_H * 4, 0,
		      &g_gpu.atlas_fs[0]) < 0) ||
	    bo_alloc(g_gpu.sgxfd, (size_t)ATLAS_FS_W * ATLAS_FS_H * 4, 0,
		     &g_gpu.atlas_fs[1]) < 0)
		goto err_cleanup;

	/* Export DMABUF FD for zero-copy M2SC1 720p hardware scaling */
	bo_export_dmabuf(g_gpu.sgxfd, &g_gpu.atlas_fs[0]);

	/* Import DRM scanout buffers via PRIME (or allocate headless render targets) */
	if (drm_fd >= 0 && num_scanout_bufs > 0 && scanout_dmabuf_fds) {
		g_gpu.headless_targets = false;
		g_gpu.num_targets = num_scanout_bufs > 3 ? 3 : num_scanout_bufs;
		for (i = 0; i < g_gpu.num_targets; i++) {
			if (bo_import_dmabuf(g_gpu.sgxfd, scanout_dmabuf_fds[i],
					     (size_t)width * height * 4,
					     &g_gpu.targets[i]) < 0)
				goto err_cleanup;
		}
	} else {
		g_gpu.headless_targets = true;
		g_gpu.num_targets = 2;
		if (bo_alloc(g_gpu.sgxfd, (size_t)width * height * 4, 0,
			     &g_gpu.targets[0]) < 0)
			goto err_cleanup;
		if (bo_alloc(g_gpu.sgxfd, (size_t)width * height * 4,
			     APPLE_SGX_BO_CONTIGUOUS, &g_gpu.targets[1]) < 0 &&
		    bo_alloc(g_gpu.sgxfd, (size_t)width * height * 4, 0,
			     &g_gpu.targets[1]) < 0)
			goto err_cleanup;
		bo_export_dmabuf(g_gpu.sgxfd, &g_gpu.targets[1]);
	}

	/* Set up RT buffer addresses */
	{
		uint32_t at = g_gpu.rtb.va;

		for (i = 0; i < SGX_RT_NBUF; i++) {
			g_gpu.rt_va[i] = at;
			at += (g_gpu.rt.size[i] + 4095u) & ~4095u;
		}
		g_gpu.blk_va = at;
	}

	/* Upload template USSE programs and our 3 custom pixel shaders */
	{
		const uint64_t prog_ui[3] = {
			USSE_PHAS, USSE_SOP2M_MOD, USSE_SOP2_BLEND_UI
		};
		const uint64_t prog_mask[3] = {
			USSE_PHAS, USSE_SOP2M_MOD, USSE_SOP2_MASK_ALPHA
		};
		const uint64_t prog_card[3] = {
			USSE_PHAS, USSE_SOP2M_MOD, USSE_SOP2_CARD_DSTA
		};

		g_gpu.tmpl.code_base = g_gpu.code_base;
		for (i = 0; i < SGX_TP_N; i++) {
			uint64_t c[SGX_TMPL_PROG_MAX];
			unsigned int len;

			g_gpu.tmpl.prog[i] = g_gpu.tprog.va + (uint32_t)i * PROG_SLOT;
			len = sgx_tmpl_program((enum sgx_tmpl_prog)i,
					       (g_gpu.tmpl.prog[i] - g_gpu.code_base) / 8, c);
			memcpy((uint8_t *)g_gpu.tprog.map + (uint32_t)i * PROG_SLOT,
			       c, len * 8);
		}

		memcpy((uint8_t *)g_gpu.code.map + 0 * PROG_SLOT, prog_ui, sizeof(prog_ui));
		memcpy((uint8_t *)g_gpu.code.map + 1 * PROG_SLOT, prog_mask, sizeof(prog_mask));
		memcpy((uint8_t *)g_gpu.code.map + 2 * PROG_SLOT, prog_card, sizeof(prog_card));

		g_gpu.tmpl.pixel = g_gpu.code.va;
		g_gpu.tmpl.eot = g_gpu.code.va + PROGS_SIZE;
		for (i = 0; i < g_gpu.num_targets; i++)
			write_eot_program((unsigned int)i, g_gpu.targets[i].va);
	}

	/* Build template PDS, state, white texture, and quad index buffer */
	g_gpu.tmpl.pds = g_gpu.glb.va + BUILT_PDS;
	g_gpu.tmpl.state = g_gpu.glb.va + BUILT_STATE;
	g_gpu.tmpl.idx = g_gpu.glb.va + BUILT_IDX;
	g_gpu.tmpl.tex0 = g_gpu.tmpl.tex1 = g_gpu.glb.va + BUILT_WHITE;
	g_gpu.tmpl.fb = g_gpu.targets[0].va;
	g_gpu.tmpl.fb_w = (uint32_t)width;
	g_gpu.tmpl.fb_h = (uint32_t)height;
	g_gpu.tmpl.fb_stride = (uint32_t)width;
	g_gpu.tmpl.w = (uint32_t)width;
	g_gpu.tmpl.h = (uint32_t)height;

	sgx_tmpl_pds(&g_gpu.tmpl, event, pds);
	sgx_tmpl_state(&g_gpu.tmpl, state);
	memcpy((uint8_t *)g_gpu.glb.map + BUILT_PDS, pds, sizeof(pds));
	memcpy((uint8_t *)g_gpu.glb.map + BUILT_STATE, state, sizeof(state));
	memset((uint8_t *)g_gpu.glb.map + BUILT_WHITE, 0xff, 16 * sizeof(uint32_t));

	/* Pre-populate quad index buffer: 0, 1, 2, 0, 2, 3 for MAX_QUADS */
	{
		uint16_t *idx = (uint16_t *)((uint8_t *)g_gpu.glb.map + BUILT_IDX);
		uint32_t q;

		for (q = 0; q < MAX_QUADS; q++) {
			uint16_t base = (uint16_t)(q * 4);
			idx[q * 6 + 0] = base + 0;
			idx[q * 6 + 1] = base + 1;
			idx[q * 6 + 2] = base + 2;
			idx[q * 6 + 3] = base + 0;
			idx[q * 6 + 4] = base + 2;
			idx[q * 6 + 5] = base + 3;
		}
	}

	sgx_tmpl_tail(&g_gpu.tmpl, g_gpu.tail);
	sgx_tmpl_ta_cmd(&g_gpu.tmpl, g_gpu.vdm.va, 0, g_gpu.cmd);
	sgx_tmpl_block3d(&g_gpu.tmpl, blk);

	/* Initialize render target details, state, and 3D block */
	{
		uint32_t *details = (uint32_t *)((uint8_t *)g_gpu.rtb.map +
				    (g_gpu.rt_va[SGX_RT_DETAILS] - g_gpu.rtb.va));
		uint32_t *rtstate = (uint32_t *)((uint8_t *)g_gpu.rtb.map +
				    (g_gpu.rt_va[SGX_RT_STATE] - g_gpu.rtb.va));
		uint32_t *blk_map = (uint32_t *)((uint8_t *)g_gpu.rtb.map +
				    (g_gpu.blk_va - g_gpu.rtb.va));

		sgx_rt_fill(&g_gpu.rt, g_gpu.rt_va, details, rtstate);
		memcpy(blk_map, blk, sizeof(blk));
		sgx_rt_block3d(&g_gpu.rt, g_gpu.rt_va, blk_map);
	}
	sgx_rt_ta_cmd(&g_gpu.rt, g_gpu.rt_va, g_gpu.blk_va, g_gpu.cmd);
	g_gpu.cmd[0x28 / 4] = g_gpu.pb_va;

	/* Build the 6 pre-configured (Atlas, Shader, Filter) state & PDS blocks */
	{
		static const struct {
			unsigned int prog_slot;
			unsigned int atlas_id;	/* 0=Atlas0, 1=Atlas1, 2=FS0, 3=FS1 */
			bool bilinear;
			uint32_t swizzle;	/* 1=ARGB, 5=1RGB */
		} cfg[GPU_STATE_COUNT] = {
			[GPU_STATE_UI]          = { 0, 0, false, 1 },
			[GPU_STATE_MASK]        = { 1, 0, false, 1 },
			[GPU_STATE_CARD_MASKED] = { 2, 1, false, 5 },
			[GPU_STATE_CARD_OPAQUE] = { 0, 1, false, 5 },
			[GPU_STATE_FS0]         = { 0, 2, false, 5 },
			[GPU_STATE_FS1]         = { 0, 3, false, 5 },
		};
		uint32_t full[STATE_WORDS];
		uint32_t prog[16];

		memset(g_gpu.base_fetch, 0, sizeof(g_gpu.base_fetch));
		memcpy(g_gpu.base_fetch, state + 0x180 / 4, 0x58);

		for (i = 0; i < GPU_STATE_COUNT; i++) {
			uint32_t pix_pds[12];
			uint32_t pix_pds_va = g_gpu.glb.va + BUILT_PIX_PDS + (uint32_t)i * 0x40;
			uint32_t full_va = g_gpu.ext.va + EXT_STATE_FULL + (uint32_t)i * 0x80;
			uint32_t prog_va = g_gpu.ext.va + EXT_STATE_PROG + (uint32_t)i * 0x40;
			uint32_t usse_va = g_gpu.code.va + cfg[i].prog_slot * PROG_SLOT;
			uint32_t tex_w, tex_h, tex_va;

			if (cfg[i].atlas_id == 0) {
				tex_w = ATLAS0_W;
				tex_h = ATLAS0_H;
				tex_va = g_gpu.atlas0.va;
			} else if (cfg[i].atlas_id == 1) {
				tex_w = ATLAS1_W;
				tex_h = ATLAS1_H;
				tex_va = g_gpu.atlas1.va;
			} else if (cfg[i].atlas_id == 2) {
				tex_w = ATLAS_FS_W;
				tex_h = ATLAS_FS_H;
				tex_va = g_gpu.atlas_fs[0].va;
			} else {
				tex_w = ATLAS_FS_W;
				tex_h = ATLAS_FS_H;
				tex_va = g_gpu.atlas_fs[1].va;
			}

			/* 12-word (48-byte) textured PDS block */
			memcpy(pix_pds, pds + 0x160 / 4, sizeof(pix_pds));
			pix_pds[0] = doutu(&g_gpu.tmpl, usse_va);
			pix_pds[4] = ((tex_w * 4 / 16 - 2) << 16) |
				     (cfg[i].bilinear ? 0x1e90u : 0x0e90u);
			pix_pds[5] = 0xcc000000u | ((tex_w - 1) << 12) | (tex_h - 1);
			pix_pds[6] = tex_va;
			pix_pds[7] = cfg[i].swizzle << 28;
			memcpy((uint8_t *)g_gpu.glb.map + BUILT_PIX_PDS + (uint32_t)i * 0x40,
			       pix_pds, sizeof(pix_pds));

			/* 21-word full state block + 11-word state loader program */
			memset(full, 0, sizeof(full));
			memcpy(full, state + 0xe0 / 4, sizeof(full));
			state_size(full, (unsigned int)width, (unsigned int)height);
			full[6] = 0x10000000u | ((pix_pds_va >> 4) & 0x07ffffffu);

			memset(prog, 0, sizeof(prog));
			memcpy(prog, state + 0x140 / 4, 0x2c);
			prog[0] = full_va;

			memcpy((uint8_t *)g_gpu.ext.map + EXT_STATE_FULL + (uint32_t)i * 0x80,
			       full, sizeof(full));
			memcpy((uint8_t *)g_gpu.ext.map + EXT_STATE_PROG + (uint32_t)i * 0x40,
			       prog, 0x2c);
			g_gpu.state_prog_va[i] = prog_va;
		}
	}

	init_atlas0_primitives();

	g_gpu.static_bmp_count = 0;
	g_gpu.static_bmp_y = 0;
	g_gpu.staging_y = 0;
	g_gpu.ndraws = 0;
	g_gpu.nquads = 0;
	g_gpu.frame_cleared = false;
	g_gpu.active = true;

	fprintf(stderr, "tvshell: SGX543 TBDR UI compositor online (%dx%d, %d targets)\n",
		width, height, g_gpu.num_targets);
	return true;

err_cleanup:
	shell_gpu_close();
	return false;
}

void shell_gpu_close(void)
{
	int i;

	if (g_gpu.sgxfd >= 0) {
		for (i = 0; i < g_gpu.num_targets; i++)
			bo_free(g_gpu.sgxfd, &g_gpu.targets[i]);
		bo_free(g_gpu.sgxfd, &g_gpu.atlas_fs[1]);
		bo_free(g_gpu.sgxfd, &g_gpu.atlas_fs[0]);
		bo_free(g_gpu.sgxfd, &g_gpu.atlas1);
		bo_free(g_gpu.sgxfd, &g_gpu.atlas0);
		bo_free(g_gpu.sgxfd, &g_gpu.rtb);
		bo_free(g_gpu.sgxfd, &g_gpu.ext);
		bo_free(g_gpu.sgxfd, &g_gpu.vdm);
		bo_free(g_gpu.sgxfd, &g_gpu.glb);
		bo_free(g_gpu.sgxfd, &g_gpu.code);
		bo_free(g_gpu.sgxfd, &g_gpu.tprog);
		close(g_gpu.sgxfd);
		g_gpu.sgxfd = -1;
	}
	g_gpu.active = false;
}

bool shell_gpu_active(void)
{
	return g_gpu.active;
}

uint32_t shell_gpu_draw_count(void)
{
	return g_gpu.nquads;
}

const uint32_t *shell_gpu_get_headless_pixels(int buf_idx)
{
	if (!g_gpu.active || !g_gpu.headless_targets)
		return NULL;
	if (buf_idx < 0 || buf_idx >= g_gpu.num_targets)
		buf_idx = 0;
	return (const uint32_t *)g_gpu.targets[buf_idx].map;
}

int shell_gpu_target_dmabuf_fd(int buf_idx)
{
	if (!g_gpu.active || buf_idx < 0 || buf_idx >= g_gpu.num_targets)
		return -1;
	return g_gpu.targets[buf_idx].dmabuf_fd;
}

/* Emit a single textured/colored quad with per-corner colors */
static void emit_quad_grad(enum gpu_state_id state,
			   float x0, float y0, float x1, float y1,
			   float u0, float v0, float u1, float v1,
			   uint32_t c0, uint8_t a0,
			   uint32_t c1, uint8_t a1,
			   uint32_t c2, uint8_t a2,
			   uint32_t c3, uint8_t a3)
{
	float *vtx;
	float inv_w, inv_h;
	float nx0, ny0, nx1, ny1;
	uint32_t cols[4] = { c0, c1, c2, c3 };
	uint8_t alphas[4] = { a0, a1, a2, a3 };
	float uvs[4][2] = {
		{ u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 }
	};
	float pos[4][2];
	int i;

	if (!g_gpu.active || x1 <= x0 || y1 <= y0)
		return;

	/* Clip full-screen clear detection */
	if (g_gpu.nquads == 0 && state == GPU_STATE_UI &&
	    x0 <= 0.0f && y0 <= 0.0f &&
	    x1 >= (float)g_gpu.width && y1 >= (float)g_gpu.height &&
	    a0 == 255 && a1 == 255 && a2 == 255 && a3 == 255) {
		g_gpu.frame_cleared = true;
	}

	if (g_gpu.nquads >= MAX_QUADS)
		return;

	/* Coalesce consecutive quads sharing the same GPU state */
	if (g_gpu.ndraws == 0 ||
	    g_gpu.draws[g_gpu.ndraws - 1].state != state) {
		if (g_gpu.ndraws >= MAX_DRAWS)
			return;
		g_gpu.draws[g_gpu.ndraws].state = state;
		g_gpu.draws[g_gpu.ndraws].first_quad = g_gpu.nquads;
		g_gpu.draws[g_gpu.ndraws].nquads = 1;
		g_gpu.ndraws++;
	} else {
		g_gpu.draws[g_gpu.ndraws - 1].nquads++;
	}

	inv_w = 2.0f / (float)g_gpu.width;
	inv_h = 2.0f / (float)g_gpu.height;
	nx0 = x0 * inv_w - 1.0f;
	ny0 = y0 * inv_h - 1.0f;
	nx1 = x1 * inv_w - 1.0f;
	ny1 = y1 * inv_h - 1.0f;

	pos[0][0] = nx0; pos[0][1] = ny0;
	pos[1][0] = nx1; pos[1][1] = ny0;
	pos[2][0] = nx1; pos[2][1] = ny1;
	pos[3][0] = nx0; pos[3][1] = ny1;

	vtx = (float *)((uint8_t *)g_gpu.ext.map + EXT_VB +
			g_gpu.nquads * 4 * PACK_VTX_FLOATS * sizeof(float));

	for (i = 0; i < 4; i++) {
		uint32_t c = cols[i];
		vtx[0] = ((c >> 16) & 0xff) * (1.0f / 255.0f);
		vtx[1] = ((c >> 8) & 0xff) * (1.0f / 255.0f);
		vtx[2] = (c & 0xff) * (1.0f / 255.0f);
		vtx[3] = alphas[i] * (1.0f / 255.0f);
		vtx[4] = uvs[i][0];
		vtx[5] = uvs[i][1];
		vtx[6] = pos[i][0];
		vtx[7] = pos[i][1];
		vtx += PACK_VTX_FLOATS;
	}

	g_gpu.nquads++;
}

static void emit_quad_solid(enum gpu_state_id state,
			    float x0, float y0, float x1, float y1,
			    float u0, float v0, float u1, float v1,
			    uint32_t color, uint8_t alpha)
{
	emit_quad_grad(state, x0, y0, x1, y1, u0, v0, u1, v1,
		       color, alpha, color, alpha, color, alpha, color, alpha);
}

void shell_gpu_draw_rect(int x, int y, int w, int h,
			 uint32_t color, uint8_t alpha)
{
	const float u = 4.0f / (float)ATLAS0_W;
	const float v = 4.0f / (float)ATLAS0_H;

	if (w <= 0 || h <= 0 || alpha == 0)
		return;
	emit_quad_solid(GPU_STATE_UI,
			(float)x, (float)y, (float)(x + w), (float)(y + h),
			u, v, u, v, color, alpha);
}

void shell_gpu_draw_grad_v(int x, int y, int w, int h,
			   uint32_t c_top, uint8_t a_top,
			   uint32_t c_bot, uint8_t a_bot)
{
	const float u = 4.0f / (float)ATLAS0_W;
	const float v = 4.0f / (float)ATLAS0_H;

	if (w <= 0 || h <= 0 || (a_top == 0 && a_bot == 0))
		return;
	emit_quad_grad(GPU_STATE_UI,
		       (float)x, (float)y, (float)(x + w), (float)(y + h),
		       u, v, u, v,
		       c_top, a_top, c_top, a_top,
		       c_bot, a_bot, c_bot, a_bot);
}

void shell_gpu_draw_grad_h(int x, int y, int w, int h,
			   uint32_t c_left, uint8_t a_left,
			   uint32_t c_right, uint8_t a_right)
{
	const float u = 4.0f / (float)ATLAS0_W;
	const float v = 4.0f / (float)ATLAS0_H;

	if (w <= 0 || h <= 0 || (a_left == 0 && a_right == 0))
		return;
	emit_quad_grad(GPU_STATE_UI,
		       (float)x, (float)y, (float)(x + w), (float)(y + h),
		       u, v, u, v,
		       c_left, a_left, c_right, a_right,
		       c_right, a_right, c_left, a_left);
}

void shell_gpu_draw_shimmer_bar(int x, int y, int w, int h, int radius,
				uint32_t base_color, uint32_t highlight_color,
				int phase_pct)
{
	int center_x, band_w, pad, rx0, rx1, l0, r1;

	if (w <= 0 || h <= 0)
		return;

	shell_gpu_draw_rounded_rect(x, y, w, h, radius, base_color, 255);

	center_x = x + (w * phase_pct) / 100;
	band_w = w / 3 > 1 ? w / 3 : 1;
	pad = radius / 2;
	rx0 = x + pad;
	rx1 = x + w - pad;
	if (rx1 <= rx0)
		return;

	l0 = center_x - band_w;
	r1 = center_x + band_w;

	if (center_x > rx0 && l0 < rx1) {
		int cx0 = l0 < rx0 ? rx0 : l0;
		int cx1 = center_x > rx1 ? rx1 : center_x;
		if (cx1 > cx0) {
			uint8_t a0 = (uint8_t)((cx0 - l0) * 180 / band_w);
			uint8_t a1 = (uint8_t)((cx1 - l0) * 180 / band_w);
			shell_gpu_draw_grad_h(cx0, y, cx1 - cx0, h,
					      highlight_color, a0,
					      highlight_color, a1);
		}
	}
	if (r1 > rx0 && center_x < rx1) {
		int cx0 = center_x < rx0 ? rx0 : center_x;
		int cx1 = r1 > rx1 ? rx1 : r1;
		if (cx1 > cx0) {
			uint8_t a0 = (uint8_t)((r1 - cx0) * 180 / band_w);
			uint8_t a1 = (uint8_t)((r1 - cx1) * 180 / band_w);
			shell_gpu_draw_grad_h(cx0, y, cx1 - cx0, h,
					      highlight_color, a0,
					      highlight_color, a1);
		}
	}
}

void shell_gpu_draw_card_bottom_scrim(int card_x, int card_y,
				      int card_w, int card_h,
				      int radius, int scrim_h)
{
	const float su = 4.0f / (float)ATLAS0_W;
	const float sv = 4.0f / (float)ATLAS0_H;
	const float inv_a0 = 1.0f / (float)ATLAS0_W;
	int y0, y1, y2, y3, y_bot;
	float u0, um, u1, vm, v1;

	if (card_w <= 0 || card_h <= 0)
		return;
	if (scrim_h <= 0 || scrim_h > card_h)
		scrim_h = card_h;
	if (radius > card_w / 2)
		radius = card_w / 2;
	if (radius > card_h / 2)
		radius = card_h / 2;
	if (radius > MAX_RADIUS)
		radius = MAX_RADIUS;
	if (radius < 0)
		radius = 0;

	y0 = card_y + card_h - scrim_h;
	y_bot = card_y + card_h;

	if (card_w == CARD_SCRIM_W && scrim_h == CARD_SCRIM_H &&
	    radius == CARD_SCRIM_R && g_gpu.card_scrim_u > 0) {
		u0 = (float)g_gpu.card_scrim_u * inv_a0;
		u1 = (float)(g_gpu.card_scrim_u + CARD_SCRIM_W) * inv_a0;
		vm = (float)g_gpu.card_scrim_v * inv_a0;
		v1 = (float)(g_gpu.card_scrim_v + CARD_SCRIM_H) * inv_a0;
		emit_quad_solid(GPU_STATE_UI,
				(float)card_x, (float)y0,
				(float)(card_x + card_w), (float)y_bot,
				u0, vm, u1, v1, 0x06080cU, 255);
		return;
	}

	y1 = y0 + (scrim_h * 16) / 100;
	y2 = y0 + (scrim_h * 42) / 100;
	y3 = y_bot - radius;
	if (y1 > y3)
		y1 = y3;
	if (y2 > y3)
		y2 = y3;

	if (y1 > y0)
		shell_gpu_draw_grad_v(card_x, y0, card_w, y1 - y0,
				      0x06080cU, 0, 0x06080cU, 82);
	if (y2 > y1)
		shell_gpu_draw_grad_v(card_x, y1, card_w, y2 - y1,
				      0x06080cU, 82, 0x06080cU, 214);
	if (y3 > y2)
		shell_gpu_draw_grad_v(card_x, y2, card_w, y3 - y2,
				      0x06080cU, 214, 0x06080cU, 242);

	if (radius > 0) {
		u0 = (float)g_gpu.fill_corner_u[radius] * inv_a0;
		um = (float)(g_gpu.fill_corner_u[radius] + radius) * inv_a0;
		u1 = (float)(g_gpu.fill_corner_u[radius] + 2 * radius) * inv_a0;
		vm = (float)(g_gpu.fill_corner_v[radius] + radius) * inv_a0;
		v1 = (float)(g_gpu.fill_corner_v[radius] + 2 * radius) * inv_a0;

		emit_quad_solid(GPU_STATE_UI,
				(float)card_x, (float)y3,
				(float)(card_x + radius), (float)y_bot,
				u0, vm, um, v1, 0x06080cU, 245);
		emit_quad_solid(GPU_STATE_UI,
				(float)(card_x + card_w - radius), (float)y3,
				(float)(card_x + card_w), (float)y_bot,
				um, vm, u1, v1, 0x06080cU, 245);
		if (card_w > 2 * radius) {
			emit_quad_solid(GPU_STATE_UI,
					(float)(card_x + radius), (float)y3,
					(float)(card_x + card_w - radius), (float)y_bot,
					su, sv, su, sv, 0x06080cU, 245);
		}
	}
}

void shell_gpu_draw_top_scrim(int w, int h)
{
	const float inv_a0 = 1.0f / (float)ATLAS0_W;

	if (w <= 0 || h <= 0)
		return;

	if (h == TOP_SCRIM_H && g_gpu.top_scrim_u > 0) {
		float v0 = (float)g_gpu.top_scrim_v * inv_a0;
		float v1 = (float)(g_gpu.top_scrim_v + TOP_SCRIM_H) * inv_a0;
		float u0 = (float)g_gpu.top_scrim_u * inv_a0;
		int x;

		for (x = 0; x < w; x += TOP_SCRIM_W) {
			int tw = (x + TOP_SCRIM_W > w) ? (w - x) : TOP_SCRIM_W;
			float u1 = (float)(g_gpu.top_scrim_u + tw) * inv_a0;

			emit_quad_solid(GPU_STATE_UI,
					(float)x, 0.0f,
					(float)(x + tw), (float)h,
					u0, v0, u1, v1, 0x06080cU, 255);
		}
		return;
	}

	{
		int h1 = (h * 65) / 100;

		if (h1 > 0)
			shell_gpu_draw_grad_v(0, 0, w, h1,
					      0x06080cU, 224, 0x06080cU, 115);
		if (h > h1)
			shell_gpu_draw_grad_v(0, h1, w, h - h1,
					      0x06080cU, 115, 0x06080cU, 0);
	}
}

void shell_gpu_draw_bottom_scrim(int y0, int w, int h)
{
	const float inv_a0 = 1.0f / (float)ATLAS0_W;

	if (w <= 0 || h <= 0)
		return;

	if (h == BOT_SCRIM_H && g_gpu.bot_scrim_u > 0) {
		float v0 = (float)g_gpu.bot_scrim_v * inv_a0;
		float v1 = (float)(g_gpu.bot_scrim_v + BOT_SCRIM_H) * inv_a0;
		float u0 = (float)g_gpu.bot_scrim_u * inv_a0;
		int x;

		for (x = 0; x < w; x += BOT_SCRIM_W) {
			int tw = (x + BOT_SCRIM_W > w) ? (w - x) : BOT_SCRIM_W;
			float u1 = (float)(g_gpu.bot_scrim_u + tw) * inv_a0;

			emit_quad_solid(GPU_STATE_UI,
					(float)x, (float)y0,
					(float)(x + tw), (float)(y0 + h),
					u0, v0, u1, v1, 0x06080cU, 255);
		}
		return;
	}

	{
		int h1 = (h * 18) / 100;
		int h2 = (h * 45) / 100;

		if (h1 > 0)
			shell_gpu_draw_grad_v(0, y0, w, h1,
					      0x06080cU, 0, 0x06080cU, 89);
		if (h2 > h1)
			shell_gpu_draw_grad_v(0, y0 + h1, w, h2 - h1,
					      0x06080cU, 89, 0x06080cU, 209);
		if (h > h2)
			shell_gpu_draw_grad_v(0, y0 + h2, w, h - h2,
					      0x06080cU, 209, 0x06080cU, 245);
	}
}

static void emit_rounded_rect_state(enum gpu_state_id state,
				    int x, int y, int w, int h, int r,
				    uint32_t color, uint8_t alpha)
{
	const float su = 4.0f / (float)ATLAS0_W;
	const float sv = 4.0f / (float)ATLAS0_H;
	const float inv_a0 = 1.0f / (float)ATLAS0_W;
	float u0, v0, um, vm, u1, v1;

	if (w <= 0 || h <= 0 || alpha == 0)
		return;

	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	if (r > MAX_RADIUS)
		r = MAX_RADIUS;

	if (r <= 0) {
		emit_quad_solid(state,
				(float)x, (float)y, (float)(x + w), (float)(y + h),
				su, sv, su, sv, color, alpha);
		return;
	}

	u0 = (float)g_gpu.fill_corner_u[r] * inv_a0;
	v0 = (float)g_gpu.fill_corner_v[r] * inv_a0;
	um = (float)(g_gpu.fill_corner_u[r] + r) * inv_a0;
	vm = (float)(g_gpu.fill_corner_v[r] + r) * inv_a0;
	u1 = (float)(g_gpu.fill_corner_u[r] + 2 * r) * inv_a0;
	v1 = (float)(g_gpu.fill_corner_v[r] + 2 * r) * inv_a0;

	/* Pure circle optimization: 1 single quad covering all 4 quadrants */
	if (w == 2 * r && h == 2 * r) {
		emit_quad_solid(state,
				(float)x, (float)y, (float)(x + w), (float)(y + h),
				u0, v0, u1, v1, color, alpha);
		return;
	}

	/* 4 corners at exact 1:1 texel scale */
	emit_quad_solid(state, (float)x, (float)y, (float)(x + r), (float)(y + r),
			u0, v0, um, vm, color, alpha);
	emit_quad_solid(state, (float)(x + w - r), (float)y, (float)(x + w), (float)(y + r),
			um, v0, u1, vm, color, alpha);
	emit_quad_solid(state, (float)x, (float)(y + h - r), (float)(x + r), (float)(y + h),
			u0, vm, um, v1, color, alpha);
	emit_quad_solid(state, (float)(x + w - r), (float)(y + h - r), (float)(x + w), (float)(y + h),
			um, vm, u1, v1, color, alpha);

	/* Top & bottom horizontal bridges */
	if (w > 2 * r) {
		emit_quad_solid(state, (float)(x + r), (float)y,
				(float)(x + w - r), (float)(y + r),
				su, sv, su, sv, color, alpha);
		emit_quad_solid(state, (float)(x + r), (float)(y + h - r),
				(float)(x + w - r), (float)(y + h),
				su, sv, su, sv, color, alpha);
	}

	/* Center full-width body */
	if (h > 2 * r) {
		emit_quad_solid(state, (float)x, (float)(y + r),
				(float)(x + w), (float)(y + h - r),
				su, sv, su, sv, color, alpha);
	}
}

void shell_gpu_draw_rounded_rect(int x, int y, int w, int h, int r,
				 uint32_t color, uint8_t alpha)
{
	emit_rounded_rect_state(GPU_STATE_UI, x, y, w, h, r, color, alpha);
}

void shell_gpu_draw_rounded_outline(int x, int y, int w, int h, int r,
				    int thickness, uint32_t color, uint8_t alpha)
{
	const float su = 4.0f / (float)ATLAS0_W;
	const float sv = 4.0f / (float)ATLAS0_H;
	const float inv_a0 = 1.0f / (float)ATLAS0_W;
	float u0, v0, um, vm, u1, v1;

	if (w <= 2 || h <= 2 || thickness <= 0 || alpha == 0)
		return;

	if (thickness > MAX_STROKE_THICK)
		thickness = MAX_STROKE_THICK;
	if (r > w / 2)
		r = w / 2;
	if (r > h / 2)
		r = h / 2;
	if (r > MAX_RADIUS)
		r = MAX_RADIUS;
	if (r < 1)
		r = 1;

	u0 = (float)g_gpu.outl_corner_u[thickness][r] * inv_a0;
	v0 = (float)g_gpu.outl_corner_v[thickness][r] * inv_a0;
	um = (float)(g_gpu.outl_corner_u[thickness][r] + r) * inv_a0;
	vm = (float)(g_gpu.outl_corner_v[thickness][r] + r) * inv_a0;
	u1 = (float)(g_gpu.outl_corner_u[thickness][r] + 2 * r) * inv_a0;
	v1 = (float)(g_gpu.outl_corner_v[thickness][r] + 2 * r) * inv_a0;

	/* 4 1:1 2x2-supersampled SDF outline corner quads */
	emit_quad_solid(GPU_STATE_UI, (float)x, (float)y, (float)(x + r), (float)(y + r),
			u0, v0, um, vm, color, alpha);
	emit_quad_solid(GPU_STATE_UI, (float)(x + w - r), (float)y, (float)(x + w), (float)(y + r),
			um, v0, u1, vm, color, alpha);
	emit_quad_solid(GPU_STATE_UI, (float)x, (float)(y + h - r), (float)(x + r), (float)(y + h),
			u0, vm, um, v1, color, alpha);
	emit_quad_solid(GPU_STATE_UI, (float)(x + w - r), (float)(y + h - r), (float)(x + w), (float)(y + h),
			um, vm, u1, v1, color, alpha);

	/* 4 straight perimeter strips of exact width/height @thickness */
	if (w > 2 * r) {
		emit_quad_solid(GPU_STATE_UI, (float)(x + r), (float)y,
				(float)(x + w - r), (float)(y + thickness),
				su, sv, su, sv, color, alpha);
		emit_quad_solid(GPU_STATE_UI, (float)(x + r), (float)(y + h - thickness),
				(float)(x + w - r), (float)(y + h),
				su, sv, su, sv, color, alpha);
	}
	if (h > 2 * r) {
		emit_quad_solid(GPU_STATE_UI, (float)x, (float)(y + r),
				(float)(x + thickness), (float)(y + h - r),
				su, sv, su, sv, color, alpha);
		emit_quad_solid(GPU_STATE_UI, (float)(x + w - thickness), (float)(y + r),
				(float)(x + w), (float)(y + h - r),
				su, sv, su, sv, color, alpha);
	}
}

bool shell_gpu_upload_glyph(uint16_t w, uint16_t h,
			    const uint8_t *alpha_bitmap,
			    uint16_t *out_u, uint16_t *out_v)
{
	uint32_t *dst;
	uint16_t u0, v0, row, col;

	if (!g_gpu.active || w == 0 || h == 0 || !alpha_bitmap)
		return false;

	if (!atlas0_alloc(w, h, &u0, &v0))
		return false;

	dst = (uint32_t *)g_gpu.atlas0.map;
	for (row = 0; row < h; row++) {
		uint32_t *line = dst + (v0 + row) * ATLAS0_W + u0;
		const uint8_t *src = alpha_bitmap + row * w;

		for (col = 0; col < w; col++)
			line[col] = ((uint32_t)src[col] << 24) | 0x00FFFFFFu;
	}

	*out_u = u0;
	*out_v = v0;
	return true;
}

void shell_gpu_draw_glyph(int x, int y, int w, int h,
			  uint16_t atlas_u, uint16_t atlas_v,
			  uint32_t color, uint8_t alpha)
{
	const float inv_a0 = 1.0f / (float)ATLAS0_W;
	float u0, v0, u1, v1;

	if (!g_gpu.active || w <= 0 || h <= 0 || alpha == 0)
		return;

	u0 = (float)atlas_u * inv_a0;
	v0 = (float)atlas_v * inv_a0;
	u1 = (float)(atlas_u + w) * inv_a0;
	v1 = (float)(atlas_v + h) * inv_a0;

	emit_quad_solid(GPU_STATE_UI,
			(float)x, (float)y, (float)(x + w), (float)(y + h),
			u0, v0, u1, v1, color, alpha);
}

/* Locate a pixel pointer inside Atlas 1, Atlas 2, or Atlas 3 */
static bool resolve_atlas_ptr(const uint32_t *pixels, int w, int h, int src_stride,
			      int *out_atlas_id, int *out_sx, int *out_sy)
{
	const uint8_t *p = (const uint8_t *)pixels;
	const uint8_t *a1 = (const uint8_t *)g_gpu.atlas1.map;
	int i;

	if (p >= a1 && p < a1 + g_gpu.atlas1.size) {
		size_t pixel_off = (size_t)(p - a1) / 4;
		*out_atlas_id = 1;
		*out_sy = (int)(pixel_off / ATLAS1_W);
		*out_sx = (int)(pixel_off % ATLAS1_W);
		return true;
	}

	for (i = 0; i < 2; i++) {
		const uint8_t *fs = (const uint8_t *)g_gpu.atlas_fs[i].map;
		if (p >= fs && p < fs + g_gpu.atlas_fs[i].size) {
			size_t pixel_off = (size_t)(p - fs) / 4;
			*out_atlas_id = 2 + i;
			*out_sy = (int)(pixel_off / ATLAS_FS_W);
			*out_sx = (int)(pixel_off % ATLAS_FS_W);
			return true;
		}
	}

	/* Fallback: copy external full-screen 1280x720 bitmap into Atlas 3 */
	if (w == ATLAS_FS_W && h == ATLAS_FS_H && src_stride == ATLAS_FS_W &&
	    g_gpu.atlas_fs[1].map) {
		memcpy(g_gpu.atlas_fs[1].map, pixels,
		       (size_t)ATLAS_FS_W * ATLAS_FS_H * sizeof(uint32_t));
		*out_atlas_id = 3;
		*out_sx = 0;
		*out_sy = 0;
		return true;
	}

	/*
	 * Persistent Atlas 1 static bitmap cache (column x=1682):
	 * Uploads small static bitmaps (e.g. animated header logos) once into
	 * Atlas 1 and reuses their coordinates with zero CPU memcpy on all
	 * subsequent frames.
	 */
	if (w > 0 && w <= 276 && h > 0 && src_stride >= w) {
		for (i = 0; i < g_gpu.static_bmp_count; i++) {
			if (g_gpu.static_bmps[i].pixels == pixels &&
			    g_gpu.static_bmps[i].w == w &&
			    g_gpu.static_bmps[i].h == h &&
			    g_gpu.static_bmps[i].stride == src_stride) {
				*out_atlas_id = 1;
				*out_sx = 1682;
				*out_sy = g_gpu.static_bmps[i].sy;
				return true;
			}
		}
		if (g_gpu.static_bmp_count < 16 &&
		    g_gpu.static_bmp_y + h <= ATLAS1_H) {
			int idx = g_gpu.static_bmp_count++;
			int sy = g_gpu.static_bmp_y;
			uint32_t *dst = (uint32_t *)g_gpu.atlas1.map +
					sy * ATLAS1_W + 1682;
			int row;

			for (row = 0; row < h; row++) {
				uint32_t *drow = dst + row * ATLAS1_W;
				const uint32_t *srow = pixels + row * src_stride;

				drow[-1] = srow[0];
				memcpy(drow, srow, (size_t)w * sizeof(uint32_t));
				drow[w] = srow[w - 1];
			}
			g_gpu.static_bmps[idx].pixels = pixels;
			g_gpu.static_bmps[idx].w = w;
			g_gpu.static_bmps[idx].h = h;
			g_gpu.static_bmps[idx].stride = src_stride;
			g_gpu.static_bmps[idx].sy = sy;
			g_gpu.static_bmp_y += h + 2;

			*out_atlas_id = 1;
			*out_sx = 1682;
			*out_sy = sy;
			return true;
		}
	}

	/* Fallback: copy external small bitmap into Atlas 1 staging column (x=1402) */
	if (w > 0 && w <= 276 && h > 0 && g_gpu.staging_y + h <= ATLAS1_H &&
	    src_stride >= w) {
		uint32_t *dst = (uint32_t *)g_gpu.atlas1.map +
				g_gpu.staging_y * ATLAS1_W + 1402;
		int row;

		for (row = 0; row < h; row++) {
			uint32_t *drow = dst + row * ATLAS1_W;
			const uint32_t *srow = pixels + row * src_stride;

			drow[-1] = srow[0];
			memcpy(drow, srow, (size_t)w * sizeof(uint32_t));
			drow[w] = srow[w - 1];
		}
		*out_atlas_id = 1;
		*out_sx = 1402;
		*out_sy = g_gpu.staging_y;
		g_gpu.staging_y += h + 2;
		return true;
	}

	return false;
}

bool shell_gpu_draw_rounded_bitmap(int x, int y, int w, int h, int r,
				   const uint32_t *pixels, int src_stride)
{
	int atlas_id = 0, sx = 0, sy = 0;
	float u0, v0, u1, v1;

	if (!g_gpu.active || !pixels || w <= 0 || h <= 0)
		return false;

	if (!resolve_atlas_ptr(pixels, w, h, src_stride, &atlas_id, &sx, &sy) ||
	    atlas_id != 1)
		return false;

	u0 = (float)sx / (float)ATLAS1_W;
	v0 = (float)sy / (float)ATLAS1_H;
	u1 = (float)(sx + w) / (float)ATLAS1_W;
	v1 = (float)(sy + h) / (float)ATLAS1_H;

	if (r > 0) {
		/*
		 * Pass 1: Write 9-slice rounded-rect alpha mask into on-chip tile o0.a
		 * Pass 2: Blend Atlas 1 card RGB against on-chip tile o0.a
		 */
		emit_rounded_rect_state(GPU_STATE_MASK, x, y, w, h, r, 0xFFFFFFu, 255);
		if (h == 164) {
			const float su = 4.0f / (float)ATLAS0_W;
			const float sv = 4.0f / (float)ATLAS0_H;
			emit_quad_solid(GPU_STATE_MASK,
					(float)x, (float)(y + h - r),
					(float)(x + w), (float)(y + h),
					su, sv, su, sv, 0xFFFFFFu, 255);
		}
		emit_quad_solid(GPU_STATE_CARD_MASKED,
				(float)x, (float)y, (float)(x + w), (float)(y + h),
				u0, v0, u1, v1, 0xFFFFFFu, 255);
	} else {
		emit_quad_solid(GPU_STATE_CARD_OPAQUE,
				(float)x, (float)y, (float)(x + w), (float)(y + h),
				u0, v0, u1, v1, 0xFFFFFFu, 255);
	}
	return true;
}

bool shell_gpu_draw_bitmap_rect(int dst_x, int dst_y,
				int src_x, int src_y, int w, int h,
				const uint32_t *pixels, int src_stride,
				uint8_t alpha)
{
	int atlas_id = 0, sx = 0, sy = 0;
	enum gpu_state_id state;
	float tex_w, tex_h;
	float u0, v0, u1, v1;

	if (!g_gpu.active || !pixels || w <= 0 || h <= 0 || alpha == 0)
		return false;

	if (!resolve_atlas_ptr(pixels, w, h, src_stride, &atlas_id, &sx, &sy))
		return false;

	sx += src_x;
	sy += src_y;

	if (atlas_id == 1) {
		state = GPU_STATE_CARD_OPAQUE;
		tex_w = (float)ATLAS1_W;
		tex_h = (float)ATLAS1_H;
	} else if (atlas_id == 2) {
		state = GPU_STATE_FS0;
		tex_w = (float)ATLAS_FS_W;
		tex_h = (float)ATLAS_FS_H;
	} else {
		state = GPU_STATE_FS1;
		tex_w = (float)ATLAS_FS_W;
		tex_h = (float)ATLAS_FS_H;
	}

	u0 = (float)sx / tex_w;
	v0 = (float)sy / tex_h;
	u1 = (float)(sx + w) / tex_w;
	v1 = (float)(sy + h) / tex_h;

	emit_quad_solid(state,
			(float)dst_x, (float)dst_y,
			(float)(dst_x + w), (float)(dst_y + h),
			u0, v0, u1, v1, 0xFFFFFFu, alpha);
	return true;
}

int shell_gpu_flush(int dst_buf_idx, int bg_buf_idx)
{
	uint32_t *vdmbuf;
	uint32_t *pds_map;
	uint32_t bg[4];
	uint32_t handles[16];
	uint32_t nhandles = 0;
	uint32_t n = 0, k, i;
	uint32_t dst_va, dst_handle;

	if (!g_gpu.active)
		return -1;

	if (g_gpu.ndraws == 0 || g_gpu.nquads == 0) {
		g_gpu.frame_cleared = false;
		return 0;
	}

	if (dst_buf_idx < 0 || dst_buf_idx >= g_gpu.num_targets)
		dst_buf_idx = 0;
	dst_va = g_gpu.targets[dst_buf_idx].va;
	dst_handle = g_gpu.targets[dst_buf_idx].handle;
	(void)dst_va;

	/*
	 * Patch PBE End-of-Tile DOUTU (+8) and hardware background tile reload
	 * descriptor (+0x130) in PDS:
	 * - If the frame started with a full-screen opaque draw (frame_cleared)
	 *   or bg_buf_idx < 0, sample the cached 4x4 white texture (zero DRAM read).
	 * - Otherwise, reload 32x32 tiles from the previous completed scanout
	 *   buffer (bg_buf_idx) into on-chip tile memory before executing draws.
	 */
	pds_map = (uint32_t *)((uint8_t *)g_gpu.glb.map + BUILT_PDS);
	pds_map[2] = doutu(&g_gpu.tmpl,
			   g_gpu.code.va + PROGS_SIZE + (uint32_t)dst_buf_idx * PROG_SLOT);
	if (g_gpu.frame_cleared || bg_buf_idx < 0 || bg_buf_idx >= g_gpu.num_targets) {
		bg[0] = 0x03fe0000u;
		bg[1] = 0x0c020002u;
		bg[2] = g_gpu.tmpl.tex0;
		bg[3] = 0;
	} else {
		uint32_t w = (uint32_t)g_gpu.width;
		uint32_t h = (uint32_t)g_gpu.height;

		bg[0] = ((w * 4 / 16 - 2) << 16) | 0x0e90u;
		bg[1] = 0xcc000000u | ((w - 1) << 12) | (h - 1);
		bg[2] = g_gpu.targets[bg_buf_idx].va;
		bg[3] = 0x50000000u;
	}
	memcpy(pds_map + 0x130 / 4, bg, sizeof(bg));

	/* Build VDM command stream and per-draw vertex fetch blocks */
	vdmbuf = (uint32_t *)g_gpu.vdm.map;
	for (k = 0; k < g_gpu.ndraws; k++) {
		const struct gpu_draw_pkt *d = &g_gpu.draws[k];
		uint32_t vb = g_gpu.ext.va + EXT_VB +
			      d->first_quad * 4 * PACK_VTX_FLOATS * sizeof(float);
		uint32_t fetch_va = g_gpu.ext.va + EXT_FETCH + k * 0x80;
		uint32_t *fetch_dst = (uint32_t *)((uint8_t *)g_gpu.ext.map +
						   EXT_FETCH + k * 0x80);
		uint32_t nindices = d->nquads * 6;

		memcpy(fetch_dst, g_gpu.base_fetch, 0x58);
		fetch_dst[0] = vb;
		fetch_dst[4] = vb + 16;
		fetch_dst[8] = vb + 24;

		vdmbuf[n++] = vdm4(4, g_gpu.tmpl.state + 0xa0);
		vdmbuf[n++] = 0x1000e102u;
		vdmbuf[n++] = vdm4(4, g_gpu.state_prog_va[d->state]);
		vdmbuf[n++] = 0x12022206u;
		vdmbuf[n++] = 0x81c00000u | nindices;
		vdmbuf[n++] = g_gpu.tmpl.idx;
		vdmbuf[n++] = 0x70000000u;
		vdmbuf[n++] = 0x003fffffu;
		vdmbuf[n++] = vdm4(9, fetch_va);
		vdmbuf[n++] = 0x07800604u;
	}
	for (i = 0; i < 5; i++)
		vdmbuf[n++] = g_gpu.tail[i];
	memset(&vdmbuf[n], 0, 512);

	/* Reset draw queue for the next frame before submitting */
	g_gpu.ndraws = 0;
	g_gpu.nquads = 0;
	g_gpu.staging_y = 0;
	g_gpu.frame_cleared = false;

	/* Collect BO handles for kernel reservation & fence tracking */
	handles[nhandles++] = g_gpu.tprog.handle;
	handles[nhandles++] = g_gpu.code.handle;
	handles[nhandles++] = g_gpu.glb.handle;
	handles[nhandles++] = g_gpu.vdm.handle;
	handles[nhandles++] = g_gpu.ext.handle;
	handles[nhandles++] = g_gpu.rtb.handle;
	handles[nhandles++] = g_gpu.atlas0.handle;
	handles[nhandles++] = g_gpu.atlas1.handle;
	handles[nhandles++] = g_gpu.atlas_fs[0].handle;
	handles[nhandles++] = g_gpu.atlas_fs[1].handle;
	handles[nhandles++] = dst_handle;
	if (bg_buf_idx >= 0 && bg_buf_idx < g_gpu.num_targets &&
	    g_gpu.targets[bg_buf_idx].handle != dst_handle) {
		handles[nhandles++] = g_gpu.targets[bg_buf_idx].handle;
	}

	{
		struct drm_apple_sgx_submit sub = {
			.cmd = (uintptr_t)g_gpu.cmd,
			.cmd_size = g_gpu.cmd[0],
			.pb_va = 0,
			.details_handle = g_gpu.rtb.handle,
			.details_offset = g_gpu.rt_va[SGX_RT_DETAILS] - g_gpu.rtb.va,
			.bo_handles = (uintptr_t)handles,
			.bo_handle_count = nhandles,
		};
		struct drm_apple_sgx_gem_wait gw = {
			.handle = dst_handle,
			.timeout_ns = (int64_t)(now_ns() + 2000000000ull),
		};

		if (ioctl(g_gpu.sgxfd, DRM_IOCTL_APPLE_SGX_SUBMIT, &sub) < 0)
			return -1;
		if (ioctl(g_gpu.sgxfd, DRM_IOCTL_APPLE_SGX_GEM_WAIT, &gw) < 0)
			return -1;
	}

	return 0;
}

uint32_t *shell_gpu_atlas1_slot_ptr(bool is_tile, int slot_idx,
				    int *out_x, int *out_y,
				    int *out_stride_bytes,
				    int *out_full_w, int *out_full_h)
{
	int col, row, sx, sy;

	if (!g_gpu.active || slot_idx < 0 || slot_idx >= 10)
		return NULL;

	col = slot_idx % 5;
	row = slot_idx / 5;
	sx = col * 280;
	sy = is_tile ? (624 + row * 164) : (row * 312);

	if (out_x)
		*out_x = sx;
	if (out_y)
		*out_y = sy;
	if (out_stride_bytes)
		*out_stride_bytes = ATLAS1_W * 4;
	if (out_full_w)
		*out_full_w = ATLAS1_W;
	if (out_full_h)
		*out_full_h = ATLAS1_H;

	return (uint32_t *)g_gpu.atlas1.map + sy * ATLAS1_W + sx;
}

int shell_gpu_fs_atlas_dmabuf_fd(int fs_idx, uint32_t **out_ptr,
				 int *out_stride_bytes)
{
	if (!g_gpu.active || fs_idx < 0 || fs_idx >= 2)
		return -1;

	if (out_ptr)
		*out_ptr = (uint32_t *)g_gpu.atlas_fs[fs_idx].map;
	if (out_stride_bytes)
		*out_stride_bytes = ATLAS_FS_W * 4;

	return g_gpu.atlas_fs[fs_idx].dmabuf_fd;
}
