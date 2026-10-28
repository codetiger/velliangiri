// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X audio path diagnostic utility (/bin/audiotest)
 *
 * Usage:
 *   audiotest rate  [-c txcon]                 measure the I2S0 frame rate
 *   audiotest pio   [-t sec] [-f hz] [-c txcon] [-C cts]
 *                                              FIFO-paced tone straight to I2S0
 *                                              with HDMI audio enabled
 *   audiotest alsa  [-t sec] [-f hz]           tone through the ALSA PCM
 *
 * rate and pio program I2S0 directly through /dev/mem and refuse to run while
 * the ALSA PCM is open. Exit status: 0 pass, 1 fail, 2 error.
 *
 * I2S register model, measured on the hardware:
 *   +0x00 CTL     bit0 enable, bit6 soft reset (self-clearing)
 *   +0x04 TXCON   BCLK = 24 MHz / (2 * (DIV + 1)), DIV = [10:8]; bit7 BCLK =
 *                 24 MHz; [4:3] frame = 32/48/64 BCLK (16/24/32-bit slots)
 *   +0x08 TXCOM   bit2 TX enable, bit0 TX pause; keep the reset bits [24:19]
 *   +0x10 TXDATA  one FIFO entry per channel sample
 *   +0x3c STATUS  bit0 LRCK, [12:7] TX FIFO level (64 entries; writes to a
 *                 full FIFO are not refused, so the level must be watched)
 */

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
#include <sound/asound.h>

#define I2S0_PHYS		0x34190000UL
#define HDMI_ACR_PHYS		0x39710000UL	/* HDMI + 0x10000 */

#define I2S_CTL			(0x00 / 4)
#define I2S_TXCON		(0x04 / 4)
#define I2S_TXCOM		(0x08 / 4)
#define I2S_TXDATA		(0x10 / 4)
#define I2S_STATUS		(0x3c / 4)

#define I2S_CTL_ENABLE		0x00000021U
#define I2S_CTL_RESET		0x00000041U
#define I2S_TXCON_BYPASS	0x00000080U
#define I2S_TXCON_DIV(v)	(((v) >> 8) & 0x7U)
#define I2S_TXCON_FRAME(v)	(((v) >> 3) & 0x3U)
#define I2S_TXCOM_DEFAULT	0x01f80000U
#define I2S_TXCOM_EN		0x00000004U
#define I2S_TXCOM_PAUSE		0x00000001U
#define I2S_LEVEL(st)		(((st) >> 7) & 0x3fU)
#define I2S_FIFO_HIGH		48U

/* HDMI Audio Clock Regeneration, one byte per 32-bit register */
#define ACR_CTS0		(0x420 / 4)
#define ACR_N0			(0x430 / 4)
#define TMDS_HZ			74250000.0

#define I2S_REF_HZ		24000000.0
#define DEF_TXCON		0x00000700U	/* BCLK 1.5 MHz, 32-bit frame: 46875 Hz */
#define RATE			48000U		/* rate the ALSA driver advertises */
#define RATE_TOLERANCE		0.005
#define STALL_NS		50000000LL

#define PCM_DEV			"/dev/snd/pcmC0D0p"
#define PCM_STATUS		"/proc/asound/card0/pcm0p/sub0/status"
#define HDMI_AUDIO_ATTR		"/sys/bus/platform/devices/3a200000.display-controller/hdmi_audio"
#define AUDIO_STATS_ATTR	"/sys/bus/platform/devices/34190000.audio-controller/fifo_stats"

struct audio_map {
	int fd;
	volatile uint32_t *i2s;
	volatile uint32_t *acr;
};

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void print_file(const char *label, const char *path)
{
	char buf[512];
	FILE *f = fopen(path, "r");

	if (!f)
		return;
	while (fgets(buf, sizeof(buf), f))
		printf("%s%s", label, buf);
	fclose(f);
}

static int write_file(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	int ret;

	if (!f)
		return -1;
	ret = fputs(val, f) < 0 ? -1 : 0;
	if (fclose(f))
		ret = -1;
	return ret;
}

static int pcm_is_idle(void)
{
	char buf[64] = "";
	FILE *f = fopen(PCM_STATUS, "r");

	if (!f)
		return 1;
	if (!fgets(buf, sizeof(buf), f))
		buf[0] = '\0';
	fclose(f);
	if (strncmp(buf, "closed", 6) == 0)
		return 1;
	fprintf(stderr, "audiotest: ALSA PCM is in use (%s); stop playback first\n",
		strtok(buf, "\n"));
	return 0;
}

/* Frame rate the TXCON value should produce, 0 for unsupported settings */
static double expected_rate(uint32_t txcon)
{
	static const unsigned int frame_bits[4] = { 32, 48, 64, 0 };
	unsigned int bclk_div, bits = frame_bits[I2S_TXCON_FRAME(txcon)];

	bclk_div = txcon & I2S_TXCON_BYPASS ? 1 : 2 * (I2S_TXCON_DIV(txcon) + 1);
	return bits ? I2S_REF_HZ / (bclk_div * bits) : 0;
}

static int map_audio(struct audio_map *m)
{
	void *i2s, *acr;

	m->fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (m->fd < 0) {
		perror("audiotest: open(/dev/mem)");
		return -1;
	}
	i2s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, I2S0_PHYS);
	acr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, HDMI_ACR_PHYS);
	if (i2s == MAP_FAILED || acr == MAP_FAILED) {
		perror("audiotest: mmap");
		close(m->fd);
		return -1;
	}
	m->i2s = i2s;
	m->acr = acr;
	return 0;
}

static void unmap_audio(struct audio_map *m)
{
	munmap((void *)m->i2s, 4096);
	munmap((void *)m->acr, 4096);
	close(m->fd);
}

static void i2s_stop(struct audio_map *m)
{
	m->i2s[I2S_CTL] = I2S_CTL_RESET;
}

/* Reset, configure and enable I2S0 transmit with the FIFO paused */
static void i2s_setup(struct audio_map *m, uint32_t txcon)
{
	i2s_stop(m);
	m->i2s[I2S_TXCOM] = I2S_TXCOM_DEFAULT | I2S_TXCOM_EN | I2S_TXCOM_PAUSE;
	m->i2s[I2S_TXCON] = txcon;
	m->i2s[I2S_CTL] = I2S_CTL_ENABLE;
}

static void i2s_start(struct audio_map *m)
{
	m->i2s[I2S_TXCOM] = I2S_TXCOM_DEFAULT | I2S_TXCOM_EN;
}

static unsigned int i2s_level(struct audio_map *m)
{
	return I2S_LEVEL(m->i2s[I2S_STATUS]);
}

/*
 * Keep the TX FIFO between empty and I2S_FIFO_HIGH entries for @secs, taking
 * samples from @next (or silence). Returns frames/s, or 0 if the FIFO stopped
 * draining. @underruns counts samples where the FIFO was found empty.
 */
static double i2s_stream(struct audio_map *m, unsigned int secs,
			 uint32_t (*next)(void *), void *ctx,
			 unsigned long *underruns)
{
	int64_t t0, t, last, end;
	uint64_t words = 0;
	unsigned int lvl;

	*underruns = 0;
	while (i2s_level(m) < I2S_FIFO_HIGH) {
		m->i2s[I2S_TXDATA] = next ? next(ctx) : 0;
		words++;
	}
	i2s_start(m);
	t0 = last = now_ns();
	end = t0 + (int64_t)secs * 1000000000LL;
	words = 0;
	while ((t = now_ns()) < end) {
		lvl = i2s_level(m);
		if (lvl >= I2S_FIFO_HIGH) {
			if (t - last >= STALL_NS)
				return 0;
			continue;
		}
		if (!lvl)
			(*underruns)++;
		m->i2s[I2S_TXDATA] = next ? next(ctx) : 0;
		words++;
		last = t;
	}
	return (double)words * 1e9 / 2.0 / (double)(t - t0);
}

static int report_rate(double rate, double expect, unsigned long underruns)
{
	double err;

	if (rate == 0) {
		printf("[rate] FAIL: TX FIFO stopped draining; no I2S0 frame clock\n");
		return 1;
	}
	err = (rate - expect) / expect;
	printf("[rate] measured %.1f Hz (expected %.1f Hz, error %+.3f%%), %lu FIFO-empty samples\n",
	       rate, expect, err * 100.0, underruns);
	if (fabs(err) > RATE_TOLERANCE) {
		printf("[rate] FAIL: frame clock outside +/-%.1f%%\n",
		       RATE_TOLERANCE * 100.0);
		return 1;
	}
	printf("[rate] PASS\n");
	return 0;
}

static int cmd_rate(uint32_t txcon)
{
	double expect = expected_rate(txcon);
	unsigned long underruns;
	struct audio_map m;
	double rate;

	if (!expect) {
		fprintf(stderr, "audiotest: unsupported txcon 0x%08x\n", txcon);
		return 2;
	}
	if (!pcm_is_idle() || map_audio(&m))
		return 2;
	i2s_setup(&m, txcon);
	rate = i2s_stream(&m, 1, NULL, NULL, &underruns);
	i2s_stop(&m);
	unmap_audio(&m);
	return report_rate(rate, expect, underruns);
}

struct tone {
	double phase, step;
	int right;
	uint32_t sample;
};

/* Next FIFO word: one channel sample, copied into both 16-bit halves */
static uint32_t tone_next(void *ctx)
{
	struct tone *tn = ctx;

	if (!tn->right) {
		uint16_t v = (uint16_t)(int16_t)(12000.0 * sin(tn->phase));

		tn->sample = (uint32_t)v << 16 | v;
		tn->phase += tn->step;
		if (tn->phase >= 2.0 * M_PI)
			tn->phase -= 2.0 * M_PI;
	}
	tn->right = !tn->right;
	return tn->sample;
}

static uint32_t acr_read(struct audio_map *m, unsigned int reg)
{
	return (m->acr[reg] & 0xffU) | (m->acr[reg + 1] & 0xffU) << 8 |
	       (m->acr[reg + 2] & 0xffU) << 16;
}

static void acr_write(struct audio_map *m, unsigned int reg, uint32_t val)
{
	m->acr[reg] = val & 0xffU;
	m->acr[reg + 1] = (val >> 8) & 0xffU;
	m->acr[reg + 2] = (val >> 16) & 0xffU;
}

static int cmd_pio(unsigned int secs, unsigned int freq, uint32_t txcon,
		   uint32_t cts)
{
	double expect = expected_rate(txcon), rate;
	struct tone tn = { 0 };
	unsigned long underruns;
	struct audio_map m;
	uint32_t n;
	int ret;

	if (!expect) {
		fprintf(stderr, "audiotest: unsupported txcon 0x%08x\n", txcon);
		return 2;
	}
	if (!pcm_is_idle() || map_audio(&m))
		return 2;

	if (write_file(HDMI_AUDIO_ATTR, "48000"))
		printf("[pio] WARN: could not enable HDMI audio via %s\n",
		       HDMI_AUDIO_ATTR);
	n = acr_read(&m, ACR_N0);
	if (cts)
		acr_write(&m, ACR_CTS0, cts);
	printf("[pio] I2S0 frame rate %.1f Hz; HDMI N=%u CTS=%u (CTS for this rate: %.1f)\n",
	       expect, n, acr_read(&m, ACR_CTS0), TMDS_HZ * n / (128.0 * expect));
	print_file("[pio] hdmi: ", HDMI_AUDIO_ATTR);

	tn.step = 2.0 * M_PI * freq / expect;
	i2s_setup(&m, txcon);
	printf("[pio] playing %u Hz for %u s on I2S0; listen on the TV\n", freq, secs);
	rate = i2s_stream(&m, secs, tone_next, &tn, &underruns);
	print_file("[pio] hdmi: ", HDMI_AUDIO_ATTR);
	ret = report_rate(rate, expect, underruns);

	i2s_stop(&m);
	write_file(HDMI_AUDIO_ATTR, "off");
	unmap_audio(&m);
	return ret;
}

static void fill_tone(int16_t *table, unsigned int len, unsigned int freq)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		table[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * freq * i / RATE));
}

static void hw_mask_set(struct snd_pcm_hw_params *p, int param, unsigned int bit)
{
	struct snd_mask *mask = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(mask, 0, sizeof(*mask));
	mask->bits[bit / 32] |= 1U << (bit % 32);
}

static void hw_interval_set(struct snd_pcm_hw_params *p, int param,
			    unsigned int val)
{
	struct snd_interval *iv =
		&p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	memset(iv, 0, sizeof(*iv));
	iv->min = val;
	iv->max = val;
	iv->integer = 1;
}

static int cmd_alsa(unsigned int secs, unsigned int freq)
{
	enum { PERIOD = 1024, BUFFER = 4096 };
	static int16_t tone[RATE];
	int16_t period[PERIOD * 2];
	struct snd_pcm_hw_params hw;
	struct snd_xferi xfer;
	unsigned int idx = 0, p, periods;
	int64_t t0, elapsed;
	double expect, err;
	int fd, i, ret = 1;

	fd = open(PCM_DEV, O_RDWR);
	if (fd < 0) {
		perror("audiotest: open(" PCM_DEV ")");
		return 2;
	}

	memset(&hw, 0, sizeof(hw));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&hw.masks[i], 0xff, sizeof(hw.masks[i]));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
		hw.intervals[i].max = ~0U;
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER);
	hw.rmask = ~0U;
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
		perror("audiotest: SNDRV_PCM_IOCTL_HW_PARAMS");
		goto out;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		perror("audiotest: SNDRV_PCM_IOCTL_PREPARE");
		goto out;
	}

	fill_tone(tone, RATE, freq);
	periods = secs * RATE / PERIOD;
	print_file("[alsa] before: ", AUDIO_STATS_ATTR);
	printf("[alsa] playing %u Hz for %u s via " PCM_DEV "; listen on the TV\n",
	       freq, secs);

	t0 = now_ns();
	for (p = 0; p < periods; p++) {
		for (i = 0; i < PERIOD; i++) {
			period[i * 2] = tone[idx];
			period[i * 2 + 1] = tone[idx];
			if (++idx == RATE)
				idx = 0;
		}
		memset(&xfer, 0, sizeof(xfer));
		xfer.buf = period;
		xfer.frames = PERIOD;
		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
			printf("[alsa] FAIL: write after %u periods: %s\n", p,
			       strerror(errno));
			print_file("[alsa] after:  ", AUDIO_STATS_ATTR);
			goto out;
		}
		if (p == RATE / PERIOD)
			print_file("[alsa] hdmi:   ", HDMI_AUDIO_ATTR);
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_DRAIN) < 0) {
		printf("[alsa] FAIL: drain: %s\n", strerror(errno));
		goto out;
	}
	elapsed = now_ns() - t0;
	print_file("[alsa] after:  ", AUDIO_STATS_ATTR);

	expect = (double)periods * PERIOD / RATE;
	err = ((double)elapsed / 1e9 - expect) / expect;
	printf("[alsa] %.3f s of audio played in %.3f s (%+.1f%%)\n",
	       expect, (double)elapsed / 1e9, err * 100.0);
	if (fabs(err) > 0.10) {
		printf("[alsa] FAIL: playback not paced by the I2S frame clock\n");
		goto out;
	}
	printf("[alsa] PASS\n");
	ret = 0;
out:
	close(fd);
	return ret;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s rate  [-c txcon]\n"
		"       %s pio   [-t sec] [-f hz] [-c txcon] [-C cts]\n"
		"       %s alsa  [-t sec] [-f hz]\n",
		prog, prog, prog);
}

int main(int argc, char **argv)
{
	uint32_t txcon = DEF_TXCON, cts = 0;
	unsigned int secs = 3, freq = 440;
	const char *cmd;
	int opt;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[1];
	optind = 2;
	while ((opt = getopt(argc, argv, "c:C:t:f:")) != -1) {
		switch (opt) {
		case 'c':
			txcon = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'C':
			cts = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 't':
			secs = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'f':
			freq = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (secs < 1 || secs > 60 || freq < 20 || freq > 20000) {
		usage(argv[0]);
		return 2;
	}

	if (strcmp(cmd, "rate") == 0)
		return cmd_rate(txcon);
	if (strcmp(cmd, "pio") == 0)
		return cmd_pio(secs, freq, txcon, cts);
	if (strcmp(cmd, "alsa") == 0)
		return cmd_alsa(secs, freq);
	usage(argv[0]);
	return 2;
}
