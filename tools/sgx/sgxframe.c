// SPDX-License-Identifier: GPL-2.0-only
/*
 * sgxframe -- one clear through the render node's TA/3D queue, with no Mesa
 * (/bin/sgxframe).
 *
 * The template-frame and render-target builders it uses (sgx_template.c,
 * sgx_rt.c) are MIT, from the clean-room SGX543 work published in Project
 * Cascadia; see README.md in this directory.
 *
 * This is the first render the port runs.  It builds the template frame the
 * reference's Mesa driver lays out (sgx_template.c: the PDS block, the state
 * area, the TA command, the 3D register block) and a render target's data
 * (sgx_rt.c: what the stock driver computes for a target of a size), covers
 * the target with one full-screen quad and reads it back.  When that is
 * right, the TA, the tiler, the 3D pass, the USSE and the PDS, the pixel
 * back end and the GPU MMU have all run.
 *
 * It needs one piece that is not in this tree: the stock graphics driver's
 * event PDS program (0xa8 bytes, 42 words: five data rows and 22
 * instructions).  The device's own system image carries it; -e names a copy.
 * The three DOUTU words and word 16 are filled in here, as the reference
 * does.
 *
 * Usage:
 *   sgxframe [-e EVENT.PDS] [-w 1280] [-h 720] [-c 0xAARRGGBB]
 *            [-s SECONDS] [-r /dev/dri/renderD128]
 *
 *   -e defaults to /lib/firmware/s5l8947x/gl-event.pds, which stage2
 *      provisions from the device's own system image (glcarve).
 *
 *   -s draws the result on the display the kernel is already scanning out,
 *      through DRM_APPLE_SGX_BLIT with dst_handle 0 (APPLE_SGX_PARAM_FB_*).
 *
 * Exit status 0 when every checked pixel came back the colour asked for.
 */
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "sgx_rt.h"
#include "sgx_template.h"

/* ---- the render node's interface (include/uapi/drm/apple_sgx_drm.h) ----- */

#define SGX_CMD_BASE			0x40

struct drm_apple_sgx_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct drm_apple_sgx_gem_create {
	uint64_t size;
	uint32_t flags, handle, va, pad;
};

struct drm_apple_sgx_gem_mmap_offset {
	uint32_t handle, flags;
	uint64_t offset;
};

struct drm_apple_sgx_gem_wait {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct drm_apple_sgx_submit {
	uint64_t cmd;
	uint32_t cmd_size, flags;
	uint32_t pb_va, details_handle, details_offset, cache_control;
	uint64_t bo_handles;
	uint32_t bo_handle_count, in_sync_count;
	uint64_t in_syncs;
	uint32_t out_sync, pad;
};

struct drm_apple_sgx_blit {
	uint32_t src_handle, dst_handle;
	uint32_t src_width, src_height, src_stride;
	uint32_t dst_width, dst_height, dst_stride;
	uint32_t flags, pad;
};

#define DRM_APPLE_SGX_GET_PARAM		0x00
#define DRM_APPLE_SGX_GEM_CREATE	0x01
#define DRM_APPLE_SGX_GEM_MMAP_OFFSET	0x02
#define DRM_APPLE_SGX_GEM_WAIT		0x03
#define DRM_APPLE_SGX_SUBMIT		0x04
#define DRM_APPLE_SGX_BLIT		0x07

#define DRM_IOCTL_APPLE_SGX_GET_PARAM \
	DRM_IOWR(SGX_CMD_BASE + DRM_APPLE_SGX_GET_PARAM, struct drm_apple_sgx_get_param)
#define DRM_IOCTL_APPLE_SGX_GEM_CREATE \
	DRM_IOWR(SGX_CMD_BASE + DRM_APPLE_SGX_GEM_CREATE, struct drm_apple_sgx_gem_create)
#define DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET \
	DRM_IOWR(SGX_CMD_BASE + DRM_APPLE_SGX_GEM_MMAP_OFFSET, struct drm_apple_sgx_gem_mmap_offset)
#define DRM_IOCTL_APPLE_SGX_GEM_WAIT \
	DRM_IOW(SGX_CMD_BASE + DRM_APPLE_SGX_GEM_WAIT, struct drm_apple_sgx_gem_wait)
#define DRM_IOCTL_APPLE_SGX_SUBMIT \
	DRM_IOW(SGX_CMD_BASE + DRM_APPLE_SGX_SUBMIT, struct drm_apple_sgx_submit)
#define DRM_IOCTL_APPLE_SGX_BLIT \
	DRM_IOWR(SGX_CMD_BASE + DRM_APPLE_SGX_BLIT, struct drm_apple_sgx_blit)

enum {
	APPLE_SGX_PARAM_UAPI_VERSION = 0,
	APPLE_SGX_PARAM_CORE_ID = 1,
	APPLE_SGX_PARAM_CORE_REVISION = 2,
	APPLE_SGX_PARAM_NUM_CORES = 3,
	APPLE_SGX_PARAM_UKERNEL_UP = 5,
	APPLE_SGX_PARAM_CODE_BASE = 8,
	APPLE_SGX_PARAM_FB_VA = 11,
	APPLE_SGX_PARAM_FB_WIDTH = 12,
	APPLE_SGX_PARAM_FB_HEIGHT = 13,
	APPLE_SGX_PARAM_FB_STRIDE = 14,
	APPLE_SGX_PARAM_PB_VA = 19,
};

#define APPLE_SGX_BO_USSE_CODE		(1u << 1)
#define APPLE_SGX_BO_TA_HEAP		(1u << 4)

/* ---- the template frame's layout (the reference's built frame) ---------- */

#define BUILT_PDS		0x0
#define BUILT_STATE		0x400
#define BUILT_WHITE		0xc00
#define BUILT_IDX		0x1000
#define GL_SIZE			(BUILT_IDX + SGX_TMPL_IDX_COUNT * 2)
#define PROG_SLOT		0x80
#define PROGS_SIZE		0x200
#define EOT_SLOT		0x58
#define CODE_SIZE		0x4000
#define VDM_SIZE		0x1000
#define EXT_VB			0x280000
#define EXT_FRAME		0x3c0000
#define EXT_SIZE		0x400000
#define STATE_WORDS		21
#define PACK_VTX_FLOATS		8

/* ---- USSE (sgx_frame.c's programs) ------------------------------------- */

#define USSE_PHAS		0xfa44070000000000ull
#define USSE_EMIT_PIXEL		0xfb24000003200082ull
#define USSE_SOP2M_MOD		0x90807982a0000140ull	/* texel * vertex colour */
#define USSE_MOV_O0_PA0		0x50850009a0000000ull	/* o0 = pa0, end */
#define USSE_END		(1ull << 50)

static const uint64_t usse_dummy_load[3] = {
	0x488b0281a00c0000ull, 0xe9a30084a0000000ull, 0xf920000000000000ull,
};

static uint64_t usse_limm_r(unsigned int reg, uint32_t imm)
{
	return 0xfca0000000000000ull | (uint64_t)(reg & 0x7f) << 21 |
	       (uint64_t)((imm >> 26) & 0x3f) << 44 |
	       (uint64_t)((imm >> 21) & 0x1f) << 36 | (imm & 0x1fffff);
}

/* ---- buffers ----------------------------------------------------------- */

struct bo {
	uint32_t handle, va;
	size_t size;
	void *map;
};

#define MAX_BO	8

static struct bo bo[MAX_BO];
static unsigned int nbo;
static int sgxfd = -1;

static void fail(const char *what)
{
	fprintf(stderr, "sgxframe: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void xioctl(int fd, unsigned long req, void *arg, const char *what)
{
	if (ioctl(fd, req, arg) < 0) {
		fprintf(stderr, "sgxframe: %s: %s\n", what, strerror(errno));
		exit(1);
	}
}

static struct bo *bo_new(size_t size, uint32_t flags)
{
	struct drm_apple_sgx_gem_create gc = { .size = size, .flags = flags };
	struct drm_apple_sgx_gem_mmap_offset gm = { 0 };
	struct bo *b;

	if (nbo == MAX_BO) {
		fprintf(stderr, "sgxframe: too many buffers\n");
		exit(1);
	}
	b = &bo[nbo++];
	xioctl(sgxfd, DRM_IOCTL_APPLE_SGX_GEM_CREATE, &gc, "GEM_CREATE");
	b->handle = gc.handle;
	b->va = gc.va;
	b->size = size;

	gm.handle = gc.handle;
	xioctl(sgxfd, DRM_IOCTL_APPLE_SGX_GEM_MMAP_OFFSET, &gm, "GEM_MMAP_OFFSET");
	b->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, sgxfd, gm.offset);
	if (b->map == MAP_FAILED)
		fail("mmap");
	memset(b->map, 0, size);
	return b;
}

/* Write to a GPU address, through whichever buffer holds it. */
static void put(uint32_t va, const void *p, size_t n)
{
	unsigned int i;

	for (i = 0; i < nbo; i++)
		if (va >= bo[i].va && va + n < bo[i].va + bo[i].size + 1) {
			memcpy(bo[i].map + (va - bo[i].va), p, n);
			return;
		}
	fprintf(stderr, "sgxframe: no buffer holds GPU 0x%08x\n", va);
	exit(1);
}

/* ---- the frame --------------------------------------------------------- */

static uint32_t doutu(const struct sgx_tmpl *t, uint32_t va)
{
	return ((va - t->code_base) / 8) << 4 | 3;
}

static uint32_t vdm4(uint32_t tag, uint32_t va)
{
	return tag << 28 | ((va >> 4) & 0x0fffffff);
}

/* two triangles over the whole target, the pack's 8-float vertices:
 * r g b a (f32), u v (f32), x y (f32) */
static void quad(float *v, const float rgba[4])
{
	static const float corner[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	unsigned int i, f;

	for (i = 0; i < 6; i++) {
		const float *c = corner[order[i]];

		for (f = 0; f < 4; f++)
			*v++ = rgba[f];
		*v++ = c[0];
		*v++ = c[1];
		*v++ = c[0] * 2 - 1;
		*v++ = c[1] * 2 - 1;
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

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *event_path = "/lib/firmware/s5l8947x/gl-event.pds";
	const char *render = "/dev/dri/renderD128";
	unsigned int w = 1280, h = 720, ncores;
	uint32_t core_id, code_base, colour = 0xffff0000u;	/* AARRGGBB: red */
	float rgba[4];
	int opt, seconds = 0;
	struct bo *tprog, *code, *glb, *vdm, *ext, *rtb, *target;
	struct sgx_tmpl t = { 0 };
	struct sgx_rt rt;
	uint32_t rt_va[SGX_RT_NBUF], blk_va, rt_size = 0;
	uint32_t event[SGX_TMPL_EVENT_WORDS];
	uint32_t pds[SGX_TMPL_PDS_SIZE / 4], state[SGX_TMPL_STATE_SIZE / 4];
	uint32_t cmd[0x200 / 4], blk[SGX_TMPL_BLOCK_SIZE / 4];
	uint32_t full[STATE_WORDS], prog[16], fetch[32], tail[5], vdmbuf[16];
	uint32_t gp, i, n = 0;
	float verts[6 * PACK_VTX_FLOATS];
	uint32_t handles[MAX_BO];
	FILE *fp;
	int ret;

	while ((opt = getopt(argc, argv, "e:w:h:c:s:r:")) != -1) {
		switch (opt) {
		case 'e': event_path = optarg; break;
		case 'w': w = strtoul(optarg, NULL, 0); break;
		case 'h': h = strtoul(optarg, NULL, 0); break;
		case 'c': colour = strtoul(optarg, NULL, 0); break;
		case 's': seconds = atoi(optarg); break;
		case 'r': render = optarg; break;
		default:
			fprintf(stderr, "usage: %s -e EVENT.PDS [-w W] [-h H] "
				"[-c 0xAARRGGBB] [-s SECONDS] [-r NODE]\n", argv[0]);
			return 2;
		}
	}
	if (!w || w > 4096 || !h || h > 4096) {
		fprintf(stderr, "sgxframe: bad size %ux%u\n", w, h);
		return 2;
	}
	fp = fopen(event_path, "rb");
	if (!fp)
		fail(event_path);
	if (fread(event, 1, sizeof(event), fp) != sizeof(event)) {
		fprintf(stderr, "sgxframe: %s is not 0xa8 bytes\n", event_path);
		return 1;
	}
	fclose(fp);

	rgba[0] = ((colour >> 16) & 0xff) / 255.0f;
	rgba[1] = ((colour >> 8) & 0xff) / 255.0f;
	rgba[2] = (colour & 0xff) / 255.0f;
	rgba[3] = ((colour >> 24) & 0xff) / 255.0f;

	sgxfd = open(render, O_RDWR);
	if (sgxfd < 0)
		fail(render);

#define PARAM(p) ({ \
	struct drm_apple_sgx_get_param _p = { .param = (p) }; \
	xioctl(sgxfd, DRM_IOCTL_APPLE_SGX_GET_PARAM, &_p, "GET_PARAM"); \
	(uint32_t)_p.value; })

	gp = PARAM(APPLE_SGX_PARAM_UAPI_VERSION);
	core_id = PARAM(APPLE_SGX_PARAM_CORE_ID);
	ncores = PARAM(APPLE_SGX_PARAM_NUM_CORES);
	code_base = PARAM(APPLE_SGX_PARAM_CODE_BASE);
	printf("render node: UAPI %u, core 0x%08x, %u core(s), code base 0x%08x\n",
	       gp, core_id, ncores, code_base);
	if (gp < 3) {
		fprintf(stderr, "sgxframe: UAPI %u has no kernel parameter buffer\n", gp);
		return 1;
	}
	if (!PARAM(APPLE_SGX_PARAM_UKERNEL_UP))
		fprintf(stderr, "sgxframe: warning: the microkernel is not up "
			"(dmesg | grep apple-sgx)\n");
	if (!sgx_rt_layout(&rt, w, h, ncores ? ncores : 1)) {
		fprintf(stderr, "sgxframe: no render target layout for %ux%u\n", w, h);
		return 1;
	}

	/* the template's programs, the frame's own programs, and the frame */
	tprog = bo_new(0x1000, APPLE_SGX_BO_USSE_CODE);
	code = bo_new(CODE_SIZE, APPLE_SGX_BO_USSE_CODE);
	glb = bo_new(GL_SIZE, 0);
	vdm = bo_new(VDM_SIZE, 0);
	ext = bo_new(EXT_SIZE, 0);
	target = bo_new((size_t)w * h * 4, 0);

	for (i = 0; i < SGX_RT_NBUF; i++)
		rt_size += (rt.size[i] + 4095) & ~4095u;
	rt_size += SGX_TMPL_BLOCK_SIZE;
	rtb = bo_new(rt_size, APPLE_SGX_BO_TA_HEAP);
	{
		uint32_t at = rtb->va;

		for (i = 0; i < SGX_RT_NBUF; i++) {
			rt_va[i] = at;
			at += (rt.size[i] + 4095) & ~4095u;
		}
		blk_va = at;
	}

	/* the USSE programs: the replace program at the code base (the pixel
	 * block starts it), the end-of-tile program in its own slot */
	{
		const uint64_t replace[3] = { USSE_PHAS, USSE_SOP2M_MOD, USSE_MOV_O0_PA0 };

		t.code_base = code_base;
		t.prog[SGX_TP_BG_RELOAD] = tprog->va + 0 * PROG_SLOT;
		t.prog[SGX_TP_EMPTY_A] = tprog->va + 1 * PROG_SLOT;
		t.prog[SGX_TP_EMPTY_B] = tprog->va + 2 * PROG_SLOT;
		t.prog[SGX_TP_EMPTY_C] = tprog->va + 3 * PROG_SLOT;
		t.prog[SGX_TP_EMPTY_D] = tprog->va + 4 * PROG_SLOT;
		t.prog[SGX_TP_STATE_2] = tprog->va + 5 * PROG_SLOT;
		t.prog[SGX_TP_STATE_4] = tprog->va + 6 * PROG_SLOT;
		t.prog[SGX_TP_STATE_21] = tprog->va + 7 * PROG_SLOT;
		t.prog[SGX_TP_DONE] = tprog->va + 8 * PROG_SLOT;
		t.prog[SGX_TP_EOR] = tprog->va + 9 * PROG_SLOT;
		t.prog[SGX_TP_VERTEX] = tprog->va + 10 * PROG_SLOT;
		for (i = 0; i < SGX_TP_N; i++) {
			uint64_t c[SGX_TMPL_PROG_MAX];
			unsigned int len = sgx_tmpl_program(i,
				(t.prog[i] - code_base) / 8, c);

			put(t.prog[i], c, len * 8);
		}

		t.eot = code->va + PROGS_SIZE;
		t.pixel = code->va;
		put(code->va, replace, sizeof(replace));
		{
			uint64_t eot[11];

			eot[0] = USSE_PHAS;
			memcpy(&eot[1], usse_dummy_load, sizeof(usse_dummy_load));
			eot[4] = usse_limm_r(0, 0x00110000);	/* linear BGRA8 */
			eot[5] = usse_limm_r(1, target->va);
			eot[6] = usse_limm_r(2, w / 2 - 1);	/* stride, pixels / 2 */
			eot[7] = usse_limm_r(3, 0);
			eot[8] = usse_limm_r(4, 0);
			eot[9] = usse_limm_r(5, (h - 1) << 12 | (w - 1));
			eot[10] = USSE_EMIT_PIXEL;
			put(t.eot, eot, sizeof(eot));
		}
	}

	t.pds = glb->va + BUILT_PDS;
	t.state = glb->va + BUILT_STATE;
	t.idx = glb->va + BUILT_IDX;
	t.tex0 = t.tex1 = glb->va + BUILT_WHITE;
	t.fb = target->va;
	t.fb_w = w;
	t.fb_h = h;
	t.fb_stride = w;	/* pixels, as the template takes it */
	t.w = w;
	t.h = h;

	sgx_tmpl_pds(&t, event, pds);
	sgx_tmpl_state(&t, state);
	/* the pixel block starts the replace program on a 4x4 white texel */
	pds[(0x160 + 0 * 4) / 4] = doutu(&t, t.pixel);
	pds[(0x160 + 5 * 4) / 4] = 0x0c000000 | 2 << 16 | 2;
	pds[(0x160 + 6 * 4) / 4] = t.tex0;
	put(t.pds, pds, sizeof(pds));
	put(t.state, state, sizeof(state));
	{
		uint32_t white[16];

		memset(white, 0xff, sizeof(white));
		put(t.tex0, white, sizeof(white));
	}
	{
		uint16_t idx[SGX_TMPL_IDX_COUNT];

		for (i = 0; i < SGX_TMPL_IDX_COUNT; i++)
			idx[i] = i;
		put(t.idx, idx, sizeof(idx));
	}
	sgx_tmpl_tail(&t, tail);
	sgx_tmpl_ta_cmd(&t, vdm->va, 0, cmd);
	sgx_tmpl_block3d(&t, blk);

	/* the render target's own data, and the words that point at it */
	{
		uint32_t *details = (uint32_t *)(rtb->map + (rt_va[SGX_RT_DETAILS] - rtb->va));
		uint32_t *rtstate = (uint32_t *)(rtb->map + (rt_va[SGX_RT_STATE] - rtb->va));

		sgx_rt_fill(&rt, rt_va, details, rtstate);
		put(blk_va, blk, sizeof(blk));
		sgx_rt_block3d(&rt, rt_va,
			       (uint32_t *)(rtb->map + (blk_va - rtb->va)));
	}
	sgx_rt_ta_cmd(&rt, rt_va, blk_va, cmd);
	/* the parameter buffer the TA names: the kernel's, shared by every
	 * client (SUBMIT pb_va 0 selects it, but the TA command still holds
	 * its descriptor address) */
	cmd[0x28 / 4] = PARAM(APPLE_SGX_PARAM_PB_VA);

	/* the draw: the template's state and vertex side, one quad */
	memset(full, 0, sizeof(full));
	memcpy(full, state + 0xe0 / 4, 0x50);
	state_size(full, w, h);
	memset(prog, 0, sizeof(prog));
	memcpy(prog, state + 0x140 / 4, 0x2c);
	memset(fetch, 0, sizeof(fetch));
	memcpy(fetch, state + 0x180 / 4, 0x58);
	quad(verts, rgba);

	{
		uint32_t frame = ext->va + EXT_FRAME, vb = ext->va + EXT_VB;
		uint32_t p0 = (frame + STATE_WORDS * 4 + 0x3f) & ~0x3fu;
		uint32_t fb = (p0 + 0x2c + 0x3f) & ~0x3fu;

		prog[0] = frame;
		full[6] = 0x10000000 | ((t.pds + 0x160) >> 4 & 0x07ffffff);
		put(frame, full, sizeof(full));
		put(p0, prog, 0x2c);
		put(vb, verts, sizeof(verts));
		fetch[0] = vb;
		fetch[4] = vb + 16;
		fetch[8] = vb + 24;
		put(fb, fetch, 0x58);

		vdmbuf[n++] = vdm4(4, t.state + 0xa0);
		vdmbuf[n++] = 0x1000e102;
		vdmbuf[n++] = vdm4(4, p0);
		vdmbuf[n++] = 0x12022206;
		vdmbuf[n++] = 0x81c00000 | 6;		/* 6 vertices */
		vdmbuf[n++] = t.idx;
		vdmbuf[n++] = 0x70000000;
		vdmbuf[n++] = 0x003fffff;
		vdmbuf[n++] = vdm4(9, fb);
		vdmbuf[n++] = 0x07800604;
		for (i = 0; i < 5; i++)
			vdmbuf[n++] = tail[i];
		put(vdm->va, vdmbuf, n * 4);
	}

	/* submit and wait for the fence on the target */
	for (i = 0; i < nbo; i++)
		handles[i] = bo[i].handle;
	{
		struct drm_apple_sgx_submit s = {
			.cmd = (uintptr_t)cmd,
			.cmd_size = cmd[0],
			.pb_va = 0,
			.details_handle = rtb->handle,
			.details_offset = rt_va[SGX_RT_DETAILS] - rtb->va,
			.bo_handles = (uintptr_t)handles,
			.bo_handle_count = nbo,
		};

		printf("submitting a %ux%u clear: TA command %u bytes, target GPU 0x%08x, "
		       "details 0x%08x, 3D block 0x%08x\n", w, h, s.cmd_size,
		       target->va, rt_va[SGX_RT_DETAILS], blk_va);
		fflush(stdout);
		xioctl(sgxfd, DRM_IOCTL_APPLE_SGX_SUBMIT, &s, "SUBMIT");
	}
	{
		struct drm_apple_sgx_gem_wait gw = {
			.handle = target->handle,
			.timeout_ns = (int64_t)(now_ns() + 5000000000ull),
		};

		if (ioctl(sgxfd, DRM_IOCTL_APPLE_SGX_GEM_WAIT, &gw) < 0) {
			fprintf(stderr, "sgxframe: the render did not finish: %s\n",
				strerror(errno));
			fprintf(stderr, "sgxframe: see dmesg and "
				"/sys/kernel/debug/apple-sgx/regs\n");
			return 1;
		}
	}

	/* every checked pixel is the colour asked for */
	ret = 0;
	{
		uint32_t *px = target->map;
		static const unsigned int at[][2] = {
			{ 1, 1 }, { 640, 360 }, { 1278, 718 }, { 3, 700 }, { 1200, 5 },
		};

		for (i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
			unsigned int x = at[i][0] % w, y = at[i][1] % h;
			uint32_t got = px[y * w + x];

			if (got != colour) {
				fprintf(stderr, "sgxframe: (%u,%u) is 0x%08x, wanted 0x%08x\n",
					x, y, got, colour);
				ret = 1;
			}
		}
	}
	printf("read back: %s\n", ret ? "MISMATCH" : "every checked pixel is right");

	if (!ret && seconds > 0) {
		struct drm_apple_sgx_blit b = { 0 };
		uint32_t fb_va = PARAM(APPLE_SGX_PARAM_FB_VA);
		uint32_t fb_w = PARAM(APPLE_SGX_PARAM_FB_WIDTH);
		uint32_t fb_h = PARAM(APPLE_SGX_PARAM_FB_HEIGHT);
		uint32_t fb_stride = PARAM(APPLE_SGX_PARAM_FB_STRIDE);

		if (!fb_va || !fb_w || !fb_h) {
			fprintf(stderr, "sgxframe: -s: the display is not scanning "
				"anything out\n");
			return 1;
		}
		b.src_handle = target->handle;
		b.dst_handle = 0;	/* the screen */
		b.src_width = w;
		b.src_height = h;
		b.src_stride = w * 4;
		b.dst_width = fb_w;
		b.dst_height = fb_h;
		b.dst_stride = fb_stride;
		xioctl(sgxfd, DRM_IOCTL_APPLE_SGX_BLIT, &b, "BLIT(screen)");
		printf("on the screen for %d s\n", seconds);
		fflush(stdout);
		sleep(seconds);
	}

	close(sgxfd);
	return ret;
}
