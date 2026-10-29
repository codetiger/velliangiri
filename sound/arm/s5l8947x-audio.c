// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Audio Engine — ALSA PCM for the S/PDIF transmitter (HDMI + optical)
 * and the I2S0 controller.
 *
 * The engine is programmed from the measured register model, not the Samsung
 * v5 I2S map:
 *
 *   S/PDIF 0x34194000        I2S0 0x34190000
 *   +0x00 CLKCON  power      +0x00 CTL     bit0 enable, bit6 soft reset
 *   +0x04 CON     format,    +0x04 TXCON   BCLK = 24 MHz / (2 * (div+1)),
 *                 FIFO level              [4:3] frame = 32/48/64 BCLK
 *   +0x0c CSTAS   status     +0x08 TXCOM   bit2 TX enable, bit0 pause,
 *   +0x10 DATA    16-bit                    keep [24:19]
 *                            +0x10 TXDATA  one word per channel sample
 *                            +0x3c STATUS  [12:7] FIFO level
 *
 * Both blocks are clocked from the 24 MHz reference, so the reachable frame
 * rates are 46875 Hz (24 MHz / 512) and 31250 Hz (24 MHz / 768). Those are the
 * only rates advertised: the hardware cannot produce 48000, and pretending it
 * does would make the HDMI ACR announce a rate that is not being sent.
 *
 * The S/PDIF transmitter is fed by the on-chip central DMA (CDMA, channel 31),
 * so the CPU no longer has to touch the 16-entry FIFO every 32 us. The DMA
 * moves 32-bit words from a coherent ring in the FIFO word layout (0, L, 0, R)
 * and loops over 4 KB descriptors, one word per FIFO request. The application's S16 stereo frames
 * are converted into that layout by the PCM copy callback.
 *
 * The DMA is paced by the same 24 MHz reference as the S/PDIF transmitter, so
 * the playback position is derived from the monotonic clock rather than from
 * the controller's prefetch address (MAR), which runs ahead of the FIFO. MAR is
 * still sampled as a liveness signal: if it stops moving the frame clock is
 * gone and the stream is stopped with an XRUN.
 *
 * I2S0 has no DMA channel wired on this board, so it keeps the PIO path: the
 * timer period is derived from the FIFO depth and the frame rate (256 us for
 * the 64-entry I2S FIFO) and the hardware pointer is written - occupancy.
 */

#define pr_fmt(fmt) "s5l8947x-audio: " fmt

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <linux/spinlock.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

/* I2S0 controller (measured layout) */
#define I2S_CTL			0x00
#define I2S_TXCON		0x04
#define I2S_TXCOM		0x08
#define I2S_TXDATA		0x10
#define I2S_STATUS		0x3c

#define I2S_CTL_SOFT_RESET	0x00000041U	/* bit6 resets the block */
#define I2S_CTL_ENABLE		0x00000021U
#define I2S_TXCOM_BASE		0x01f80000U	/* reserved, must be kept */
#define I2S_TXCOM_TX_EN		0x00000004U
#define I2S_TXCOM_TX_PAUSE	0x00000001U
#define I2S_STATUS_LEVEL(v)	(((v) >> 7) & 0x3fU)
#define I2S_FIFO_DEPTH		64U

/* S/PDIF transmitter (measured layout, matches mainline samsung/spdif.c) */
#define SPDIF_CLKCON		0x00
#define SPDIF_CON		0x04
#define SPDIF_CSTAS		0x0c
#define SPDIF_DATA		0x10

#define SPDIF_CLKCON_PWR	0x00000001U
#define SPDIF_CON_SW_RESET	0x00000020U
#define SPDIF_CON_BASE		0x00381001U	/* PCM, 16-bit, FIFO threshold 7 */
#define SPDIF_CON_MCLK_384	0x00000008U	/* CON[4:3] = 1 -> /768 */
#define SPDIF_CON_LEVEL(v)	(((v) >> 22) & 0x1fU)
#define SPDIF_FIFO_DEPTH	16U
#define SPDIF_CSTAS_BASE	0x00000104U	/* consumer, PCM, no copyright */
#define SPDIF_CSTAS_FS(code)	((u32)(code) << 24)

/* Central DMA (CDMA) — measured register model, see research/docs/cdma-registers.md */
#define CDMA_CH_SPDIF		31
#define CDMA_CH_STRIDE		0x1000
#define CDMA_CSR		0x00
#define CDMA_DCR		0x04
#define CDMA_DAR		0x08
#define CDMA_DBR		0x0c
#define CDMA_MAR		0x10
#define CDMA_CAR		0x14
#define CDMA_ERR		0x18

#define CDMA_CSR_ENABLE		0x00000004U	/* clock/kick request */
#define CDMA_CSR_READY		0x00200000U	/* kick acknowledged */
#define CDMA_CSR_START		0x02000003U	/* vendor start command */
#define CDMA_CSR_ABORT		0x00000002U
#define CDMA_CSR_W1C_ERROR	0x00040000U
#define CDMA_CSR_W1C_DONE	0x00080000U

/*
 * Request 8, write, 32-bit.  The burst field must stay at the single-word
 * setting: with bit 6 set (16-word bursts) the channel outruns the 16-entry
 * FIFO about fourteen-fold, the overflow is dropped and the transmitter plays
 * the ring at a multiple of the intended pitch.
 */
#define CDMA_DCR_SPDIF		0x0008008aU
#define CDMA_DAR_SPDIF		0x194010U	/* S/PDIF DATA, 0x34000000 window */
#define CDMA_DESC_CMD		0x02000003U
#define CDMA_DESC_STRIDE	32
#define CDMA_SEG_FRAMES		256		/* one 4 KB descriptor */
#define CDMA_SEG_BYTES		(CDMA_SEG_FRAMES * 16)

#define CDMA_ENABLE_WORD(ch)	(((ch) >> 5) * 4)
#define CDMA_DISABLE_WORD(ch)	(8 + ((ch) >> 5) * 4)

/* Reachable frame rates from the 24 MHz reference */
#define AUDIO_RATE_HIGH		46875U
#define AUDIO_RATE_LOW		31250U

enum s5l8947x_out_kind {
	OUT_SPDIF,
	OUT_I2S,
};

struct s5l8947x_out {
	enum s5l8947x_out_kind kind;
	unsigned int wpf;	/* words per stereo frame */
	unsigned int depth;	/* FIFO entries */
	unsigned int maxlevel;	/* highest level value that is not ambiguous */
	unsigned int high;	/* refill watermark, 3/4 of maxlevel */
};

struct s5l8947x_audio {
	struct device *dev;
	struct snd_card *card;
	struct snd_pcm_substream *substream;

	void __iomem *i2s;
	void __iomem *spdif;
	struct s5l8947x_out out;

	spinlock_t lock;
	struct hrtimer timer;
	ktime_t tick;
	unsigned int rate;

	u64 frames_written;	/* frames handed to the FIFO this stream */
	u32 pos;		/* ring write index */
	u64 period_base;	/* frame count of the last reported period */
	u64 last_frames;	/* consumed frames at the last progress check */
	ktime_t last_progress;

	bool running;
	bool armed;
	bool hdmi_enable;
	unsigned long gaps;	/* FIFO found empty while data was queued */
	unsigned long xruns;	/* no progress: the frame clock stopped */

	/* Central DMA transport (S/PDIF only) */
	void __iomem *cdma;
	unsigned int cdma_ch;
	bool dma;
	bool dma_started;
	void *dma_ring;
	dma_addr_t dma_ring_phys;
	size_t dma_ring_bytes;
	unsigned int dma_ring_frames;
	void *dma_desc;
	dma_addr_t dma_desc_phys;
	unsigned int dma_desc_count;
	size_t dma_seg_bytes;
	u64 dma_start_ns;
	u64 dma_frames;		/* frames the audio clock has played */
	u64 dma_period_base;
	u64 copy_frames;
	u32 dma_last_mar;
	ktime_t dma_mar_time;
};

/*
 * S/PDIF feeds the HDMI transmitter and the optical output from the same
 * stream. I2S0 is selectable for bring-up; the HDMI I2S receiver has never
 * been seen to clock it, so S/PDIF is the default.
 */
static unsigned int output = OUT_SPDIF;
module_param(output, uint, 0444);
MODULE_PARM_DESC(output, "0 = S/PDIF transmitter (HDMI + optical), 1 = I2S0");

static const unsigned int s5l8947x_rates[] = {
	AUDIO_RATE_HIGH, AUDIO_RATE_LOW,
};

static const struct snd_pcm_hw_constraint_list s5l8947x_rate_constraint = {
	.count	= ARRAY_SIZE(s5l8947x_rates),
	.list	= s5l8947x_rates,
};

static const struct snd_pcm_hardware s5l8947x_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_KNOT,
	.rate_min		= AUDIO_RATE_LOW,
	.rate_max		= AUDIO_RATE_HIGH,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= 64 * 1024,
	.period_bytes_min	= 1024,
	.period_bytes_max	= 16 * 1024,
	.periods_min		= 2,
	.periods_max		= 32,
};

/* TXCON for a frame rate, 0 if the 24 MHz reference cannot produce it */
static u32 s5l8947x_i2s_txcon(unsigned int rate)
{
	static const unsigned int frame_bits[3] = { 32, 48, 64 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(frame_bits); i++) {
		unsigned int denom = 2 * frame_bits[i] * rate;
		unsigned int div;

		if (!denom || 24000000U % denom)
			continue;
		div = 24000000U / denom;
		if (div >= 1 && div <= 8)
			return ((div - 1) << 8) | (i << 3);
	}
	return 0;
}

static u32 s5l8947x_spdif_con(unsigned int rate)
{
	switch (rate) {
	case AUDIO_RATE_HIGH:
		return SPDIF_CON_BASE;
	case AUDIO_RATE_LOW:
		return SPDIF_CON_BASE | SPDIF_CON_MCLK_384;
	default:
		return 0;
	}
}

/* Consumer channel status: 48 kHz code for 46875, 32 kHz code for 31250 */
static u32 s5l8947x_spdif_cstas(unsigned int rate)
{
	if (rate == AUDIO_RATE_LOW)
		return SPDIF_CSTAS_BASE | SPDIF_CSTAS_FS(0x3);
	return SPDIF_CSTAS_BASE | SPDIF_CSTAS_FS(0x2);
}

static unsigned int s5l8947x_level(struct s5l8947x_audio *aud)
{
	if (aud->out.kind == OUT_SPDIF)
		return SPDIF_CON_LEVEL(readl_relaxed(aud->spdif + SPDIF_CON));
	return I2S_STATUS_LEVEL(readl_relaxed(aud->i2s + I2S_STATUS));
}

static void s5l8947x_put_frame(struct s5l8947x_audio *aud, u32 frame)
{
	u16 l = frame & 0xffffU;
	u16 r = frame >> 16;

	if (aud->out.kind == OUT_I2S) {
		/* One FIFO entry per channel sample, sample in both halves */
		writel_relaxed((u32)l | (u32)l << 16, aud->i2s + I2S_TXDATA);
		writel_relaxed((u32)r | (u32)r << 16, aud->i2s + I2S_TXDATA);
		return;
	}

	/*
	 * One subframe is two FIFO words holding the low and high 16 bits of
	 * the 32-bit sample, so a 16-bit sample is left-justified: 0, L, 0, R.
	 * Feeding two words, or right-justified samples, is silent.
	 */
	writel_relaxed(0, aud->spdif + SPDIF_DATA);
	writel_relaxed(l, aud->spdif + SPDIF_DATA);
	writel_relaxed(0, aud->spdif + SPDIF_DATA);
	writel_relaxed(r, aud->spdif + SPDIF_DATA);
}

/* Frames written to the FIFO but not yet played = written - occupancy */
static u64 s5l8947x_frames_done(struct s5l8947x_audio *aud)
{
	unsigned int level = s5l8947x_level(aud) / aud->out.wpf;

	if (level >= aud->frames_written)
		return 0;
	return aud->frames_written - level;
}

/*
 * Frames in the ring that the application has queued for playback. Both
 * counters start at zero in prepare(); the application can never be more than
 * one buffer ahead, so the 32-bit modular difference is the true distance even
 * when appl_ptr wraps at runtime->boundary.
 */
static unsigned int s5l8947x_ring_avail(struct s5l8947x_audio *aud,
					struct snd_pcm_runtime *runtime)
{
	u32 appl = READ_ONCE(runtime->control->appl_ptr);
	u64 delta = (u32)(appl - (u32)aud->frames_written);

	if (delta > runtime->buffer_size)
		delta = runtime->buffer_size;
	return delta;
}

/* Push as many queued frames as the FIFO has room for. Called with lock held. */
static void s5l8947x_fill(struct s5l8947x_audio *aud,
			  struct snd_pcm_runtime *runtime)
{
	unsigned int buf = runtime->buffer_size;
	unsigned int level, room, avail, n, i, pos;

	if (!buf || !aud->out.wpf)
		return;

	/*
	 * The occupancy field wraps to 0 when the FIFO is full, so a written
	 * level of 0 is only unambiguous while the FIFO never fills up. Stay
	 * below maxlevel and the level can be trusted.
	 */
	level = s5l8947x_level(aud);
	if (level > aud->out.maxlevel)
		level = aud->out.maxlevel;
	room = (aud->out.maxlevel - level) / aud->out.wpf;
	if (!room)
		return;

	pos = aud->pos;
	avail = s5l8947x_ring_avail(aud, runtime);
	n = min(room, avail);

	for (i = 0; i < n; i++) {
		u32 frame = *(u32 *)(runtime->dma_area + pos * 4);

		s5l8947x_put_frame(aud, frame);
		if (++pos == buf)
			pos = 0;
	}
	aud->pos = pos;
	aud->frames_written += n;
}

static u64 s5l8947x_tick_ns(struct s5l8947x_audio *aud)
{
	u64 ns;

	if (!aud->rate || !aud->out.wpf)
		return 256000;
	/* Half the time it takes to drain the refill watermark */
	ns = div_u64((u64)aud->out.high * NSEC_PER_SEC,
		     2U * aud->rate * aud->out.wpf);
	return ns < 20000 ? 20000 : ns;
}

static int s5l8947x_engine_config(struct s5l8947x_audio *aud)
{
	if (aud->out.kind == OUT_SPDIF) {
		u32 con = s5l8947x_spdif_con(aud->rate);

		if (!con)
			return -EINVAL;
		writel(0, aud->spdif + SPDIF_CLKCON);
		writel(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
		writel(con, aud->spdif + SPDIF_CON);
		writel(s5l8947x_spdif_cstas(aud->rate), aud->spdif + SPDIF_CSTAS);
	} else {
		u32 txcon = s5l8947x_i2s_txcon(aud->rate);

		if (!txcon)
			return -EINVAL;
		writel(I2S_CTL_SOFT_RESET, aud->i2s + I2S_CTL);
		writel(I2S_TXCOM_BASE | I2S_TXCOM_TX_EN | I2S_TXCOM_TX_PAUSE,
		       aud->i2s + I2S_TXCOM);
		writel(txcon, aud->i2s + I2S_TXCON);
		writel(I2S_CTL_ENABLE, aud->i2s + I2S_CTL);
	}
	return 0;
}

static void s5l8947x_engine_start(struct s5l8947x_audio *aud)
{
	if (aud->out.kind == OUT_SPDIF)
		writel(SPDIF_CLKCON_PWR, aud->spdif + SPDIF_CLKCON);
	else
		writel(I2S_TXCOM_BASE | I2S_TXCOM_TX_EN,
		       aud->i2s + I2S_TXCOM);
}

static void s5l8947x_engine_stop(struct s5l8947x_audio *aud)
{
	if (aud->out.kind == OUT_SPDIF) {
		writel(0, aud->spdif + SPDIF_CLKCON);
		writel(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
	} else {
		writel(I2S_CTL_SOFT_RESET, aud->i2s + I2S_CTL);
	}
}

static inline void __iomem *s5l8947x_cdma_page(struct s5l8947x_audio *aud)
{
	return aud->cdma + aud->cdma_ch * CDMA_CH_STRIDE;
}

/* Gate the channel's clock through the controller's set/clear bitmaps. */
static void s5l8947x_cdma_enable(struct s5l8947x_audio *aud, bool enable)
{
	u32 bit = 1U << (aud->cdma_ch & 31);

	writel(bit, aud->cdma + (enable ? CDMA_ENABLE_WORD(aud->cdma_ch)
				       : CDMA_DISABLE_WORD(aud->cdma_ch)));
	udelay(1000);
}

/*
 * Program and start the channel.  The controller needs its stale error/done
 * bits cleared and the previous command aborted before the registers accept a
 * new descriptor list; then bit 2 is set and bit 21 polled, which is the
 * vendor's kick handshake.  The final write is the command that makes the
 * channel fetch and run the list.
 */
static void s5l8947x_dma_program(struct s5l8947x_audio *aud)
{
	void __iomem *base = s5l8947x_cdma_page(aud);
	int i;

	writel(CDMA_CSR_W1C_ERROR, base + CDMA_CSR);
	writel(CDMA_CSR_W1C_DONE, base + CDMA_CSR);
	writel(CDMA_CSR_ABORT, base + CDMA_CSR);
	udelay(1000);

	writel(CDMA_DCR_SPDIF, base + CDMA_DCR);
	writel(CDMA_DAR_SPDIF, base + CDMA_DAR);
	writel(0, base + CDMA_DBR);
	writel((u32)aud->dma_desc_phys, base + CDMA_CAR);

	writel(readl(base + CDMA_CSR) | CDMA_CSR_ENABLE, base + CDMA_CSR);
	for (i = 0; i < 1000; i++) {
		if (readl(base + CDMA_CSR) & CDMA_CSR_READY)
			break;
		cpu_relax();
	}
	writel(CDMA_CSR_START, base + CDMA_CSR);
}

static void s5l8947x_dma_halt(struct s5l8947x_audio *aud)
{
	void __iomem *base = s5l8947x_cdma_page(aud);

	writel(CDMA_CSR_ABORT, base + CDMA_CSR);
	udelay(1000);
	s5l8947x_cdma_enable(aud, false);
}

static void s5l8947x_dma_build(struct s5l8947x_audio *aud)
{
	u32 *desc = aud->dma_desc;
	unsigned int i;

	for (i = 0; i < aud->dma_desc_count; i++) {
		u32 *d = desc + i * (CDMA_DESC_STRIDE / 4);

		d[0] = (u32)aud->dma_desc_phys +
		       ((i + 1) % aud->dma_desc_count) * CDMA_DESC_STRIDE;
		d[1] = CDMA_DESC_CMD;
		d[2] = (u32)aud->dma_ring_phys + i * aud->dma_seg_bytes;
		d[3] = aud->dma_seg_bytes;
	}
}

static void s5l8947x_dma_free(struct s5l8947x_audio *aud)
{
	if (aud->dma_ring) {
		dma_free_coherent(aud->dev, aud->dma_ring_bytes,
				  aud->dma_ring, aud->dma_ring_phys);
		aud->dma_ring = NULL;
	}
	if (aud->dma_desc) {
		dma_free_coherent(aud->dev, PAGE_SIZE, aud->dma_desc,
				  aud->dma_desc_phys);
		aud->dma_desc = NULL;
	}
	aud->dma_ring_frames = 0;
	aud->dma_desc_count = 0;
}

/*
 * The DMA ring mirrors the application's frames one-for-one, but each frame
 * is four FIFO words (0, L, 0, R).  It is rebuilt whenever the application
 * changes the buffer size.
 */
static int s5l8947x_dma_setup(struct s5l8947x_audio *aud,
			      struct snd_pcm_runtime *runtime)
{
	unsigned int frames = runtime->buffer_size;
	size_t bytes = frames * 16;
	unsigned int count;

	if (!frames)
		return -EINVAL;
	if (aud->dma_ring && aud->dma_ring_frames == frames)
		goto build;

	s5l8947x_dma_free(aud);

	aud->dma_ring = dma_alloc_coherent(aud->dev, bytes,
					   &aud->dma_ring_phys, GFP_KERNEL);
	if (!aud->dma_ring)
		return -ENOMEM;
	aud->dma_ring_bytes = bytes;
	aud->dma_ring_frames = frames;

	aud->dma_desc = dma_alloc_coherent(aud->dev, PAGE_SIZE,
					   &aud->dma_desc_phys, GFP_KERNEL);
	if (!aud->dma_desc) {
		s5l8947x_dma_free(aud);
		return -ENOMEM;
	}

	count = bytes / CDMA_SEG_BYTES;
	if (!count || bytes % CDMA_SEG_BYTES || count > PAGE_SIZE / CDMA_DESC_STRIDE) {
		s5l8947x_dma_free(aud);
		return -EINVAL;
	}
	aud->dma_seg_bytes = CDMA_SEG_BYTES;
	aud->dma_desc_count = count;

build:
	/* Never replay a previous stream's samples */
	memset(aud->dma_ring, 0, aud->dma_ring_bytes);
	s5l8947x_dma_build(aud);
	return 0;
}

/* Frames the audio clock has played since the stream started. */
static u64 s5l8947x_dma_clock(struct s5l8947x_audio *aud)
{
	u64 ns = ktime_get_ns() - aud->dma_start_ns;

	return mul_u64_u32_div(ns, aud->rate, NSEC_PER_SEC);
}

/*
 * Convert the application's S16 stereo frames into the FIFO word layout.
 * Called from the PCM copy callback, which is serialized with the stream.
 */
static int s5l8947x_pcm_copy(struct snd_pcm_substream *substream, int channel,
			     unsigned long pos, struct iov_iter *iter,
			     unsigned long count)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned int frames = count / 4;
	unsigned int frame = (pos / 4) % aud->dma_ring_frames;
	u32 *ring = aud->dma_ring;
	s16 tmp[128];
	unsigned int n, i;

	if (!aud->dma || !ring)
		return -EINVAL;

	while (frames) {
		n = min(frames, ARRAY_SIZE(tmp) / 2);
		if (copy_from_iter(tmp, n * 4, iter) != n * 4)
			return -EFAULT;
		for (i = 0; i < n; i++) {
			u32 *dst = ring + frame * 4;

			dst[0] = 0;
			dst[1] = (u16)tmp[i * 2];
			dst[2] = 0;
			dst[3] = (u16)tmp[i * 2 + 1];
			if (++frame == aud->dma_ring_frames)
				frame = 0;
		}
		frames -= n;
		aud->copy_frames += n;
	}
	return 0;
}

static enum hrtimer_restart s5l8947x_timer_cb(struct hrtimer *timer)
{
	struct s5l8947x_audio *aud =
		container_of(timer, struct s5l8947x_audio, timer);
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	unsigned int level = 0, avail = 0;
	bool elapsed = false, stalled = false;
	u64 frames;
	ktime_t now;

	spin_lock_irqsave(&aud->lock, flags);
	substream = aud->substream;
	if (!aud->running || !substream || !substream->runtime) {
		spin_unlock_irqrestore(&aud->lock, flags);
		return HRTIMER_NORESTART;
	}
	runtime = substream->runtime;
	now = ktime_get();

	if (aud->dma) {
		u64 clock = s5l8947x_dma_clock(aud);
		u32 mar = readl(s5l8947x_cdma_page(aud) + CDMA_MAR);

		/*
		 * The frame clock (24 MHz, the same one that clocks the S/PDIF
		 * transmitter) is the playback position.  Keep one descriptor
		 * of margin so the application never rewrites data the engine
		 * has not read, even with its descriptor prefetch.
		 */
		frames = clock > CDMA_SEG_FRAMES ? clock - CDMA_SEG_FRAMES : 0;
		aud->dma_frames = frames;

		if (mar != aud->dma_last_mar) {
			aud->dma_last_mar = mar;
			aud->dma_mar_time = now;
		} else if (ktime_after(now,
				       ktime_add_ms(aud->dma_mar_time, 200))) {
			stalled = true;
		}
		if (frames - aud->dma_period_base >= runtime->period_size) {
			u32 rem;

			div_u64_rem(frames, runtime->period_size, &rem);
			aud->dma_period_base = frames - rem;
			elapsed = true;
		}
	} else {
		if (!aud->armed) {
			/* Arm the ACR on the first tick: the clock is running */
			if (aud->hdmi_enable)
				s5l8947x_hdmi_audio_arm(aud->rate);
			aud->armed = true;
		}

		level = s5l8947x_level(aud);
		avail = s5l8947x_ring_avail(aud, runtime);
		s5l8947x_fill(aud, runtime);

		frames = s5l8947x_frames_done(aud);
		if (frames != aud->last_frames) {
			aud->last_frames = frames;
			aud->last_progress = now;
		} else if (level || avail) {
			/* Data is queued (or stuck) and nothing moves */
			if (ktime_after(now, ktime_add_ns(aud->last_progress,
							 200 * NSEC_PER_MSEC)))
				stalled = true;
		}
		if (frames - aud->period_base >= runtime->period_size) {
			aud->period_base += runtime->period_size;
			elapsed = true;
		}
		if (!level && avail)
			aud->gaps++;
	}
	if (stalled)
		aud->running = false;
	spin_unlock_irqrestore(&aud->lock, flags);

	if (stalled) {
		aud->xruns++;
		dev_err_ratelimited(aud->dev,
			"audio clock stopped (level=%u, rate=%u); frame clock lost\n",
			level, aud->rate);
		snd_pcm_stop_xrun(substream);
		return HRTIMER_NORESTART;
	}
	if (elapsed)
		snd_pcm_period_elapsed(substream);

	hrtimer_forward_now(timer, aud->tick);
	return HRTIMER_RESTART;
}

static int s5l8947x_pcm_open(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	runtime->hw = s5l8947x_pcm_hw;
	if (aud->dma)
		runtime->hw.info &= ~(SNDRV_PCM_INFO_MMAP |
				      SNDRV_PCM_INFO_MMAP_VALID);

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &s5l8947x_rate_constraint);
	if (ret < 0)
		return ret;

	if (aud->dma) {
		/* The DMA ring is made of whole descriptor segments */
		ret = snd_pcm_hw_constraint_step(runtime, 0,
						 SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
						 CDMA_SEG_FRAMES);
		if (ret < 0)
			return ret;
		ret = snd_pcm_hw_constraint_step(runtime, 0,
						 SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
						 CDMA_SEG_FRAMES);
		if (ret < 0)
			return ret;
	}

	aud->substream = substream;
	return 0;
}

static int s5l8947x_pcm_close(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	hrtimer_cancel(&aud->timer);
	if (aud->dma && aud->dma_started) {
		aud->dma_started = false;
		s5l8947x_dma_halt(aud);
	}
	aud->running = false;
	s5l8947x_engine_stop(aud);
	aud->substream = NULL;
	if (aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(aud->rate, false,
					       aud->out.kind == OUT_SPDIF);
	s5l8947x_dma_free(aud);
	return 0;
}

static int s5l8947x_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret;

	aud->rate = substream->runtime->rate;
	aud->out.wpf = aud->out.kind == OUT_SPDIF ? 4 : 2;

	if (aud->dma && aud->dma_started) {
		aud->dma_started = false;
		s5l8947x_dma_halt(aud);
	}

	ret = s5l8947x_engine_config(aud);
	if (ret < 0)
		return ret;

	if (aud->dma) {
		ret = s5l8947x_dma_setup(aud, substream->runtime);
		if (ret < 0)
			return ret;
	}

	spin_lock_irqsave(&aud->lock, flags);
	aud->frames_written = 0;
	aud->pos = 0;
	aud->armed = false;
	aud->period_base = 0;
	aud->last_frames = 0;
	aud->last_progress = ktime_get();
	aud->dma_frames = 0;
	aud->dma_period_base = 0;
	aud->dma_started = false;
	aud->copy_frames = 0;
	spin_unlock_irqrestore(&aud->lock, flags);

	if (aud->dma) {
		aud->tick = ns_to_ktime(NSEC_PER_MSEC);
		/* Prime the whole ring before the engine fetches it. */
		substream->runtime->start_threshold =
			substream->runtime->buffer_size;
	} else {
		aud->tick = ns_to_ktime(s5l8947x_tick_ns(aud));
	}

	if (aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(aud->rate, true,
					       aud->out.kind == OUT_SPDIF);
	return 0;
}

static int s5l8947x_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		if (aud->dma) {
			/*
			 * Power the transmitter first: it is the frame clock
			 * the DMA request line is paced by.
			 */
			s5l8947x_engine_start(aud);
			s5l8947x_cdma_enable(aud, true);
			s5l8947x_dma_program(aud);

			spin_lock_irqsave(&aud->lock, flags);
			aud->dma_start_ns = ktime_get_ns();
			aud->dma_frames = 0;
			aud->dma_period_base = 0;
			aud->dma_last_mar =
				readl(s5l8947x_cdma_page(aud) + CDMA_MAR);
			aud->dma_mar_time = ktime_get();
			aud->dma_started = true;
			aud->running = true;
			aud->armed = true;
			spin_unlock_irqrestore(&aud->lock, flags);

			/* The clock is running now, so the ACR will latch */
			if (aud->hdmi_enable)
				s5l8947x_hdmi_audio_arm(aud->rate);
		} else {
			spin_lock_irqsave(&aud->lock, flags);
			aud->last_progress = ktime_get();
			aud->last_frames = 0;
			if (aud->out.kind == OUT_SPDIF) {
				s5l8947x_engine_start(aud);
				s5l8947x_fill(aud, substream->runtime);
			} else {
				s5l8947x_fill(aud, substream->runtime);
				s5l8947x_engine_start(aud);
			}
			if (aud->hdmi_enable)
				s5l8947x_hdmi_audio_arm(aud->rate);
			aud->running = true;
			spin_unlock_irqrestore(&aud->lock, flags);
		}
		hrtimer_start(&aud->timer, aud->tick, HRTIMER_MODE_REL);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		if (aud->dma) {
			spin_lock_irqsave(&aud->lock, flags);
			aud->dma_started = false;
			aud->running = false;
			aud->armed = false;
			spin_unlock_irqrestore(&aud->lock, flags);
			s5l8947x_dma_halt(aud);
			s5l8947x_engine_stop(aud);
		} else {
			spin_lock_irqsave(&aud->lock, flags);
			aud->running = false;
			aud->armed = false;
			s5l8947x_engine_stop(aud);
			spin_unlock_irqrestore(&aud->lock, flags);
		}
		hrtimer_try_to_cancel(&aud->timer);
		return 0;

	default:
		return -EINVAL;
	}
}

static int s5l8947x_pcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	hrtimer_cancel(&aud->timer);
	if (aud->dma && aud->dma_started) {
		aud->dma_started = false;
		aud->running = false;
		s5l8947x_dma_halt(aud);
	}
	return 0;
}

static snd_pcm_uframes_t
s5l8947x_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned long flags;
	u64 frames;
	u32 buf = substream->runtime->buffer_size;

	if (!buf)
		return 0;
	spin_lock_irqsave(&aud->lock, flags);
	if (aud->dma)
		frames = aud->dma_frames;
	else
		frames = s5l8947x_frames_done(aud);
	spin_unlock_irqrestore(&aud->lock, flags);
	return do_div(frames, buf);
}

static const struct snd_pcm_ops s5l8947x_pcm_ops = {
	.open		= s5l8947x_pcm_open,
	.close		= s5l8947x_pcm_close,
	.prepare	= s5l8947x_pcm_prepare,
	.trigger	= s5l8947x_pcm_trigger,
	.sync_stop	= s5l8947x_pcm_sync_stop,
	.pointer	= s5l8947x_pcm_pointer,
};

static const struct snd_pcm_ops s5l8947x_pcm_ops_dma = {
	.open		= s5l8947x_pcm_open,
	.close		= s5l8947x_pcm_close,
	.prepare	= s5l8947x_pcm_prepare,
	.trigger	= s5l8947x_pcm_trigger,
	.sync_stop	= s5l8947x_pcm_sync_stop,
	.pointer	= s5l8947x_pcm_pointer,
	.copy		= s5l8947x_pcm_copy,
};

static int s5l8947x_ctl_switch_info(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int s5l8947x_ctl_switch_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct s5l8947x_audio *aud = snd_kcontrol_chip(kcontrol);

	ucontrol->value.integer.value[0] = aud->hdmi_enable ? 1 : 0;
	return 0;
}

static int s5l8947x_ctl_switch_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct s5l8947x_audio *aud = snd_kcontrol_chip(kcontrol);
	bool en = !!ucontrol->value.integer.value[0];

	if (aud->hdmi_enable == en)
		return 0;
	aud->hdmi_enable = en;
	if (aud->substream)
		s5l8947x_hdmi_audio_configure(aud->rate, en,
					       aud->out.kind == OUT_SPDIF);
	return 1;
}

static const struct snd_kcontrol_new s5l8947x_audio_controls[] = {
	{
		.iface		= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name		= "HDMI Audio Playback Switch",
		.info		= s5l8947x_ctl_switch_info,
		.get		= s5l8947x_ctl_switch_get,
		.put		= s5l8947x_ctl_switch_put,
	},
};

static ssize_t fifo_stats_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_audio *aud = dev_get_drvdata(dev);
	unsigned long flags;
	unsigned int level;
	u64 frames;

	spin_lock_irqsave(&aud->lock, flags);
	level = s5l8947x_level(aud);
	if (aud->dma)
		frames = aud->dma_frames;
	else
		frames = s5l8947x_frames_done(aud);
	spin_unlock_irqrestore(&aud->lock, flags);

	return sysfs_emit(buf,
			  "output=%s rate=%u level=%u depth=%u frames=%llu gaps=%lu xruns=%lu tick_us=%llu mode=%s mar=%08x copied=%llu\n",
			  aud->out.kind == OUT_SPDIF ? "spdif" : "i2s",
			  aud->rate, level, aud->out.depth,
			  frames, aud->gaps, aud->xruns,
			  div_u64((u64)ktime_to_ns(aud->tick), 1000),
			  aud->dma ? "dma" : "pio",
			  aud->cdma ? readl(s5l8947x_cdma_page(aud) + CDMA_MAR) : 0,
			  aud->copy_frames);
}
static DEVICE_ATTR_RO(fifo_stats);

static struct attribute *s5l8947x_audio_attrs[] = {
	&dev_attr_fifo_stats.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_audio);

static int s5l8947x_audio_probe(struct platform_device *pdev)
{
	struct s5l8947x_audio *aud;
	struct snd_card *card;
	struct clk_bulk_data *clks;
	struct snd_pcm *pcm;
	struct device_node *cdma_np;
	struct resource res;
	unsigned int i;
	u32 ch;
	int ret;

	ret = snd_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1, "S5L8947XAudio",
			   THIS_MODULE, sizeof(*aud), &card);
	if (ret < 0)
		return ret;

	aud = card->private_data;
	aud->dev = &pdev->dev;
	aud->card = card;
	aud->hdmi_enable = true;
	aud->rate = AUDIO_RATE_HIGH;
	spin_lock_init(&aud->lock);
	hrtimer_init(&aud->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	aud->timer.function = s5l8947x_timer_cb;
	aud->tick = ns_to_ktime(s5l8947x_tick_ns(aud));
	platform_set_drvdata(pdev, aud);

	aud->i2s = devm_platform_ioremap_resource_byname(pdev, "i2s0");
	if (IS_ERR(aud->i2s)) {
		ret = PTR_ERR(aud->i2s);
		goto err_free_card;
	}

	aud->spdif = devm_platform_ioremap_resource_byname(pdev, "spdif");
	if (IS_ERR(aud->spdif)) {
		ret = PTR_ERR(aud->spdif);
		goto err_free_card;
	}

	aud->cdma_ch = CDMA_CH_SPDIF;
	cdma_np = of_parse_phandle(pdev->dev.of_node, "cdma", 0);
	if (cdma_np) {
		if (!of_property_read_u32(pdev->dev.of_node, "cdma-channel",
					  &ch) && ch >= 1 && ch <= 42)
			aud->cdma_ch = ch;
		if (!of_address_to_resource(cdma_np, 0, &res)) {
			aud->cdma = devm_ioremap_resource(&pdev->dev, &res);
			if (IS_ERR(aud->cdma)) {
				dev_warn(&pdev->dev,
					 "CDMA window unavailable, using PIO\n");
				aud->cdma = NULL;
			}
		}
		of_node_put(cdma_np);
	}
	if (output == OUT_SPDIF && aud->cdma) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret)
			dev_warn(&pdev->dev,
				 "no 32-bit DMA mask, using PIO: %d\n", ret);
		else
			aud->dma = true;
	}

	if (output == OUT_I2S) {
		aud->out.kind = OUT_I2S;
		aud->out.wpf = 2;
		aud->out.depth = I2S_FIFO_DEPTH;
	} else {
		aud->out.kind = OUT_SPDIF;
		aud->out.wpf = 4;
		aud->out.depth = SPDIF_FIFO_DEPTH;
	}
	aud->out.maxlevel = aud->out.depth - 1;
	aud->out.high = aud->out.maxlevel / 4 * 3;

	ret = devm_clk_bulk_get_all_enable(&pdev->dev, &clks);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to enable clocks\n");
		goto err_free_card;
	}

	strscpy(card->driver, "s5l8947x-audio", sizeof(card->driver));
	strscpy(card->shortname, "S5L8947X HDMI/SPDIF", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "S5L8947X Audio Engine (%s) at 0x34190000",
		 aud->out.kind == OUT_SPDIF ? "S/PDIF -> HDMI/optical" : "I2S0");

	ret = snd_pcm_new(card, "S5L8947X Digital PCM", 0, 1, 0, &pcm);
	if (ret < 0)
		goto err_free_card;

	pcm->private_data = aud;
	strscpy(pcm->name, "S5L8947X HDMI + Optical PCM", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			aud->dma ? &s5l8947x_pcm_ops_dma : &s5l8947x_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       32 * 1024, 64 * 1024);

	for (i = 0; i < ARRAY_SIZE(s5l8947x_audio_controls); i++) {
		ret = snd_ctl_add(card,
				  snd_ctl_new1(&s5l8947x_audio_controls[i], aud));
		if (ret < 0)
			goto err_free_card;
	}

	ret = snd_card_register(card);
	if (ret < 0)
		goto err_free_card;

	dev_info(&pdev->dev,
		 "registered '%s': %s, %u Hz, %u words/frame, %u-entry FIFO, %s, tick %llu us\n",
		 card->shortname,
		 aud->out.kind == OUT_SPDIF ? "S/PDIF -> HDMI + optical" : "I2S0",
		 aud->rate, aud->out.wpf, aud->out.depth,
		 aud->dma ? "CDMA channel" : "PIO transport",
		 div_u64((u64)ktime_to_ns(aud->tick), 1000));
	return 0;

err_free_card:
	snd_card_free(card);
	return ret;
}

static void s5l8947x_audio_remove(struct platform_device *pdev)
{
	struct s5l8947x_audio *aud = platform_get_drvdata(pdev);

	hrtimer_cancel(&aud->timer);
	if (aud->dma_started) {
		aud->dma_started = false;
		s5l8947x_dma_halt(aud);
	}
	s5l8947x_dma_free(aud);
	snd_card_free(aud->card);
}

static const struct of_device_id s5l8947x_audio_of_match[] = {
	{ .compatible = "apple,s5l8947x-audio" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_audio_of_match);

static struct platform_driver s5l8947x_audio_driver = {
	.probe	= s5l8947x_audio_probe,
	.remove	= s5l8947x_audio_remove,
	.driver	= {
		.name		= "s5l8947x-audio",
		.of_match_table	= s5l8947x_audio_of_match,
		.dev_groups	= s5l8947x_audio_groups,
	},
};
module_platform_driver(s5l8947x_audio_driver);

MODULE_DESCRIPTION("S5L8947X Audio Engine (S/PDIF -> HDMI/optical, I2S0) ALSA Driver");
MODULE_LICENSE("GPL");
