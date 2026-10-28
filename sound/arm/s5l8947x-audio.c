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
 * Playback is fed by PIO from an hrtimer. The timer period is derived from the
 * FIFO depth and the frame rate (32 us for the 16-entry S/PDIF FIFO, 256 us for
 * the 64-entry I2S FIFO), and the hardware pointer is computed from the FIFO
 * occupancy: consumed = written - level. That pointer is the real one, so an
 * underrun is visible to ALSA instead of being hidden by the byte count.
 *
 * DMA (central DMA at 0x37000000) is the next step; until then PIO is the only
 * transport. The 85 us S/PDIF FIFO is the reason the tick is this short.
 */

#define pr_fmt(fmt) "s5l8947x-audio: " fmt

#include <linux/clk.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
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

static enum hrtimer_restart s5l8947x_timer_cb(struct hrtimer *timer)
{
	struct s5l8947x_audio *aud =
		container_of(timer, struct s5l8947x_audio, timer);
	struct snd_pcm_substream *substream;
	struct snd_pcm_runtime *runtime;
	unsigned long flags;
	unsigned int level, avail;
	bool elapsed = false, stalled = false;
	u64 frames;

	spin_lock_irqsave(&aud->lock, flags);
	substream = aud->substream;
	if (!aud->running || !substream || !substream->runtime) {
		spin_unlock_irqrestore(&aud->lock, flags);
		return HRTIMER_NORESTART;
	}
	runtime = substream->runtime;

	if (!aud->armed) {
		/* Arm the ACR on the first tick: the audio clock is running now */
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
		aud->last_progress = ktime_get();
	} else if (level || avail) {
		/* Data is queued (or stuck in the FIFO) and nothing moves */
		if (ktime_after(ktime_get(),
				ktime_add_ns(aud->last_progress, 200 * NSEC_PER_MSEC)))
			stalled = true;
	}
	if (frames - aud->period_base >= runtime->period_size) {
		aud->period_base += runtime->period_size;
		elapsed = true;
	}
	if (!level && avail)
		aud->gaps++;
	if (stalled)
		aud->running = false;
	spin_unlock_irqrestore(&aud->lock, flags);

	if (stalled) {
		aud->xruns++;
		dev_err_ratelimited(aud->dev,
			"audio FIFO stopped draining (level=%u, rate=%u); frame clock lost\n",
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

	ret = snd_pcm_hw_constraint_list(runtime, 0, SNDRV_PCM_HW_PARAM_RATE,
					 &s5l8947x_rate_constraint);
	if (ret < 0)
		return ret;

	aud->substream = substream;
	return 0;
}

static int s5l8947x_pcm_close(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);

	hrtimer_cancel(&aud->timer);
	aud->running = false;
	s5l8947x_engine_stop(aud);
	aud->substream = NULL;
	if (aud->hdmi_enable)
		s5l8947x_hdmi_audio_configure(aud->rate, false,
					       aud->out.kind == OUT_SPDIF);
	return 0;
}

static int s5l8947x_pcm_prepare(struct snd_pcm_substream *substream)
{
	struct s5l8947x_audio *aud = snd_pcm_substream_chip(substream);
	unsigned long flags;
	int ret;

	aud->rate = substream->runtime->rate;
	aud->out.wpf = aud->out.kind == OUT_SPDIF ? 4 : 2;

	ret = s5l8947x_engine_config(aud);
	if (ret < 0)
		return ret;

	spin_lock_irqsave(&aud->lock, flags);
	aud->frames_written = 0;
	aud->pos = 0;
	aud->armed = false;
	aud->period_base = 0;
	aud->last_frames = 0;
	aud->last_progress = ktime_get();
	spin_unlock_irqrestore(&aud->lock, flags);

	aud->tick = ns_to_ktime(s5l8947x_tick_ns(aud));

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
		spin_lock_irqsave(&aud->lock, flags);
		aud->last_progress = ktime_get();
		aud->last_frames = 0;
		/*
		 * S/PDIF starts draining as soon as the block is powered, so
		 * power it first; I2S accepts FIFO writes while paused, so fill
		 * before releasing the pause bit.
		 */
		if (aud->out.kind == OUT_SPDIF) {
			s5l8947x_engine_start(aud);
			s5l8947x_fill(aud, substream->runtime);
		} else {
			s5l8947x_fill(aud, substream->runtime);
			s5l8947x_engine_start(aud);
		}
		/* The clock is running now, so the ACR will latch */
		if (aud->hdmi_enable)
			s5l8947x_hdmi_audio_arm(aud->rate);
		aud->running = true;
		spin_unlock_irqrestore(&aud->lock, flags);
		hrtimer_start(&aud->timer, aud->tick, HRTIMER_MODE_REL);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		spin_lock_irqsave(&aud->lock, flags);
		aud->running = false;
		aud->armed = false;
		s5l8947x_engine_stop(aud);
		spin_unlock_irqrestore(&aud->lock, flags);
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
	unsigned long flags;
	u64 frames;
	u32 buf = substream->runtime->buffer_size;

	if (!buf)
		return 0;
	/* frames_written is 64-bit and updated from the timer */
	spin_lock_irqsave(&aud->lock, flags);
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
	frames = s5l8947x_frames_done(aud);
	spin_unlock_irqrestore(&aud->lock, flags);

	return sysfs_emit(buf,
			  "output=%s rate=%u level=%u depth=%u frames=%llu gaps=%lu xruns=%lu tick_us=%llu\n",
			  aud->out.kind == OUT_SPDIF ? "spdif" : "i2s",
			  aud->rate, level, aud->out.depth,
			  frames, aud->gaps, aud->xruns,
			  div_u64((u64)ktime_to_ns(aud->tick), 1000));
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

	dev_info(&pdev->dev,
		 "registered '%s': %s, %u Hz, %u words/frame, %u-entry FIFO, tick %llu us\n",
		 card->shortname,
		 aud->out.kind == OUT_SPDIF ? "S/PDIF -> HDMI + optical" : "I2S0",
		 aud->rate, aud->out.wpf, aud->out.depth,
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
