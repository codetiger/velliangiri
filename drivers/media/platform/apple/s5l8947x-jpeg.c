// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X JPEG codec, used as a baseline JPEG decoder
 *
 * The core parses the JPEG headers itself (quantisation and Huffman tables
 * included): the driver hands it the file and starts it, the core stops
 * with a header-done interrupt and reports the image size and chroma mode,
 * then the driver programs the output planes and lets it decode the body.
 * Register model, measured on hardware (version 0x0001000e):
 *
 *   0x000  mode: bit 3 decode; [2:0] chroma mode after the header
 *          (0 4:4:4, 1 4:2:2, 2 4:2:0, 3 grayscale, 6 4:1:1)
 *   0x00c  reset handshake, 0x078 soft reset (self-clearing)
 *   0x010  height, 0x014 width (from the frame header)
 *   0x01c  interrupt acknowledge, 0x020 status (bit 3 header, bit 4
 *          stream error, bit 6 done)
 *   0x050  output plane 0 address, 0x054 stride; 0x0a4 / 0x0a8 plane 1;
 *          0x0a0 / 0x0ac start offsets
 *   0x058  bitstream address (32-byte aligned), 0x094 its length bound
 *   0x06c  power, 0x070 start (header phase), 0x074 continue (body)
 *   0x07c  watchdog (bit 31 enable), 0x080 bit 31 expired
 *   0x088  output format: [1:0] 0 NV12, 1 YUYV, 3 UYVY
 *   0x08c  version
 *
 * This version has no decode-time downscaling; the M2 scaler does that.
 * Output is always 4:2:0 or packed 4:2:2, whatever the input subsampling.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-dma-contig.h>

#define JPG_NAME		"s5l8947x-jpeg"

#define JPG_MODE		0x000
#define JPG_MODE_DECODE		BIT(3)
#define JPG_MODE_CHROMA		GENMASK(2, 0)
#define JPG_RESET2		0x00c
#define JPG_HEIGHT		0x010
#define JPG_WIDTH		0x014
#define JPG_IRQ_ACK		0x01c
#define JPG_IRQ_ACK_ALL		0x78
#define JPG_STATUS		0x020
#define JPG_ST_HEADER		BIT(3)
#define JPG_ST_ERROR		BIT(4)
#define JPG_ST_DONE		BIT(6)
#define JPG_BS_CTRL		0x028
#define JPG_OUT_ADDR0		0x050
#define JPG_OUT_STRIDE0		0x054
#define JPG_BS_ADDR		0x058
#define JPG_CFG68		0x068
#define JPG_POWER		0x06c
#define JPG_START		0x070
#define JPG_CONTINUE		0x074
#define JPG_RESET		0x078
#define JPG_TIMER		0x07c
#define JPG_TIMER_ENABLE	BIT(31)
#define JPG_TIMER_STAT		0x080
#define JPG_TIMER_EXPIRED	BIT(31)
#define JPG_OUT_FMT		0x088
#define JPG_OUT_FMT_MASK	GENMASK(1, 0)
#define JPG_OUT_FMT_LINEAR	(BIT(8) | BIT(10))	/* cleared: linear output */
#define JPG_VERSION		0x08c
#define JPG_BS_LEN		0x094
#define JPG_BS_LEN_IDLE		0x1ffffffe
#define JPG_OUT_OFF0		0x0a0
#define JPG_OUT_ADDR1		0x0a4
#define JPG_OUT_STRIDE1		0x0a8
#define JPG_OUT_OFF1		0x0ac

#define JPG_MAX_SIZE		8192
#define JPG_MIN_SIZE		16
#define JPG_MIN_BS		257
#define JPG_MAX_BS		(16 << 20)
#define JPG_HEADER_MS		100
#define JPG_BODY_MS		1000

struct jpg_fmt {
	u32 fourcc;
	u8 hw;
	u8 planes;
	u8 cpp;
};

static const struct jpg_fmt jpg_formats[] = {
	{ V4L2_PIX_FMT_NV12, 0, 2, 1 },
	{ V4L2_PIX_FMT_YUYV, 1, 1, 2 },
	{ V4L2_PIX_FMT_UYVY, 3, 1, 2 },
};

static const struct jpg_fmt *jpg_find_format(u32 fourcc)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(jpg_formats); i++)
		if (jpg_formats[i].fourcc == fourcc)
			return &jpg_formats[i];
	return NULL;
}

enum jpg_phase { JPG_IDLE, JPG_HEADER, JPG_BODY };

/* One decode: bitstream in, planes out; the header fields are filled in */
struct jpg_job {
	dma_addr_t bs;
	u32 bs_len;
	const struct jpg_fmt *fmt;
	dma_addr_t out[2];
	u32 stride;
	u32 max_w;
	u32 max_h;
	u32 width;
	u32 height;
	u32 chroma;
	u32 status;
};

struct jpg_dev {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	int irq;
	struct mutex lock;	/* one decode at a time */
	struct completion done;
	enum jpg_phase phase;
	u32 status;
	bool expired;
	u32 version;

	struct v4l2_device v4l2;
	struct video_device vdev;
	struct v4l2_m2m_dev *m2m;
	struct mutex vdev_lock;	/* serialises ioctls */
	struct work_struct work;
};

static int jpg_reset(struct jpg_dev *j)
{
	u32 v;

	writel(readl(j->base + JPG_RESET) | 1, j->base + JPG_RESET);
	if (readl_poll_timeout(j->base + JPG_RESET, v, !v, 1, 1000))
		return -ETIMEDOUT;
	writel(1, j->base + JPG_RESET2);
	if (readl_poll_timeout(j->base + JPG_RESET2, v, v == 1, 1, 1000))
		return -ETIMEDOUT;
	writel(0, j->base + JPG_RESET2);
	writel(JPG_MODE_DECODE, j->base + JPG_MODE);
	return 0;
}

static irqreturn_t jpg_irq(int irq, void *data)
{
	struct jpg_dev *j = data;
	u32 st = readl(j->base + JPG_STATUS);
	bool expired = readl(j->base + JPG_TIMER_STAT) & JPG_TIMER_EXPIRED;

	if (!(st & (JPG_ST_HEADER | JPG_ST_ERROR | JPG_ST_DONE)) && !expired)
		return IRQ_NONE;
	if (expired)
		writel(readl(j->base + JPG_TIMER_STAT) | JPG_TIMER_EXPIRED,
		       j->base + JPG_TIMER_STAT);
	j->status = st;
	j->expired = expired;
	/* The line is released by the acknowledge after the header, else by a reset */
	if (j->phase == JPG_HEADER && (st & JPG_ST_HEADER) && !(st & JPG_ST_ERROR))
		writel(JPG_IRQ_ACK_ALL, j->base + JPG_IRQ_ACK);
	else
		writel(readl(j->base + JPG_RESET) | 1, j->base + JPG_RESET);
	complete(&j->done);
	return IRQ_HANDLED;
}

/* Wait for one phase; returns 0, -EIO for a bad stream or -ETIMEDOUT */
static int jpg_wait(struct jpg_dev *j, unsigned int ms, u32 want, const char *what)
{
	if (!wait_for_completion_timeout(&j->done, msecs_to_jiffies(ms))) {
		dev_err(j->dev, "%s: no interrupt (status %08x, timer %08x)\n", what,
			readl(j->base + JPG_STATUS), readl(j->base + JPG_TIMER_STAT));
		return -ETIMEDOUT;
	}
	if (j->expired) {
		dev_err(j->dev, "%s: watchdog expired (status %08x)\n", what, j->status);
		return -ETIMEDOUT;
	}
	if ((j->status & JPG_ST_ERROR) || !(j->status & want))
		return -EIO;
	return 0;
}

static int jpg_decode(struct jpg_dev *j, struct jpg_job *job)
{
	u32 cfg;
	int ret;

	if (job->bs & 31 || job->bs_len < JPG_MIN_BS || job->bs > U32_MAX ||
	    job->out[0] > U32_MAX || job->out[1] > U32_MAX || job->stride & 3)
		return -EINVAL;

	mutex_lock(&j->lock);
	ret = clk_prepare_enable(j->clk);
	if (ret)
		goto unlock;
	ret = jpg_reset(j);
	if (ret) {
		dev_err(j->dev, "reset timed out\n");
		goto off;
	}
	writel(1, j->base + JPG_POWER);

	/* Prepare, then let the core parse the headers */
	writel(JPG_MODE_DECODE, j->base + JPG_MODE);
	writel(JPG_IRQ_ACK_ALL, j->base + JPG_IRQ_ACK);
	writel(JPG_TIMER_ENABLE | 0x0fffffff, j->base + JPG_TIMER);
	writel(0x20, j->base + JPG_CFG68);
	writel(1, j->base + JPG_OUT_FMT);
	writel(job->bs, j->base + JPG_BS_ADDR);
	writel(job->bs_len & JPG_BS_LEN_IDLE, j->base + JPG_BS_LEN);
	writel(1, j->base + JPG_BS_CTRL);
	writel(readl(j->base + JPG_OUT_FMT) & ~JPG_OUT_FMT_LINEAR, j->base + JPG_OUT_FMT);

	reinit_completion(&j->done);
	j->phase = JPG_HEADER;
	writel(1, j->base + JPG_START);
	ret = jpg_wait(j, JPG_HEADER_MS, JPG_ST_HEADER, "header");
	job->status = j->status;
	if (ret)
		goto finish;

	job->width = readl(j->base + JPG_WIDTH);
	job->height = readl(j->base + JPG_HEIGHT);
	job->chroma = readl(j->base + JPG_MODE) & JPG_MODE_CHROMA;
	if (!job->width || !job->height || job->width > job->max_w || job->height > job->max_h) {
		ret = -ERANGE;
		goto finish;
	}

	/* Output planes, then decode the body */
	cfg = readl(j->base + JPG_OUT_FMT) & ~JPG_OUT_FMT_MASK;
	writel(cfg | job->fmt->hw, j->base + JPG_OUT_FMT);
	writel(0, j->base + JPG_OUT_OFF0);
	writel(0, j->base + JPG_OUT_OFF1);
	writel(job->stride, j->base + JPG_OUT_STRIDE0);
	writel(job->fmt->planes > 1 ? job->stride : 0, j->base + JPG_OUT_STRIDE1);
	writel(job->fmt->planes > 1 ? job->out[1] : 0, j->base + JPG_OUT_ADDR1);
	writel(job->out[0], j->base + JPG_OUT_ADDR0);

	reinit_completion(&j->done);
	j->phase = JPG_BODY;
	writel(1, j->base + JPG_CONTINUE);
	ret = jpg_wait(j, JPG_BODY_MS, JPG_ST_DONE, "decode");
	job->status = j->status;

finish:
	j->phase = JPG_IDLE;
	writel(JPG_BS_LEN_IDLE, j->base + JPG_BS_LEN);
	jpg_reset(j);
	writel(0, j->base + JPG_POWER);
off:
	clk_disable_unprepare(j->clk);
unlock:
	mutex_unlock(&j->lock);
	return ret;
}

/* ---- V4L2 memory-to-memory decoder ------------------------------------------ */

/*
 * OUTPUT takes whole JPEG files, CAPTURE receives NV12, YUYV or UYVY. The
 * capture format must be at least as large as the image (an application
 * reads the size from the JPEG frame header); the picture is decoded into
 * the top-left corner and a larger image fails the buffer.
 */
struct jpg_ctx {
	struct v4l2_fh fh;
	struct jpg_dev *j;
	struct v4l2_pix_format pix[2];	/* 0 = OUTPUT (JPEG), 1 = CAPTURE */
	const struct jpg_fmt *fmt;
	u32 sequence[2];
};

static struct jpg_ctx *fh_to_ctx(struct file *file)
{
	return container_of(file->private_data, struct jpg_ctx, fh);
}

static bool jpg_is_cap(enum v4l2_buf_type type)
{
	return type == V4L2_BUF_TYPE_VIDEO_CAPTURE;
}

static const struct jpg_fmt *jpg_try_pix(struct v4l2_pix_format *pix, bool cap)
{
	const struct jpg_fmt *f = NULL;
	u32 bpl;

	pix->width = clamp(pix->width, JPG_MIN_SIZE, JPG_MAX_SIZE);
	pix->height = clamp(pix->height, JPG_MIN_SIZE, JPG_MAX_SIZE);
	pix->field = V4L2_FIELD_NONE;
	pix->colorspace = V4L2_COLORSPACE_JPEG;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_SRGB;
	if (!cap) {
		pix->pixelformat = V4L2_PIX_FMT_JPEG;
		pix->bytesperline = 0;
		pix->sizeimage = clamp(pix->sizeimage, (u32)SZ_64K, (u32)JPG_MAX_BS);
		return NULL;
	}
	f = jpg_find_format(pix->pixelformat);
	if (!f)
		f = &jpg_formats[0];
	pix->pixelformat = f->fourcc;
	pix->width = ALIGN(pix->width, 2);
	pix->height = ALIGN(pix->height, 2);
	bpl = max(pix->bytesperline, pix->width * f->cpp);
	pix->bytesperline = min_t(u32, ALIGN(bpl, 4), 0xfffc);
	pix->sizeimage = pix->bytesperline * pix->height;
	if (f->planes > 1)
		pix->sizeimage += pix->bytesperline * pix->height / 2;
	return f;
}

static void jpg_work(struct work_struct *work)
{
	struct jpg_dev *j = container_of(work, struct jpg_dev, work);
	struct jpg_ctx *ctx = v4l2_m2m_get_curr_priv(j->m2m);
	struct vb2_v4l2_buffer *src, *dst;
	const struct v4l2_pix_format *cp;
	struct jpg_job job = {};
	int ret;

	if (!ctx)
		return;
	cp = &ctx->pix[1];
	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	job.bs = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	job.bs_len = vb2_plane_size(&src->vb2_buf, 0);
	job.fmt = ctx->fmt;
	job.out[0] = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);
	job.out[1] = job.out[0] + (dma_addr_t)cp->bytesperline * cp->height;
	job.stride = cp->bytesperline;
	job.max_w = cp->width;
	job.max_h = cp->height;
	ret = jpg_decode(j, &job);
	if (ret == -ERANGE)
		dev_warn_ratelimited(j->dev, "image %ux%u larger than the %ux%u capture format\n",
				     job.width, job.height, cp->width, cp->height);
	else if (ret)
		dev_warn_ratelimited(j->dev, "decode failed: %d (status %08x)\n", ret, job.status);

	v4l2_m2m_buf_copy_metadata(src, dst);
	src->sequence = ctx->sequence[0]++;
	dst->sequence = ctx->sequence[1]++;
	vb2_set_plane_payload(&dst->vb2_buf, 0, ret ? 0 : cp->sizeimage);
	v4l2_m2m_buf_done(src, ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, ret ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(j->m2m, ctx->fh.m2m_ctx);
}

static void jpg_device_run(void *priv)
{
	struct jpg_ctx *ctx = priv;

	queue_work(system_highpri_wq, &ctx->j->work);
}

static const struct v4l2_m2m_ops jpg_m2m_ops = {
	.device_run = jpg_device_run,
};

static int jpg_queue_setup(struct vb2_queue *vq, unsigned int *nbufs, unsigned int *nplanes,
			   unsigned int sizes[], struct device *alloc_devs[])
{
	struct jpg_ctx *ctx = vb2_get_drv_priv(vq);
	u32 size = ctx->pix[jpg_is_cap(vq->type)].sizeimage;

	if (*nplanes)
		return *nplanes == 1 && sizes[0] >= size ? 0 : -EINVAL;
	*nplanes = 1;
	sizes[0] = size;
	return 0;
}

static int jpg_buf_prepare(struct vb2_buffer *vb)
{
	struct jpg_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	bool cap = jpg_is_cap(vb->vb2_queue->type);
	dma_addr_t addr = vb2_dma_contig_plane_dma_addr(vb, 0);
	u32 size = ctx->pix[cap].sizeimage;

	if (vb2_plane_size(vb, 0) < size)
		return -EINVAL;
	if (addr + vb2_plane_size(vb, 0) - 1 > U32_MAX)
		return -EFAULT;
	if (cap) {
		if (addr & 3)
			return -EINVAL;
		vb2_set_plane_payload(vb, 0, size);
	} else if (addr & 31 || vb2_get_plane_payload(vb, 0) < 4) {
		return -EINVAL;
	}
	return 0;
}

static void jpg_buf_queue(struct vb2_buffer *vb)
{
	struct jpg_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static int jpg_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct jpg_ctx *ctx = vb2_get_drv_priv(vq);

	ctx->sequence[jpg_is_cap(vq->type)] = 0;
	return 0;
}

static void jpg_stop_streaming(struct vb2_queue *vq)
{
	struct jpg_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vb;

	for (;;) {
		vb = jpg_is_cap(vq->type) ? v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx) :
					    v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		if (!vb)
			break;
		v4l2_m2m_buf_done(vb, VB2_BUF_STATE_ERROR);
	}
}

static const struct vb2_ops jpg_qops = {
	.queue_setup	 = jpg_queue_setup,
	.buf_prepare	 = jpg_buf_prepare,
	.buf_queue	 = jpg_buf_queue,
	.start_streaming = jpg_start_streaming,
	.stop_streaming	 = jpg_stop_streaming,
};

static int jpg_queue_init(void *priv, struct vb2_queue *src, struct vb2_queue *dst)
{
	struct jpg_ctx *ctx = priv;
	struct vb2_queue *qs[2] = { src, dst };
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		struct vb2_queue *q = qs[i];

		q->type = i ? V4L2_BUF_TYPE_VIDEO_CAPTURE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
		q->io_modes = VB2_MMAP | VB2_DMABUF;
		q->drv_priv = ctx;
		q->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
		q->ops = &jpg_qops;
		q->mem_ops = &vb2_dma_contig_memops;
		q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
		q->lock = &ctx->j->vdev_lock;
		q->dev = ctx->j->dev;
		ret = vb2_queue_init(q);
		if (ret)
			return ret;
	}
	return 0;
}

static int jpg_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	strscpy(cap->driver, JPG_NAME, sizeof(cap->driver));
	strscpy(cap->card, "S5L8947X JPEG decoder", sizeof(cap->card));
	return 0;
}

static int jpg_enum_fmt_cap(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index >= ARRAY_SIZE(jpg_formats))
		return -EINVAL;
	f->pixelformat = jpg_formats[f->index].fourcc;
	return 0;
}

static int jpg_enum_fmt_out(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	if (f->index)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_JPEG;
	f->flags = V4L2_FMT_FLAG_COMPRESSED;
	return 0;
}

static int jpg_g_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	f->fmt.pix = fh_to_ctx(file)->pix[jpg_is_cap(f->type)];
	return 0;
}

static int jpg_try_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	jpg_try_pix(&f->fmt.pix, jpg_is_cap(f->type));
	return 0;
}

static int jpg_s_fmt(struct file *file, void *fh, struct v4l2_format *f)
{
	struct jpg_ctx *ctx = fh_to_ctx(file);
	bool cap = jpg_is_cap(f->type);
	const struct jpg_fmt *fmt;

	if (vb2_is_busy(v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type)))
		return -EBUSY;
	fmt = jpg_try_pix(&f->fmt.pix, cap);
	if (cap)
		ctx->fmt = fmt;
	ctx->pix[cap] = f->fmt.pix;
	return 0;
}

static const struct v4l2_ioctl_ops jpg_ioctl_ops = {
	.vidioc_querycap		= jpg_querycap,
	.vidioc_enum_fmt_vid_cap	= jpg_enum_fmt_cap,
	.vidioc_enum_fmt_vid_out	= jpg_enum_fmt_out,
	.vidioc_g_fmt_vid_cap		= jpg_g_fmt,
	.vidioc_g_fmt_vid_out		= jpg_g_fmt,
	.vidioc_try_fmt_vid_cap		= jpg_try_fmt,
	.vidioc_try_fmt_vid_out		= jpg_try_fmt,
	.vidioc_s_fmt_vid_cap		= jpg_s_fmt,
	.vidioc_s_fmt_vid_out		= jpg_s_fmt,
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

static int jpg_open(struct file *file)
{
	struct jpg_dev *j = video_drvdata(file);
	struct jpg_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->j = j;
	ctx->pix[0].width = 1920;
	ctx->pix[0].height = 1080;
	ctx->pix[0].sizeimage = SZ_2M;
	jpg_try_pix(&ctx->pix[0], false);
	ctx->pix[1].pixelformat = V4L2_PIX_FMT_NV12;
	ctx->pix[1].width = 1920;
	ctx->pix[1].height = 1080;
	ctx->fmt = jpg_try_pix(&ctx->pix[1], true);

	if (mutex_lock_interruptible(&j->vdev_lock)) {
		kfree(ctx);
		return -ERESTARTSYS;
	}
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(j->m2m, ctx, jpg_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		v4l2_fh_exit(&ctx->fh);
		mutex_unlock(&j->vdev_lock);
		kfree(ctx);
		return ret;
	}
	v4l2_fh_add(&ctx->fh, file);
	mutex_unlock(&j->vdev_lock);
	return 0;
}

static int jpg_release(struct file *file)
{
	struct jpg_ctx *ctx = fh_to_ctx(file);
	struct jpg_dev *j = ctx->j;

	mutex_lock(&j->vdev_lock);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh, file);
	v4l2_fh_exit(&ctx->fh);
	mutex_unlock(&j->vdev_lock);
	kfree(ctx);
	return 0;
}

static const struct v4l2_file_operations jpg_fops = {
	.owner		= THIS_MODULE,
	.open		= jpg_open,
	.release	= jpg_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static int jpg_v4l2_register(struct jpg_dev *j)
{
	struct video_device *vdev = &j->vdev;
	int ret;

	ret = v4l2_device_register(j->dev, &j->v4l2);
	if (ret)
		return ret;
	j->m2m = v4l2_m2m_init(&jpg_m2m_ops);
	if (IS_ERR(j->m2m)) {
		ret = PTR_ERR(j->m2m);
		goto err_v4l2;
	}
	strscpy(vdev->name, JPG_NAME, sizeof(vdev->name));
	vdev->fops = &jpg_fops;
	vdev->ioctl_ops = &jpg_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->lock = &j->vdev_lock;
	vdev->v4l2_dev = &j->v4l2;
	vdev->vfl_dir = VFL_DIR_M2M;
	vdev->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	video_set_drvdata(vdev, j);
	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_m2m;
	return 0;

err_m2m:
	v4l2_m2m_release(j->m2m);
err_v4l2:
	v4l2_device_unregister(&j->v4l2);
	return ret;
}

/* ---- platform device -------------------------------------------------------- */

static int jpg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct jpg_dev *j;
	int ret;

	j = devm_kzalloc(dev, sizeof(*j), GFP_KERNEL);
	if (!j)
		return -ENOMEM;
	j->dev = dev;
	mutex_init(&j->lock);
	mutex_init(&j->vdev_lock);
	init_completion(&j->done);
	INIT_WORK(&j->work, jpg_work);

	j->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(j->base))
		return PTR_ERR(j->base);
	/* The register window must not be touched while this clock is off */
	j->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(j->clk))
		return dev_err_probe(dev, PTR_ERR(j->clk), "no clock\n");
	j->irq = platform_get_irq(pdev, 0);
	if (j->irq < 0)
		return j->irq;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	ret = clk_prepare_enable(j->clk);
	if (ret)
		return ret;
	j->version = readl(j->base + JPG_VERSION);
	ret = jpg_reset(j);
	writel(0, j->base + JPG_POWER);
	clk_disable_unprepare(j->clk);
	if (ret)
		return dev_err_probe(dev, ret, "reset failed\n");

	ret = devm_request_irq(dev, j->irq, jpg_irq, 0, dev_name(dev), j);
	if (ret)
		return ret;
	ret = jpg_v4l2_register(j);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, j);
	dev_info(dev, "JPEG codec version %08x as %s\n", j->version,
		 video_device_node_name(&j->vdev));
	return 0;
}

static void jpg_remove(struct platform_device *pdev)
{
	struct jpg_dev *j = platform_get_drvdata(pdev);

	video_unregister_device(&j->vdev);
	cancel_work_sync(&j->work);
	v4l2_m2m_release(j->m2m);
	v4l2_device_unregister(&j->v4l2);
}

static const struct of_device_id jpg_of_match[] = {
	{ .compatible = "apple,s5l8947x-jpeg" },
	{ }
};
MODULE_DEVICE_TABLE(of, jpg_of_match);

static struct platform_driver jpg_driver = {
	.probe	= jpg_probe,
	.remove	= jpg_remove,
	.driver	= {
		.name		= JPG_NAME,
		.of_match_table	= jpg_of_match,
	},
};
module_platform_driver(jpg_driver);

MODULE_DESCRIPTION("S5L8947X JPEG decoder");
MODULE_LICENSE("GPL");
