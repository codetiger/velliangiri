/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X 720p60 HDMI + rgbout pre-kernel display initialization
 *
 * Configures:
 *   - PMGR display clock gates at 0x3f101008 + n * 4 (gates 15, 21, 20, 25, 80)
 *   - rgbout display pipe at 0x3a200000 scanning out 1280x720x32bpp ARGB
 *     (stride 5120) from reserved RAM at 0x9ec00000
 *   - Output stage at 0x39600000 (gamma LUTs + timing)
 *   - HDMI transmitter & PHY bridge at 0x39700000 for 74.25 MHz (720p@60Hz, VIC 4)
 */

#ifndef VELLIANGIRI_HDMI_720P_H
#define VELLIANGIRI_HDMI_720P_H

#include <stdint.h>

#define S5L_FB_PHYS		0x9ec00000U
#define S5L_FB_W		1280
#define S5L_FB_H		720
#define S5L_HTOTAL		1650
#define S5L_VTOTAL		750
#define S5L_HSW			40
#define S5L_HBP			110
#define S5L_HFP			220
#define S5L_VSW			5
#define S5L_VBP			5
#define S5L_VFP			20
#define S5L_BPP			4
#define S5L_STRIDE		((S5L_FB_W * S5L_BPP + 63) & ~63)
#define S5L_FB_SIZE		((S5L_STRIDE * S5L_FB_H + 0xfff) & ~0xfff)

/* PMGR clock gates: 0x3f101008 + n * 4 */
#define PMGR_GATE(n)		(0x3f101008U + (n) * 4U)
#define GATE_HPERF_RT_A		15
#define GATE_HPERF_RT_B		21
#define GATE_RGBOUT_A		20
#define GATE_RGBOUT_B		25
#define GATE_HDMI		80

/* rgbout display pipe at 0x3a200000 */
#define RGB_BASE		0x3a200000U
#define RGB_SIZE		(RGB_BASE + 0x1030)
#define RGB_CTL			(RGB_BASE + 0x1038)
#define RGB_CFG			(RGB_BASE + 0x104c)
#define RGB_105C		(RGB_BASE + 0x105c)
#define RGB_2040		(RGB_BASE + 0x2040)
#define RGB_205C		(RGB_BASE + 0x205c)
#define RGB_2060		(RGB_BASE + 0x2060)
#define RGB_2064		(RGB_BASE + 0x2064)
#define RGB_2068		(RGB_BASE + 0x2068)
#define RGB_2078		(RGB_BASE + 0x2078)
#define RGB_2088		(RGB_BASE + 0x2088)
#define RGB_PLANE(n)		(RGB_BASE + 0x4000 + (n) * 0x1000)

/* rgbout output stage at 0x39600000 */
#define OUT_BASE		0x39600000U
#define OUT_START		(OUT_BASE + 0x00)
#define OUT_CFG			(OUT_BASE + 0x04)
#define OUT_RESET		(OUT_BASE + 0x08)
#define OUT_1C			(OUT_BASE + 0x1c)
#define OUT_24			(OUT_BASE + 0x24)
#define OUT_VSIZE		(OUT_BASE + 0x34)
#define OUT_HSIZE		(OUT_BASE + 0x40)
#define OUT_GAMMA_EN		(OUT_BASE + 0x4c)
#define OUT_GAMMA_IDX		(OUT_BASE + 0x54)
#define OUT_GAMMA_VAL		(OUT_BASE + 0x58)
#define OUT_VSYNC		(OUT_BASE + 0x6c)

/* HDMI transmitter at 0x39700000 */
#define HDMI_BASE		0x39700000U
#define HCTRL(x)		(HDMI_BASE + 0x00000 + (x))
#define HCORE(x)		(HDMI_BASE + 0x10000 + (x))
#define HTG(x)			(HDMI_BASE + 0x50000 + (x))
#define HPHYB(x)		(HDMI_BASE + 0x80000 + (x))

#define HDMI_HPD_STATUS		HCTRL(0x0c)
#define HDMI_PHY_STATUS_0	HCTRL(0x20)
#define HDMI_PHY_CON_0		HCTRL(0x30)
#define HDMI_PHY_RSTOUT		HCTRL(0x74)
#define HDMI_CON_0		HCORE(0x00)
#define HDMI_CON_1		HCORE(0x04)
#define HDMI_CON_2		HCORE(0x08)
#define HDMI_MODE_SEL		HCORE(0x40)
#define HDMI_GCP_CON		HCORE(0x200)
#define HDMI_GCP_BYTE1		HCORE(0x210)
#define HDMI_GCP_BYTE2		HCORE(0x214)
#define HDMI_AVI_CON		HCORE(0x700)
#define HDMI_AVI_HEADER0	HCORE(0x710)
#define HDMI_AVI_CHECK_SUM	HCORE(0x71c)
#define HDMI_AVI_BYTE(n)	HCORE(0x720 + 4 * ((n) - 1))
#define HDMI_DC_CONTROL		HCORE(0xd00)
#define HDMI_VIDEO_PATTERN_GEN	HCORE(0xd04)
#define HDMI_TG_CMD		HTG(0x00)
#define HPHY_BUSY		HPHYB(0x84)
#define HPHY_CFG(word)		HPHYB(0x88 + 4 * (word))

static inline uint32_t io_rd(uint32_t pa)
{
	return *(volatile uint32_t *)pa;
}

static inline void io_wr(uint32_t pa, uint32_t v)
{
	*(volatile uint32_t *)pa = v;
}

static void io_udelay(uint32_t us)
{
	uint32_t t0 = *(volatile uint32_t *)0x3f103020U;

	while ((uint32_t)(*(volatile uint32_t *)0x3f103020U - t0) < us * 24U)
		;
}

static void hdmi_gate(int n, int on)
{
	uint32_t reg = PMGR_GATE(n), v = io_rd(reg);
	int i;

	io_wr(reg, (v & ~0x10fU) | (on ? 0xfU : 0U));
	for (i = 0; i < 200000; i++) {
		v = io_rd(reg);
		if (!((v ^ (v >> 4)) & 0xfU))
			break;
	}
}

static void hdmi_reset_pulse(void)
{
	uint32_t reg = PMGR_GATE(GATE_HDMI);

	io_wr(reg, io_rd(reg) | 0x80000000U);
	io_udelay(10);
	io_wr(reg, io_rd(reg) & ~0x80000000U);
}

static void hdmi_rgbout_init(void)
{
	uint32_t v;

	v = io_rd(RGB_CFG) | 0x10U;
	io_wr(RGB_CFG, v);
	v = (io_rd(RGB_CFG) & ~0x700U) | 0x100U;
	io_wr(RGB_CFG, v);
	v = (io_rd(RGB_CFG) & ~0x07ff0000U) | 0x04000000U;
	io_wr(RGB_CFG, v);
	io_wr(RGB_SIZE, S5L_FB_H | (S5L_FB_W << 16));
	io_wr(RGB_205C, 0x01c001f0U);
	io_wr(RGB_2060, 0x00000100U);
	io_wr(RGB_105C, 0x00400001U);
	io_wr(RGB_2068, 0x1000U);
	io_wr(RGB_2078, 0x1000U);
	io_wr(RGB_2088, 0x1000U);
	io_wr(RGB_2064, 0xbff00000U);
}

static void hdmi_out_init(void)
{
	int i;

	io_wr(OUT_RESET, 1);
	for (i = 0; i < 200000 && (io_rd(OUT_RESET) & 1); i++)
		;
	io_wr(OUT_1C, 0);
	io_wr(OUT_24, 0);
	io_wr(OUT_HSIZE, S5L_FB_W - 1);
	io_wr(OUT_VSIZE, S5L_FB_H - 1);
	io_wr(OUT_CFG, 0x23);
	io_wr(OUT_VSYNC, S5L_VFP + S5L_VSW - 2);
}

static void hdmi_plane_init(uint32_t fb_phys)
{
	uint32_t p = RGB_PLANE(0), wh = S5L_FB_H | (S5L_FB_W << 16);

	io_wr(p + 0x40, (0 << 8) | 1);
	io_wr(p + 0x44, fb_phys);
	io_wr(p + 0x48, (io_rd(p + 0x48) & 0x3fU) | (S5L_STRIDE & ~0x3fU));
	io_wr(p + 0x4c, 0);
	io_wr(p + 0x50, wh);
	io_wr(p + 0x54, 0);
	io_wr(p + 0x58, wh);
	io_wr(p + 0x5c, 1);
	io_wr(p + 0x90, 0);
	io_wr(p + 0x80, 0);
	io_wr(p + 0x84, 0);
	io_wr(p + 0x88, 0x100000U);
	io_wr(p + 0x8c, 0x100000U);
	io_wr(p + 0x60, 0);
	io_wr(p + 0x70, wh);
	io_wr(RGB_2040, 0xffff0202U);
}

static void hdmi_rgbout_enable(void)
{
	io_wr(RGB_CTL, io_rd(RGB_CTL) | 0x100U);
}

static void hdmi_gamma_identity(void)
{
	uint32_t base = io_rd(OUT_GAMMA_IDX) & 0xfffe0f00U;
	int c, i;

	io_wr(OUT_GAMMA_IDX, base);
	for (c = 0; c < 3; c++)
		for (i = 0; i < 256; i++) {
			io_wr(OUT_GAMMA_IDX, base | i | (0x14000U + c * 0x1000U));
			io_wr(OUT_GAMMA_VAL, ((i * 1023 + 127) / 255) & 0xfffffU);
		}
	io_wr(OUT_GAMMA_EN, 9);
}

static void hdmi_out_start(void)
{
	io_wr(OUT_START, 9);
}

static void hdmi_hw(uint32_t reg, uint32_t v, int nbytes)
{
	while (nbytes-- > 0) {
		io_wr(reg, v & 0xffU);
		reg += 4;
		v >>= 8;
	}
}

static void hdmi_hmask(uint32_t reg, uint32_t clear, uint32_t set)
{
	io_wr(reg, ((io_rd(reg) & 0xffU) & ~clear) | set);
}

static int hdmi_phy_busy(void)
{
	int i;

	for (i = 0; i < 20; i++) {
		if (!(io_rd(HPHY_BUSY) & 1))
			return 0;
		io_udelay(1);
	}
	return 1;
}

static int hdmi_phy_write(int reg, uint32_t val)
{
	uint32_t word = HPHY_CFG(reg >> 2), v;
	int sh = (reg & 3) * 8;

	if (hdmi_phy_busy())
		return -1;
	v = io_rd(word);
	io_wr(word, (v & ~(0xffU << sh)) | ((val & 0xffU) << sh));
	hdmi_phy_busy();
	return 0;
}

static uint32_t hdmi_phy_byte(int reg)
{
	return (io_rd(HPHY_CFG(reg >> 2)) >> ((reg & 3) * 8)) & 0xffU;
}

static void hdmi_phy_power(int on)
{
	uint32_t r28 = hdmi_phy_byte(28) >> 1;

	hdmi_phy_write(29, on ? (r28 & 0x6bU) : ((r28 & 0xffU) | 0x94U));
}

static void hdmi_phy_power_down(void)
{
	hdmi_phy_write(29, (hdmi_phy_byte(28) >> 1) | 0xb7U);
}

static int hdmi_phy_wait_ready(void)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (io_rd(HDMI_PHY_STATUS_0) & 1)
			return 0;
		io_udelay(100);
	}
	return -1;
}

static int hdmi_controller_start(void)
{
	int i;

	hdmi_gate(GATE_HDMI, 1);
	hdmi_reset_pulse();
	io_wr(HDMI_PHY_CON_0, io_rd(HDMI_PHY_CON_0) & ~1U);
	io_wr(HDMI_PHY_RSTOUT, io_rd(HDMI_PHY_RSTOUT) | 1U);
	io_udelay(1000);
	io_wr(HDMI_PHY_RSTOUT, io_rd(HDMI_PHY_RSTOUT) & ~1U);
	io_wr(HDMI_CON_0, io_rd(HDMI_CON_0) & ~0x10U);

	io_wr(HPHYB(0x2c), 1);
	for (i = 0; i < 1000; i++) {
		io_wr(HPHYB(0x80), 0x70);
		if (io_rd(HPHYB(0x80)) == 0x70)
			break;
	}
	io_wr(HPHYB(0x20), 0x3f00U);
	io_wr(HPHYB(0x00), 0x281U);
	io_wr(HPHYB(0x18), 0);
	io_wr(HPHYB(0x30), 0);
	io_wr(HPHYB(0x34), 0xffffffffU);
	io_wr(HPHYB(0x04), 0xd0U);
	hdmi_phy_power_down();

	return io_rd(HDMI_HPD_STATUS) & 0xffU;
}

static void hdmi_timing_init(void)
{
	hdmi_hw(HCORE(0x0a0), S5L_HTOTAL - S5L_FB_W, 2);
	hdmi_hw(HCORE(0x0b8), S5L_VTOTAL - S5L_FB_H, 2);
	hdmi_hw(HCORE(0x0b0), S5L_VTOTAL, 2);
	hdmi_hw(HCORE(0x0c0), S5L_VTOTAL, 2);
	hdmi_hw(HCORE(0x0c8), S5L_HTOTAL, 2);
	hdmi_hw(HCORE(0x0e8), 0, 1);
	hdmi_hw(HCORE(0x110), S5L_VTOTAL + (S5L_VTOTAL - S5L_FB_H) + 1, 2);
	hdmi_hw(HCORE(0x118), S5L_VTOTAL, 2);
	hdmi_hw(HCORE(0x0e0), 0, 1);
	hdmi_hw(HCORE(0x120), S5L_HFP - 2, 2);
	hdmi_hw(HCORE(0x128), S5L_HFP - 2 + S5L_HSW, 2);
	hdmi_hw(HCORE(0x0e4), 0, 1);
	hdmi_hw(HCORE(0x138), S5L_VFP, 2);
	hdmi_hw(HCORE(0x130), S5L_VFP + S5L_VSW, 2);
	hdmi_hw(HCORE(0x148), S5L_VFP + S5L_VTOTAL, 2);
	hdmi_hw(HCORE(0x140), S5L_VFP + S5L_VSW + S5L_VTOTAL, 2);
	hdmi_hw(HCORE(0x158), S5L_HFP + S5L_HTOTAL / 2, 2);
	hdmi_hw(HCORE(0x150), S5L_HFP + S5L_HTOTAL / 2, 2);

	hdmi_hw(HTG(0x00), 0, 1);
	hdmi_hw(HTG(0x18), S5L_HTOTAL, 2);
	hdmi_hw(HTG(0x20), S5L_HTOTAL - S5L_FB_W, 2);
	hdmi_hw(HTG(0x28), S5L_FB_W, 2);
	hdmi_hw(HTG(0x30), S5L_VTOTAL, 2);
	hdmi_hw(HTG(0x38), 1, 2);
	hdmi_hw(HTG(0x40), S5L_VTOTAL + 1, 2);
	hdmi_hw(HTG(0x48), S5L_VTOTAL - S5L_FB_H, 2);
	hdmi_hw(HTG(0x50), S5L_FB_H, 2);
	hdmi_hw(HTG(0x58), S5L_VTOTAL + 1, 2);
	hdmi_hw(HTG(0x60), S5L_VTOTAL - S5L_FB_H + S5L_VTOTAL + 1, 2);
	hdmi_hw(HTG(0x78), 1, 2);
	hdmi_hw(HTG(0x80), S5L_VTOTAL + 1, 2);
	hdmi_hw(HTG(0x88), 1, 2);
	hdmi_hw(HTG(0x90), S5L_VTOTAL + 1, 2);
}

static void hdmi_color_init(void)
{
	hdmi_hmask(HDMI_CON_0, 0x08, 0);
	hdmi_hw(HCORE(0xd520), 0, 1);
	hdmi_hw(HCORE(0xd528), 0, 1);
	hdmi_hw(HCORE(0xd530), 0, 1);
	hdmi_hw(HCORE(0xd524), 0, 1);
	hdmi_hw(HCORE(0xd52c), 0, 1);
	hdmi_hw(HCORE(0xd534), 0, 1);
	hdmi_hmask(HDMI_CON_1, 0x60, 0);
	hdmi_hmask(HDMI_DC_CONTROL, 0x03, 0);
	hdmi_hmask(HDMI_VIDEO_PATTERN_GEN, 0x01, 0);
}

static const uint8_t hdmi_avi_720p[13] = {
	0x10, 0x68, 0x00, 0x04, 0x00, 0, 0, 0, 0, 0, 0, 0, 0
};

static const uint8_t hdmi_phy_74250[31] = {
	0xd1, 0x3e, 0x35, 0x40, 0x20, 0xf8, 0x04, 0x81, 0xa0, 0x73, 0xd9, 0x45,
	0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02, 0x22, 0x44, 0x82, 0x54,
	0xa5, 0x24, 0x01, 0x00, 0x00, 0x01, 0x00,
};

static int hdmi_start_video(const uint8_t *phy_cfg, int hdmi_mode)
{
	uint8_t img[32];
	int i;

	hdmi_timing_init();
	hdmi_color_init();
	hdmi_hmask(HDMI_MODE_SEL, 0x03, hdmi_mode ? 2 : 1);
	hdmi_hmask(HDMI_CON_2, 0x22, 0);

	img[0] = hdmi_phy_byte(0);
	for (i = 0; i < 30; i++)
		img[1 + i] = phy_cfg[i];
	img[7] = ((img[4] | (img[5] << 8) | (img[6] << 16) | (img[7] << 24)) >> 3) & 0xffU;
	img[31] = 0;

	if (hdmi_phy_write(31, 0x00))
		return -1;
	for (i = 0; i < 32; i++)
		if (hdmi_phy_write(i, img[i]))
			return -1;
	hdmi_phy_write(31, 0x80);
	if (hdmi_phy_wait_ready())
		return -1;
	hdmi_phy_power(0);
	hdmi_hmask(HDMI_TG_CMD, 0x01, 0x01);

	if (!hdmi_mode) {
		hdmi_hmask(HDMI_CON_0, 0, 0x01);
		io_udelay(100);
		hdmi_phy_power(1);
		hdmi_phy_wait_ready();
	} else {
		hdmi_hw(HDMI_AVI_HEADER0, 0x82, 1);
		hdmi_hw(HCORE(0x714), 0x02, 1);
		hdmi_hw(HCORE(0x718), 0x0d, 1);
		hdmi_hw(HDMI_AVI_CHECK_SUM, 0xf3, 1);
		for (i = 0; i < 13; i++)
			hdmi_hw(HDMI_AVI_BYTE(i + 1), hdmi_avi_720p[i], 1);
		hdmi_hmask(HDMI_AVI_CON, 0x03, 0x02);
		hdmi_hw(HDMI_GCP_BYTE1, 0, 3);
		hdmi_hmask(HDMI_GCP_BYTE2, 0x07, 0);
		hdmi_hmask(HDMI_GCP_CON, 0, 0x0c);
		hdmi_hmask(HDMI_GCP_CON, 0x03, 0x02);
		hdmi_hmask(HDMI_GCP_BYTE1, 0x11, 0x01);
		io_udelay(16668);
		hdmi_hmask(HDMI_CON_0, 0, 0x01);
		io_udelay(100);
		hdmi_phy_power(1);
		hdmi_phy_wait_ready();
		io_udelay(60);
		hdmi_hmask(HDMI_GCP_BYTE1, 0x11, 0x10);
	}
	hdmi_color_init();
	hdmi_hmask(HDMI_GCP_BYTE1, 0x11, 0x10);
	return 0;
}

static void hdmi_draw_pattern(uint32_t fb)
{
	static const uint32_t bars[8] = {
		0xffffffU, 0xffff00U, 0x00ffffU, 0x00ff00U,
		0xff00ffU, 0xff0000U, 0x0000ffU, 0x000000U
	};
	int x, y;

	for (y = 0; y < S5L_FB_H; y++) {
		volatile uint32_t *row = (volatile uint32_t *)(fb + y * S5L_STRIDE);

		for (x = 0; x < S5L_FB_W; x++) {
			uint32_t c = (y < S5L_FB_H * 3 / 4) ?
				     bars[x * 8 / S5L_FB_W] :
				     ((x * 255 / S5L_FB_W) * 0x010101U);
			if (x < 2 || y < 2 || x >= S5L_FB_W - 2 || y >= S5L_FB_H - 2)
				c = 0xffffffU;
			row[x] = c;
		}
	}
}

static int hdmi_720p_init(void)
{
	hdmi_draw_pattern(S5L_FB_PHYS);
	hdmi_gate(GATE_HPERF_RT_A, 1);
	hdmi_gate(GATE_HPERF_RT_B, 1);
	hdmi_gate(GATE_RGBOUT_A, 1);
	hdmi_gate(GATE_RGBOUT_B, 1);
	hdmi_rgbout_init();
	hdmi_out_init();
	hdmi_plane_init(S5L_FB_PHYS);
	hdmi_rgbout_enable();
	(void)hdmi_controller_start();
	hdmi_start_video(hdmi_phy_74250, 1);
	hdmi_out_start();
	hdmi_gamma_identity();
	return 0;
}

#endif /* VELLIANGIRI_HDMI_720P_H */
