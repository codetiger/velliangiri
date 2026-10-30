// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Audio Engine - ALSA playback for HDMI (I2S) and optical (S/PDIF).
 *
 * The card has two PCM devices:
 *
 *   0  HDMI     I2S0 (0x34190000) as a slave of the I2S clock master
 *               (0x34195000); the HDMI I2S receiver takes I2S0's data line
 *               and the master's bit and frame clocks.
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
 * Everything is clocked from the 24 MHz reference, so the frame rates are
 * 24 MHz divisions: HDMI runs at 46875 Hz (32-bit frames, BCLK / 16) or 31250
 * Hz (48-bit frames), optical at 46875 Hz (MCLK / 512). The hardware cannot
 * produce 48000 from this reference, and pretending it does would make the
 * HDMI ACR announce a rate that is not being sent. Applications that need
 * another rate must resample.
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
#define I2S_CTL_RESET		0x00000040U	/* self-clearing soft reset */
#define I2S_CTL_STOP		0x00000034U
#define I2S_TXCON_BASE		0x03000001U	/* always set, 16-bit samples */
#define I2S_TXCON_46875		0x00000700U	/* BCLK = 24 MHz / 16, 32-bit frame */
#define I2S_TXCON_31250		0x00000708U	/* BCLK = 24 MHz / 16, 48-bit frame */
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
#define AUDIO_FRAME_BYTES	8	/* L, R */

#define AUDIO_RATE_HIGH		46875U
#define AUDIO_RATE_LOW		31250U

enum s5l8947x_output {
	AUDIO_HDMI,
	AUDIO_OPTICAL,
	AUDIO_NUM_OUTPUTS,
};

struct s5l8947x_audio;

struct s5l8947x_stream {
	struct s5l8947x_audio *aud;
	enum s5l8947x_output out;
	struct snd_pcm_substream *substream;

	phys_addr_t fifo_phys;
	struct dma_chan *dma;
	dma_cookie_t cookie;
	bool dma_started;

	unsigned int rate;
	void *dma_ring;
	dma_addr_t dma_ring_phys;
	size_t dma_ring_bytes;
	unsigned int dma_ring_frames;
};

struct s5l8947x_audio {
	struct device *dev;
	struct snd_card *card;

	void __iomem *i2s;		/* I2S0, data */
	void __iomem *i2s_master;	/* bit and frame clocks */
	void __iomem *spdif;
	bool hdmi_enable;

	struct s5l8947x_stream streams[AUDIO_NUM_OUTPUTS];
};

static const unsigned int s5l8947x_hdmi_rates[] = {
	AUDIO_RATE_HIGH, AUDIO_RATE_LOW,
};

static const unsigned int s5l8947x_optical_rates[] = {
	AUDIO_RATE_HIGH,
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

static const struct snd_pcm_hardware s5l8947x_pcm_hw = {
	.info			= SNDRV_PCM_INFO_INTERLEAVED,
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

static int s5l8947x_engine_config(struct s5l8947x_stream *st)
{
	struct s5l8947x_audio *aud = st->aud;
	u32 clk;

	if (st->out == AUDIO_OPTICAL) {
		if (st->rate != AUDIO_RATE_HIGH)
			return -EINVAL;
		writel(0, aud->spdif + SPDIF_CLKCON);
		writel(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
		writel(SPDIF_CON_46875, aud->spdif + SPDIF_CON);
		writel(SPDIF_CSTAS_48K, aud->spdif + SPDIF_CSTAS);
		return 0;
	}

	switch (st->rate) {
	case AUDIO_RATE_HIGH:
		clk = I2S_TXCON_46875;
		break;
	case AUDIO_RATE_LOW:
		clk = I2S_TXCON_31250;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * The master's clocks must be running before the slave is reset: a
	 * slave brought out of reset without a bit clock never starts draining.
	 * The master's own FIFO stays empty; only I2S0's data reaches HDMI.
	 */
	writel(I2S_CTL_RESET, aud->i2s_master + I2S_CTL);
	writel(0, aud->i2s_master + I2S_TXCOM);
	writel(I2S_TXCON_BASE | clk, aud->i2s_master + I2S_TXCON);
	writel(I2S_CTL_ENABLE, aud->i2s_master + I2S_CTL);
	writel(I2S_TXCOM_RUN, aud->i2s_master + I2S_TXCOM);

	writel(I2S_CTL_RESET, aud->i2s + I2S_CTL);
	writel(0, aud->i2s + I2S_TXCOM);
	writel(I2S_TXCON_BASE | I2S_TXCON_SLAVE, aud->i2s + I2S_TXCON);
	writel(I2S_CTL_ENABLE, aud->i2s + I2S_CTL);
	return 0;
}

static void s5l8947x_engine_start(struct s5l8947x_stream *st)
{
	if (st->out == AUDIO_OPTICAL)
		writel(SPDIF_CLKCON_PWR, st->aud->spdif + SPDIF_CLKCON);
	else
		writel(I2S_TXCOM_DMA, st->aud->i2s + I2S_TXCOM);
}

static void s5l8947x_engine_stop(struct s5l8947x_stream *st)
{
	struct s5l8947x_audio *aud = st->aud;

	if (st->out == AUDIO_OPTICAL) {
		writel(0, aud->spdif + SPDIF_CLKCON);
		writel(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
		return;
	}
	writel(0, aud->i2s + I2S_TXCOM);
	writel(I2S_CTL_STOP, aud->i2s + I2S_CTL);
	writel(0, aud->i2s_master + I2S_TXCOM);
	writel(I2S_CTL_STOP, aud->i2s_master + I2S_CTL);
}

static void s5l8947x_dma_halt(struct s5l8947x_stream *st)
{
	if (st->dma_started) {
		st->dma_started = false;
		dmaengine_terminate_async(st->dma);
	}
}

static void s5l8947x_dma_free(struct s5l8947x_stream *st)
{
	if (st->dma_ring) {
		dma_free_coherent(st->aud->dev, st->dma_ring_bytes,
				  st->dma_ring, st->dma_ring_phys);
		st->dma_ring = NULL;
	}
	st->dma_ring_frames = 0;
}

/*
 * The DMA ring mirrors the application's frames one-for-one, but each frame
 * is two 32-bit FIFO words.  It is rebuilt whenever the application changes
 * the buffer size.
 */
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
	size_t bytes = frames * AUDIO_FRAME_BYTES;
	int ret;

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
	/* Never replay a previous stream's samples */
	memset(st->dma_ring, 0, st->dma_ring_bytes);
	return 0;
}

static void s5l8947x_dma_period(void *data)
{
	snd_pcm_period_elapsed(data);
}

static int s5l8947x_dma_start(struct s5l8947x_stream *st)
{
	struct snd_pcm_runtime *runtime = st->substream->runtime;
	struct dma_async_tx_descriptor *desc;

	desc = dmaengine_prep_dma_cyclic(st->dma, st->dma_ring_phys,
					 st->dma_ring_bytes,
					 runtime->period_size * AUDIO_FRAME_BYTES,
					 DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT);
	if (!desc)
		return -ENOMEM;
	desc->callback = s5l8947x_dma_period;
	desc->callback_param = st->substream;
	st->cookie = dmaengine_submit(desc);
	dma_async_issue_pending(st->dma);
	st->dma_started = true;
	return 0;
}

static struct s5l8947x_stream *
s5l8947x_substream_stream(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	return &aud->streams[substream->pcm->device];
}

/*
 * Convert the application's S16 stereo frames into the FIFO word layout.
 * Called from the PCM copy callback, which is serialized with the stream.
 */
static int s5l8947x_pcm_copy(struct snd_pcm_substream *substream, int channel,
			     unsigned long pos, struct iov_iter *iter,
			     unsigned long count)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	unsigned int frames = count / 4;
	u32 *ring = st->dma_ring;
	unsigned int frame;
	s16 tmp[128];
	unsigned int n, i;

	if (!ring)
		return -EINVAL;
	frame = (pos / 4) % st->dma_ring_frames;

	while (frames) {
		n = min(frames, ARRAY_SIZE(tmp) / 2);
		if (copy_from_iter(tmp, n * 4, iter) != n * 4)
			return -EFAULT;
		for (i = 0; i < n; i++) {
			u32 l = (u16)tmp[i * 2], r = (u16)tmp[i * 2 + 1];
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
	dmaengine_synchronize(st->dma);
	s5l8947x_engine_stop(st);
	st->substream = NULL;
	if (st->out == AUDIO_HDMI && st->aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(st->rate, false);
	s5l8947x_dma_free(st);
	return 0;
}

static int s5l8947x_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	st->rate = runtime->rate;
	s5l8947x_dma_halt(st);
	dmaengine_synchronize(st->dma);

	ret = s5l8947x_engine_config(st);
	if (ret < 0)
		return ret;
	ret = s5l8947x_dma_setup(st, runtime);
	if (ret < 0)
		return ret;

	/* Prime the whole ring before the engine fetches it. */
	runtime->start_threshold = runtime->buffer_size;

	if (st->out == AUDIO_HDMI && st->aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(st->rate, true);
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

	dmaengine_synchronize(st->dma);
	return 0;
}

static snd_pcm_uframes_t
s5l8947x_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct s5l8947x_stream *st = s5l8947x_substream_stream(substream);
	struct dma_tx_state state;
	size_t played;

	if (!st->dma_started || !st->dma_ring_bytes)
		return 0;
	dmaengine_tx_status(st->dma, st->cookie, &state);
	if (state.residue > st->dma_ring_bytes)
		return 0;
	played = st->dma_ring_bytes - state.residue;
	return (played / AUDIO_FRAME_BYTES) % substream->runtime->buffer_size;
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
		s5l8947x_hdmi_audio_configure(st->rate, en);
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

static int s5l8947x_audio_probe(struct platform_device *pdev)
{
	static const char * const dma_names[AUDIO_NUM_OUTPUTS] = {
		[AUDIO_HDMI]	= "hdmi",
		[AUDIO_OPTICAL]	= "optical",
	};
	static const char * const pcm_names[AUDIO_NUM_OUTPUTS] = {
		[AUDIO_HDMI]	= "S5L8947X HDMI (I2S)",
		[AUDIO_OPTICAL]	= "S5L8947X Optical (S/PDIF)",
	};
	phys_addr_t phys[AUDIO_NUM_OUTPUTS];
	struct s5l8947x_audio *aud;
	struct snd_card *card;
	struct clk_bulk_data *clks;
	struct snd_pcm *pcm;
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
	platform_set_drvdata(pdev, aud);

	aud->i2s = s5l8947x_map(pdev, "i2s0", &phys[AUDIO_HDMI]);
	aud->i2s_master = s5l8947x_map(pdev, "i2s-master", NULL);
	aud->spdif = s5l8947x_map(pdev, "spdif", &phys[AUDIO_OPTICAL]);
	if (IS_ERR(aud->i2s) || IS_ERR(aud->i2s_master) || IS_ERR(aud->spdif)) {
		ret = -ENODEV;
		goto err_free_card;
	}

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_free_card;

	ret = devm_clk_bulk_get_all_enable(&pdev->dev, &clks);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to enable clocks\n");
		goto err_free_card;
	}

	for (i = 0; i < AUDIO_NUM_OUTPUTS; i++) {
		struct s5l8947x_stream *st = &aud->streams[i];

		st->aud = aud;
		st->out = i;
		st->rate = AUDIO_RATE_HIGH;
		st->fifo_phys = phys[i] + (i == AUDIO_HDMI ? I2S_TXDATA : SPDIF_DATA);
		st->dma = dma_request_chan(&pdev->dev, dma_names[i]);
		if (IS_ERR(st->dma)) {
			ret = dev_err_probe(&pdev->dev, PTR_ERR(st->dma),
					    "failed to get the %s DMA channel\n",
					    dma_names[i]);
			st->dma = NULL;
			goto err_release_dma;
		}
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
		snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
					       32 * 1024, 64 * 1024);
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
		 "registered '%s': pcm0 I2S -> HDMI (%u/%u Hz, DMA %s), pcm1 S/PDIF -> optical (%u Hz, DMA %s)\n",
		 card->shortname, AUDIO_RATE_HIGH, AUDIO_RATE_LOW,
		 dma_chan_name(aud->streams[AUDIO_HDMI].dma), AUDIO_RATE_HIGH,
		 dma_chan_name(aud->streams[AUDIO_OPTICAL].dma));
	return 0;

err_release_dma:
	for (i = 0; i < AUDIO_NUM_OUTPUTS; i++)
		if (aud->streams[i].dma)
			dma_release_channel(aud->streams[i].dma);
err_free_card:
	snd_card_free(card);
	return ret;
}

static void s5l8947x_audio_remove(struct platform_device *pdev)
{
	struct s5l8947x_audio *aud = platform_get_drvdata(pdev);
	struct dma_chan *dma[AUDIO_NUM_OUTPUTS];
	unsigned int i;

	for (i = 0; i < AUDIO_NUM_OUTPUTS; i++)
		dma[i] = aud->streams[i].dma;
	/* Closing the PCMs stops the transfers and frees the rings */
	snd_card_free(aud->card);
	for (i = 0; i < AUDIO_NUM_OUTPUTS; i++)
		dma_release_channel(dma[i]);
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
