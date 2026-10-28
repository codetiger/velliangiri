// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X audio path diagnostic utility (/bin/audiotest)
 *
 * Usage:
 *   audiotest rate  [-m mod] [-p psr]          measure the I2S0 TX frame rate
 *   audiotest sweep                            measure each I2S0 clock source
 *   audiotest pio   [-t sec] [-f hz] [-m mod] [-p psr]
 *                                              FIFO-paced tone straight to I2S0
 *   audiotest alsa  [-t sec] [-f hz]           tone through the ALSA PCM
 *
 * rate, sweep and pio program I2S0 directly through /dev/mem and refuse to
 * run while the ALSA PCM is open. Exit status: 0 pass, 1 fail, 2 error.
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
#define SPDIF_PHYS		0x34194000UL

#define I2S_CON			(0x00 / 4)
#define I2S_MOD			(0x04 / 4)
#define I2S_FIC			(0x08 / 4)
#define I2S_PSR			(0x0c / 4)
#define I2S_TXD			(0x10 / 4)

#define I2S_CON_RSTCLR		0x80000000U
#define I2S_CON_TXFIFO_FULL	0x00000100U
#define I2S_CON_START		0x80000005U
#define I2S_FIC_TXFLUSH		0x00008000U
#define I2S_FIC_TXCOUNT(v)	(((v) >> 8) & 0x7fU)
#define I2S_MOD_OPCLK_MASK	0xc0000000U
#define I2S_PSR_PSREN		0x00008000U

#define SPDIF_CLKCON		(0x00 / 4)
#define SPDIF_CON		(0x04 / 4)
#define SPDIF_CSTAS		(0x0c / 4)
#define SPDIF_DAT		(0x10 / 4)

/* Defaults match the s5l8947x-audio driver module parameters */
#define DEF_MOD			0xc0000000U
#define DEF_PSR			0x00008100U

#define RATE			48000U
#define RATE_TOLERANCE		0.01
#define FIFO_FILL_LIMIT		512U
#define STALL_NS		50000000LL

#define PCM_DEV			"/dev/snd/pcmC0D0p"
#define PCM_STATUS		"/proc/asound/card0/pcm0p/sub0/status"
#define HDMI_AUDIO_ATTR		"/sys/bus/platform/devices/3a200000.display-controller/hdmi_audio"
#define AUDIO_STATS_ATTR	"/sys/bus/platform/devices/34190000.audio-controller/fifo_stats"

struct i2s_map {
	int fd;
	volatile uint32_t *i2s;
	volatile uint32_t *spdif;
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

static int map_audio(struct i2s_map *m)
{
	void *i2s, *spdif;

	m->fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (m->fd < 0) {
		perror("audiotest: open(/dev/mem)");
		return -1;
	}
	i2s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, I2S0_PHYS);
	spdif = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, SPDIF_PHYS);
	if (i2s == MAP_FAILED || spdif == MAP_FAILED) {
		perror("audiotest: mmap");
		close(m->fd);
		return -1;
	}
	m->i2s = i2s;
	m->spdif = spdif;
	return 0;
}

static void unmap_audio(struct i2s_map *m)
{
	munmap((void *)m->i2s, 4096);
	munmap((void *)m->spdif, 4096);
	close(m->fd);
}

static void i2s_stop(struct i2s_map *m)
{
	m->i2s[I2S_CON] = I2S_CON_RSTCLR;
	m->i2s[I2S_FIC] = I2S_FIC_TXFLUSH;
	m->i2s[I2S_FIC] = 0;
}

static void i2s_setup(struct i2s_map *m, uint32_t mod, uint32_t psr)
{
	i2s_stop(m);
	m->i2s[I2S_MOD] = mod;
	m->i2s[I2S_PSR] = psr;
}

/* Fill the stopped TX FIFO; returns frames accepted before FULL, or -1 */
static int i2s_prefill(struct i2s_map *m)
{
	unsigned int n;

	for (n = 0; n < FIFO_FILL_LIMIT; n++) {
		if (m->i2s[I2S_CON] & I2S_CON_TXFIFO_FULL)
			return (int)n;
		m->i2s[I2S_TXD] = 0;
	}
	return -1;
}

/*
 * Measure how fast the I2S0 TX FIFO drains. Returns frames per second,
 * 0 when the FIFO does not drain, or -1 when FULL is never reported.
 */
static double measure_rate(struct i2s_map *m, uint32_t mod, uint32_t psr,
			   int verbose)
{
	int64_t t0, last, t;
	uint64_t frames = 0;
	int depth;

	i2s_setup(m, mod, psr);
	depth = i2s_prefill(m);
	if (verbose)
		printf("[rate] mod=0x%08x psr=0x%08x readback mod=0x%08x psr=0x%08x\n",
		       mod, psr, m->i2s[I2S_MOD], m->i2s[I2S_PSR]);
	if (depth < 0) {
		if (verbose)
			printf("[rate] TX FIFO accepted %u frames without reporting FULL (CON=0x%08x FIC=0x%08x)\n",
			       FIFO_FILL_LIMIT, m->i2s[I2S_CON], m->i2s[I2S_FIC]);
		i2s_stop(m);
		return -1;
	}
	if (verbose)
		printf("[rate] TX FIFO depth %d frames (FIC count %u)\n",
		       depth, I2S_FIC_TXCOUNT(m->i2s[I2S_FIC]));

	m->i2s[I2S_CON] = I2S_CON_START;
	t0 = last = now_ns();
	for (;;) {
		t = now_ns();
		if (t - t0 >= 500000000LL)
			break;
		if (t - last >= STALL_NS) {
			i2s_stop(m);
			return 0;
		}
		if (!(m->i2s[I2S_CON] & I2S_CON_TXFIFO_FULL)) {
			m->i2s[I2S_TXD] = 0;
			frames++;
			last = t;
		}
	}
	i2s_stop(m);
	return (double)frames * 1e9 / (double)(t - t0);
}

static int report_rate(double rate)
{
	double err;

	if (rate < 0) {
		printf("[rate] FAIL: FULL flag never set; I2S0 unclocked or flag differs\n");
		return 1;
	}
	if (rate == 0) {
		printf("[rate] FAIL: TX FIFO stopped draining; no I2S0 frame clock\n");
		return 1;
	}
	err = (rate - RATE) / RATE;
	printf("[rate] measured %.1f Hz (target %u Hz, error %+.2f%%)\n",
	       rate, RATE, err * 100.0);
	if (fabs(err) > RATE_TOLERANCE) {
		printf("[rate] FAIL: frame clock outside +/-%.0f%%\n",
		       RATE_TOLERANCE * 100.0);
		return 1;
	}
	printf("[rate] PASS\n");
	return 0;
}

static int cmd_rate(uint32_t mod, uint32_t psr)
{
	struct i2s_map m;
	int ret;

	if (!pcm_is_idle() || map_audio(&m))
		return 2;
	ret = report_rate(measure_rate(&m, mod, psr, 1));
	unmap_audio(&m);
	return ret;
}

static int cmd_sweep(void)
{
	static const char *const names[4] = {
		"CDCLK out", "CDCLK in", "audio bus", "PCLK"
	};
	struct i2s_map m;
	unsigned int src;
	int found = 0;

	if (!pcm_is_idle() || map_audio(&m))
		return 2;

	printf("[sweep] prescaler disabled; LRCK = source / 256\n");
	for (src = 0; src < 4; src++) {
		uint32_t mod = (DEF_MOD & ~I2S_MOD_OPCLK_MASK) | (src << 30);
		double rate = measure_rate(&m, mod, 0, 0);

		if (rate <= 0) {
			printf("[sweep] opclk=%u (%-9s): %s\n", src, names[src],
			       rate < 0 ? "FIFO never full" : "no clock");
			continue;
		}
		printf("[sweep] opclk=%u (%-9s): %.1f Hz (source ~%.3f MHz)\n",
		       src, names[src], rate, rate * 256.0 / 1e6);
		if (rate >= RATE * (1.0 - RATE_TOLERANCE)) {
			unsigned int div = (unsigned int)(rate / RATE + 0.5);
			double out;

			if (div < 1)
				div = 1;
			if (div > 64)
				div = 64;
			out = rate / div;
			printf("[sweep]   divide by %u -> %.1f Hz (%+.2f%%): i2s_mod=0x%08x i2s_psr=0x%08x\n",
			       div, out, (out - RATE) * 100.0 / RATE, mod,
			       div > 1 ? I2S_PSR_PSREN | ((div - 1) << 8) : 0U);
			found = 1;
		}
	}
	unmap_audio(&m);
	return found ? 0 : 1;
}

static void fill_tone(int16_t *table, unsigned int len, unsigned int freq)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		table[i] = (int16_t)(12000.0 * sin(2.0 * M_PI * freq * i / RATE));
}

static int cmd_pio(unsigned int secs, unsigned int freq, uint32_t mod,
		   uint32_t psr)
{
	static int16_t tone[RATE];
	struct i2s_map m;
	int64_t t0, last, t, end;
	uint64_t frames = 0;
	unsigned long empties = 0;
	unsigned int idx = 0;
	int ret = 0;

	if (!pcm_is_idle() || map_audio(&m))
		return 2;

	fill_tone(tone, RATE, freq);
	if (write_file(HDMI_AUDIO_ATTR, "48000"))
		printf("[pio] WARN: could not enable HDMI audio via %s\n",
		       HDMI_AUDIO_ATTR);
	print_file("[pio] hdmi: ", HDMI_AUDIO_ATTR);

	m.spdif[SPDIF_CON] = 0x00000020U;
	m.spdif[SPDIF_CON] = 0x00381001U;
	m.spdif[SPDIF_CSTAS] = 0x02000104U;
	m.spdif[SPDIF_CLKCON] = 0x00000001U;

	i2s_setup(&m, mod, psr);
	if (i2s_prefill(&m) < 0) {
		printf("[pio] FAIL: TX FIFO never reports FULL\n");
		ret = 1;
		goto out;
	}

	printf("[pio] playing %u Hz for %u s on I2S0 + SPDIF; listen on the TV\n",
	       freq, secs);
	m.i2s[I2S_CON] = I2S_CON_START;
	t0 = last = now_ns();
	end = t0 + (int64_t)secs * 1000000000LL;
	while ((t = now_ns()) < end) {
		uint32_t frame;

		if (m.i2s[I2S_CON] & I2S_CON_TXFIFO_FULL) {
			if (t - last >= STALL_NS) {
				printf("[pio] FAIL: TX FIFO stopped draining\n");
				ret = 1;
				break;
			}
			continue;
		}
		if (!I2S_FIC_TXCOUNT(m.i2s[I2S_FIC]))
			empties++;
		frame = (uint16_t)tone[idx];
		frame |= frame << 16;
		m.i2s[I2S_TXD] = frame;
		m.spdif[SPDIF_DAT] = frame;
		if (++idx == RATE)
			idx = 0;
		frames++;
		last = t;
	}
	if (!ret) {
		double rate = (double)frames * 1e9 / (double)(now_ns() - t0);

		printf("[pio] pushed %llu frames, %lu FIFO-empty events\n",
		       (unsigned long long)frames, empties);
		ret = report_rate(rate);
	}
out:
	i2s_stop(&m);
	write_file(HDMI_AUDIO_ATTR, "off");
	unmap_audio(&m);
	return ret;
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
		"Usage: %s rate  [-m mod] [-p psr]\n"
		"       %s sweep\n"
		"       %s pio   [-t sec] [-f hz] [-m mod] [-p psr]\n"
		"       %s alsa  [-t sec] [-f hz]\n",
		prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	uint32_t mod = DEF_MOD, psr = DEF_PSR;
	unsigned int secs = 3, freq = 440;
	const char *cmd;
	int opt;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[1];
	optind = 2;
	while ((opt = getopt(argc, argv, "m:p:t:f:")) != -1) {
		switch (opt) {
		case 'm':
			mod = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'p':
			psr = (uint32_t)strtoul(optarg, NULL, 0);
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
		return cmd_rate(mod, psr);
	if (strcmp(cmd, "sweep") == 0)
		return cmd_sweep();
	if (strcmp(cmd, "pio") == 0)
		return cmd_pio(secs, freq, mod, psr);
	if (strcmp(cmd, "alsa") == 0)
		return cmd_alsa(secs, freq);
	usage(argv[0]);
	return 2;
}
