// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X M2 scaler: memory-to-memory scaling and colour-space conversion
 *
 * The SoC has two identical instances, each behind its own PMGR clock gate.
 * The register model was measured on hardware (block version 0x00070000):
 *
 *   0x004  control: bit 0 start, bit 1 soft reset (self-clearing), bit 8
 *          colour conversion, bit 9 add output offsets (16/128), bit 10
 *          remove input offsets
 *   0x008  done interrupt enable, 0x00c done status (write 1 to clear)
 *   0x010  source format, 0x014/0x018 plane addresses, 0x01c strides,
 *          0x020/0x028 luma/chroma start (14.2 fixed point), 0x024 size
 *   0x030  destination format, 0x034/0x038 addresses, 0x03c strides,
 *          0x040 size, 0x044 start
 *   0x050  horizontal and 0x054 vertical step, source/destination in 3.16
 *   0x058  rotation and mirroring
 *   0x060  polyphase filters: horizontal luma (8 taps), 0x0f0 vertical
 *          luma (4 taps), 0x140 horizontal chroma, 0x1d0 vertical chroma;
 *          9 of 16 phases are stored, the hardware mirrors the rest
 *   0x220  3x3 conversion matrix, 12-bit two's complement, 0x200 = 1.0
 *   0x250  dither, 0x260 version
 *
 * Plane addresses are 32-bit physical addresses (the IOMMU in front of the
 * block is left in bypass). Crop offsets are folded into the addresses, so
 * the start registers stay zero.
 *
 * A source crop rectangle (OUTPUT, V4L2_SEL_TGT_CROP) selects the part of
 * the source that is scaled, and a compose rectangle (CAPTURE,
 * V4L2_SEL_TGT_COMPOSE) places the result inside the destination frame,
 * for example to letterbox an image. Destination pixels outside the
 * compose rectangle are left untouched.
 *
 * Each instance is a V4L2 memory-to-memory device: the OUTPUT queue takes
 * the source frames and the CAPTURE queue receives the scaled, converted
 * result, both as single contiguous buffers (NV12 and NV16 with the CbCr
 * plane right after the luma plane). Jobs run one at a time per instance
 * from a work item, which sleeps on the done interrupt.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define M2SC_NAME		"s5l8947x-m2sc"

#define M2SC_STATUS		0x000
#define M2SC_CTRL		0x004
#define M2SC_CTRL_START		BIT(0)
#define M2SC_CTRL_RESET		BIT(1)
#define M2SC_CTRL_CSC		BIT(8)
#define M2SC_CTRL_OUT_OFFSETS	BIT(9)
#define M2SC_CTRL_IN_OFFSETS	BIT(10)
#define M2SC_CTRL_CSC_MASK	GENMASK(10, 8)
#define M2SC_IRQ_EN		0x008
#define M2SC_IRQ_STAT		0x00c
#define M2SC_IRQ_DONE		BIT(0)

#define M2SC_SRC_CFG		0x010
#define M2SC_SRC_ADDR0		0x014
#define M2SC_SRC_ADDR1		0x018
#define M2SC_SRC_STRIDE		0x01c
#define M2SC_SRC_START		0x020
#define M2SC_SRC_SIZE		0x024
#define M2SC_SRC_CSTART		0x028
#define M2SC_DST_CFG		0x030
#define M2SC_DST_ADDR0		0x034
#define M2SC_DST_ADDR1		0x038
#define M2SC_DST_STRIDE		0x03c
#define M2SC_DST_SIZE		0x040
#define M2SC_DST_START		0x044
#define M2SC_CFG_FORMAT		GENMASK(3, 0)
#define M2SC_CFG_SWAP_RB	BIT(6)
#define M2SC_CFG_TILED		BIT(10)
#define M2SC_CFG_MASK		(M2SC_CFG_FORMAT | GENMASK(6, 5) | M2SC_CFG_TILED)

#define M2SC_HSTEP		0x050
#define M2SC_VSTEP		0x054
#define M2SC_STEP_MASK		GENMASK(18, 0)
#define M2SC_ROT		0x058
#define M2SC_ROT_MASK		GENMASK(3, 0)

#define M2SC_COEF_H_LUMA	0x060
#define M2SC_COEF_V_LUMA	0x0f0
#define M2SC_COEF_H_CHROMA	0x140
#define M2SC_COEF_V_CHROMA	0x1d0
#define M2SC_PHASES		9	/* stored phases out of 16 */

#define M2SC_CSC(i)		(0x220 + 4 * (i))
#define M2SC_DITHER		0x250
#define M2SC_VERSION		0x260

#define M2SC_MAX_SRC		16384
#define M2SC_MAX_DST		4096
#define M2SC_MIN_SIZE		16
#define M2SC_TIMEOUT_MS		500

/* Pixel formats with their hardware codes */
struct m2sc_fmt {
	u32 fourcc;
	u8 hw;
	u8 planes;
	u8 cpp;		/* bytes per pixel in plane 0 */
	u8 hsub;	/* chroma subsampling (YCbCr only) */
	u8 vsub;
	bool yuv;
};

static const struct m2sc_fmt m2sc_formats[] = {
	{ V4L2_PIX_FMT_NV12,   0x0, 2, 1, 2, 2, true },
	{ V4L2_PIX_FMT_NV16,   0x2, 2, 1, 2, 1, true },
	{ V4L2_PIX_FMT_YUYV,   0xa, 1, 2, 2, 1, true },
	{ V4L2_PIX_FMT_RGB565, 0x4, 1, 2, 1, 1, false },
	{ V4L2_PIX_FMT_XRGB555, 0x5, 1, 2, 1, 1, false },
	{ V4L2_PIX_FMT_XRGB32, 0x6, 1, 4, 1, 1, false },
};

static const struct m2sc_fmt *m2sc_find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(m2sc_formats); i++)
		if (m2sc_formats[i].fourcc == fourcc)
			return &m2sc_formats[i];
	return NULL;
}

/* One plane-addressed frame */
struct m2sc_surface {
	const struct m2sc_fmt *fmt;
	u32 width;
	u32 height;
	u32 pitch[2];		/* bytes per row of each plane */
	dma_addr_t addr[2];
	bool limited;		/* YCbCr with 16..235 / 16..240 range */
};

struct m2sc_job {
	struct m2sc_surface src;
	struct m2sc_surface dst;
	bool bt709;
};

struct m2sc_dev {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	int irq;
	struct mutex lock;	/* one job at a time */
	struct completion done;
	u32 version;
	struct dentry *debugfs;

	struct v4l2_device v4l2;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m;
	struct mutex vdev_lock;	/* serialises ioctls */
	struct work_struct work;
};

/* Per-open state: index 0 is the source (OUTPUT), 1 the destination (CAPTURE) */
struct m2sc_q_data {
	const struct m2sc_fmt *fmt;
	struct v4l2_pix_format pix;
	struct v4l2_rect rect;	/* crop (source) or compose (destination) */
	u32 sequence;
};

struct m2sc_ctx {
	struct v4l2_fh fh;
	struct m2sc_dev *m;
	struct m2sc_q_data q[2];
};

static struct m2sc_ctx *fh_to_ctx(struct file *file)
{
	return container_of(file->private_data, struct m2sc_ctx, fh);
}

/*
 * Colour conversion matrices, rows are output components and columns input
 * components ((Y, Cb, Cr) or (R, G, B)), in 1.9 fixed point. They are the
 * BT.601 and BT.709 equations scaled by 512 and rounded (for example
 * 1.164 -> 596); the 16/128 offsets are applied by control bits.
 */
static const s16 m2sc_yuv2rgb[2][2][9] = {
	{	/* BT.601 */
		{ 596, 0, 817, 596, -200, -416,
		  596, 1033, 0 },				/* limited */
		{ 512, 0, 718, 512, -176, -366,
		  512, 907, 0 },				/* full */
	},
	{	/* BT.709 */
		{ 596, 0, 918, 596, -109, -273,
		  596, 1081, 0 },
		{ 512, 0, 806, 512, -96, -240,
		  512, 950, 0 },
	},
};

static const s16 m2sc_rgb2yuv[2][2][9] = {
	{	/* BT.601 */
		{ 132, 258, 50, -76, -149, 225,
		  225, -188, -36 },
		{ 153, 301, 58, -87, -169, 256,
		  256, -215, -41 },
	},
	{	/* BT.709 */
		{ 94, 314, 32, -52, -174, 225,
		  225, -204, -20 },
		{ 109, 366, 37, -59, -197, 256,
		  256, -232, -24 },
	},
};

static const s16 m2sc_identity[9] = { 512, 0, 0, 0, 512, 0, 0, 0, 512 };

static int m2sc_reset(struct m2sc_dev *m)
{
	u32 v;

	writel(readl(m->base + M2SC_CTRL) | M2SC_CTRL_RESET, m->base + M2SC_CTRL);
	if (readl_poll_timeout(m->base + M2SC_CTRL, v, !(v & M2SC_CTRL_RESET), 10, 1000))
		return -ETIMEDOUT;
	writel(readl(m->base + M2SC_SRC_CFG) | BIT(0), m->base + M2SC_SRC_CFG);
	if (readl_poll_timeout(m->base + M2SC_SRC_CFG, v, v & BIT(0), 10, 1000))
		return -ETIMEDOUT;
	writel(M2SC_IRQ_DONE, m->base + M2SC_IRQ_STAT);
	return 0;
}

/*
 * Polyphase filters, generated for each job from its scaling ratio. A tap's
 * weight is the overlap between its source pixel and the footprint of the
 * destination pixel, which is r source pixels wide (r = max(ratio, 1)). At
 * ratio 1 and when upscaling this is linear interpolation; when
 * downscaling it averages the covered area, so detail finer than a
 * destination pixel is blended instead of aliased. A footprint wider than
 * the taps reach (beyond 6x horizontally, 2x vertically) is clipped.
 *
 * Phase p (0..8 of 16) puts the sample p/16 of a pixel before the centre
 * tap (tap 4 of 8 horizontally, tap 2 of 4 vertically). Taps are 2.7 fixed
 * point and sum to 128; rounding error goes to the largest tap.
 */
static void m2sc_make_filter(u32 step, unsigned int taps, unsigned int p, s16 *c)
{
	s32 half = max_t(u32, step, 1 << 16) / 2;	/* 16.16 */
	unsigned int k, big = taps / 2;
	u32 w[8], sum = 0;
	int total = 0;

	for (k = 0; k < taps; k++) {
		s32 d = ((s32)k - (s32)(taps / 2)) * 65536 + (s32)p * 4096;
		s32 lo = max(d - 32768, -half), hi = min(d + 32768, half);

		w[k] = hi > lo ? hi - lo : 0;
		sum += w[k];
	}
	for (k = 0; k < taps; k++) {
		c[k] = (w[k] * 128 + sum / 2) / sum;
		total += c[k];
		if (w[k] > w[big])
			big = k;
	}
	c[big] += 128 - total;
}

static u32 m2sc_pair(const s16 *c, unsigned int k)
{
	return (u16)c[k] | (u32)(u16)c[k + 1] << 16;
}

static void m2sc_write_filters(struct m2sc_dev *m, u32 hstep, u32 vstep)
{
	static const u32 h_banks[] = { M2SC_COEF_H_LUMA, M2SC_COEF_H_CHROMA };
	static const u32 v_banks[] = { M2SC_COEF_V_LUMA, M2SC_COEF_V_CHROMA };
	unsigned int b, p;
	s16 hc[8], vc[4];

	for (p = 0; p < M2SC_PHASES; p++) {
		m2sc_make_filter(hstep, 8, p, hc);
		m2sc_make_filter(vstep, 4, p, vc);
		for (b = 0; b < 2; b++) {
			void __iomem *h = m->base + h_banks[b] + 16 * p;
			void __iomem *v = m->base + v_banks[b] + 8 * p;

			/* words hold taps (6,7) (4,5) (2,3) (0,1), low half first */
			writel(m2sc_pair(hc, 6), h);
			writel(m2sc_pair(hc, 4), h + 4);
			writel(m2sc_pair(hc, 2), h + 8);
			writel(m2sc_pair(hc, 0), h + 12);
			/* words hold taps (2,3) (0,1) */
			writel(m2sc_pair(vc, 2), v);
			writel(m2sc_pair(vc, 0), v + 4);
		}
	}
}

static void m2sc_write_matrix(struct m2sc_dev *m, const s16 *mat)
{
	unsigned int i;

	for (i = 0; i < 9; i++)
		writel((u16)mat[i] & 0xfff, m->base + M2SC_CSC(i));
}

/* Strides as the block wants them: plane 0 in pixels, plane 1 in bytes */
static u32 m2sc_stride(const struct m2sc_surface *s, bool dst)
{
	u32 px = s->pitch[0] / s->fmt->cpp;

	if (s->fmt->planes == 1)
		return dst ? px | px << 16 : px;
	return px | s->pitch[1] << 16;
}

static int m2sc_check(const struct m2sc_job *j)
{
	const struct m2sc_surface *s = &j->src, *d = &j->dst;

	if (!s->fmt || !d->fmt)
		return -EINVAL;
	if (s->width < M2SC_MIN_SIZE || s->height < M2SC_MIN_SIZE ||
	    s->width > M2SC_MAX_SRC || s->height > M2SC_MAX_SRC ||
	    d->width < M2SC_MIN_SIZE || d->height < M2SC_MIN_SIZE ||
	    d->width > M2SC_MAX_DST || d->height > M2SC_MAX_DST)
		return -ERANGE;
	/*
	 * Steps are 3.16 fixed point, so at most 8x down. Vertically the
	 * block reads the wrong source lines beyond exactly 7x (measured: a
	 * step of 7.000 works, 7.027 does not), so stop there.
	 */
	if (s->width >= 8 * d->width || s->height > 7 * d->height)
		return -ERANGE;
	if (s->addr[0] > U32_MAX || d->addr[0] > U32_MAX ||
	    s->addr[1] > U32_MAX || d->addr[1] > U32_MAX)
		return -EFAULT;
	return 0;
}

/* Run one job and wait for it. Returns 0 or a negative error. */
static int m2sc_run(struct m2sc_dev *m, const struct m2sc_job *j)
{
	const struct m2sc_surface *s = &j->src, *d = &j->dst;
	u32 ctrl, hstep, vstep;
	const s16 *mat;
	int ret;

	ret = m2sc_check(j);
	if (ret)
		return ret;

	mutex_lock(&m->lock);
	ret = clk_prepare_enable(m->clk);
	if (ret)
		goto unlock;
	ret = m2sc_reset(m);
	if (ret) {
		dev_err(m->dev, "reset timed out\n");
		goto off;
	}

	writel((readl(m->base + M2SC_SRC_CFG) & ~M2SC_CFG_MASK) | s->fmt->hw,
	       m->base + M2SC_SRC_CFG);
	writel(s->addr[0], m->base + M2SC_SRC_ADDR0);
	writel(s->fmt->planes > 1 ? s->addr[1] : 0, m->base + M2SC_SRC_ADDR1);
	writel(m2sc_stride(s, false), m->base + M2SC_SRC_STRIDE);
	writel(0, m->base + M2SC_SRC_START);
	writel(0, m->base + M2SC_SRC_CSTART);
	writel(s->height | s->width << 16, m->base + M2SC_SRC_SIZE);

	writel((readl(m->base + M2SC_DST_CFG) & ~M2SC_CFG_MASK) | d->fmt->hw,
	       m->base + M2SC_DST_CFG);
	writel(d->addr[0], m->base + M2SC_DST_ADDR0);
	writel(d->fmt->planes > 1 ? d->addr[1] : 0, m->base + M2SC_DST_ADDR1);
	writel(m2sc_stride(d, true), m->base + M2SC_DST_STRIDE);
	writel(0, m->base + M2SC_DST_START);
	writel(d->height | d->width << 16, m->base + M2SC_DST_SIZE);

	if (s->fmt->yuv && !d->fmt->yuv)
		mat = m2sc_yuv2rgb[j->bt709][!s->limited];
	else if (!s->fmt->yuv && d->fmt->yuv)
		mat = m2sc_rgb2yuv[j->bt709][!d->limited];
	else
		mat = m2sc_identity;
	m2sc_write_matrix(m, mat);
	ctrl = readl(m->base + M2SC_CTRL) & ~(M2SC_CTRL_CSC_MASK | M2SC_CTRL_START);
	ctrl |= M2SC_CTRL_CSC;
	if (s->fmt->yuv && s->limited)
		ctrl |= M2SC_CTRL_IN_OFFSETS;
	if (d->fmt->yuv && d->limited)
		ctrl |= M2SC_CTRL_OUT_OFFSETS;
	writel(ctrl, m->base + M2SC_CTRL);

	hstep = ((s->width << 16) / d->width) & M2SC_STEP_MASK;
	vstep = ((s->height << 16) / d->height) & M2SC_STEP_MASK;
	writel(hstep, m->base + M2SC_HSTEP);
	writel(vstep, m->base + M2SC_VSTEP);
	m2sc_write_filters(m, hstep, vstep);
	writel(0, m->base + M2SC_DITHER);
	writel(readl(m->base + M2SC_ROT) & ~M2SC_ROT_MASK, m->base + M2SC_ROT);

	reinit_completion(&m->done);
	writel(M2SC_IRQ_DONE, m->base + M2SC_IRQ_EN);
	writel(ctrl | M2SC_CTRL_START, m->base + M2SC_CTRL);

	if (!wait_for_completion_timeout(&m->done, msecs_to_jiffies(M2SC_TIMEOUT_MS))) {
		dev_err(m->dev, "job timed out: status %08x ctrl %08x irq %08x\n",
			readl(m->base + M2SC_STATUS), readl(m->base + M2SC_CTRL),
			readl(m->base + M2SC_IRQ_STAT));
		ret = -ETIMEDOUT;
	}
	writel(0, m->base + M2SC_IRQ_EN);
	if (ret)
		m2sc_reset(m);
off:
	clk_disable_unprepare(m->clk);
unlock:
	mutex_unlock(&m->lock);
	return ret;
}

static irqreturn_t m2sc_irq(int irq, void *data)
{
	struct m2sc_dev *m = data;

	if (!(readl(m->base + M2SC_IRQ_STAT) & M2SC_IRQ_DONE))
		return IRQ_NONE;
	writel(M2SC_IRQ_DONE, m->base + M2SC_IRQ_STAT);
	complete(&m->done);
	return IRQ_HANDLED;
}

/* ---- debugfs self-test ---------------------------------------------------- */

struct m2sc_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

static int m2sc_buf_alloc(struct m2sc_dev *m, struct m2sc_buf *b, size_t size)
{
	b->size = size;
	b->cpu = dma_alloc_coherent(m->dev, size, &b->dma, GFP_KERNEL);
	return b->cpu ? 0 : -ENOMEM;
}

static void m2sc_buf_free(struct m2sc_dev *m, struct m2sc_buf *b)
{
	if (b->cpu)
		dma_free_coherent(m->dev, b->size, b->cpu, b->dma);
	b->cpu = NULL;
}

static bool m2sc_near(u32 px, u32 want, int tol)
{
	int i;

	for (i = 0; i < 24; i += 8)
		if (abs((int)((px >> i) & 0xff) - (int)((want >> i) & 0xff)) > tol)
			return false;
	return true;
}

/*
 * Count destination pixels that do not match the expected colour, skipping a
 * band of `edge` pixels around the boundary between regions, where the
 * filter blends the two colours.
 */
static u32 m2sc_count_bad(const u32 *px, u32 w, u32 h, u32 split_x, u32 split_y,
			  const u32 want[4], u32 edge, u32 *untouched, u32 *alpha)
{
	u32 x, y, bad = 0;

	*untouched = 0;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			u32 v = px[y * w + x];
			unsigned int q = (x >= split_x) | (y >= split_y) << 1;

			if (v == 0xdeadbeef)
				(*untouched)++;
			if (x == w / 2 && y == h / 2)
				*alpha = v >> 24;
			if (abs((int)x - (int)split_x) < edge || abs((int)y - (int)split_y) < edge)
				continue;
			if (!m2sc_near(v, want[q], 6))
				bad++;
		}
	}
	return bad;
}

static int m2sc_selftest_show(struct seq_file *sf, void *unused)
{
	struct m2sc_dev *m = sf->private;
	struct m2sc_buf src = {}, dst = {};
	struct m2sc_job j = {};
	u32 x, y, bad, untouched, alpha = 0, *px;
	ktime_t t0;
	s64 us;
	u8 *yp, *cp;
	int ret;

	seq_printf(sf, "version %08x\n", m->version);

	/*
	 * Test 1: NV12 640x360 -> XRGB8888 1280x720 (2x up, BT.601 limited).
	 * Left half (Y 81, Cb 90, Cr 240) is red, right half (Y 145, Cb 54,
	 * Cr 34) is green.
	 */
	ret = m2sc_buf_alloc(m, &src, 640 * 360 * 3 / 2);
	if (!ret)
		ret = m2sc_buf_alloc(m, &dst, 1280 * 720 * 4);
	if (ret)
		goto out;
	yp = src.cpu;
	cp = yp + 640 * 360;
	for (y = 0; y < 360; y++) {
		memset(yp + y * 640, 81, 320);
		memset(yp + y * 640 + 320, 145, 320);
	}
	for (y = 0; y < 180; y++)
		for (x = 0; x < 320; x++) {
			u8 *c = cp + y * 640 + 2 * x;

			c[0] = x < 160 ? 90 : 54;
			c[1] = x < 160 ? 240 : 34;
		}
	px = dst.cpu;
	for (x = 0; x < 1280 * 720; x++)
		px[x] = 0xdeadbeef;

	j.src = (struct m2sc_surface){ m2sc_find_format(V4L2_PIX_FMT_NV12), 640, 360,
				       { 640, 640 }, { src.dma, src.dma + 640 * 360 }, true };
	j.dst = (struct m2sc_surface){ m2sc_find_format(V4L2_PIX_FMT_XRGB32), 1280, 720,
				       { 1280 * 4, 0 }, { dst.dma, 0 }, false };
	t0 = ktime_get();
	ret = m2sc_run(m, &j);
	us = ktime_us_delta(ktime_get(), t0);
	seq_printf(sf, "nv12 640x360 @%pad -> xrgb 1280x720 @%pad: %d in %lld us\n",
		   &src.dma, &dst.dma, ret, us);
	if (!ret) {
		static const u32 want[4] = { 0xff0000, 0x00ff00, 0xff0000, 0x00ff00 };

		bad = m2sc_count_bad(px, 1280, 720, 640, 720, want, 8, &untouched, &alpha);
		seq_printf(sf, "  pixels: %u off by more than 6, %u not written, alpha %02x\n",
			   bad, untouched, alpha);
		seq_printf(sf, "  samples: (320,360) %08x (960,360) %08x (0,0) %08x (1279,719) %08x\n",
			   px[360 * 1280 + 320], px[360 * 1280 + 960], px[0], px[1280 * 720 - 1]);
		seq_printf(sf, "  %s\n", bad || untouched ? "FAIL" : "PASS");
	}
	m2sc_buf_free(m, &src);
	m2sc_buf_free(m, &dst);
	if (ret)
		goto out;

	/*
	 * Test 2: XRGB8888 1280x720 -> XRGB8888 640x360 (2x down, no colour
	 * conversion), four quadrants of different colours.
	 */
	ret = m2sc_buf_alloc(m, &src, 1280 * 720 * 4);
	if (!ret)
		ret = m2sc_buf_alloc(m, &dst, 640 * 360 * 4);
	if (ret)
		goto out;
	{
		static const u32 quad[4] = { 0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff };
		u32 *s = src.cpu;

		for (y = 0; y < 720; y++)
			for (x = 0; x < 1280; x++)
				s[y * 1280 + x] = quad[(x >= 640) | (y >= 360) << 1];
		px = dst.cpu;
		for (x = 0; x < 640 * 360; x++)
			px[x] = 0xdeadbeef;
		j.src = (struct m2sc_surface){ m2sc_find_format(V4L2_PIX_FMT_XRGB32), 1280, 720,
					       { 1280 * 4, 0 }, { src.dma, 0 }, false };
		j.dst = (struct m2sc_surface){ m2sc_find_format(V4L2_PIX_FMT_XRGB32), 640, 360,
					       { 640 * 4, 0 }, { dst.dma, 0 }, false };
		t0 = ktime_get();
		ret = m2sc_run(m, &j);
		us = ktime_us_delta(ktime_get(), t0);
		seq_printf(sf, "xrgb 1280x720 -> xrgb 640x360: %d in %lld us\n", ret, us);
		if (!ret) {
			bad = m2sc_count_bad(px, 640, 360, 320, 180, quad, 4, &untouched, &alpha);
			seq_printf(sf, "  pixels: %u off by more than 6, %u not written\n",
				   bad, untouched);
			seq_printf(sf, "  samples: %08x %08x %08x %08x\n", px[90 * 640 + 160],
				   px[90 * 640 + 480], px[270 * 640 + 160], px[270 * 640 + 480]);
			seq_printf(sf, "  %s\n", bad || untouched ? "FAIL" : "PASS");
		}
	}
out:
	m2sc_buf_free(m, &src);
	m2sc_buf_free(m, &dst);
	if (ret)
		seq_printf(sf, "error %d\n", ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(m2sc_selftest);

/* ---- V4L2 memory-to-memory interface ----------------------------------------- */

static bool m2sc_is_dst(enum v4l2_buf_type type)
{
	return type == V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

/* Clamp a format to what the block and the contiguous-buffer layout support */
static const struct m2sc_fmt *m2sc_try_pix(struct v4l2_pix_format *pix, bool dst)
{
	const struct m2sc_fmt *f = m2sc_find_format(pix->pixelformat);
	u32 max = dst ? M2SC_MAX_DST : M2SC_MAX_SRC;
	u32 bpl;

	if (!f)
		f = m2sc_find_format(dst ? V4L2_PIX_FMT_XRGB32 : V4L2_PIX_FMT_NV12);
	pix->pixelformat = f->fourcc;
	pix->width = clamp(pix->width, M2SC_MIN_SIZE, max);
	pix->height = clamp(pix->height, M2SC_MIN_SIZE, max);
	pix->width = ALIGN(pix->width, f->hsub);
	pix->height = ALIGN(pix->height, f->vsub);

	/* Plane 0 stride is programmed in pixels, plane 1 in bytes, 16 bits each */
	bpl = max(pix->bytesperline, pix->width * f->cpp);
	bpl = min_t(u32, ALIGN(bpl, f->cpp * f->hsub), 0xffff);
	pix->bytesperline = bpl;
	pix->sizeimage = bpl * pix->height;
	if (f->planes > 1)
		pix->sizeimage += bpl * pix->height / f->vsub;
	pix->field = V4L2_FIELD_NONE;

	if (pix->colorspace == V4L2_COLORSPACE_DEFAULT)
		pix->colorspace = f->yuv ? V4L2_COLORSPACE_SMPTE170M : V4L2_COLORSPACE_SRGB;
	if (pix->ycbcr_enc != V4L2_YCBCR_ENC_601 && pix->ycbcr_enc != V4L2_YCBCR_ENC_709)
		pix->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(pix->colorspace);
	if (pix->ycbcr_enc != V4L2_YCBCR_ENC_709)
		pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	if (pix->quantization == V4L2_QUANTIZATION_DEFAULT)
		pix->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(!f->yuv, pix->colorspace,
								  pix->ycbcr_enc);
	if (!f->yuv)
		pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(pix->colorspace);
	return f;
}

/* Describe the rectangle of a buffer, offsets folded into the plane addresses */
static void m2sc_surface_from(struct m2sc_surface *s, const struct m2sc_q_data *q,
			      struct vb2_buffer *vb)
{
	const struct v4l2_pix_format *pix = &q->pix;
	const struct v4l2_rect *r = &q->rect;
	dma_addr_t base = vb2_dma_contig_plane_dma_addr(vb, 0);

	s->fmt = q->fmt;
	s->width = r->width;
	s->height = r->height;
	s->pitch[0] = pix->bytesperline;
	s->pitch[1] = q->fmt->planes > 1 ? pix->bytesperline : 0;
	s->addr[0] = base + (dma_addr_t)r->top * s->pitch[0] + r->left * q->fmt->cpp;
	/* the CbCr plane has one byte pair per two luma columns */
	s->addr[1] = base + (dma_addr_t)pix->bytesperline * pix->height +
		     (dma_addr_t)(r->top / q->fmt->vsub) * s->pitch[1] + r->left;
	s->limited = q->fmt->yuv && pix->quantization == V4L2_QUANTIZATION_LIM_RANGE;
}

static void m2sc_work(struct work_struct *work)
{
	struct m2sc_dev *m = container_of(work, struct m2sc_dev, work);
	struct m2sc_ctx *ctx = v4l2_m2m_get_curr_priv(m->m2m);
	struct vb2_v4l2_buffer *src, *dst;
	enum vb2_buffer_state state;
	struct m2sc_job job = {};
	const struct m2sc_q_data *yq;
	int ret;

	if (!ctx)
		return;
	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);
	m2sc_surface_from(&job.src, &ctx->q[0], &src->vb2_buf);
	m2sc_surface_from(&job.dst, &ctx->q[1], &dst->vb2_buf);
	yq = ctx->q[0].fmt->yuv ? &ctx->q[0] : &ctx->q[1];
	job.bt709 = yq->pix.ycbcr_enc == V4L2_YCBCR_ENC_709;

	ret = m2sc_run(m, &job);
	if (ret)
		dev_err_ratelimited(m->dev, "job failed: %d\n", ret);
	state = ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
	v4l2_m2m_buf_copy_metadata(src, dst, true);
	src->sequence = ctx->q[0].sequence++;
	dst->sequence = ctx->q[1].sequence++;
	vb2_set_plane_payload(&dst->vb2_buf, 0, ctx->q[1].pix.sizeimage);
	v4l2_m2m_buf_done(src, state);
	v4l2_m2m_buf_done(dst, state);
	v4l2_m2m_job_finish(m->m2m, ctx->fh.m2m_ctx);
}

static void m2sc_device_run(void *priv)
{
	struct m2sc_ctx *ctx = priv;

	queue_work(system_highpri_wq, &ctx->m->work);
}

static const struct v4l2_m2m_ops m2sc_m2m_ops = {
	.device_run = m2sc_device_run,
};

static int m2sc_queue_setup(struct vb2_queue *vq, unsigned int *nbufs,
			    unsigned int *nplanes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct m2sc_ctx *ctx = vb2_get_drv_priv(vq);
	u32 size = ctx->q[m2sc_is_dst(vq->type)].pix.sizeimage;

	if (*nplanes)
		return *nplanes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int m2sc_buf_prepare(struct vb2_buffer *vb)
{
	struct m2sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	u32 size = ctx->q[m2sc_is_dst(vb->vb2_queue->type)].pix.sizeimage;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	/* The block only drives 32 address bits */
	if (vb2_dma_contig_plane_dma_addr(vb, 0) + size - 1 > U32_MAX)
		return -EFAULT;
	if (m2sc_is_dst(vb->vb2_queue->type))
		vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void m2sc_buf_queue(struct vb2_buffer *vb)
{
	struct m2sc_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int m2sc_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct m2sc_ctx *ctx = vb2_get_drv_priv(vq);

	ctx->q[m2sc_is_dst(vq->type)].sequence = 0;
	return 0;
}

static void m2sc_stop_streaming(struct vb2_queue *vq)
{
	struct m2sc_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vb;

	for (;;) {
		vb = m2sc_is_dst(vq->type) ? v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx) :
					     v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		if (!vb)
			break;
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops m2sc_qops = {
	.queue_setup	 = m2sc_queue_setup,
	.buf_prepare	 = m2sc_buf_prepare,
	.buf_queue	 = m2sc_buf_queue,
	.start_streaming = m2sc_start_streaming,
	.stop_streaming	 = m2sc_stop_streaming,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int m2sc_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct m2sc_ctx *ctx = priv;
	struct vb2_queue *qs[2] = { src, dst };
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		struct vb2_queue *q = qs[i];

		q->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
		q->io_modes = VB2_MMAP | VB2_DMABUF;
		q->drv_priv = ctx;
		q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q->ops = &m2sc_qops;
		q->mem_ops = &vb2_dma_contig_memops;
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q->lock = &ctx->m->vdev_lock;
		q->dev = ctx->m->dev;
		ret = vb2_queue_init(q);
		if (ret)
			return ret;
	}
	return 0;
}

static int m2sc_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, M2SC_NAME, sizeof(cap->driver));
	strscpy(cap->card, "S5L8947X M2 scaler", sizeof(cap->card));
	return 0;
}

static int m2sc_enum_fmt(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(m2sc_formats))
		return -EINVAL;
	f->pixelformat = m2sc_formats[f->index].fourcc;
	return 0;
}

static int m2sc_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct m2sc_ctx *ctx = fh_to_ctx(file);

	f->fmt.pix = ctx->q[m2sc_is_dst(f->type)].pix;
	return 0;
}

static int m2sc_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	m2sc_try_pix(&f->fmt.pix, m2sc_is_dst(f->type));
	return 0;
}

static int m2sc_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct m2sc_ctx *ctx = fh_to_ctx(file);
	struct m2sc_q_data *q = &ctx->q[m2sc_is_dst(f->type)];
	struct vb2_queue *vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);

	if (vb2_is_busy(vq))
		return -EBUSY;
	q->fmt = m2sc_try_pix(&f->fmt.pix, m2sc_is_dst(f->type));
	q->pix = f->fmt.pix;
	q->rect = (struct v4l2_rect){ 0, 0, q->pix.width, q->pix.height };
	return 0;
}

/* Source: crop targets on OUTPUT. Destination: compose targets on CAPTURE. */
static int m2sc_sel_queue(u32 type, u32 target)
{
	switch (target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		return type == V4L2_BUF_TYPE_VIDEO_OUTPUT ? 0 : -EINVAL;
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		return type == V4L2_BUF_TYPE_VIDEO_CAPTURE ? 1 : -EINVAL;
	}
	return -EINVAL;
}

static int m2sc_g_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct m2sc_ctx *ctx = fh_to_ctx(file);
	int i = m2sc_sel_queue(s->type, s->target);

	if (i < 0)
		return i;
	if (s->target == V4L2_SEL_TGT_CROP || s->target == V4L2_SEL_TGT_COMPOSE)
		s->r = ctx->q[i].rect;
	else
		s->r = (struct v4l2_rect){ 0, 0, ctx->q[i].pix.width, ctx->q[i].pix.height };
	return 0;
}

static int m2sc_s_selection(struct file *file, void *fh, struct v4l2_selection *s)
{
	struct m2sc_ctx *ctx = fh_to_ctx(file);
	int i = m2sc_sel_queue(s->type, s->target);
	struct m2sc_q_data *q;
	struct v4l2_rect *r = &s->r;
	u32 w, h;

	if (i < 0)
		return i;
	if (s->target != V4L2_SEL_TGT_CROP && s->target != V4L2_SEL_TGT_COMPOSE)
		return -EINVAL;
	q = &ctx->q[i];
	w = q->pix.width;
	h = q->pix.height;

	/* keep chroma sites whole: even offsets and sizes for subsampled planes */
	r->left = ALIGN_DOWN(clamp_t(s32, r->left, 0, w - M2SC_MIN_SIZE), q->fmt->hsub);
	r->top = ALIGN_DOWN(clamp_t(s32, r->top, 0, h - M2SC_MIN_SIZE), q->fmt->vsub);
	r->width = ALIGN_DOWN(clamp_t(u32, r->width, M2SC_MIN_SIZE, w - r->left),
			      q->fmt->hsub);
	r->height = ALIGN_DOWN(clamp_t(u32, r->height, M2SC_MIN_SIZE, h - r->top),
			       q->fmt->vsub);
	q->rect = *r;
	return 0;
}

static const struct v4l2_ioctl_ops m2sc_ioctl_ops = {
	.vidioc_querycap		= m2sc_querycap,
	.vidioc_enum_fmt_vid_cap	= m2sc_enum_fmt,
	.vidioc_enum_fmt_vid_out	= m2sc_enum_fmt,
	.vidioc_g_fmt_vid_cap		= m2sc_g_fmt,
	.vidioc_g_fmt_vid_out		= m2sc_g_fmt,
	.vidioc_try_fmt_vid_cap		= m2sc_try_fmt,
	.vidioc_try_fmt_vid_out		= m2sc_try_fmt,
	.vidioc_s_fmt_vid_cap		= m2sc_s_fmt,
	.vidioc_s_fmt_vid_out		= m2sc_s_fmt,
	.vidioc_g_selection		= m2sc_g_selection,
	.vidioc_s_selection		= m2sc_s_selection,
	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,
	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,
};

static int m2sc_open(struct file *file)
{
	struct m2sc_dev *m = video_drvdata(file);
	struct m2sc_ctx *ctx;
	unsigned int i;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->m = m;
	for (i = 0; i < 2; i++) {
		struct v4l2_pix_format *pix = &ctx->q[i].pix;

		pix->pixelformat = i ? V4L2_PIX_FMT_XRGB32 : V4L2_PIX_FMT_NV12;
		pix->width = 1280;
		pix->height = 720;
		ctx->q[i].fmt = m2sc_try_pix(pix, i);
		ctx->q[i].rect = (struct v4l2_rect){ 0, 0, pix->width, pix->height };
	}

	if (mutex_lock_interruptible(&m->vdev_lock)) {
		kfree(ctx);
		return -ERESTARTSYS;
	}
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(m->m2m, ctx, m2sc_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		mutex_unlock(&m->vdev_lock);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_add(&ctx->fh);
	mutex_unlock(&m->vdev_lock);
	return 0;
}

static int m2sc_release(struct file *file)
{
	struct m2sc_ctx *ctx = fh_to_ctx(file);
	struct m2sc_dev *m = ctx->m;

	mutex_lock(&m->vdev_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	mutex_unlock(&m->vdev_lock);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations m2sc_fops = {
	.owner		= THIS_MODULE,
	.open		= m2sc_open,
	.release	= m2sc_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int m2sc_v4l2_register(struct m2sc_dev *m)
{
	struct video_device *vdev = &m->vdev;
	int ret;

	ret = v4l2_device_register(m->dev, &m->v4l2);
	if (ret)
		return ret;
	m->m2m = v4l2_m2m_init(&m2sc_m2m_ops);
	if (IS_ERR(m->m2m)) {
		ret = PTR_ERR(m->m2m);
		goto err_v4l2;
	}

	strscpy(vdev->name, M2SC_NAME, sizeof(vdev->name));
	vdev->fops = &m2sc_fops;
	vdev->ioctl_ops = &m2sc_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &m->vdev_lock;
	vdev->v4l2_dev = &m->v4l2;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, m);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	return 0;

err_m2m:
	v4l2_m2m_release(m->m2m);
err_v4l2:
	v4l2_device_unregister(&m->v4l2);
	return ret;
}

static void m2sc_v4l2_unregister(struct m2sc_dev *m)
{
	video_unregister_device(&m->vdev);
	cancel_work_sync(&m->work);
	v4l2_m2m_release(m->m2m);
	v4l2_device_unregister(&m->v4l2);
}

/* ---- platform device -------------------------------------------------------- */

static int m2sc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct m2sc_dev *m;
	int ret;

	m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->dev = dev;
	mutex_init(&m->lock);
	mutex_init(&m->vdev_lock);
	init_completion(&m->done);
	INIT_WORK(&m->work, m2sc_work);

	m->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(m->base))
		return PTR_ERR(m->base);
	/* The register window must not be touched while this clock is off */
	m->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(m->clk))
		return dev_err_probe(dev, PTR_ERR(m->clk), "no clock\n");
	m->irq = platform_get_irq(pdev, 0);
	if (m->irq < 0)
		return m->irq;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = clk_prepare_enable(m->clk);
	if (ret)
		return ret;
	m->version = readl(m->base + M2SC_VERSION);
	writel(0, m->base + M2SC_IRQ_EN);
	writel(M2SC_IRQ_DONE, m->base + M2SC_IRQ_STAT);
	clk_disable_unprepare(m->clk);

	ret = devm_request_irq(dev, m->irq, m2sc_irq, 0, dev_name(dev), m);
	if (ret)
		return ret;

	ret = m2sc_v4l2_register(m);
	if (ret)
		return ret;

	m->debugfs = debugfs_create_dir(dev_name(dev), NULL);
	debugfs_create_file("selftest", 0400, m->debugfs, m, &m2sc_selftest_fops);
	platform_set_drvdata(pdev, m);
	dev_info(dev, "M2 scaler version %08x as %s\n", m->version,
		 video_device_node_name(&m->vdev));
	return 0;
}

static void m2sc_remove(struct platform_device *pdev)
{
	struct m2sc_dev *m = platform_get_drvdata(pdev);

	debugfs_remove_recursive(m->debugfs);
	m2sc_v4l2_unregister(m);
}

static const struct of_device_id m2sc_of_match[] = {
	{ .compatible = "apple,s5l8947x-m2sc" },
	{ }
};
MODULE_DEVICE_TABLE(of, m2sc_of_match);

static struct platform_driver m2sc_driver = {
	.probe	= m2sc_probe,
	.remove	= m2sc_remove,
	.driver	= {
		.name		= "s5l8947x-m2sc",
		.of_match_table	= m2sc_of_match,
	},
};
module_platform_driver(m2sc_driver);

MODULE_DESCRIPTION("S5L8947X M2 scaler and colour-space converter");
MODULE_LICENSE("GPL");
