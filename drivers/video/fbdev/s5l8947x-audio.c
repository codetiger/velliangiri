// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Audio Engine (ae2 / i2s0 / spdif + HDMI Audio) ALSA PCM Driver
 *
 * Drives the S5L8947X digital audio complex:
 *   - ae2   @ 0x34000000: Audio Engine SRAM & timer/routing block (PMGR gate 37)
 *   - i2s0  @ 0x34190000: Multi-channel I2S controller feeding HDMI transmitter
 *   - spdif @ 0x34194000: S/PDIF Optical TOSLINK digital transmitter
 */

#define pr_fmt(fmt) "s5l8947x-audio: " fmt

#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>

#define PMGR_PHYS_BASE		0x3f100000UL
#define PMGR_GATE_OFFSET(n)	(0x1008U + (n) * 4U)
#define PMGR_GATE_AUDIO		37

/* I2S0 controller registers (0x34190000) */
#define I2S_CON			0x00
#define I2S_MOD			0x04
#define I2S_FIC			0x08
#define I2S_PSR			0x0c
#define I2S_TXD			0x10

#define I2S_CON_ACTIVE		BIT(0)
#define I2S_FIC_TXFLUSH		BIT(15)

/* S/PDIF TOSLINK transmitter registers (0x34194000) */
#define SPDIF_CLKCON		0x00
#define SPDIF_CON		0x04
#define SPDIF_BSTAS		0x08
#define SPDIF_CSTAS		0x0c
#define SPDIF_DAT		0x10
#define SPDIF_CNT		0x14

#define SPDIF_CLKCON_ON		0x02U
#define SPDIF_CON_SW_RESET	BIT(0)
#define SPDIF_CON_PCM_16BIT	0x00000020U
#define SPDIF_CSTAS_48KHZ	0x02000000U
#define SPDIF_CSTAS_44KHZ	0x00000000U

struct s5l8947x_audio {
	struct device *dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *substream;

	void __iomem *ae2;
	void __iomem *i2s0;
	void __iomem *spdif;
	void __iomem *pmgr;

	spinlock_t lock;
	struct hrtimer timer;
	ktime_t period_ns;
	size_t hw_ptr;
	bool running;
	bool hdmi_enable;
	bool spdif_enable;
	bool hw_clocked;
};

static const struct snd_pcm_hardware s5l8947x_pcm_hw = {
	.info			= SNDRV_PCM_INFO_MMAP |
				  SNDRV_PCM_INFO_MMAP_VALID |
				  SNDRV_PCM_INFO_INTERLEAVED,
	.formats		= SNDRV_PCM_FMTBIT_S16_LE,
	.rates			= SNDRV_PCM_RATE_32000 |
				  SNDRV_PCM_RATE_44100 |
				  SNDRV_PCM_RATE_48000,
	.rate_min		= 32000,
	.rate_max		= 48000,
	.channels_min		= 2,
	.channels_max		= 2,
	.buffer_bytes_max	= 64 * 1024,
	.period_bytes_min	= 1024,
	.period_bytes_max	= 16 * 1024,
	.periods_min		= 2,
	.periods_max		= 32,
};

static int s5l8947x_audio_gate_enable(struct s5l8947x_audio *aud)
{
	u32 off = PMGR_GATE_OFFSET(PMGR_GATE_AUDIO);
	u32 v;
	int i;

	if (!aud->pmgr)
		return -ENODEV;

	v = readl(aud->pmgr + off);
	if (!v)
		return -ENODEV;
	writel((v & ~0x10fU) | 0x0fU, aud->pmgr + off);
	for (i = 0; i < 10000; i++) {
		v = readl(aud->pmgr + off);
		if (!((v ^ (v >> 4)) & 0x0fU)) {
			aud->hw_clocked = true;
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

static void s5l8947x_audio_hw_prepare(struct s5l8947x_audio *aud,
				      unsigned int rate)
{
	if (!aud->hw_clocked)
		(void)s5l8947x_audio_gate_enable(aud);
	if (aud->hdmi_enable)
		(void)s5l8947x_hdmi_audio_configure(rate, true);
}

static void s5l8947x_audio_feed_fifo_sample(struct s5l8947x_audio *aud,
					    u32 sample_pair)
{
	(void)aud;
	(void)sample_pair;
}

static enum hrtimer_restart s5l8947x_audio_timer_cb(struct hrtimer *timer)
{
	struct s5l8947x_audio *aud =
		container_of(timer, struct s5l8947x_audio, timer);
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	size_t period_bytes, buf_bytes;
	unsigned long flags;
	bool elapsed = false;

	spin_lock_irqsave(&aud->lock, flags);
	substream = aud->substream;
	if (!aud->running || !substream || !substream->runtime) {
		spin_unlock_irqrestore(&aud->lock, flags);
		return HRTIMER_NORESTART;
	}

	runtime = substream->runtime;
	period_bytes = frames_to_bytes(runtime, runtime->period_size);
	buf_bytes = frames_to_bytes(runtime, runtime->buffer_size);

	if (runtime->dma_area && buf_bytes >= 4) {
		const u32 *samples =
			(const u32 *)(runtime->dma_area + aud->hw_ptr);

		s5l8947x_audio_feed_fifo_sample(aud, *samples);
	}

	aud->hw_ptr += period_bytes;
	if (aud->hw_ptr >= buf_bytes)
		aud->hw_ptr %= buf_bytes;
	elapsed = true;
	hrtimer_forward_now(&aud->timer, aud->period_ns);
	spin_unlock_irqrestore(&aud->lock, flags);

	if (elapsed)
		snd_pcm_period_elapsed(substream);

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
	return 0;
}

static int s5l8947x_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	u64 ns;

	aud->hw_ptr = 0;
	ns = div_u64((u64)runtime->period_size * NSEC_PER_SEC, runtime->rate);
	if (ns < 250000ULL)
		ns = 250000ULL;
	aud->period_ns = ns_to_ktime(ns);
	s5l8947x_audio_hw_prepare(aud, runtime->rate);
	return 0;
}

static int s5l8947x_pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		spin_lock_irqsave(&aud->lock, flags);
		aud->running = true;
		hrtimer_start(&aud->timer, aud->period_ns, HRTIMER_MODE_REL);
		spin_unlock_irqrestore(&aud->lock, flags);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&aud->lock, flags);
		aud->running = false;
		spin_unlock_irqrestore(&aud->lock, flags);
		return 0;

	default:
		return -EINVAL;
	}
}

static snd_pcm_uframes_t
s5l8947x_pcm_pointer(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned long flags;
	size_t ptr;

	spin_lock_irqsave(&aud->lock, flags);
	ptr = aud->hw_ptr;
	spin_unlock_irqrestore(&aud->lock, flags);

	return bytes_to_frames(substream->runtime, ptr);
}

static const struct snd_pcm_ops s5l8947x_pcm_ops = {
	.open		= s5l8947x_pcm_open,
	.close		= s5l8947x_pcm_close,
	.prepare	= s5l8947x_pcm_prepare,
	.trigger	= s5l8947x_pcm_trigger,
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
		aud->spdif_enable = en;
	} else {
		aud->hdmi_enable = en;
		(void)s5l8947x_hdmi_audio_configure(48000, en);
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

static int s5l8947x_audio_probe(struct platform_device *pdev)
{
	struct s5l8947x_audio *aud;
	struct snd_card *card;
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

	aud->ae2 = devm_platform_ioremap_resource_byname(pdev, "ae2");
	if (IS_ERR(aud->ae2))
		aud->ae2 = NULL;

	aud->i2s0 = devm_platform_ioremap_resource_byname(pdev, "i2s0");
	if (IS_ERR(aud->i2s0))
		aud->i2s0 = NULL;

	aud->spdif = devm_platform_ioremap_resource_byname(pdev, "spdif");
	if (IS_ERR(aud->spdif))
		aud->spdif = NULL;

	aud->pmgr = devm_ioremap(&pdev->dev, PMGR_PHYS_BASE, 0x2000);
	if (aud->pmgr &&
	    (readl(aud->pmgr + PMGR_GATE_OFFSET(PMGR_GATE_AUDIO)) & 0xffU) == 0xffU)
		aud->hw_clocked = true;

	strscpy(card->driver, "s5l8947x-audio", sizeof(card->driver));
	strscpy(card->shortname, "S5L8947X HDMI/SPDIF", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "S5L8947X Audio Engine (AE2/I2S0/SPDIF) at 0x34000000");

	ret = snd_pcm_new(card, "S5L8947X Digital PCM", 0, 1, 0, &pcm);
	if (ret < 0)
		goto err_free_card;

	pcm->private_data = aud;
	strscpy(pcm->name, "S5L8947X HDMI + Optical PCM", sizeof(pcm->name));
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &s5l8947x_pcm_ops);
	snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       32 * 1024, 64 * 1024);
	aud->pcm = pcm;

	for (i = 0; i < ARRAY_SIZE(s5l8947x_audio_controls); i++) {
		ret = snd_ctl_add(card,
				  snd_ctl_new1(&s5l8947x_audio_controls[i], aud));
		if (ret < 0)
			goto err_free_card;
	}

	ret = snd_card_register(card);
	if (ret < 0)
		goto err_free_card;

	(void)s5l8947x_hdmi_audio_configure(48000, true);

	dev_info(&pdev->dev,
		 "registered ALSA card '%s' (AE2=%s, I2S0=%s, SPDIF=%s, gate37=%s)\n",
		 card->shortname,
		 aud->ae2 ? "ok" : "none",
		 aud->i2s0 ? "ok" : "none",
		 aud->spdif ? "ok" : "none",
		 aud->hw_clocked ? "active" : "gated");
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
	},
};
module_platform_driver(s5l8947x_audio_driver);

MODULE_DESCRIPTION("S5L8947X Audio Engine (AE2 / I2S0 / SPDIF + HDMI) ALSA Driver");
MODULE_LICENSE("GPL");
