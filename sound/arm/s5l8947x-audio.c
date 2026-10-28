// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Audio Engine (i2s0 / spdif + HDMI Audio) ALSA PCM Driver
 *
 * Drives the S5L8947X digital audio complex:
 *   - i2s0  @ 0x34190000: I2S controller feeding the HDMI transmitter
 *   - spdif @ 0x34194000: S/PDIF Optical TOSLINK digital transmitter
 *
 * There is no DMA engine driver yet, so the ring buffer is fed into the
 * I2S0 TX FIFO by PIO from a high-resolution timer. The FIFO full flag
 * paces the feeder, so the reported hardware pointer follows the real
 * I2S frame clock; a FIFO that never fills or never drains surfaces as an
 * ALSA error instead of silent progress.
 */

#define pr_fmt(fmt) "s5l8947x-audio: " fmt

#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <linux/spinlock.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

/* I2S0 controller registers (0x34190000) */
#define I2S_CON			0x00
#define I2S_MOD			0x04
#define I2S_FIC			0x08
#define I2S_PSR			0x0c
#define I2S_TXD			0x10

#define I2S_CON_RSTCLR		BIT(31)
#define I2S_CON_TXFIFO_FULL	BIT(8)
#define I2S_CON_TXDMA_ACTIVE	BIT(2)
#define I2S_CON_ACTIVE		BIT(0)
#define I2S_FIC_TXFLUSH		BIT(15)
#define I2S_FIC_TXCOUNT(v)	(((v) >> 8) & 0x7fU)

/* S/PDIF TOSLINK transmitter registers (0x34194000) */
#define SPDIF_CLKCON		0x00
#define SPDIF_CON		0x04
#define SPDIF_CSTAS		0x0c
#define SPDIF_DAT		0x10

#define SPDIF_CLKCON_ON		0x01U
#define SPDIF_CON_FIFO_LEVEL(v)	(((v) >> 22) & 0x1fU)
#define SPDIF_CON_SW_RESET	BIT(5)
#define SPDIF_CON_PCM_16BIT	0x00381001U
#define SPDIF_CSTAS_48KHZ	0x02000104U
/* The S/PDIF FIFO takes one 16-bit sample per write, so two per frame */
#define SPDIF_FIFO_DEPTH	16U

#define AUDIO_RATE		48000U
#define AUDIO_TICK_NS		(250 * NSEC_PER_USEC)
/* Frames pushed per tick at most; 48 kHz drains only 12 frames per tick */
#define AUDIO_TX_BURST		64U
/* Consecutive full bursts without the FIFO filling before giving up */
#define AUDIO_STALL_TICKS	8U

/*
 * I2S0 clock configuration. The operating clock source and prescaler that
 * yield an exact 48 kHz frame clock are still being characterised; use
 * "audiotest rate" to measure candidates and set these at runtime through
 * /sys/module/s5l8947x_audio/parameters/ before opening the PCM.
 */
static uint i2s_mod = 0xc0000000U;	/* PCLK, TX only, 16-bit, 256fs, 32fs */
module_param(i2s_mod, uint, 0644);
MODULE_PARM_DESC(i2s_mod, "I2S0 MOD register value used for playback");

static uint i2s_psr = 0x00008100U;	/* prescaler enabled, divide by 2 */
module_param(i2s_psr, uint, 0644);
MODULE_PARM_DESC(i2s_psr, "I2S0 PSR register value used for playback");

struct s5l8947x_audio {
	struct device *dev;
	struct snd_card *card;
	struct snd_pcm_substream *substream;

	void __iomem *i2s0;
	void __iomem *spdif;

	spinlock_t lock;
	struct hrtimer timer;
	size_t hw_ptr;
	size_t period_pos;
	unsigned int stall_ticks;
	bool running;
	bool hdmi_enable;
	bool spdif_enable;

	u64 frames_pushed;
	unsigned long empty_ticks;
	unsigned long stalls;
	unsigned long spdif_drops;
};

static const struct snd_pcm_hardware s5l8947x_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_48000,
	.rate_min		= AUDIO_RATE,
	.rate_max		= AUDIO_RATE,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= 64 * 1024,
	.period_bytes_min	= 1024,
	.period_bytes_max	= 16 * 1024,
	.periods_min		= 2,
	.periods_max		= 32,
};

static void s5l8947x_audio_hw_prepare(struct s5l8947x_audio *aud)
{
	writel_relaxed(I2S_CON_RSTCLR, aud->i2s0 + I2S_CON);
	writel_relaxed(I2S_FIC_TXFLUSH, aud->i2s0 + I2S_FIC);
	writel_relaxed(0, aud->i2s0 + I2S_FIC);
	writel_relaxed(READ_ONCE(i2s_mod), aud->i2s0 + I2S_MOD);
	writel_relaxed(READ_ONCE(i2s_psr), aud->i2s0 + I2S_PSR);

	if (aud->spdif && aud->spdif_enable) {
		writel_relaxed(SPDIF_CON_SW_RESET, aud->spdif + SPDIF_CON);
		writel_relaxed(SPDIF_CON_PCM_16BIT, aud->spdif + SPDIF_CON);
		writel_relaxed(SPDIF_CSTAS_48KHZ, aud->spdif + SPDIF_CSTAS);
		writel_relaxed(SPDIF_CLKCON_ON, aud->spdif + SPDIF_CLKCON);
	}
}

/*
 * I2S0 paces the feeder; S/PDIF runs from its own clock, so a frame is
 * dropped rather than overrunning its FIFO.
 */
static void s5l8947x_audio_spdif_put(struct s5l8947x_audio *aud, u32 frame)
{
	u32 con = readl_relaxed(aud->spdif + SPDIF_CON);

	if (SPDIF_CON_FIFO_LEVEL(con) > SPDIF_FIFO_DEPTH - 2) {
		aud->spdif_drops++;
		return;
	}
	writel_relaxed(frame & 0xffffU, aud->spdif + SPDIF_DAT);
	writel_relaxed(frame >> 16, aud->spdif + SPDIF_DAT);
}

/* Push frames until the TX FIFO is full. Called with aud->lock held. */
static bool s5l8947x_audio_fill(struct s5l8947x_audio *aud,
				struct snd_pcm_runtime *runtime)
{
	size_t period_bytes = frames_to_bytes(runtime, runtime->period_size);
	size_t buf_bytes = frames_to_bytes(runtime, runtime->buffer_size);
	bool elapsed = false;
	unsigned int n;

	if (!I2S_FIC_TXCOUNT(readl_relaxed(aud->i2s0 + I2S_FIC)))
		aud->empty_ticks++;

	for (n = 0; n < AUDIO_TX_BURST; n++) {
		u32 frame;

		if (readl_relaxed(aud->i2s0 + I2S_CON) & I2S_CON_TXFIFO_FULL)
			break;

		frame = *(const u32 *)(runtime->dma_area + aud->hw_ptr);
		writel_relaxed(frame, aud->i2s0 + I2S_TXD);
		if (aud->spdif && aud->spdif_enable)
			s5l8947x_audio_spdif_put(aud, frame);

		aud->hw_ptr += 4;
		if (aud->hw_ptr >= buf_bytes)
			aud->hw_ptr = 0;
		aud->period_pos += 4;
		if (aud->period_pos >= period_bytes) {
			aud->period_pos -= period_bytes;
			elapsed = true;
		}
	}

	aud->frames_pushed += n;
	if (n == AUDIO_TX_BURST)
		aud->stall_ticks++;
	else
		aud->stall_ticks = 0;
	return elapsed;
}

static enum hrtimer_restart s5l8947x_audio_timer_cb(struct hrtimer *timer)
{
	struct s5l8947x_audio *aud =
		container_of(timer, struct s5l8947x_audio, timer);
	struct snd_pcm_substream *substream;
	bool elapsed, stalled;

	spin_lock(&aud->lock);
	substream = aud->substream;
	if (!aud->running || !substream) {
		spin_unlock(&aud->lock);
		return HRTIMER_NORESTART;
	}
	elapsed = s5l8947x_audio_fill(aud, substream->runtime);
	stalled = aud->stall_ticks >= AUDIO_STALL_TICKS;
	if (stalled) {
		aud->running = false;
		aud->stalls++;
	}
	spin_unlock(&aud->lock);

	if (stalled) {
		dev_err_ratelimited(aud->dev,
			"I2S0 TX FIFO never reports full (CON=0x%08x FIC=0x%08x); frame clock not running\n",
			readl_relaxed(aud->i2s0 + I2S_CON),
			readl_relaxed(aud->i2s0 + I2S_FIC));
		snd_pcm_stop_xrun(substream);
		return HRTIMER_NORESTART;
	}
	if (elapsed)
		snd_pcm_period_elapsed(substream);

	hrtimer_forward_now(timer, ns_to_ktime(AUDIO_TICK_NS));
	return HRTIMER_RESTART;
}

static int s5l8947x_pcm_open(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	substream->runtime->hw = s5l8947x_pcm_hw;
	aud->substream = substream;
	return 0;
}

static int s5l8947x_pcm_close(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	hrtimer_cancel(&aud->timer);
	aud->substream = NULL;
	if (aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(AUDIO_RATE, false);
	return 0;
}

static int s5l8947x_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	aud->hw_ptr = 0;
	aud->period_pos = 0;
	aud->stall_ticks = 0;
	s5l8947x_audio_hw_prepare(aud);
	if (aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(AUDIO_RATE, true);
	return 0;
}

static int s5l8947x_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock(&aud->lock);
		aud->running = true;
		s5l8947x_audio_fill(aud, substream->runtime);
		writel(I2S_CON_RSTCLR | I2S_CON_TXDMA_ACTIVE | I2S_CON_ACTIVE,
		       aud->i2s0 + I2S_CON);
		spin_unlock(&aud->lock);
		hrtimer_start(&aud->timer, ns_to_ktime(AUDIO_TICK_NS),
			      HRTIMER_MODE_REL);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock(&aud->lock);
		aud->running = false;
		writel(I2S_CON_RSTCLR, aud->i2s0 + I2S_CON);
		spin_unlock(&aud->lock);
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
	return 0;
}

static snd_pcm_uframes_t
s5l8947x_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	return bytes_to_frames(substream->runtime, READ_ONCE(aud->hw_ptr));
}

static const struct snd_pcm_ops s5l8947x_pcm_ops = {
	.open		= s5l8947x_pcm_open,
	.close		= s5l8947x_pcm_close,
	.prepare	= s5l8947x_pcm_prepare,
	.trigger	= s5l8947x_pcm_trigger,
	.sync_stop	= s5l8947x_pcm_sync_stop,
	.pointer	= s5l8947x_pcm_pointer,
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
	bool val = kcontrol->private_value ? aud->spdif_enable : aud->hdmi_enable;

	ucontrol->value.integer.value[0] = val ? 1 : 0;
	return 0;
}

static int s5l8947x_ctl_switch_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct s5l8947x_audio *aud = snd_kcontrol_chip(kcontrol);
	bool en = !!ucontrol->value.integer.value[0];

	if (kcontrol->private_value) {
		if (aud->spdif_enable == en)
			return 0;
		aud->spdif_enable = en;
		return 1;
	}

	if (aud->hdmi_enable == en)
		return 0;
	aud->hdmi_enable = en;
	if (aud->substream)
		s5l8947x_hdmi_audio_configure(AUDIO_RATE, en);
	return 1;
}

static const struct snd_kcontrol_new s5l8947x_audio_controls[] = {
	{
		.iface		= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name		= "HDMI Audio Playback Switch",
		.info		= s5l8947x_ctl_switch_info,
		.get		= s5l8947x_ctl_switch_get,
		.put		= s5l8947x_ctl_switch_put,
		.private_value	= 0,
	},
	{
		.iface		= SNDRV_CTL_ELEM_IFACE_MIXER,
		.name		= "IEC958 Playback Switch",
		.info		= s5l8947x_ctl_switch_info,
		.get		= s5l8947x_ctl_switch_get,
		.put		= s5l8947x_ctl_switch_put,
		.private_value	= 1,
	},
};

static ssize_t fifo_stats_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_audio *aud = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "frames=%llu empty_ticks=%lu stalls=%lu con=0x%08x mod=0x%08x fic=0x%08x psr=0x%08x spdif_con=0x%08x spdif_drops=%lu\n",
			  aud->frames_pushed, aud->empty_ticks, aud->stalls,
			  readl(aud->i2s0 + I2S_CON), readl(aud->i2s0 + I2S_MOD),
			  readl(aud->i2s0 + I2S_FIC), readl(aud->i2s0 + I2S_PSR),
			  aud->spdif ? readl(aud->spdif + SPDIF_CON) : 0,
			  aud->spdif_drops);
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
	aud->spdif_enable = true;
	spin_lock_init(&aud->lock);
	hrtimer_init(&aud->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	aud->timer.function = s5l8947x_audio_timer_cb;
	platform_set_drvdata(pdev, aud);

	aud->i2s0 = devm_platform_ioremap_resource_byname(pdev, "i2s0");
	if (IS_ERR(aud->i2s0)) {
		ret = PTR_ERR(aud->i2s0);
		goto err_free_card;
	}

	aud->spdif = devm_platform_ioremap_resource_byname(pdev, "spdif");
	if (IS_ERR(aud->spdif))
		aud->spdif = NULL;

	ret = devm_clk_bulk_get_all_enable(&pdev->dev, &clks);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "failed to enable clocks\n");
		goto err_free_card;
	}

	strscpy(card->driver, "s5l8947x-audio", sizeof(card->driver));
	strscpy(card->shortname, "S5L8947X HDMI/SPDIF", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "S5L8947X Audio Engine (I2S0/SPDIF) at 0x34190000");

	ret = snd_pcm_new(card, "S5L8947X Digital PCM", 0, 1, 0, &pcm);
	if (ret < 0)
		goto err_free_card;

	pcm->private_data = aud;
	strscpy(pcm->name, "S5L8947X HDMI + Optical PCM", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &s5l8947x_pcm_ops);
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

	dev_info(&pdev->dev, "registered ALSA card '%s' (I2S0 PIO, SPDIF=%s)\n",
		 card->shortname, aud->spdif ? "ok" : "none");
	return 0;

err_free_card:
	snd_card_free(card);
	return ret;
}

static void s5l8947x_audio_remove(struct platform_device *pdev)
{
	struct s5l8947x_audio *aud = platform_get_drvdata(pdev);

	hrtimer_cancel(&aud->timer);
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

MODULE_DESCRIPTION("S5L8947X Audio Engine (I2S0 / SPDIF + HDMI) ALSA Driver");
MODULE_LICENSE("GPL");
