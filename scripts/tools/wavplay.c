// SPDX-License-Identifier: GPL-2.0-only
/*
 * WAV file player for the S5L8947X ALSA card (/bin/wavplay)
 *
 * Usage: wavplay [-d pcm-device] [-r rate] [-c channels] [-p pair] file.wav
 *
 * Plays 8-bit and 16-bit PCM WAV files with any number of channels at any
 * sample rate. The audio engine plays S16 at 32, 44.1 and 48 kHz on HDMI
 * (2, 4, 6 or 8 channels) and 46875 Hz stereo on optical, so the file is
 * converted on the fly: a file with at least -c channels is copied channel
 * for channel, otherwise its first two channels are placed in pair -p (mono
 * is duplicated). When the device does not take the file's rate (or -r asks
 * for another), the rate is converted by linear interpolation.
 *
 * The ALSA PCM is driven through the kernel ioctl interface directly so the
 * player builds statically without alsa-lib.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sound/asound.h>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define DEFAULT_PCM	"/dev/snd/pcmC0D0p"
#define PERIOD_FRAMES	1024	/* multiples of the 256-frame DMA segment */
#define BUFFER_FRAMES	8192

#define WAVE_FORMAT_PCM		1
#define WAVE_FORMAT_EXTENSIBLE	0xfffe

struct wav {
	uint8_t *data;		/* start of the sample data */
	size_t frames;
	unsigned int channels;
	unsigned int bytes;	/* bytes per sample: 1 or 2 */
	unsigned int rate;
};

static uint16_t le16(const uint8_t *p)
{
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
	return le16(p) | (uint32_t)le16(p + 2) << 16;
}

/* Read the whole file; the returned buffer is owned by the caller. */
static uint8_t *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf = NULL;
	long size;

	if (!f) {
		perror(path);
		return NULL;
	}
	if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		perror(path);
		goto out;
	}
	buf = malloc(size ? size : 1);
	if (!buf || fread(buf, 1, size, f) != (size_t)size) {
		fprintf(stderr, "wavplay: cannot read %s\n", path);
		free(buf);
		buf = NULL;
		goto out;
	}
	*len = size;
out:
	fclose(f);
	return buf;
}

static int wav_parse(uint8_t *file, size_t len, struct wav *w)
{
	size_t pos = 12;
	bool have_fmt = false;
	unsigned int bits = 0;

	if (len < 12 || memcmp(file, "RIFF", 4) || memcmp(file + 8, "WAVE", 4)) {
		fprintf(stderr, "wavplay: not a RIFF/WAVE file\n");
		return -1;
	}

	while (pos + 8 <= len) {
		uint32_t size = le32(file + pos + 4);
		const uint8_t *body = file + pos + 8;
		size_t avail = len - pos - 8;

		if (!memcmp(file + pos, "fmt ", 4) && size >= 16 && avail >= 16) {
			unsigned int tag = le16(body);

			if (tag == WAVE_FORMAT_EXTENSIBLE && size >= 26 && avail >= 26)
				tag = le16(body + 24);
			if (tag != WAVE_FORMAT_PCM) {
				fprintf(stderr, "wavplay: unsupported format tag 0x%x (PCM only)\n",
					tag);
				return -1;
			}
			w->channels = le16(body + 2);
			w->rate = le32(body + 4);
			bits = le16(body + 14);
			have_fmt = true;
		} else if (!memcmp(file + pos, "data", 4)) {
			if (!have_fmt) {
				fprintf(stderr, "wavplay: data chunk before fmt chunk\n");
				return -1;
			}
			if (bits != 8 && bits != 16) {
				fprintf(stderr, "wavplay: unsupported sample size %u bits\n",
					bits);
				return -1;
			}
			if (!w->channels || !w->rate) {
				fprintf(stderr, "wavplay: invalid fmt chunk\n");
				return -1;
			}
			/* Streamed files may carry a bogus size: trust the file length */
			if (size > avail)
				size = avail;
			w->bytes = bits / 8;
			w->data = file + pos + 8;
			w->frames = size / (w->bytes * w->channels);
			return 0;
		}
		pos += 8 + (size_t)size + (size & 1);
	}
	fprintf(stderr, "wavplay: no data chunk\n");
	return -1;
}

static int16_t wav_sample(const struct wav *w, size_t frame, unsigned int ch)
{
	const uint8_t *p = w->data + (frame * w->channels + ch) * w->bytes;

	if (w->bytes == 1)
		return (int16_t)(((int)p[0] - 128) << 8);
	return (int16_t)le16(p);
}

/*
 * Produce 'frames' interleaved S16 stereo frames at the output rate. 'pos' is
 * the source position in 32.32 fixed point and advances by src/dst per frame;
 * once it passes the end of the file the rest is silence. Returns the number of
 * frames that still came from the file.
 */
static int wav_src_channel(const struct wav *w, unsigned int out_ch, int pair,
			   unsigned int ch)
{
	if (w->channels >= out_ch)
		return ch;
	if (pair < 0)
		return ch < w->channels ? (int)ch : -1;
	if ((int)(ch / 2) != pair)
		return -1;
	ch %= 2;
	if (ch < w->channels)
		return ch;
	return w->channels == 1 ? 0 : -1;
}

static size_t wav_render(const struct wav *w, uint64_t *pos, unsigned int dst_rate,
			 int16_t *out, size_t frames, unsigned int out_ch, int pair)
{
	uint64_t step = ((uint64_t)w->rate << 32) / dst_rate;
	unsigned int right = w->channels > 1 ? 1 : 0;
	size_t i = 0, live = 0;

#ifdef __ARM_NEON
	if (out_ch == 2 && pair == 0 && w->bytes == 2 && w->channels == 2 &&
	    w->frames > 1) {
		const int16_t *in = (const int16_t *)w->data;

		for (; i + 4 <= frames; i += 4) {
			uint64_t p3 = *pos + step * 3;
			if ((p3 >> 32) + 1 >= w->frames)
				break;
			size_t idx0 = *pos >> 32;
			int16_t f0 = (int16_t)((*pos >> 17) & 0x7fff);
			*pos += step;
			size_t idx1 = *pos >> 32;
			int16_t f1 = (int16_t)((*pos >> 17) & 0x7fff);
			*pos += step;
			size_t idx2 = *pos >> 32;
			int16_t f2 = (int16_t)((*pos >> 17) & 0x7fff);
			*pos += step;
			size_t idx3 = *pos >> 32;
			int16_t f3 = (int16_t)((*pos >> 17) & 0x7fff);
			*pos += step;

			int16x4_t s0 = vld1_s16(&in[idx0 * 2]);
			int16x4_t s1 = vld1_s16(&in[idx1 * 2]);
			int16x4_t s2 = vld1_s16(&in[idx2 * 2]);
			int16x4_t s3 = vld1_s16(&in[idx3 * 2]);

			int32x2x2_t t01 = vtrn_s32(vreinterpret_s32_s16(s0), vreinterpret_s32_s16(s1));
			int32x2x2_t t23 = vtrn_s32(vreinterpret_s32_s16(s2), vreinterpret_s32_s16(s3));
			int16x8_t v0 = vreinterpretq_s16_s32(vcombine_s32(t01.val[0], t23.val[0]));
			int16x8_t v1 = vreinterpretq_s16_s32(vcombine_s32(t01.val[1], t23.val[1]));
			int16_t fracs[8] = { f0, f0, f1, f1, f2, f2, f3, f3 };
			int16x8_t vf = vld1q_s16(fracs);

			int32x4_t d_lo = vsubl_s16(vget_low_s16(v1), vget_low_s16(v0));
			int32x4_t d_hi = vsubl_s16(vget_high_s16(v1), vget_high_s16(v0));
			d_lo = vmulq_s32(d_lo, vmovl_s16(vget_low_s16(vf)));
			d_hi = vmulq_s32(d_hi, vmovl_s16(vget_high_s16(vf)));

			int16x4_t r_lo = vadd_s16(vget_low_s16(v0), vshrn_n_s32(d_lo, 15));
			int16x4_t r_hi = vadd_s16(vget_high_s16(v0), vshrn_n_s32(d_hi, 15));
			vst1q_s16(&out[i * 2], vcombine_s16(r_lo, r_hi));
			live += 4;
		}
	}
#endif

	if (out_ch == 2 && pair == 0) {
		for (; i < frames; i++, *pos += step) {
			size_t idx = *pos >> 32;
			int32_t frac = (*pos >> 16) & 0xffff;
			int32_t l, r;

			if (idx >= w->frames) {
				out[i * 2] = 0;
				out[i * 2 + 1] = 0;
				continue;
			}
			live++;
			l = wav_sample(w, idx, 0);
			r = wav_sample(w, idx, right);
			if (idx + 1 < w->frames) {
				l += ((int64_t)(wav_sample(w, idx + 1, 0) - l) * frac) >> 16;
				r += ((int64_t)(wav_sample(w, idx + 1, right) - r) * frac) >> 16;
			}
			out[i * 2] = l;
			out[i * 2 + 1] = r;
		}
		return live;
	}

	for (; i < frames; i++, *pos += step) {
		size_t idx = *pos >> 32;
		int32_t frac = (*pos >> 16) & 0xffff;
		unsigned int ch;

		for (ch = 0; ch < out_ch; ch++) {
			int src = wav_src_channel(w, out_ch, pair, ch);
			int32_t v;

			if (src < 0 || idx >= w->frames) {
				out[i * out_ch + ch] = 0;
				continue;
			}
			v = wav_sample(w, idx, src);
			if (idx + 1 < w->frames)
				v += ((int64_t)(wav_sample(w, idx + 1, src) - v) *
				      frac) >> 16;
			out[i * out_ch + ch] = v;
		}
		if (idx < w->frames)
			live++;
	}
	return live;
}

static void hw_mask_set(struct snd_pcm_hw_params *p, int param, unsigned int bit)
{
	struct snd_mask *mask = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(mask, 0, sizeof(*mask));
	mask->bits[bit / 32] |= 1U << (bit % 32);
}

static void hw_interval_set(struct snd_pcm_hw_params *p, int param, unsigned int val)
{
	struct snd_interval *iv =
		&p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	memset(iv, 0, sizeof(*iv));
	iv->min = val;
	iv->max = val;
	iv->integer = 1;
}

static int pcm_hw_params(int fd, unsigned int rate, unsigned int channels)
{
	struct snd_pcm_hw_params hw;
	int i;

	memset(&hw, 0, sizeof(hw));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&hw.masks[i], 0xff, sizeof(hw.masks[i]));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
		hw.intervals[i].max = ~0U;
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	hw_mask_set(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_RATE, rate);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD_FRAMES);
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER_FRAMES);
	hw.rmask = ~0U;
	return ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
}

/*
 * Configure the PCM at 'rate', or with rate 0 at the first of the file's own
 * rate and the device's standard rates that the device accepts. Returns the
 * rate in use, 0 on failure.
 */
static unsigned int pcm_setup(int fd, unsigned int rate, unsigned int file_rate,
			      unsigned int channels)
{
	const unsigned int tries[] = { file_rate, 48000, 44100, 32000, 46875 };
	unsigned int i;

	if (rate) {
		if (pcm_hw_params(fd, rate, channels) < 0) {
			perror("wavplay: SNDRV_PCM_IOCTL_HW_PARAMS");
			return 0;
		}
	} else {
		for (i = 0; i < sizeof(tries) / sizeof(tries[0]); i++)
			if (pcm_hw_params(fd, tries[i], channels) == 0)
				break;
		if (i == sizeof(tries) / sizeof(tries[0])) {
			fprintf(stderr, "wavplay: the device takes none of the standard rates\n");
			return 0;
		}
		rate = tries[i];
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		perror("wavplay: SNDRV_PCM_IOCTL_PREPARE");
		return 0;
	}
	return rate;
}

/* Write one period, recovering from an underrun once. */
static int pcm_write(int fd, int16_t *buf, size_t frames)
{
	struct snd_xferi xfer = { .buf = buf, .frames = frames };

	while (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
		if (errno == EINTR)
			continue;
		if (errno == EPIPE && ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) == 0) {
			fprintf(stderr, "wavplay: underrun\n");
			continue;
		}
		perror("wavplay: write");
		return -1;
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s [-d pcm-device] [-r rate] [-c channels] [-p pair] file.wav\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *dev = DEFAULT_PCM;
	unsigned int rate = 0;
	unsigned int channels = 2;
	int pair = 0;
	int16_t *period;
	struct wav w = { 0 };
	uint8_t *file;
	size_t len;
	uint64_t pos = 0;
	int opt, fd, ret = 1;

	while ((opt = getopt(argc, argv, "d:r:c:p:")) != -1) {
		switch (opt) {
		case 'd':
			dev = optarg;
			break;
		case 'r':
			rate = strtoul(optarg, NULL, 10);
			if (!rate) {
				fprintf(stderr, "wavplay: bad rate '%s'\n", optarg);
				return 2;
			}
			break;
		case 'c':
			channels = strtoul(optarg, NULL, 10);
			if (!channels || channels > 8) {
				fprintf(stderr, "wavplay: bad channel count '%s'\n",
					optarg);
				return 2;
			}
			break;
		case 'p':
			pair = strtol(optarg, NULL, 10);
			if (pair < 0 || pair > 3) {
				fprintf(stderr, "wavplay: bad channel pair '%s'\n",
					optarg);
				return 2;
			}
			break;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (optind != argc - 1) {
		usage(argv[0]);
		return 2;
	}

	file = slurp(argv[optind], &len);
	if (!file)
		return 2;
	if (wav_parse(file, len, &w)) {
		free(file);
		return 2;
	}
	fd = open(dev, O_WRONLY);
	if (fd < 0) {
		perror(dev);
		free(file);
		return 2;
	}
	period = malloc(PERIOD_FRAMES * channels * sizeof(*period));
	if (!period) {
		fprintf(stderr, "wavplay: out of memory\n");
		goto out;
	}
	rate = pcm_setup(fd, rate, w.rate, channels);
	if (!rate) {
		free(period);
		goto out;
	}
	printf("%s: %u Hz, %u ch, %u-bit, %.2f s -> %u Hz, %u ch (source pair %d)\n",
	       argv[optind], w.rate, w.channels, w.bytes * 8,
	       (double)w.frames / w.rate, rate, channels, pair);

	while (wav_render(&w, &pos, rate, period, PERIOD_FRAMES, channels, pair)) {
		if (pcm_write(fd, period, PERIOD_FRAMES)) {
			free(period);
			goto out;
		}
	}
	free(period);
	if (ioctl(fd, SNDRV_PCM_IOCTL_DRAIN) < 0 && errno != EPIPE) {
		perror("wavplay: drain");
		goto out;
	}
	ret = 0;
out:
	close(fd);
	free(file);
	return ret;
}
