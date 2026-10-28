// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X audio path diagnostic utility (/bin/audiotest)
 *
 * Usage:
 *   audiotest rate  [-c txcon]                 measure the I2S0 frame rate
 *   audiotest pio   [-t sec] [-f hz] [-c txcon] [-C cts]
 *                                              FIFO-paced tone straight to I2S0
 *                                              with HDMI audio enabled
 *   audiotest alsa  [-t sec] [-f hz]           tone through the ALSA PCM at 46875 Hz
 *   audiotest hdmi  [options]                  S/PDIF -> HDMI packet-layer matrix
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
 *
 * S/PDIF transmitter register model (measured, matches mainline samsung/spdif.c):
 *   +0x00 CLKCON  bit0 power, bit2 external MCLK
 *   +0x04 CON     bit0 PCM, [2:1] width, [4:3] MCLK/256,384,512, bit5 SW reset,
 *                 [26:22] FIFO level (16 entries, 0 = empty or overflow)
 *   +0x0c CSTAS   consumer channel status, [27:24] sample-rate code
 *   +0x10 DATA    one 16-bit sample in [15:0] per write
 *   Frame rate is 24 MHz / (2 * mclk_divider), i.e. 46875 / 31250 / 23437 Hz
 *   for 256 / 384 / 512 fs. The FIFO is only four frames deep (85 us at 46875),
 *   and its occupancy field wraps to 0 when full, so a feeder must stay at or
 *   below 15 and never fill the last entry.
 *
 * HDMI packet layer (0x39700000 window, HDMI 1.4 offsets):
 *   +0x10000 CON_0 bit0 HDMI enable, bit2 ASP (audio sample packet) enable
 *   +0x10400 ACR_CON, +0x10410 MCTS0 (measured CTS, read-only),
 *             +0x10420 CTS0, +0x10430 N0 (three byte-wide registers each)
 *   +0x10800 AUI_CON, +0x10810..0x1081c header0/1/2 + checksum, +0x10820 payload
 *   +0x30000 S/PDIF receiver (clk ctrl, op ctrl, IRQ status at +0x0c)
 *   +0x40000 I2S receiver / sample-packet mux (MUX_CON at +0x20)
 *   The AUI header is three separate bytes (type 4, version 1, length 10); the
 *   three-byte value 0x84 0x01 0x0a the kernel driver writes decodes as
 *   version 4 and fails the sink's checksum check.
 *
 * audiotest hdmi drives the S/PDIF -> HDMI chain entirely from userspace. It is
 * how the working configuration was established, and it stays useful to compare
 * a userspace stream against the driver when one of them is silent: it saves and
 * restores every register it touches and keeps the HDMI audio path off between
 * runs. The confirmed layout is "left" (0, L, 0, R); the other layouts are kept
 * as controls because they are the ones that look plausible and are silent.
 *
 * Note: the measured CTS (MCTS) is not a liveness oracle. It reads small values
 * most of the time even while the sink is playing.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sched.h>
#include <stdbool.h>
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
#define HDMI_PHYS		0x39700000UL
#define HDMI_MAP_SIZE		0x90000UL

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

/* S/PDIF transmitter (0x34194000) */
#define SPDIF_CLKCON		(0x00 / 4)
#define SPDIF_CON		(0x04 / 4)
#define SPDIF_CSTAS		(0x0c / 4)
#define SPDIF_DATA		(0x10 / 4)

#define SPDIF_CLK_ON		0x01U
#define SPDIF_CON_SW_RESET	0x20U
#define SPDIF_CON_LEVEL(v)	(((v) >> 22) & 0x1fU)
#define SPDIF_FIFO_DEPTH	16U
/* The level field wraps to 0 when the FIFO is full; never write the last entry */
#define SPDIF_FIFO_MAXLEVEL	(SPDIF_FIFO_DEPTH - 1)
#define SPDIF_CON_256FS		0x00381001U	/* PCM 16-bit, MCLK/256 -> 46875 Hz */
#define SPDIF_CON_384FS		0x00381009U	/* MCLK/384 -> 31250 Hz */
#define SPDIF_CON_512FS		0x00381011U	/* MCLK/512 -> 23437 Hz */
#define SPDIF_CSTAS_48K		0x02000104U

/* HDMI core, absolute offsets inside the 0x39700000 window */
#define HDMI_HPD_STATUS		0x00000c
#define HDMI_AUDIO_CLKSEL	0x000070
#define HDMI_CON_0		0x010000
#define HDMI_MODE_SEL		0x010040
#define HDMI_GCP_CON		0x010200
#define HDMI_GCP_BYTE1		0x010210
#define HDMI_ASP_CON		0x010300
#define HDMI_ACR_CON		0x010400
#define HDMI_ACR_MCTS0		0x010410
#define HDMI_ACR_CTS0		0x010420
#define HDMI_ACR_N0		0x010430
#define HDMI_AUI_CON		0x010800
#define HDMI_AUI_HEADER0	0x010810
#define HDMI_AUI_HEADER1	0x010814
#define HDMI_AUI_HEADER2	0x010818
#define HDMI_AUI_CHECK_SUM	0x01081c
#define HDMI_AUI_BYTE1		0x010820

#define HDMI_SPDIF_RX_CLK	0x030000
#define HDMI_SPDIF_RX_OP	0x030004
#define HDMI_SPDIF_RX_IRQ	0x03000c
#define HDMI_SPDIF_RX_CSTAS	0x030030
#define HDMI_SPDIF_RX_PERIOD	0x030048
#define HDMI_SPDIF_RX_LAST	0x030060

#define HDMI_I2S_MUX_CON	0x040020
#define HDMI_I2S_MUX_CH		0x040054
#define HDMI_I2S_MUX_CUV	0x040058

/* MUX_CON for the S/PDIF input: IN_DISABLE | AUD_SPDIF | CUV_SPDIF | MUX_EN */
#define HDMI_MUX_SPDIF		0x11U
#define HDMI_MUX_CUV_RL		0x03U
#define HDMI_MUX_CH_STEREO	0x03U
#define HDMI_MUX_CH_ALL		0xffU

/* AUI header byte 0 candidates: type alone (mainline), packed CEA, current driver */
#define AUI_HDR_TYPE		0x04U
#define AUI_HDR_PACKED		0x81U
#define AUI_HDR_DRIVER		0x84U
#define AUI_LEN			0x0aU

/* HDMI Audio Clock Regeneration, one byte per 32-bit register */
#define TMDS_HZ			74250000.0
#define ACR_N			6144U

#define I2S_REF_HZ		24000000.0
#define DEF_TXCON		0x00000700U	/* BCLK 1.5 MHz, 32-bit frame: 46875 Hz */
#define RATE			46875U		/* rate the audio engine produces */
#define RATE_TOLERANCE		0.005
#define STALL_NS		50000000LL

#define PCM_DEV			"/dev/snd/pcmC0D0p"
#define PCM_STATUS		"/proc/asound/card0/pcm0p/sub0/status"
#define HDMI_AUDIO_ATTR		"/sys/bus/platform/devices/3a200000.display-controller/hdmi_audio"
#define AUDIO_STATS_ATTR	"/sys/bus/platform/devices/34190000.audio-controller/fifo_stats"

/* Frame rate the S/PDIF transmitter produces for each reachable CON value */
struct spdif_rate {
	unsigned int rate;
	uint32_t con;
	const char *label;
};

static const struct spdif_rate spdif_rates[] = {
	{ 46875, SPDIF_CON_256FS, "256fs" },
	{ 31250, SPDIF_CON_384FS, "384fs" },
	{ 23437, SPDIF_CON_512FS, "512fs" },
};

/*
 * Words per stereo frame. A S/PDIF frame is two subframes; 4 words per frame is
 * what the transmitter consumed in the 184.4k word/s measurement, while the
 * early audible run used 4 and every silent run used 2.
 */
enum frame_pack {
	PACK_2,		/* L, R (16-bit sample in [15:0]) */
	PACK_LLRR,	/* L, L, R, R */
	PACK_LRLR,	/* L, R, L, R */
	PACK_DUP,	/* (R << 16 | L) repeated four times */
	PACK_LEFT,	/* 0, L, 0, R: 16-bit sample left-justified (confirmed) */
};

struct hdmi_run {
	const char *name;
	enum frame_pack pack;
	unsigned int aui;
	unsigned int mux_ch;	/* 0 = default */
};

static const struct hdmi_run hdmi_matrix[] = {
	{ "4w left aui=04",		PACK_LEFT, AUI_HDR_TYPE,   0 },
	{ "4w llrr aui=04",		PACK_LLRR, AUI_HDR_TYPE,   0 },
	{ "4w lrlr aui=04",		PACK_LRLR, AUI_HDR_TYPE,   0 },
	{ "4w dup  aui=04",		PACK_DUP,  AUI_HDR_TYPE,   0 },
	{ "2w lrlr aui=04",		PACK_2,    AUI_HDR_TYPE,   0 },
	{ "4w llrr aui=84",		PACK_LLRR, AUI_HDR_DRIVER, 0 },
	{ "4w llrr aui=81",		PACK_LLRR, AUI_HDR_PACKED, 0 },
	{ "4w llrr aui=04 muxch=ff",	PACK_LLRR, AUI_HDR_TYPE,   HDMI_MUX_CH_ALL },
};

struct audio_map {
	int fd;
	volatile uint32_t *i2s;
	volatile uint32_t *spdif;
	volatile uint32_t *hdmi;
};

static int64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void msleep(unsigned int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
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
	void *i2s, *spdif, *hdmi;

	m->fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (m->fd < 0) {
		perror("audiotest: open(/dev/mem)");
		return -1;
	}
	i2s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, I2S0_PHYS);
	spdif = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, SPDIF_PHYS);
	hdmi = mmap(NULL, HDMI_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd,
		    HDMI_PHYS);
	if (i2s == MAP_FAILED || spdif == MAP_FAILED || hdmi == MAP_FAILED) {
		perror("audiotest: mmap");
		close(m->fd);
		return -1;
	}
	m->i2s = i2s;
	m->spdif = spdif;
	m->hdmi = hdmi;
	return 0;
}

static void unmap_audio(struct audio_map *m)
{
	munmap((void *)m->i2s, 4096);
	munmap((void *)m->spdif, 4096);
	munmap((void *)m->hdmi, HDMI_MAP_SIZE);
	close(m->fd);
}

/*
 * HDMI register access. The block stores 24-bit ACR values as one byte per
 * 32-bit register, least significant byte first.
 */
static uint32_t hdmi_rd(struct audio_map *m, unsigned int off)
{
	return m->hdmi[off / 4];
}

static void hdmi_wr(struct audio_map *m, unsigned int off, uint32_t val)
{
	m->hdmi[off / 4] = val;
}

static uint32_t hdmi_rd3(struct audio_map *m, unsigned int off)
{
	return (hdmi_rd(m, off) & 0xffU) |
	       (hdmi_rd(m, off + 4) & 0xffU) << 8 |
	       (hdmi_rd(m, off + 8) & 0xffU) << 16;
}

static void hdmi_wr3(struct audio_map *m, unsigned int off, uint32_t val)
{
	hdmi_wr(m, off, val & 0xffU);
	hdmi_wr(m, off + 4, (val >> 8) & 0xffU);
	hdmi_wr(m, off + 8, (val >> 16) & 0xffU);
}

static uint32_t acr_cts(unsigned int rate)
{
	return (uint32_t)(TMDS_HZ * ACR_N / (128.0 * rate) + 0.5);
}

static const struct spdif_rate *spdif_rate(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < sizeof(spdif_rates) / sizeof(spdif_rates[0]); i++)
		if (spdif_rates[i].rate == rate)
			return &spdif_rates[i];
	return NULL;
}

/*
 * Audio InfoFrame. The header is three byte-wide registers (type, version,
 * length) plus a checksum register, exactly what hdmi_audio_infoframe_pack()
 * produces and what exynos_hdmi.c writes. header0 selects the variant under
 * test: 0x04 type only, 0x81 packed CEA header, 0x84 what the driver writes.
 */
static void hdmi_aui(struct audio_map *m, unsigned int header0)
{
	uint8_t payload[AUI_LEN];
	unsigned int i, sum;

	memset(payload, 0, sizeof(payload));
	payload[0] = 0x01;	/* LPCM, channel count - 1 = 1 */

	hdmi_wr(m, HDMI_AUI_HEADER0, header0);
	hdmi_wr(m, HDMI_AUI_HEADER1, 0x01);
	hdmi_wr(m, HDMI_AUI_HEADER2, AUI_LEN);
	sum = header0 + 0x01U + AUI_LEN;
	for (i = 0; i < sizeof(payload); i++)
		sum += payload[i];
	hdmi_wr(m, HDMI_AUI_CHECK_SUM, (0x100U - (sum & 0xffU)) & 0xffU);
	for (i = 0; i < sizeof(payload); i++)
		hdmi_wr(m, HDMI_AUI_BYTE1 + 4 * i, payload[i]);
}

struct hdmi_state {
	uint32_t clksel, mux_con, mux_ch, mux_cuv;
	uint32_t spdif_rx_clk, spdif_rx_op;
	uint32_t spdif_clkcon, spdif_con, spdif_cstas;
	uint32_t con0, acr_con, aui_con, asp_con, n, cts;
	uint32_t aui[14];	/* header 0..2, checksum, 10 payload bytes */
};

static void hdmi_state_save(struct audio_map *m, struct hdmi_state *s)
{
	unsigned int i;

	s->clksel = hdmi_rd(m, HDMI_AUDIO_CLKSEL) & 0xffU;
	s->mux_con = hdmi_rd(m, HDMI_I2S_MUX_CON) & 0xffU;
	s->mux_ch = hdmi_rd(m, HDMI_I2S_MUX_CH) & 0xffU;
	s->mux_cuv = hdmi_rd(m, HDMI_I2S_MUX_CUV) & 0xffU;
	s->spdif_rx_clk = hdmi_rd(m, HDMI_SPDIF_RX_CLK) & 0xffU;
	s->spdif_rx_op = hdmi_rd(m, HDMI_SPDIF_RX_OP) & 0xffU;
	s->spdif_clkcon = m->spdif[SPDIF_CLKCON];
	s->spdif_con = m->spdif[SPDIF_CON];
	s->spdif_cstas = m->spdif[SPDIF_CSTAS];
	s->con0 = hdmi_rd(m, HDMI_CON_0);
	s->acr_con = hdmi_rd(m, HDMI_ACR_CON);
	s->aui_con = hdmi_rd(m, HDMI_AUI_CON);
	s->asp_con = hdmi_rd(m, HDMI_ASP_CON);
	s->n = hdmi_rd3(m, HDMI_ACR_N0);
	s->cts = hdmi_rd3(m, HDMI_ACR_CTS0);
	for (i = 0; i < 14; i++)
		s->aui[i] = hdmi_rd(m, HDMI_AUI_HEADER0 + 4 * i) & 0xffU;
}

static void hdmi_state_restore(struct audio_map *m, const struct hdmi_state *s)
{
	unsigned int i;

	/* Quiet the path before the packet data and enables go back */
	hdmi_wr(m, HDMI_AUI_CON, 0);
	hdmi_wr(m, HDMI_ACR_CON, 0);
	hdmi_wr(m, HDMI_CON_0, hdmi_rd(m, HDMI_CON_0) & ~0x04U);

	hdmi_wr(m, HDMI_I2S_MUX_CON, s->mux_con);
	hdmi_wr(m, HDMI_I2S_MUX_CH, s->mux_ch);
	hdmi_wr(m, HDMI_I2S_MUX_CUV, s->mux_cuv);
	hdmi_wr(m, HDMI_AUDIO_CLKSEL, s->clksel);
	hdmi_wr(m, HDMI_SPDIF_RX_IRQ, 0xff);
	hdmi_wr(m, HDMI_SPDIF_RX_OP, s->spdif_rx_op);
	hdmi_wr(m, HDMI_SPDIF_RX_CLK, s->spdif_rx_clk);

	m->spdif[SPDIF_CLKCON] = 0;
	m->spdif[SPDIF_CON] = SPDIF_CON_SW_RESET;
	m->spdif[SPDIF_CSTAS] = s->spdif_cstas;
	m->spdif[SPDIF_CON] = s->spdif_con;
	m->spdif[SPDIF_CLKCON] = s->spdif_clkcon;

	hdmi_wr3(m, HDMI_ACR_N0, s->n);
	hdmi_wr3(m, HDMI_ACR_CTS0, s->cts);
	for (i = 0; i < 14; i++)
		hdmi_wr(m, HDMI_AUI_HEADER0 + 4 * i, s->aui[i]);

	hdmi_wr(m, HDMI_ASP_CON, s->asp_con);
	hdmi_wr(m, HDMI_CON_0, s->con0);
	hdmi_wr(m, HDMI_ACR_CON, s->acr_con);
	hdmi_wr(m, HDMI_AUI_CON, s->aui_con);
}

static void spdif_tx_start(struct audio_map *m, uint32_t con)
{
	m->spdif[SPDIF_CLKCON] = 0;
	m->spdif[SPDIF_CON] = SPDIF_CON_SW_RESET;
	m->spdif[SPDIF_CON] = con;
	m->spdif[SPDIF_CSTAS] = SPDIF_CSTAS_48K;
	m->spdif[SPDIF_CLKCON] = SPDIF_CLK_ON;
}

static void spdif_tx_stop(struct audio_map *m)
{
	m->spdif[SPDIF_CLKCON] = 0;
	m->spdif[SPDIF_CON] = SPDIF_CON_SW_RESET;
}

/*
 * Program everything except the audio sample packet enable, so the packetiser
 * starts on a stream that is already running.
 */
static void hdmi_prepare(struct audio_map *m, const struct hdmi_run *run,
			 unsigned int rate, unsigned int clksel,
			 unsigned int mux_ch, bool gcp)
{
	hdmi_wr(m, HDMI_AUI_CON, 0);
	hdmi_wr(m, HDMI_ACR_CON, 0);
	hdmi_wr(m, HDMI_CON_0, hdmi_rd(m, HDMI_CON_0) & ~0x04U);

	/* S/PDIF receiver -> sample-packet mux */
	hdmi_wr(m, HDMI_I2S_MUX_CON, HDMI_MUX_SPDIF);
	hdmi_wr(m, HDMI_I2S_MUX_CH, mux_ch);
	hdmi_wr(m, HDMI_I2S_MUX_CUV, HDMI_MUX_CUV_RL);
	hdmi_wr(m, HDMI_SPDIF_RX_IRQ, 0xff);
	hdmi_wr(m, HDMI_SPDIF_RX_CLK, 1);
	hdmi_wr(m, HDMI_SPDIF_RX_OP, 3);

	hdmi_wr(m, HDMI_AUDIO_CLKSEL, clksel);
	hdmi_wr(m, HDMI_MODE_SEL, 2);
	if (gcp) {
		/* Loader defaults: send a GCP with Clear_AVMUTE */
		hdmi_wr(m, HDMI_GCP_CON, 0x0e);
		hdmi_wr(m, HDMI_GCP_BYTE1, 0x10);
	}

	hdmi_wr3(m, HDMI_ACR_N0, ACR_N);
	hdmi_wr3(m, HDMI_ACR_CTS0, acr_cts(rate));
	hdmi_wr(m, HDMI_ACR_CON, 4);
	hdmi_aui(m, run->aui);
}

static void hdmi_go(struct audio_map *m)
{
	hdmi_wr(m, HDMI_CON_0, hdmi_rd(m, HDMI_CON_0) | 0x05U);
	hdmi_wr(m, HDMI_AUI_CON, 2);
}

static void hdmi_stop(struct audio_map *m)
{
	hdmi_wr(m, HDMI_AUI_CON, 0);
	hdmi_wr(m, HDMI_CON_0, hdmi_rd(m, HDMI_CON_0) & ~0x04U);
	hdmi_wr(m, HDMI_ACR_CON, 0);
}

static void hdmi_dump(struct audio_map *m, const char *tag)
{
	uint32_t n = hdmi_rd3(m, HDMI_ACR_N0);
	uint32_t cts = hdmi_rd3(m, HDMI_ACR_CTS0);
	uint32_t mcts = hdmi_rd3(m, HDMI_ACR_MCTS0);

	printf("%s con0=%02x clksel=%u mux=%02x muxch=%02x cuv=%02x asp=%02x "
	       "acr_con=%u n=%u cts=%u mcts=%u",
	       tag, hdmi_rd(m, HDMI_CON_0) & 0xffU,
	       hdmi_rd(m, HDMI_AUDIO_CLKSEL) & 0xffU,
	       hdmi_rd(m, HDMI_I2S_MUX_CON) & 0xffU,
	       hdmi_rd(m, HDMI_I2S_MUX_CH) & 0xffU,
	       hdmi_rd(m, HDMI_I2S_MUX_CUV) & 0xffU,
	       hdmi_rd(m, HDMI_ASP_CON) & 0xffU,
	       hdmi_rd(m, HDMI_ACR_CON) & 0xffU, n, cts, mcts);
	if (mcts)
		printf(" -> %.1f Hz", TMDS_HZ * n / (128.0 * mcts));
	printf("\n");

	printf("%s aui=%02x/%02x/%02x gcp=%02x/%02x spdif_rx clk=%u op=%x irq=%02x "
	       "cstas=%02x%02x%02x%02x period=%04x last=%08x %08x %08x %08x\n",
	       tag,
	       hdmi_rd(m, HDMI_AUI_HEADER0) & 0xffU,
	       hdmi_rd(m, HDMI_AUI_HEADER1) & 0xffU,
	       hdmi_rd(m, HDMI_AUI_HEADER2) & 0xffU,
	       hdmi_rd(m, HDMI_GCP_CON) & 0xffU,
	       hdmi_rd(m, HDMI_GCP_BYTE1) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_CLK) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_OP) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_IRQ) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_CSTAS) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_CSTAS + 4) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_CSTAS + 8) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_CSTAS + 12) & 0xffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_PERIOD) & 0xffffU,
	       hdmi_rd(m, HDMI_SPDIF_RX_LAST),
	       hdmi_rd(m, HDMI_SPDIF_RX_LAST + 4),
	       hdmi_rd(m, HDMI_SPDIF_RX_LAST + 8),
	       hdmi_rd(m, HDMI_SPDIF_RX_LAST + 12));
}

static unsigned int frame_words(enum frame_pack pack, int16_t l, int16_t r,
				uint32_t *w)
{
	switch (pack) {
	case PACK_2:
		w[0] = (uint16_t)l;
		w[1] = (uint16_t)r;
		return 2;
	case PACK_LLRR:
		w[0] = w[1] = (uint16_t)l;
		w[2] = w[3] = (uint16_t)r;
		return 4;
	case PACK_LRLR:
		w[0] = w[2] = (uint16_t)l;
		w[1] = w[3] = (uint16_t)r;
		return 4;
	case PACK_DUP:
		w[0] = w[1] = w[2] = w[3] =
			(uint32_t)(uint16_t)r << 16 | (uint16_t)l;
		return 4;
	case PACK_LEFT:
		w[0] = 0;
		w[1] = (uint16_t)l;
		w[2] = 0;
		w[3] = (uint16_t)r;
		return 4;
	}
	return 0;
}

struct tone {
	double phase, step;
	unsigned long underruns;	/* FIFO found empty: a gap in the stream */
	unsigned long waits;		/* FIFO full: feeder back-pressure, normal */
	uint64_t frames;
};

/*
 * Keep the 16-entry TX FIFO fed for @ms. The level is checked once per frame;
 * writing past the FIFO is not refused, it wraps the level and drops data, so
 * the watermark has to account for the whole frame. Returns the elapsed time.
 */
static int64_t spdif_feed(struct audio_map *m, enum frame_pack pack, struct tone *tn,
			  unsigned int ms, bool mute)
{
	uint32_t w[4];
	unsigned int n = frame_words(pack, 0, 0, w);
	unsigned int high = SPDIF_FIFO_MAXLEVEL - n;
	int64_t t0 = now_ns(), end = t0 + (int64_t)ms * 1000000LL, now;
	unsigned int lvl, i;

	/* Wait for room; a FIFO that never drains means there is no clock */
	while ((lvl = SPDIF_CON_LEVEL(m->spdif[SPDIF_CON])) > high) {
		if (now_ns() - t0 > 20000000LL)
			return -1;
	}

	while ((now = now_ns()) < end) {
		lvl = SPDIF_CON_LEVEL(m->spdif[SPDIF_CON]);
		if (lvl > high) {
			tn->waits++;
			continue;
		}
		if (!lvl)
			tn->underruns++;

		if (mute) {
			n = frame_words(pack, 0, 0, w);
		} else {
			int16_t v = (int16_t)(12000.0 * sin(tn->phase));

			n = frame_words(pack, v, v, w);
			tn->phase += tn->step;
			if (tn->phase >= 2.0 * M_PI)
				tn->phase -= 2.0 * M_PI;
		}
		for (i = 0; i < n; i++)
			m->spdif[SPDIF_DATA] = w[i];
		tn->frames++;
	}
	return now - t0;
}

static int run_selected(const char *sel, unsigned int idx)
{
	const char *p = sel;

	if (!sel || !strcmp(sel, "all"))
		return 1;
	while (*p) {
		char *end;
		unsigned long v = strtoul(p, &end, 0);

		if (end == p)
			return 0;
		if (v == idx)
			return 1;
		if (*end != ',')
			return 0;
		p = end + 1;
	}
	return 0;
}

static int cmd_hdmi(unsigned int secs, unsigned int freq, unsigned int rate,
		    const char *sel, const char *pack, const char *aui,
		    int list_only, int dump_only, int clksel_force,
		    int mux_ch_force, int gcp)
{
	const struct spdif_rate *sr = spdif_rate(rate);
	struct hdmi_run custom;
	const struct hdmi_run *runs;
	struct hdmi_state saved;
	struct audio_map m;
	unsigned int nruns, i, run_no = 0;
	int ret = 0;

	if (list_only) {
		printf("runs (default matrix):\n");
		for (i = 0; i < sizeof(hdmi_matrix) / sizeof(hdmi_matrix[0]); i++)
			printf("  %u  %s\n", i + 1, hdmi_matrix[i].name);
		return 0;
	}
	if (!sr) {
		fprintf(stderr, "audiotest: unsupported S/PDIF rate %u (use 46875, 31250 or 23437)\n",
			rate);
		return 2;
	}

	if (pack || aui) {
		/* Single custom combination instead of the matrix */
		memset(&custom, 0, sizeof(custom));
		custom.name = "custom";
		custom.pack = PACK_LEFT;
		custom.aui = AUI_HDR_TYPE;
		if (pack) {
			if (!strcmp(pack, "2"))
				custom.pack = PACK_2;
			else if (!strcmp(pack, "llrr"))
				custom.pack = PACK_LLRR;
			else if (!strcmp(pack, "lrlr"))
				custom.pack = PACK_LEFT;
			else if (!strcmp(pack, "dup"))
				custom.pack = PACK_DUP;
			else if (!strcmp(pack, "left"))
				custom.pack = PACK_LEFT;
			else {
				fprintf(stderr, "audiotest: unknown pack '%s' (2, llrr, lrlr, dup, left)\n",
					pack);
				return 2;
			}
		}
		if (aui) {
			char *end;
			unsigned long v = strtoul(aui, &end, 16);

			if (*end || v > 0xff) {
				fprintf(stderr, "audiotest: bad AUI header '%s'\n", aui);
				return 2;
			}
			custom.aui = v;
		}
		runs = &custom;
		nruns = 1;
	} else {
		runs = hdmi_matrix;
		nruns = sizeof(hdmi_matrix) / sizeof(hdmi_matrix[0]);
	}

	if (!pcm_is_idle())
		return 2;
	{
		unsigned int selected = 0;

		for (i = 0; i < nruns; i++)
			selected += run_selected(sel, i + 1);
		if (!selected) {
			fprintf(stderr, "audiotest: no run selected (see -l)\n");
			return 2;
		}
	}
	if (map_audio(&m))
		return 2;

	if (dump_only) {
		hdmi_dump(&m, "[hdmi]");
		unmap_audio(&m);
		return 0;
	}

	hdmi_state_save(&m, &saved);

	/*
	 * The S/PDIF FIFO is four frames deep (85 us at 46875 Hz); a normal
	 * priority feeder cannot keep it full.
	 */
	{
		struct sched_param sp = { .sched_priority = 50 };

		if (sched_setscheduler(0, SCHED_FIFO, &sp))
			printf("[hdmi] WARN: no SCHED_FIFO (%s); expect FIFO gaps\n",
			       strerror(errno));
		else
			printf("[hdmi] feeder runs at SCHED_FIFO priority 50\n");
	}

	if (!(hdmi_rd(&m, HDMI_HPD_STATUS) & 1))
		printf("[hdmi] WARN: no hot-plug detect; audio needs a connected sink\n");

	if (clksel_force >= 0)
		printf("[hdmi] AUDIO_CLKSEL forced to %d (boot value %u)\n",
		       clksel_force, saved.clksel);
	else
		printf("[hdmi] using boot AUDIO_CLKSEL %u\n", saved.clksel);

	printf("[hdmi] S/PDIF %s -> %u Hz, %u Hz tone, %u s per run, %u run(s)\n",
	       sr->label, sr->rate, freq, secs, nruns);

	for (i = 0; i < nruns; i++) {
		const struct hdmi_run *run = &runs[i];
		unsigned int clksel = clksel_force >= 0 ? (unsigned int)clksel_force
							: saved.clksel;
		unsigned int mux_ch = mux_ch_force >= 0 ? (unsigned int)mux_ch_force
						       : (run->mux_ch ? run->mux_ch
								      : HDMI_MUX_CH_STEREO);
		unsigned int this_freq = freq + 60 * run_no;
		struct tone tn = { 0 };
		uint64_t frames0;
		int64_t elapsed;
		uint32_t mcts;

		if (!run_selected(sel, i + 1))
			continue;
		run_no++;

		tn.step = 2.0 * M_PI * this_freq / sr->rate;

		printf("\n[%u/%u] %s: %u Hz, %u Hz tone for %u s\n", i + 1, nruns,
		       run->name, sr->rate, this_freq, secs);
		printf("      listen on the TV now\n");

		spdif_tx_start(&m, sr->con);
		if (spdif_feed(&m, run->pack, &tn, 150, true) < 0) {
			printf("      FAIL: S/PDIF FIFO never drained, no frame clock\n");
			spdif_tx_stop(&m);
			ret = 1;
			continue;
		}

		hdmi_prepare(&m, run, sr->rate, clksel, mux_ch, gcp);
		hdmi_go(&m);
		frames0 = tn.frames;
		elapsed = spdif_feed(&m, run->pack, &tn, secs * 1000, false);
		mcts = hdmi_rd3(&m, HDMI_ACR_MCTS0);

		hdmi_dump(&m, "      ");
		hdmi_stop(&m);
		spdif_tx_stop(&m);

		printf("      fed %.1f frames/s, FIFO gaps %lu, back-pressure %lu\n",
		       elapsed > 0 ? (double)(tn.frames - frames0) * 1e9 / elapsed : 0.0,
		       tn.underruns, tn.waits);
		if (mcts)
			printf("      MCTS %u -> sink announces %.1f Hz (N=%u, CTS written %u)\n",
			       mcts, TMDS_HZ * ACR_N / (128.0 * mcts), ACR_N,
			       acr_cts(sr->rate));
		else
			printf("      MCTS reads 0: the HDMI ACR sees no audio clock\n");

		msleep(1500);
	}

	printf("\n[hdmi] done. Runs that are audible are the working configuration;\n"
	       "       write down their name and the MCTS line above.\n");

	hdmi_state_restore(&m, &saved);
	unmap_audio(&m);
	return ret;
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

struct pio_tone {
	double phase, step;
	int right;
	uint32_t sample;
};

/* Next FIFO word: one channel sample, copied into both 16-bit halves */
static uint32_t tone_next(void *ctx)
{
	struct pio_tone *tn = ctx;

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

static int cmd_pio(unsigned int secs, unsigned int freq, uint32_t txcon,
		   uint32_t cts)
{
	double expect = expected_rate(txcon), rate;
	struct pio_tone tn = { 0 };
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
	n = hdmi_rd3(&m, HDMI_ACR_N0);
	if (cts)
		hdmi_wr3(&m, HDMI_ACR_CTS0, cts);
	printf("[pio] I2S0 frame rate %.1f Hz; HDMI N=%u CTS=%u (CTS for this rate: %.1f)\n",
	       expect, n, hdmi_rd3(&m, HDMI_ACR_CTS0), TMDS_HZ * n / (128.0 * expect));
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
		"       %s alsa  [-t sec] [-f hz]\n"
		"       %s hdmi  [-t sec] [-f hz] [-r rate] [-i runs] [-p pack] [-a aui]\n"
		"                [-k clksel] [-m muxch] [-g] [-l] [-D]\n"
		"\n"
		"hdmi options:\n"
		"  -t secs    tone length per run (default 6)\n"
		"  -f hz      first tone frequency, +60 Hz per run (default 440)\n"
		"  -r rate    S/PDIF rate: 46875 (default), 31250, 23437\n"
		"  -i runs    comma-separated run numbers, or 'all' (default)\n"
		"  -p pack    layout: left (default), 2, llrr, lrlr, dup (single run)\n"
		"  -a aui     AUI header byte 0 in hex: 04, 81, 84 (single run)\n"
		"  -k clksel  force HDMI_AUDIO_CLKSEL (default: keep boot value)\n"
		"  -m muxch   force HDMI_I2S_MUX_CH (default: 03, run may override)\n"
		"  -g         send a GCP with Clear_AVMUTE before each run\n"
		"  -l         list the default matrix\n"
		"  -D         dump the current HDMI audio state and exit\n",
		prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	uint32_t txcon = DEF_TXCON, cts = 0;
	unsigned int secs = 3, freq = 440, rate = 46875;
	const char *sel = NULL, *pack = NULL, *aui = NULL;
	int clksel_force = -1, mux_ch_force = -1;
	int list_only = 0, dump_only = 0, gcp = 0;
	const char *cmd;
	int opt;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}
	cmd = argv[1];
	if (!strcmp(cmd, "hdmi"))
		secs = 6;
	optind = 2;
	while ((opt = getopt(argc, argv, "c:C:t:f:r:i:p:a:k:m:lgD")) != -1) {
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
		case 'r':
			rate = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'i':
			sel = optarg;
			break;
		case 'p':
			pack = optarg;
			break;
		case 'a':
			aui = optarg;
			break;
		case 'k':
			clksel_force = (int)strtoul(optarg, NULL, 0);
			break;
		case 'm':
			mux_ch_force = (int)strtoul(optarg, NULL, 0);
			break;
		case 'l':
			list_only = 1;
			break;
		case 'D':
			dump_only = 1;
			break;
		case 'g':
			gcp = 1;
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
	if (strcmp(cmd, "hdmi") == 0)
		return cmd_hdmi(secs, freq, rate, sel, pack, aui, list_only,
				dump_only, clksel_force, mux_ch_force, gcp);
	usage(argv[0]);
	return 2;
}
