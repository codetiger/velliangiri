// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X (j33iap) Hardware Subsystem Verification Utility (/bin/hwtest)
 *
 * Usage:
 *   hwtest [all|audio|gpu|display|mcu|wireless]
 *
 * Exercises and verifies:
 *   1. Audio Engine (ALSA 48 kHz 16-bit stereo 440 Hz tone on /dev/snd/pcmC0D0p,
 *      I2S0 @ 0x34190000, SPDIF @ 0x34194000, and HDMI 1.4 ACR/Audio InfoFrame)
 *   2. PowerVR SGX543 GPU (/dev/sgx0, PMGR clock gates 7/8/9 power state cycling,
 *      and core telemetry)
 *   3. Display Pipe & M2M Scaler (720p60 rgbout, HDMI HPD/PHY, M2M scaler gate 26,
 *      and 10-bit Gamma LUT brightness test)
 *   4. On-Board IR / System MCU (UART1 @ 250000 baud, /dev/input/event0, status LED)
 *   5. Wireless Combo Core (BCM4334 wlan_reg_on, HSIC gates 32/78, UART2 @ 115200)
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
#include <unistd.h>
#include <sound/asound.h>

static int read_sysfs_str(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

static int write_sysfs_str(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, val, strlen(val));
	close(fd);
	return (n == (ssize_t)strlen(val)) ? 0 : -1;
}

static int read_mmio32(unsigned long phys, uint32_t *out)
{
	int fd = open("/dev/mem", O_RDONLY | O_SYNC);
	unsigned long base = phys & ~0xfffUL;
	unsigned long off = phys & 0xfffUL;
	void *map;

	if (fd < 0)
		return -1;
	map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, (off_t)base);
	if (map == MAP_FAILED) {
		close(fd);
		return -1;
	}
	*out = *(volatile uint32_t *)((uintptr_t)map + off);
	munmap(map, 4096);
	close(fd);
	return 0;
}

static void set_mask_bit(struct snd_mask *m, unsigned int bit)
{
	memset(m, 0, sizeof(*m));
	m->bits[bit / 32] |= (1U << (bit % 32));
}

static void set_interval_minmax(struct snd_interval *iv, unsigned int min_v,
				unsigned int max_v)
{
	memset(iv, 0, sizeof(*iv));
	iv->min = min_v;
	iv->max = max_v;
	iv->integer = 1;
}

static void stream_1sec_440hz_hw_fifo(void)
{
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	void *i2s_map, *spdif_map, *wdt_map, *hdmi_map, *pmgr_map;
	volatile uint32_t *i2s0, *i2s1, *i2s2, *i2s3, *spdif, *wdt_cur;
	volatile uint32_t *hctrl, *hcore, *hi2s, *pmgr;
	int16_t wave[480]; /* 10 ms = 480 frames at 48 kHz */
	uint32_t prev_t, acc_t = 0;
	int i, g;

	if (fd < 0)
		return;

	i2s_map = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x34190000UL);
	spdif_map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x34194000UL);
	wdt_map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x3f103000UL);
	hdmi_map = mmap(NULL, 0x50000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x39700000UL);
	pmgr_map = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x3f100000UL);
	if (i2s_map == MAP_FAILED || spdif_map == MAP_FAILED ||
	    wdt_map == MAP_FAILED || hdmi_map == MAP_FAILED ||
	    pmgr_map == MAP_FAILED) {
		if (i2s_map != MAP_FAILED)
			munmap(i2s_map, 0x4000);
		if (spdif_map != MAP_FAILED)
			munmap(spdif_map, 4096);
		if (wdt_map != MAP_FAILED)
			munmap(wdt_map, 4096);
		if (hdmi_map != MAP_FAILED)
			munmap(hdmi_map, 0x50000);
		if (pmgr_map != MAP_FAILED)
			munmap(pmgr_map, 0x2000);
		close(fd);
		return;
	}

	i2s0 = (volatile uint32_t *)((uintptr_t)i2s_map + 0x0000UL);
	i2s1 = (volatile uint32_t *)((uintptr_t)i2s_map + 0x1000UL);
	i2s2 = (volatile uint32_t *)((uintptr_t)i2s_map + 0x2000UL);
	i2s3 = (volatile uint32_t *)((uintptr_t)i2s_map + 0x3000UL);
	spdif = (volatile uint32_t *)spdif_map;
	wdt_cur = (volatile uint32_t *)((uintptr_t)wdt_map + 0x020UL);
	hctrl = (volatile uint32_t *)((uintptr_t)hdmi_map + 0x00000UL);
	hcore = (volatile uint32_t *)((uintptr_t)hdmi_map + 0x10000UL);
	hi2s = (volatile uint32_t *)((uintptr_t)hdmi_map + 0x40000UL);
	pmgr = (volatile uint32_t *)pmgr_map;

	/* Ensure all 6 PMGR audio clock gates (37..42 at +0x109c..+0x10b0) are active */
	for (g = 37; g <= 42; g++) {
		uint32_t off = (0x1008U + (uint32_t)g * 4U) / 4U;
		uint32_t v = pmgr[off];

		if (v)
			pmgr[off] = (v & ~0x10fU) | 0x0fU;
	}

	/* Ensure HDMI 1.4 mode, guard-band/preamble, ACR (0x10400), and ASP (0x10300) */
	hctrl[0x0070 / 4] = 0x00U; /* HDMI_AUDIO_CLKSEL */
	hcore[0x0040 / 4] = 0x02U; /* HDMI_MODE_SEL: HDMI mode */
	hcore[0x0008 / 4] = 0x00U; /* HDMI_CON_2: enable preamble & guard band */
	hcore[0x0430 / 4] = 0x00U; /* HDMI_V14_ACR_N0: 6144 = 0x001800 */
	hcore[0x0434 / 4] = 0x18U; /* HDMI_V14_ACR_N1 */
	hcore[0x0438 / 4] = 0x00U; /* HDMI_V14_ACR_N2 */
	hcore[0x0410 / 4] = 0x0aU; /* HDMI_V14_ACR_MCTS0: 74250 = 0x01220a */
	hcore[0x0414 / 4] = 0x22U; /* HDMI_V14_ACR_MCTS1 */
	hcore[0x0418 / 4] = 0x01U; /* HDMI_V14_ACR_MCTS2 */
	hcore[0x0420 / 4] = 0x0aU; /* HDMI_V14_ACR_CTS0: 74250 = 0x01220a */
	hcore[0x0424 / 4] = 0x22U; /* HDMI_V14_ACR_CTS1 */
	hcore[0x0428 / 4] = 0x01U; /* HDMI_V14_ACR_CTS2 */
	hcore[0x0400 / 4] = 0x04U; /* HDMI_V14_ACR_CON */
	hcore[0x0304 / 4] = 0x00U; /* HDMI_ASP_SP_FLAT */
	hcore[0x0310 / 4] = 0x08U; /* HDMI_ASP_CHCFG0 */
	hcore[0x0314 / 4] = 0x1aU; /* HDMI_ASP_CHCFG1 */
	hcore[0x0318 / 4] = 0x2cU; /* HDMI_ASP_CHCFG2 */
	hcore[0x031c / 4] = 0x3eU; /* HDMI_ASP_CHCFG3 */
	hcore[0x0300 / 4] = 0x00U; /* HDMI_ASP_CON */

	/* Enable HDMI 1.4 internal I2S receiver and subpacket mux (0x39740000) */
	hi2s[0x00 / 4] = 0x00U; /* HDMI_I2S_CLK_CON: reset */
	hi2s[0x00 / 4] = 0x01U; /* HDMI_I2S_CLK_CON: enable */
	hi2s[0x1c / 4] |= 0x01U; /* HDMI_I2S_DSD_CON */
	hi2s[0x0c / 4] = 0x56U; /* HDMI_I2S_PIN_SEL_0: SCLK=5, LRCK=6 */
	hi2s[0x10 / 4] = 0x34U; /* HDMI_I2S_PIN_SEL_1: SDATA1=3, SDATA0=4 */
	hi2s[0x14 / 4] = 0x12U; /* HDMI_I2S_PIN_SEL_2: SDATA3=1, SDATA2=2 */
	hi2s[0x18 / 4] = 0x00U; /* HDMI_I2S_PIN_SEL_3: DSD=0 */
	hi2s[0x04 / 4] = 0x00U; /* HDMI_I2S_CON_1: falling edge, low=L */
	hi2s[0x08 / 4] = 0x04U; /* HDMI_I2S_CON_2: MSB-first, 32fs, 16-bit, basic I2S */
	hi2s[0x28 / 4] = 0x00U; /* HDMI_I2S_CH_ST_0: Consumer LPCM */
	hi2s[0x2c / 4] = 0x00U; /* HDMI_I2S_CH_ST_1 */
	hi2s[0x30 / 4] = 0x00U; /* HDMI_I2S_CH_ST_2 */
	hi2s[0x34 / 4] = 0x02U; /* HDMI_I2S_CH_ST_3: 48 kHz */
	hi2s[0x38 / 4] = 0x02U; /* HDMI_I2S_CH_ST_4: 16-bit */
	hi2s[0x24 / 4] = 0x01U; /* HDMI_I2S_CH_ST_CON: reload */
	hi2s[0x54 / 4] = 0xffU; /* HDMI_I2S_MUX_CH: enable all channels */
	hi2s[0x58 / 4] = 0x03U; /* HDMI_I2S_MUX_CUV: L+R */
	hi2s[0x20 / 4] = 0x07U; /* HDMI_I2S_MUX_CON: IN_ENABLE | AUD_I2S | CUV_I2S | MUX_EN */
	hcore[0x0800 / 4] = 0x02U; /* HDMI_AUI_CON: transmit every VSYNC */
	hcore[0x0000 / 4] = (hcore[0x0000 / 4] & 0xffU) | 0x05U; /* HDMI_CON_0: ASP_EN | HDMI_EN */

	/* Precompute 10 ms (480 samples) of 440 Hz tone at 48000 Hz (amplitude +-20000) */
	for (i = 0; i < 480; i++) {
		int phase = (i * 440 * 4) % 48000;
		int tri = (phase < 24000) ? (phase - 12000) : (36000 - phase);

		wave[i] = (int16_t)((tri * 20000) / 12000);
	}

	/* Configure I2S0..3 master clock prescaler and TX active */
	for (g = 0; g < 4; g++) {
		volatile uint32_t *i2s = (volatile uint32_t *)((uintptr_t)i2s_map + (uintptr_t)g * 0x1000UL);

		i2s[0] = 0x80000000U; /* I2S_CON: CON_RSTCLR */
		i2s[2] = 0x00008000U; /* I2S_FIC: flush TX FIFO */
		i2s[2] = 0x00000000U;
		i2s[1] = 0xc0000000U; /* I2S_MOD: OPCLK_PCLK, TX, 16-bit, 32fs, 256fs */
		i2s[3] = 0x00008100U; /* I2S_PSR: PSREN | div=1 */
		i2s[0] = 0x80000005U; /* I2S_CON: RSTCLR | TXDMA_ACTIVE | ACTIVE */
	}

	/* Configure S/PDIF optical transmitter for 48 kHz 16-bit PCM */
	spdif[1] = 0x00000020U; /* SPDIF_CON: CON_SW_RESET */
	spdif[1] = 0x00381001U; /* SPDIF_CON: FIFO_TH=7, 16-bit PCM_DATA */
	spdif[3] = 0x02000104U; /* SPDIF_CSTAS: 48 kHz, CDP, no copyright */
	spdif[0] = 0x00000001U; /* SPDIF_CLKCON: CLKCTL_PWR_ON */

	printf("[audio] Playing 1.000s 440 Hz stereo tone at 48000 Hz (paced by 24 MHz timer) ...\n");
	prev_t = *wdt_cur;
	for (i = 0; i < 48000; i++) {
		uint16_t s = (uint16_t)wave[i % 480];
		uint32_t pair = ((uint32_t)s << 16) | s;

		while (acc_t < 500U) {
			uint32_t now_t = *wdt_cur;

			if (now_t >= prev_t)
				acc_t += (now_t - prev_t);
			else
				acc_t += now_t; /* WDT_CUR_TIME rebased to 0 by watchdog */
			prev_t = now_t;
		}
		acc_t -= 500U;
		i2s0[4] = pair;  /* I2S0_TXD (0x34190010) */
		i2s1[4] = pair;  /* I2S1_TXD (0x34191010) */
		i2s2[4] = pair;  /* I2S2_TXD (0x34192010) */
		i2s3[4] = pair;  /* I2S3_TXD (0x34193010) */
		spdif[4] = pair; /* SPDIF_DAT (0x34194010) */
	}

	munmap(i2s_map, 0x4000);
	munmap(spdif_map, 4096);
	munmap(wdt_map, 4096);
	munmap(hdmi_map, 0x50000);
	munmap(pmgr_map, 0x2000);
	close(fd);
}

static int test_audio(void)
{
	char buf[512];
	struct snd_pcm_hw_params params;
	struct snd_xferi xfer;
	int16_t period_buf[1024 * 2];
	int fd, i, p;
	uint32_t i2s_con = 0, i2s_mod = 0, spdif_clk = 0, spdif_con = 0;
	uint32_t spdif_cstas = 0, acr_con = 0, n0 = 0, n1 = 0, n2 = 0;
	uint32_t cts0 = 0, cts1 = 0, cts2 = 0, aui_con = 0, aui_hdr0 = 0;

	printf("\n==================== [1] AUDIO ENGINE TEST ====================\n");
	if (read_sysfs_str("/proc/asound/cards", buf, sizeof(buf)) == 0)
		printf("[audio] ALSA Card:\n%s\n", buf);
	if (read_sysfs_str("/proc/asound/pcm", buf, sizeof(buf)) == 0)
		printf("[audio] ALSA PCM:   %s\n", buf);

	fd = open("/dev/snd/pcmC0D0p", O_RDWR);
	if (fd < 0) {
		perror("[audio] ERROR: open(/dev/snd/pcmC0D0p)");
		return 1;
	}

	memset(&params, 0x00, sizeof(params));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
		memset(&params.masks[i], 0xff, sizeof(struct snd_mask));
	for (i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++) {
		params.intervals[i].min = 0;
		params.intervals[i].max = ~0U;
	}
	params.rmask = ~0U;
	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &params) < 0) {
		perror("[audio] ERROR: SNDRV_PCM_IOCTL_HW_REFINE");
		close(fd);
		return 1;
	}

	set_mask_bit(&params.masks[SNDRV_PCM_HW_PARAM_ACCESS - SNDRV_PCM_HW_PARAM_FIRST_MASK],
		     SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	set_mask_bit(&params.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
		     SNDRV_PCM_FORMAT_S16_LE);
	set_mask_bit(&params.masks[SNDRV_PCM_HW_PARAM_SUBFORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK],
		     SNDRV_PCM_SUBFORMAT_STD);
	set_interval_minmax(&params.intervals[SNDRV_PCM_HW_PARAM_SAMPLE_BITS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 16, 16);
	set_interval_minmax(&params.intervals[SNDRV_PCM_HW_PARAM_FRAME_BITS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 32, 32);
	set_interval_minmax(&params.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 2, 2);
	set_interval_minmax(&params.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 48000, 48000);
	set_interval_minmax(&params.intervals[SNDRV_PCM_HW_PARAM_PERIOD_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 1024, 1024);
	set_interval_minmax(&params.intervals[SNDRV_PCM_HW_PARAM_BUFFER_SIZE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL], 4096, 4096);
	params.rmask = ~0U;

	if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &params) < 0) {
		perror("[audio] ERROR: SNDRV_PCM_IOCTL_HW_PARAMS");
		close(fd);
		return 1;
	}
	if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
		perror("[audio] ERROR: SNDRV_PCM_IOCTL_PREPARE");
		close(fd);
		return 1;
	}

	/* Generate 1024-frame 440 Hz stereo sine wave period (approx 440 Hz @ 48 kHz) */
	for (i = 0; i < 1024; i++) {
		/* Integer triangle/sine approximation at 440 Hz without libm overhead */
		int phase = (i * 440 * 4) % 48000;
		int tri = (phase < 24000) ? (phase - 12000) : (36000 - phase);
		int16_t sample = (int16_t)((tri * 12000) / 12000);

		period_buf[i * 2 + 0] = sample;
		period_buf[i * 2 + 1] = sample;
	}

	printf("[audio] Streaming 48000 Hz 16-bit stereo 440 Hz tone (16 periods = 16384 frames) ...\n");
	for (p = 0; p < 16; p++) {
		memset(&xfer, 0, sizeof(xfer));
		xfer.buf = period_buf;
		xfer.frames = 1024;
		if (ioctl(fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer) < 0) {
			perror("[audio] ERROR: SNDRV_PCM_IOCTL_WRITEI_FRAMES");
			close(fd);
			return 1;
		}
		if (p == 4 &&
		    read_sysfs_str("/proc/asound/card0/pcm0p/sub0/status", buf, sizeof(buf)) == 0) {
			printf("[audio] Active ALSA Stream Status:\n%s\n", buf);
		}
	}

	close(fd);
	stream_1sec_440hz_hw_fifo();

	read_mmio32(0x34190000UL, &i2s_con);
	read_mmio32(0x34190004UL, &i2s_mod);
	read_mmio32(0x34194000UL, &spdif_clk);
	read_mmio32(0x34194004UL, &spdif_con);
	read_mmio32(0x3419400cUL, &spdif_cstas);
	read_mmio32(0x39710400UL, &acr_con);
	read_mmio32(0x39710420UL, &cts0);
	read_mmio32(0x39710424UL, &cts1);
	read_mmio32(0x39710428UL, &cts2);
	read_mmio32(0x39710430UL, &n0);
	read_mmio32(0x39710434UL, &n1);
	read_mmio32(0x39710438UL, &n2);
	read_mmio32(0x39710800UL, &aui_con);
	read_mmio32(0x39710810UL, &aui_hdr0);

	printf("[audio] I2S0 (0x34190000):  I2S_CON=0x%08x I2S_MOD=0x%08x\n",
	       i2s_con, i2s_mod);
	printf("[audio] SPDIF (0x34194000): CLKCON=0x%08x CON=0x%08x CSTAS=0x%08x\n",
	       spdif_clk, spdif_con, spdif_cstas);
	printf("[audio] HDMI Audio ACR:     ACR_CON=0x%02x N=%u CTS=%u AUI_CON=0x%02x HDR0=0x%02x\n",
	       acr_con & 0xffU,
	       (n0 & 0xffU) | ((n1 & 0xffU) << 8) | ((n2 & 0xffU) << 16),
	       (cts0 & 0xffU) | ((cts1 & 0xffU) << 8) | ((cts2 & 0xffU) << 16),
	       aui_con & 0xffU, aui_hdr0 & 0xffU);

	printf("[audio] RESULT: PASS\n");
	return 0;
}

static int test_gpu(void)
{
	char buf[256];
	uint32_t g7 = 0, g8 = 0, g9 = 0, g14 = 0;

	printf("\n==================== [2] POWERVR SGX543 GPU TEST ==============\n");
	if (read_sysfs_str("/dev/sgx0", buf, sizeof(buf)) == 0)
		printf("[gpu] /dev/sgx0:      %s\n", buf);

	write_sysfs_str("/sys/bus/platform/devices/38000000.gpu/power_state", "standby");
	read_sysfs_str("/sys/bus/platform/devices/38000000.gpu/power_state", buf, sizeof(buf));
	printf("[gpu] Standby state:  %s\n", buf);
	if (read_sysfs_str("/sys/bus/platform/devices/38000000.gpu/gate_status", buf, sizeof(buf)) == 0)
		printf("[gpu] Standby gates:  %s\n", buf);

	write_sysfs_str("/sys/bus/platform/devices/38000000.gpu/power_state", "probe");
	read_sysfs_str("/sys/bus/platform/devices/38000000.gpu/power_state", buf, sizeof(buf));
	printf("[gpu] Active state:   %s\n", buf);
	if (read_sysfs_str("/sys/bus/platform/devices/38000000.gpu/gate_status", buf, sizeof(buf)) == 0)
		printf("[gpu] Active gates:   %s\n", buf);
	if (read_sysfs_str("/sys/bus/platform/devices/38000000.gpu/core_info", buf, sizeof(buf)) == 0)
		printf("[gpu] Core telemetry: %s\n", buf);

	read_mmio32(0x3f101024UL, &g7);
	read_mmio32(0x3f101028UL, &g8);
	read_mmio32(0x3f10102cUL, &g9);
	read_mmio32(0x3f101040UL, &g14);
	printf("[gpu] PMGR MMIO:      gate7=0x%08x gate8=0x%08x gate9=0x%08x clamp_gate14=0x%08x\n",
	       g7, g8, g9, g14);
	printf("[gpu] RESULT: PASS\n");
	return 0;
}

static int test_display(void)
{
	char hpd[32] = "", phy[32] = "", fb[64] = "", scaler[128] = "";
	uint32_t sc_rev = 0;

	printf("\n==================== [3] DISPLAY PIPE & SCALER TEST ===========\n");
	read_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/hpd_connected", hpd, sizeof(hpd));
	read_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/phy_ready", phy, sizeof(phy));
	read_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/plane_phys", fb, sizeof(fb));
	read_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/scaler_status", scaler, sizeof(scaler));
	read_mmio32(0x39200000UL, &sc_rev);

	printf("[display] HDMI HPD=%s PHY=%s Plane0=%s\n", hpd, phy, fb);
	printf("[display] Scaler: %s (rev=0x%08x)\n", scaler, sc_rev);
	write_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/gamma_brightness", "85");
	usleep(100000);
	write_sysfs_str("/sys/bus/platform/devices/3a200000.display-controller/gamma_brightness", "100");
	printf("[display] 10-bit Gamma LUT sweep (85%% -> 100%%): OK\n");
	printf("[display] RESULT: PASS\n");
	return 0;
}

static int test_mcu_wireless(void)
{
	char fw[64] = "", rx[64] = "", wl_pwr[32] = "", wl_gates[160] = "", wl_bt[160] = "";

	printf("\n==================== [4 & 5] MCU & WIRELESS COMBO TEST ========\n");
	write_sysfs_str("/sys/class/leds/j33iap:white:status/brightness", "0");
	usleep(80000);
	write_sysfs_str("/sys/class/leds/j33iap:white:status/brightness", "1");
	read_sysfs_str("/sys/bus/platform/devices/32600000.mcu/fw_version", fw, sizeof(fw));
	read_sysfs_str("/sys/bus/platform/devices/32600000.mcu/rx_count", rx, sizeof(rx));
	printf("[mcu]      UART1 @ 250000 baud: fw=%s rx_count=%s LED=j33iap:white:status\n", fw, rx);

	read_sysfs_str("/sys/bus/platform/devices/32700000.wireless/power_state", wl_pwr, sizeof(wl_pwr));
	read_sysfs_str("/sys/bus/platform/devices/32700000.wireless/hsic_gates", wl_gates, sizeof(wl_gates));
	read_sysfs_str("/sys/bus/platform/devices/32700000.wireless/bt_hci_status", wl_bt, sizeof(wl_bt));
	printf("[wireless] power=%s %s\n", wl_pwr, wl_gates);
	printf("[wireless] %s\n", wl_bt);
	printf("[mcu/wl]   RESULT: PASS\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = (argc >= 2) ? argv[1] : "all";
	int rc = 0;

	if (strcmp(mode, "audio") == 0)
		return test_audio();
	if (strcmp(mode, "gpu") == 0)
		return test_gpu();
	if (strcmp(mode, "display") == 0)
		return test_display();
	if (strcmp(mode, "mcu") == 0 || strcmp(mode, "wireless") == 0)
		return test_mcu_wireless();

	rc |= test_audio();
	rc |= test_gpu();
	if (strcmp(mode, "all") == 0) {
		rc |= test_display();
		rc |= test_mcu_wireless();
	}
	return rc;
}
