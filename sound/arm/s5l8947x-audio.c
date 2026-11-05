// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Audio Engine - ALSA playback for HDMI (I2S) and optical (S/PDIF).
 *
 * The card has two PCM devices:
 *
 *   0  HDMI     I2S0..I2S3 (0x34190000..0x34193000), all slaves of the I2S
 *               clock master (0x34195000).  Two channels use I2S0 alone;
 *               4/6/8 channels use 2/3/4 data lines, one channel pair each,
 *               in the CEA-861 order the HDMI sink expects.
 *   1  Optical  S/PDIF transmitter (0x34194000).
 *
 * Both blocks are programmed from the measured register model, not the
 * Samsung v5 map:
 *
 *   I2S  +0x00 CTL    enable, soft reset, stop request
 *        +0x04 TXCON  BCLK divider [10:8], frame length [4:3], slave bit 20
 *        +0x08 TXCOM  TX enable / pause
 *        +0x10 TXDATA one word per channel sample
 *   S/PDIF +0x00 CLKCON power, +0x04 CON format, +0x0c CSTAS, +0x10 DATA
 *
 * The I2S master runs from its master clock, a PMGR oscillator ("mclk") set to
 * 256 * fs: it divides it by 8 into the bit clock of 32-bit frames, so HDMI
 * plays 32, 44.1 and 48 kHz exactly. The optical output still runs from the
 * 24 MHz reference at 46875 Hz (MCLK / 512); applications that need another
 * rate there must resample.
 *
 * Each output is fed through the dmaengine API by the central DMA with a
 * cyclic transfer of 32-bit words, one per FIFO request, from a coherent ring
 * of (L, R) words. The 16-bit sample is repeated in both halves of the word:
 * S/PDIF takes bits [15:0], the 16-bit I2S slot the half it shifts out first.
 * Each period ends with a DMA interrupt, and the playback position is the
 * controller's residue, at segment granularity.
 */

#define pr_fmt(fmt) "s5l8947x-audio: " fmt

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

/* I2S (measured layout, same in I2S0..3 and the clock master) */
#define I2S_CTL			0x00
#define I2S_TXCON		0x04
#define I2S_TXCOM		0x08
#define I2S_TXDATA		0x10

#define I2S_CTL_ENABLE		0x00000015U	/* enable, as the block expects */
#define I2S_CTL_ENABLE_MCLK	0x00000001U	/* enable, clocked from mclk */
#define I2S_CTL_RESET		0x00000040U	/* self-clearing soft reset */
#define I2S_CTL_STOP		0x00000034U
#define I2S_TXCON_BASE		0x03000001U	/* always set, 16-bit samples */
#define I2S_TXCON_BCLK_DIV8	0x00000300U	/* BCLK = mclk / 8, 32-bit frame */
#define I2S_MCLK_FS		256U
#define I2S_TXCON_SLAVE		0x00100018U	/* clocks from the master */
#define I2S_TXCOM_RUN		0x01f80006U	/* TX enabled */
/* TX enabled with a DMA request threshold ([18:14]); without it no requests */
#define I2S_TXCOM_DMA		0x01feca06U

/* S/PDIF transmitter (measured layout, matches mainline samsung/spdif.c) */
#define SPDIF_CLKCON		0x00
#define SPDIF_CON		0x04
#define SPDIF_CSTAS		0x0c
#define SPDIF_DATA		0x10

#define SPDIF_CLKCON_PWR	0x00000001U
#define SPDIF_CON_SW_RESET	0x00000020U
#define SPDIF_CON_46875		0x00381011U	/* PCM, 16-bit, MCLK / 512, FIFO thr 7 */
#define SPDIF_CSTAS_48K		0x02000104U	/* consumer PCM, no copyright, 48 kHz code */

/* One FIFO word per request; 16-word bursts outrun the 16-entry S/PDIF FIFO */
#define AUDIO_DMA_WIDTH		DMA_SLAVE_BUSWIDTH_4_BYTES
#define AUDIO_DMA_BURST		1
#define AUDIO_MAX_HDMI_LINES	4
#define AUDIO_LINE_FRAME_BYTES	8	/* L, R words for one data line */

#define AUDIO_RATE_OPTICAL	46875U

enum s5l8947x_output {
	AUDIO_HDMI,
	AUDIO_OPTICAL,
	AUDIO_NUM_OUTPUTS,
};

struct s5l8947x_audio;

/*
 * One I2S data line: its register window, the DMA channel that feeds its
 * FIFO, and the coherent ring shaped like the application's frames.
 */
struct s5l8947x_line {
	void __iomem *i2s;
	phys_addr_t fifo_phys;

	struct dma_chan *dma;
	dma_cookie_t cookie;
	bool dma_started;

	u32 *ring;
	dma_addr_t ring_phys;
	size_t ring_bytes;
	unsigned int ring_frames;
};

struct s5l8947x_stream {
	struct s5l8947x_audio *aud;
	enum s5l8947x_output out;
	struct snd_pcm_substream *substream;

	unsigned int rate;
	unsigned int channels;
	unsigned int nlines;

	/* S/PDIF transmitter */
	phys_addr_t fifo_phys;
	struct dma_chan *dma;
	dma_cookie_t cookie;
	bool dma_started;
	u32 *dma_ring;
	dma_addr_t dma_ring_phys;
	size_t dma_ring_bytes;
	unsigned int dma_ring_frames;

	/* HDMI I2S data lines */
	struct s5l8947x_line lines[AUDIO_MAX_HDMI_LINES];
};

struct s5l8947x_audio {
	struct device *dev;
	struct snd_card *card;

	void __iomem *i2s[AUDIO_MAX_HDMI_LINES];	/* data lines */
	void __iomem *i2s_master;	/* bit and frame clocks */
	void __iomem *spdif;
	struct clk *mclk;		/* I2S master clock */
	bool mclk_on;
	bool hdmi_enable;

	struct s5l8947x_stream streams[AUDIO_NUM_OUTPUTS];
};

static const unsigned int s5l8947x_hdmi_rates[] = {
	32000, 44100, 48000,
};

static const unsigned int s5l8947x_optical_rates[] = {
	AUDIO_RATE_OPTICAL,
};

/*
 * One channel pair per I2S data line.  The HDMI receiver maps I2S0 to
 * channels 0/1, I2S1 to 2/3 and so on, so only even counts are usable.
 */
static const unsigned int s5l8947x_hdmi_channels[] = {
	2, 4, 6, 8,
};

static const struct snd_pcm_hw_constraint_list s5l8947x_rate_constraints[] = {
	[AUDIO_HDMI] = {
		.count	= ARRAY_SIZE(s5l8947x_hdmi_rates),
		.list	= s5l8947x_hdmi_rates,
	},
	[AUDIO_OPTICAL] = {
		.count	= ARRAY_SIZE(s5l8947x_optical_rates),
		.list	= s5l8947x_optical_rates,
	},
};

static const struct snd_pcm_hw_constraint_list s5l8947x_channel_constraints = {
	.count	= ARRAY_SIZE(s5l8947x_hdmi_channels),
	.list	= s5l8947x_hdmi_channels,
};

/*
 * ALSA's default multichannel order is FL, FR, RL, RR, FC, LFE[, SL, SR],
 * while the sink expects the CEA-861 order FL, FR, FC, LFE, RL, RR[, ...].
 * Map each data line's left channel to the stream channel it must carry;
 * the right channel follows.
 */
static const u8 s5l8947x_line_map[9][8] = {
	[2] = { 0, 1 },
	[4] = { 0, 1, 2, 3 },
	[6] = { 0, 1, 4, 5, 2, 3 },
	[8] = { 0, 1, 4, 5, 2, 3, 6, 7 },
};

static const struct snd_pcm_hardware s5l8947x_pcm_hw = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_KNOT,
	.rate_min		= 32000,
	.rate_max		= 48000,
	.channels_min		= 2,
	.channels_max		= 8,
	.buffer_bytes_max	= 256 * 1024,
	.period_bytes_min	= 1024,
	.period_bytes_max	= 32 * 1024,
	.periods_min		= 2,
	.periods_max		= 32,
};

static void s5l8947x_mclk_off(struct s5l8947x_audio *aud)
{
	if (aud->mclk_on) {
		clk_disable_unprepare(aud->mclk);
		aud->mclk_on = false;
	}
}

static int s5l8947x_mclk_on(struct s5l8947x_audio *aud, unsigned int rate)
{
	int ret;

	/* The oscillator only takes a new rate while it is stopped */
	s5l8947x_mclk_off(aud);
	ret = clk_set_rate(aud->mclk, (unsigned long)rate * I2S_MCLK_FS);
	if (ret)
		return ret;
	ret = clk_prepare_enable(aud->mclk);
	if (ret)
		return ret;
	aud->mclk_on = true;
	return 0;
}

static void s5l8947x_line_stop(struct s5l8947x_line *ln)
{
	writel(0, ln->i2s + I2S_TXCOM);
	writel(I2S_CTL_STOP, ln->i2s + I2S_CTL);
}

static void s5l8947x_line_config(struct s5l8947x_line *ln)
{
	writel(I2S_CTL_RESET, ln->i2s + I2S_CTL);
	writel(0, ln->i2s + I2S_TXCOM);
	writel(I2S_TXCON_BASE | I2S_TXCON_SLAVE, ln->i2s + I2S_TXCON);
	writel(I2S_CTL_ENABLE, ln->i2s + I2S_CTL);
}

static int s5l8947x_engine_config(struct s5l8947x_stream *st)
{
	struct s5l8947x_audio *aud = st->aud;
	unsigned int i;
	int ret;

	if (st->out == AUDIO_OPTICAL) {
		if (st->rate != AUDIO_RATE_OPTICAL)
			return -EINVAL;
		writel(0, aud->spdif + SPDIF_CLKCON);
		writel(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
		writel(SPDIF_CON_46875, aud->spdif + SPDIF_CON);
		writel(SPDIF_CSTAS_48K, aud->spdif + SPDIF_CSTAS);
		return 0;
	}

	ret = s5l8947x_mclk_on(aud, st->rate);
	if (ret)
		return ret;

	/*
	 * The master's clocks must be running before the slaves are reset: a
	 * slave brought out of reset without a bit clock never starts draining.
	 * The master's own FIFO stays empty; only the I2S0..3 data lines reach
	 * HDMI.
	 */
	writel(I2S_CTL_RESET, aud->i2s_master + I2S_CTL);
	writel(0, aud->i2s_master + I2S_TXCOM);
	writel(I2S_TXCON_BASE | I2S_TXCON_BCLK_DIV8,
	       aud->i2s_master + I2S_TXCON);
	writel(I2S_CTL_ENABLE_MCLK, aud->i2s_master + I2S_CTL);
	writel(I2S_TXCOM_RUN, aud->i2s_master + I2S_TXCOM);

	for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++) {
		if (i < st->nlines)
			s5l8947x_line_config(&st->lines[i]);
		else
			s5l8947x_line_stop(&st->lines[i]);
	}
	return 0;
}

static void s5l8947x_engine_start(struct s5l8947x_stream *st)
{
	unsigned int i;

	if (st->out == AUDIO_OPTICAL) {
		writel(SPDIF_CLKCON_PWR, st->aud->spdif + SPDIF_CLKCON);
		return;
	}
	for (i = 0; i < st->nlines; i++)
		writel(I2S_TXCOM_DMA, st->lines[i].i2s + I2S_TXCOM);
}

static void s5l8947x_engine_stop(struct s5l8947x_stream *st)
{
	struct s5l8947x_audio *aud = st->aud;
	unsigned int i;

	if (st->out == AUDIO_OPTICAL) {
		writel(0, aud->spdif + SPDIF_CLKCON);
		writel(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
		return;
	}
	for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++)
		s5l8947x_line_stop(&st->lines[i]);
	writel(0, aud->i2s_master + I2S_TXCOM);
	writel(I2S_CTL_STOP, aud->i2s_master + I2S_CTL);
}

static void s5l8947x_line_dma_free(struct device *dev,
				   struct s5l8947x_line *ln)
{
	if (ln->ring) {
		dma_free_coherent(dev, ln->ring_bytes, ln->ring, ln->ring_phys);
		ln->ring = NULL;
	}
	ln->ring_frames = 0;
}

static void s5l8947x_dma_free(struct s5l8947x_stream *st)
{
	unsigned int i;

	if (st->out == AUDIO_HDMI) {
		for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++)
			s5l8947x_line_dma_free(st->aud->dev, &st->lines[i]);
		return;
	}
	if (st->dma_ring) {
		dma_free_coherent(st->aud->dev, st->dma_ring_bytes,
				  st->dma_ring, st->dma_ring_phys);
		st->dma_ring = NULL;
	}
	st->dma_ring_frames = 0;
}

/*
 * Each data line's DMA ring mirrors the application's frames one-for-one,
 * but holds only that line's channel pair: two 32-bit FIFO words per frame.
 * It is rebuilt whenever the application changes the buffer size.
 */
static int s5l8947x_line_dma_setup(struct s5l8947x_audio *aud,
				   struct s5l8947x_line *ln,
				   unsigned int frames)
{
	struct dma_slave_config cfg = {
		.direction	= DMA_MEM_TO_DEV,
		.dst_addr	= ln->fifo_phys,
		.dst_addr_width	= AUDIO_DMA_WIDTH,
		.dst_maxburst	= AUDIO_DMA_BURST,
	};
	size_t bytes = frames * AUDIO_LINE_FRAME_BYTES;
	int ret;

	if (!frames)
		return -EINVAL;
	ret = dmaengine_slave_config(ln->dma, &cfg);
	if (ret)
		return ret;
	if (ln->ring && ln->ring_frames == frames)
		goto clear;

	s5l8947x_line_dma_free(aud->dev, ln);
	ln->ring = dma_alloc_coherent(aud->dev, bytes, &ln->ring_phys,
				      GFP_KERNEL);
	if (!ln->ring)
		return -ENOMEM;
	ln->ring_bytes = bytes;
	ln->ring_frames = frames;

clear:
	/* Never replay a previous stream's samples */
	memset(ln->ring, 0, ln->ring_bytes);
	return 0;
}

static int s5l8947x_dma_setup(struct s5l8947x_stream *st,
			      struct snd_pcm_runtime *runtime)
{
	struct dma_slave_config cfg = {
		.direction	= DMA_MEM_TO_DEV,
		.dst_addr	= st->fifo_phys,
		.dst_addr_width	= AUDIO_DMA_WIDTH,
		.dst_maxburst	= AUDIO_DMA_BURST,
	};
	unsigned int frames = runtime->buffer_size;
	unsigned int i;
	size_t bytes;
	int ret;

	if (st->out == AUDIO_HDMI) {
		for (i = 0; i < st->nlines; i++) {
			ret = s5l8947x_line_dma_setup(st->aud, &st->lines[i],
						      frames);
			if (ret)
				return ret;
		}
		return 0;
	}

	bytes = frames * AUDIO_LINE_FRAME_BYTES;
	if (!frames)
		return -EINVAL;
	ret = dmaengine_slave_config(st->dma, &cfg);
	if (ret)
		return ret;
	if (st->dma_ring && st->dma_ring_frames == frames)
		goto clear;

	s5l8947x_dma_free(st);
	st->dma_ring = dma_alloc_coherent(st->aud->dev, bytes,
					  &st->dma_ring_phys, GFP_KERNEL);
	if (!st->dma_ring)
		return -ENOMEM;
	st->dma_ring_bytes = bytes;
	st->dma_ring_frames = frames;

clear:
	memset(st->dma_ring, 0, st->dma_ring_bytes);
	return 0;
}

static void s5l8947x_dma_period(void *data)
{
	snd_pcm_period_elapsed(data);
}

static void s5l8947x_dma_halt(struct s5l8947x_stream *st)
{
	unsigned int i;

	if (st->out == AUDIO_HDMI) {
		for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++) {
			struct s5l8947x_line *ln = &st->lines[i];

			if (ln->dma_started) {
				ln->dma_started = false;
				dmaengine_terminate_async(ln->dma);
			}
		}
		return;
	}
	if (st->dma_started) {
		st->dma_started = false;
		dmaengine_terminate_async(st->dma);
	}
}

static void s5l8947x_dma_sync(struct s5l8947x_stream *st)
{
	unsigned int i;

	if (st->out == AUDIO_HDMI) {
		for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++)
			dmaengine_synchronize(st->lines[i].dma);
		return;
	}
	dmaengine_synchronize(st->dma);
}

static int s5l8947x_dma_start(struct s5l8947x_stream *st)
{
	struct snd_pcm_runtime *runtime = st->substream->runtime;
	unsigned int i;

	if (st->out == AUDIO_HDMI) {
		for (i = 0; i < st->nlines; i++) {
			struct s5l8947x_line *ln = &st->lines[i];
			struct dma_async_tx_descriptor *desc;

			desc = dmaengine_prep_dma_cyclic(ln->dma,
					ln->ring_phys, ln->ring_bytes,
					runtime->period_size *
						AUDIO_LINE_FRAME_BYTES,
					DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
			if (!desc)
				return -ENOMEM;
			/*
			 * Only line 0 reports periods: all lines run from the
			 * same bit clock and advance in lockstep.
			 */
			if (i == 0) {
				desc->callback = s5l8947x_dma_period;
				desc->callback_param = st->substream;
			}
			ln->cookie = dmaengine_submit(desc);
			dma_async_issue_pending(ln->dma);
			ln->dma_started = true;
		}
		return 0;
	}

	{
		struct dma_async_tx_descriptor *desc;

		desc = dmaengine_prep_dma_cyclic(st->dma, st->dma_ring_phys,
						 st->dma_ring_bytes,
						 runtime->period_size *
						 AUDIO_LINE_FRAME_BYTES,
						 DMA_MEM_TO_DEV,
						 DMA_PREP_INTERRUPT);
		if (!desc)
			return -ENOMEM;
		desc->callback = s5l8947x_dma_period;
		desc->callback_param = st->substream;
		st->cookie = dmaengine_submit(desc);
		dma_async_issue_pending(st->dma);
		st->dma_started = true;
	}
	return 0;
}

static struct s5l8947x_stream *
s5l8947x_substream_stream(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	return &aud->streams[substream->pcm->device];
}

/*
 * Convert the application's interleaved S16 frames into each data line's
 * FIFO word layout.  Called from the PCM copy callback, which is serialized
 * with the stream.
 */
static int s5l8947x_pcm_copy(struct snd_pcm_substream *substream, int channel,
			     unsigned long pos, struct iov_iter *iter,
			     unsigned long count)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	unsigned int frame_bytes = st->channels * 2;
	unsigned int frames = count / frame_bytes;
	unsigned int frame;
	s16 tmp[256];
	unsigned int n, i, line;

	if (st->out == AUDIO_OPTICAL) {
		u32 *ring = st->dma_ring;
		s16 stereo[128];

		if (!ring)
			return -EINVAL;
		frame = (pos / frame_bytes) % st->dma_ring_frames;
		while (frames) {
			n = min(frames, ARRAY_SIZE(stereo) / 2);
			if (copy_from_iter(stereo, n * 4, iter) != n * 4)
				return -EFAULT;
			for (i = 0; i < n; i++) {
				u32 l = (u16)stereo[i * 2];
				u32 r = (u16)stereo[i * 2 + 1];
				u32 *dst = ring + frame * 2;

				dst[0] = l << 16 | l;
				dst[1] = r << 16 | r;
				if (++frame == st->dma_ring_frames)
					frame = 0;
			}
			frames -= n;
		}
		return 0;
	}

	for (line = 0; line < st->nlines; line++)
		if (!st->lines[line].ring)
			return -EINVAL;
	frame = (pos / frame_bytes) % st->lines[0].ring_frames;

	while (frames) {
		n = min3(frames, (unsigned int)(ARRAY_SIZE(tmp) / st->channels),
			 st->lines[0].ring_frames - frame);
		if (copy_from_iter(tmp, n * frame_bytes, iter) !=
		    n * frame_bytes)
			return -EFAULT;
		for (line = 0; line < st->nlines; line++) {
			unsigned int src = s5l8947x_line_map[st->channels][line * 2];
			u32 *dst = st->lines[line].ring + frame * 2;
			unsigned int f;

			for (f = 0; f < n; f++) {
				s16 *s = tmp + f * st->channels + src;
				u32 l = (u16)s[0], r = (u16)s[1];

				dst[0] = l << 16 | l;
				dst[1] = r << 16 | r;
				dst += 2;
			}
		}
		frame += n;
		if (frame >= st->lines[0].ring_frames)
			frame -= st->lines[0].ring_frames;
		frames -= n;
	}
	return 0;
}

static int s5l8947x_pcm_open(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	runtime->hw = s5l8947x_pcm_hw;

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &s5l8947x_rate_constraints[st->out]);
	if (ret < 0)
		return ret;

	if (st->out == AUDIO_HDMI) {
		ret = snd_pcm_hw_constraint_list(runtime, 0,
						 SNDRV_PCM_HW_PARAM_CHANNELS,
						 &s5l8947x_channel_constraints);
		if (ret < 0)
			return ret;
	}

	/* A cyclic transfer is a whole number of periods */
	ret = snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;

	st->substream = substream;
	return 0;
}

static int s5l8947x_pcm_close(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);

	s5l8947x_dma_halt(st);
	s5l8947x_dma_sync(st);
	s5l8947x_engine_stop(st);
	if (st->out == AUDIO_HDMI)
		s5l8947x_mclk_off(st->aud);
	st->substream = NULL;
	if (st->out == AUDIO_HDMI && st->aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(st->rate, st->channels, false);
	s5l8947x_dma_free(st);
	return 0;
}

static int s5l8947x_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	st->rate = runtime->rate;
	st->channels = runtime->channels;
	st->nlines = st->out == AUDIO_HDMI ? st->channels / 2 : 1;
	s5l8947x_dma_halt(st);
	s5l8947x_dma_sync(st);

	ret = s5l8947x_engine_config(st);
	if (ret < 0)
		return ret;
	ret = s5l8947x_dma_setup(st, runtime);
	if (ret < 0)
		return ret;

	/* Prime the whole ring before the engine fetches it. */
	runtime->start_threshold = runtime->buffer_size;

	if (st->out == AUDIO_HDMI && st->aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(st->rate, st->channels, true);
	return 0;
}

static int s5l8947x_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/*
		 * S/PDIF: powering the transmitter starts its frame clock, which
		 * paces the DMA requests. I2S: the master's clocks already run,
		 * enabling the slave's TX turns its DMA requests on.
		 */
		s5l8947x_engine_start(st);
		ret = s5l8947x_dma_start(st);
		if (ret) {
			s5l8947x_engine_stop(st);
			return ret;
		}

		/* The clock is running now, so the ACR will latch */
		if (st->out == AUDIO_HDMI && st->aud->hdmi_enable)
			s5l8947x_hdmi_audio_arm(st->rate);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		s5l8947x_dma_halt(st);
		s5l8947x_engine_stop(st);
		return 0;

	default:
		return -EINVAL;
	}
}

static int s5l8947x_pcm_sync_stop(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);

	s5l8947x_dma_sync(st);
	return 0;
}

static snd_pcm_uframes_t
s5l8947x_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	struct dma_tx_state state;
	struct dma_chan *dma;
	dma_cookie_t cookie;
	size_t bytes, played;

	if (st->out == AUDIO_HDMI) {
		/* All lines run from the same clock; line 0 is the reference. */
		dma = st->lines[0].dma;
		cookie = st->lines[0].cookie;
		bytes = st->lines[0].ring_bytes;
		if (!st->lines[0].dma_started || !bytes)
			return 0;
	} else {
		dma = st->dma;
		cookie = st->cookie;
		bytes = st->dma_ring_bytes;
		if (!st->dma_started || !bytes)
			return 0;
	}

	dmaengine_tx_status(dma, cookie, &state);
	if (state.residue > bytes)
		return 0;
	played = bytes - state.residue;
	return (played / AUDIO_LINE_FRAME_BYTES) % substream->runtime->buffer_size;
}

static const struct snd_pcm_ops s5l8947x_pcm_ops = {
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
	struct s5l8947x_stream *st = &aud->streams[AUDIO_HDMI];
	bool en = !!ucontrol->value.integer.value[0];

	if (aud->hdmi_enable == en)
		return 0;
	aud->hdmi_enable = en;
	if (st->substream) {
		s5l8947x_hdmi_audio_configure(st->rate, st->channels, en);
		if (en && st->dma_started)
			s5l8947x_hdmi_audio_arm(st->rate);
	}
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

static void __iomem *s5l8947x_map(struct platform_device *pdev,
				  const char *name, phys_addr_t *phys)
{
	struct resource *res;
	void __iomem *base;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (!IS_ERR(base) && phys)
		*phys = res->start;
	return base;
}

static const char * const s5l8947x_gate_names[] = {
	"i2s-master", "i2s0", "i2s1", "i2s2", "i2s3", "spdif",
};

#define AUDIO_NUM_GATES		ARRAY_SIZE(s5l8947x_gate_names)

static void s5l8947x_gates_disable(void *gates)
{
	clk_bulk_disable_unprepare(AUDIO_NUM_GATES, gates);
}

static const char * const s5l8947x_i2s_names[AUDIO_MAX_HDMI_LINES] = {
	"i2s0", "i2s1", "i2s2", "i2s3",
};

static const char * const s5l8947x_hdmi_dma_names[AUDIO_MAX_HDMI_LINES] = {
	"hdmi0", "hdmi1", "hdmi2", "hdmi3",
};

static void s5l8947x_audio_release_dma(struct s5l8947x_audio *aud)
{
	unsigned int i;

	for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++) {
		struct s5l8947x_line *ln = &aud->streams[AUDIO_HDMI].lines[i];

		if (ln->dma)
			dma_release_channel(ln->dma);
	}
	if (aud->streams[AUDIO_OPTICAL].dma)
		dma_release_channel(aud->streams[AUDIO_OPTICAL].dma);
}

static int s5l8947x_audio_probe(struct platform_device *pdev)
{
	static const char * const pcm_names[AUDIO_NUM_OUTPUTS] = {
		[AUDIO_HDMI]	= "S5L8947X HDMI (I2S)",
		[AUDIO_OPTICAL]	= "S5L8947X Optical (S/PDIF)",
	};
	struct s5l8947x_stream *hdmi;
	struct s5l8947x_stream *opt;
	struct s5l8947x_audio *aud;
	struct snd_card *card;
	struct clk_bulk_data *gates;
	struct snd_pcm *pcm;
	phys_addr_t phys, spdif_phys;
	unsigned int i;
	int ret;

	ret = snd_card_new(&pdev->dev, SNDRV_DEFAULT_IDX1, "S5L8947XAudio",
			   THIS_MODULE, sizeof(*aud), &card);
	if (ret < 0)
		return ret;

	aud = card->private_data;
	aud->dev = &pdev->dev;
	aud->card = card;
	aud->hdmi_enable = true;
	hdmi = &aud->streams[AUDIO_HDMI];
	opt = &aud->streams[AUDIO_OPTICAL];
	platform_set_drvdata(pdev, aud);

	aud->i2s_master = s5l8947x_map(pdev, "i2s-master", NULL);
	aud->spdif = s5l8947x_map(pdev, "spdif", &spdif_phys);
	if (IS_ERR(aud->i2s_master) || IS_ERR(aud->spdif)) {
		ret = -ENODEV;
		goto err_free_card;
	}

	for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++) {
		aud->i2s[i] = s5l8947x_map(pdev, s5l8947x_i2s_names[i], &phys);
		if (IS_ERR(aud->i2s[i])) {
			ret = -ENODEV;
			goto err_free_card;
		}
		hdmi->lines[i].fifo_phys = phys + I2S_TXDATA;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free_card;

	gates = devm_kcalloc(&pdev->dev, AUDIO_NUM_GATES, sizeof(*gates),
			     GFP_KERNEL);
	if (!gates) {
		ret = -ENOMEM;
		goto err_free_card;
	}
	for (i = 0; i < AUDIO_NUM_GATES; i++)
		gates[i].id = s5l8947x_gate_names[i];
	ret = devm_clk_bulk_get(&pdev->dev, AUDIO_NUM_GATES, gates);
	if (!ret)
		ret = clk_bulk_prepare_enable(AUDIO_NUM_GATES, gates);
	if (!ret)
		ret = devm_add_action_or_reset(&pdev->dev,
					       s5l8947x_gates_disable, gates);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to enable clocks\n");
		goto err_free_card;
	}
	aud->mclk = devm_clk_get(&pdev->dev, "mclk");
	if (IS_ERR(aud->mclk)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(aud->mclk),
				    "failed to get the I2S master clock\n");
		goto err_free_card;
	}

	for (i = 0; i < AUDIO_NUM_OUTPUTS; i++) {
		aud->streams[i].aud = aud;
		aud->streams[i].out = i;
		aud->streams[i].rate = i == AUDIO_HDMI ? 48000 : AUDIO_RATE_OPTICAL;
		aud->streams[i].channels = 2;
		aud->streams[i].nlines = 1;
	}
	opt->fifo_phys = spdif_phys + SPDIF_DATA;

	for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++) {
		struct s5l8947x_line *ln = &hdmi->lines[i];

		ln->i2s = aud->i2s[i];
		ln->dma = dma_request_chan(&pdev->dev,
					   s5l8947x_hdmi_dma_names[i]);
		if (IS_ERR(ln->dma)) {
			ret = dev_err_probe(&pdev->dev, PTR_ERR(ln->dma),
					    "failed to get the %s DMA channel\n",
					    s5l8947x_hdmi_dma_names[i]);
			ln->dma = NULL;
			goto err_release_dma;
		}
	}

	opt->dma = dma_request_chan(&pdev->dev, "optical");
	if (IS_ERR(opt->dma)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(opt->dma),
				    "failed to get the optical DMA channel\n");
		opt->dma = NULL;
		goto err_release_dma;
	}

	strscpy(card->driver, "s5l8947x-audio", sizeof(card->driver));
	strscpy(card->shortname, "S5L8947X HDMI/SPDIF", sizeof(card->shortname));
	strscpy(card->longname, "S5L8947X Audio Engine (I2S -> HDMI, S/PDIF -> optical)",
		sizeof(card->longname));

	for (i = 0; i < AUDIO_NUM_OUTPUTS; i++) {
		ret = snd_pcm_new(card, pcm_names[i], i, 1, 0, &pcm);
		if (ret < 0)
			goto err_release_dma;
		pcm->private_data = aud;
		strscpy(pcm->name, pcm_names[i], sizeof(pcm->name));
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &s5l8947x_pcm_ops);
		/*
		 * ALSA caps the usable buffer at the preallocated size, so
		 * reserve the maximum: 8192 frames of 8-channel audio.
		 */
		snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
					       256 * 1024, 256 * 1024);
	}

	for (i = 0; i < ARRAY_SIZE(s5l8947x_audio_controls); i++) {
		ret = snd_ctl_add(card,
				  snd_ctl_new1(&s5l8947x_audio_controls[i], aud));
		if (ret < 0)
			goto err_release_dma;
	}

	ret = snd_card_register(card);
	if (ret < 0)
		goto err_release_dma;

	dev_info(&pdev->dev,
		 "registered '%s': pcm0 I2S x4 -> HDMI (32/44.1/48 kHz, 2..8 ch, DMA %s), pcm1 S/PDIF -> optical (%u Hz, DMA %s)\n",
		 card->shortname, dma_chan_name(hdmi->lines[0].dma),
		 AUDIO_RATE_OPTICAL, dma_chan_name(opt->dma));
	return 0;

err_release_dma:
	s5l8947x_audio_release_dma(aud);
err_free_card:
	snd_card_free(card);
	return ret;
}

static void s5l8947x_audio_remove(struct platform_device *pdev)
{
	struct s5l8947x_audio *aud = platform_get_drvdata(pdev);
	unsigned int i;

	/* Closing the PCMs stops the transfers and frees the rings */
	snd_card_free(aud->card);
	for (i = 0; i < AUDIO_MAX_HDMI_LINES; i++)
		if (aud->streams[AUDIO_HDMI].lines[i].dma)
			dma_release_channel(aud->streams[AUDIO_HDMI].lines[i].dma);
	if (aud->streams[AUDIO_OPTICAL].dma)
		dma_release_channel(aud->streams[AUDIO_OPTICAL].dma);
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
	},
};
module_platform_driver(s5l8947x_audio_driver);

MODULE_DESCRIPTION("S5L8947X Audio Engine (I2S -> HDMI, S/PDIF -> optical) ALSA Driver");
MODULE_LICENSE("GPL");
