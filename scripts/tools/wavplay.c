// SPDX-License-Identifier: GPL-2.0-only
/*
 * WAV file player for the S5L8947X ALSA card (/bin/wavplay)
 *
 * Usage: wavplay [-d pcm-device] [-r rate] file.wav
 *
 * Plays 8-bit and 16-bit PCM WAV files with any number of channels at any
 * sample rate. The audio engine plays S16 stereo at 32, 44.1 and 48 kHz on
 * HDMI and 46875 Hz on optical, so the file is converted on the fly: the
 * first two channels are used (mono is duplicated) and, when the device does
 * not take the file's rate (or -r asks for another), the rate is converted by
 * linear interpolation.
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
static size_t wav_render(const struct wav *w, uint64_t *pos, unsigned int dst_rate,
			 int16_t *out, size_t frames)
{
	uint64_t step = ((uint64_t)w->rate << 32) / dst_rate;
	unsigned int right = w->channels > 1 ? 1 : 0;
	size_t i, live = 0;

	for (i = 0; i < frames; i++, *pos += step) {
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

static int pcm_hw_params(int fd, unsigned int rate)
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
	hw_interval_set(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
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
static unsigned int pcm_setup(int fd, unsigned int rate, unsigned int file_rate)
{
	const unsigned int tries[] = { file_rate, 48000, 44100, 32000, 46875 };
	unsigned int i;

	if (rate) {
		if (pcm_hw_params(fd, rate) < 0) {
			perror("wavplay: SNDRV_PCM_IOCTL_HW_PARAMS");
			return 0;
		}
	} else {
		for (i = 0; i < sizeof(tries) / sizeof(tries[0]); i++)
			if (pcm_hw_params(fd, tries[i]) == 0)
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
	fprintf(stderr, "usage: %s [-d pcm-device] [-r rate] file.wav\n", prog);
}

int main(int argc, char **argv)
{
	const char *dev = DEFAULT_PCM;
	unsigned int rate = 0;
	int16_t period[PERIOD_FRAMES * 2];
	struct wav w = { 0 };
	uint8_t *file;
	size_t len;
	uint64_t pos = 0;
	int opt, fd, ret = 1;

	while ((opt = getopt(argc, argv, "d:r:")) != -1) {
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
	rate = pcm_setup(fd, rate, w.rate);
	if (!rate)
		goto out;
	printf("%s: %u Hz, %u ch, %u-bit, %.2f s -> %u Hz stereo\n", argv[optind],
	       w.rate, w.channels, w.bytes * 8, (double)w.frames / w.rate, rate);

	while (wav_render(&w, &pos, rate, period, PERIOD_FRAMES)) {
		if (pcm_write(fd, period, PERIOD_FRAMES))
			goto out;
	}
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
