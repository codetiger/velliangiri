/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X 720p60 HDMI + rgbout pre-kernel display initialization
 *
 * Configures:
 *   - PMGR display clock configs (0x3f100060..0x3f1000bc) and gates
 *     (15, 20, 21, 25, 80)
 *   - rgbout display pipe at 0x3a200000 scanning out 1280x720x32bpp ARGB
 *     from reserved RAM at 0x9ec00000
 *   - Output stage at 0x39600000
 *   - HDMI transmitter & PHY at 0x39700000 for 74.25 MHz (720p@60Hz)
 */

#ifndef VELLIANGIRI_HDMI_720P_H
#define VELLIANGIRI_HDMI_720P_H

#include <stdint.h>

#define S5L_FB_PHYS	0x9ec00000U
#define S5L_FB_W	1280U
#define S5L_FB_H	720U
#define S5L_FB_STRIDE	(S5L_FB_W * 4U)

#define S5L_PMGR_BASE	0x3f100000U
#define S5L_PIPE_BASE	0x3a200000U
#define S5L_OUT_BASE	0x39600000U
#define S5L_HDMI_BASE	0x39700000U
#define S5L_WDT_CUR	0x3f103020U

static inline uint32_t hdmi_r32(uint32_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void hdmi_w32(uint32_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static inline uint8_t hdmi_r8(uint32_t a)
{
	return *(volatile uint8_t *)a;
}

static inline void hdmi_w8(uint32_t a, uint8_t v)
{
	*(volatile uint8_t *)a = v;
}

static void hdmi_udelay(uint32_t us)
{
	uint32_t ticks = us * 24U;
	uint32_t t0 = hdmi_r32(S5L_WDT_CUR);

	if (hdmi_r32(S5L_WDT_CUR) == t0) {
		volatile uint32_t n = us * 200U;

		while (n--)
			__asm__ volatile("nop");
		return;
	}
	while ((hdmi_r32(S5L_WDT_CUR) - t0) < ticks)
		;
}

static void hdmi_pmgr_gate(unsigned int gate)
{
	uint32_t a = S5L_PMGR_BASE + 0x1c0U + gate * 4U;

	hdmi_w32(a, hdmi_r32(a) | 0xfU);
	while (hdmi_r32(a) & (1U << 31))
		;
}

static const uint8_t hdmi_phy_74250[31] = {
	0x99, 0x05, 0x00, 0xa8, 0x90, 0xd4, 0x90, 0x62, 0x40, 0x04,
	0x00, 0x01, 0x00, 0x34, 0x80, 0x03, 0x40, 0x20, 0x21, 0x04,
	0x80, 0x46, 0x54, 0xaf, 0x65, 0x00, 0x10, 0x84, 0x00, 0x00,
	0x01,
};

static int hdmi_720p_init(void)
{
	uint32_t *fb = (uint32_t *)S5L_FB_PHYS;
	unsigned int i, x, y;
	uint32_t v;

	/* 1. Configure PMGR display clocks and enable gates 15, 20, 21, 25, 80 */
	hdmi_w32(S5L_PMGR_BASE + 0x0bc, 0x80000001U);
	hdmi_w32(S5L_PMGR_BASE + 0x07c, 0x80100002U);
	hdmi_w32(S5L_PMGR_BASE + 0x064, 0x80100002U);
	hdmi_w32(S5L_PMGR_BASE + 0x060, 0x80100006U);
	while ((hdmi_r32(S5L_PMGR_BASE + 0x0bc) |
		hdmi_r32(S5L_PMGR_BASE + 0x07c) |
		hdmi_r32(S5L_PMGR_BASE + 0x064) |
		hdmi_r32(S5L_PMGR_BASE + 0x060)) & (1U << 31))
		;

	hdmi_pmgr_gate(15);
	hdmi_pmgr_gate(21);
	hdmi_pmgr_gate(20);
	hdmi_pmgr_gate(25);
	hdmi_pmgr_gate(80);

	/* 2. Program rgbout pipe @ 0x3a200000 for 1280x720 */
	hdmi_w32(S5L_PIPE_BASE + 0x0000, 1);
	hdmi_udelay(100);
	hdmi_w32(S5L_PIPE_BASE + 0x0008, (720U << 16) | 1280U);
	hdmi_w32(S5L_PIPE_BASE + 0x0024, (1U << 31) | (256U << 16) | 128U);
	hdmi_w32(S5L_PIPE_BASE + 0x001c, 0x07ff0100U);
	hdmi_w32(S5L_PIPE_BASE + 0x0020, 0x00f00000U);
	hdmi_w32(S5L_PIPE_BASE + 0x0030, 0x10U);
	hdmi_w32(S5L_PIPE_BASE + 0x0004,
		 (hdmi_r32(S5L_PIPE_BASE + 0x0004) & ~0x3ff0U) |
		 (1U << 24) | (1U << 20) | (1U << 16) | 0x70U);

	hdmi_w32(S5L_PIPE_BASE + 0x1008, 0x17U);
	hdmi_w32(S5L_PIPE_BASE + 0x104c, S5L_FB_STRIDE);
	hdmi_w32(S5L_PIPE_BASE + 0x1054, (720U << 16) | 1280U);
	hdmi_w32(S5L_PIPE_BASE + 0x1058, 0);
	hdmi_w32(S5L_PIPE_BASE + 0x105c, (720U << 16) | 1280U);
	hdmi_w32(S5L_PIPE_BASE + 0x1038, S5L_FB_PHYS);
	hdmi_w32(S5L_PIPE_BASE + 0x1004, 0x101U);

	/* 3. Program output timing generator @ 0x39600000 for 720p60 (1650x750) */
	hdmi_w32(S5L_OUT_BASE + 0x04, 0x02f00089U);
	hdmi_w32(S5L_OUT_BASE + 0x08, (1280U << 16) | (1650U - 1280U));
	hdmi_w32(S5L_OUT_BASE + 0x0c, (110U << 16) | 40U);
	hdmi_w32(S5L_OUT_BASE + 0x10, (720U << 16) | (750U - 720U));
	hdmi_w32(S5L_OUT_BASE + 0x14, (5U << 16) | 5U);
	hdmi_w32(S5L_OUT_BASE + 0x00, 0x80000002U);

	/* 4. Program HDMI transmitter & PHY @ 0x39700000 */
	hdmi_w32(S5L_HDMI_BASE + 0x00, hdmi_r32(S5L_HDMI_BASE + 0x00) | 1U);
	hdmi_udelay(20);
	hdmi_w32(S5L_HDMI_BASE + 0x04, hdmi_r32(S5L_HDMI_BASE + 0x04) | 0x13U);

	hdmi_w8(S5L_HDMI_BASE + 0x201f, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1000, 0x00);
	for (i = 0; i < 31; i++)
		hdmi_w8(S5L_HDMI_BASE + 0x2000 + i, hdmi_phy_74250[i]);
	hdmi_udelay(1000);

	for (i = 0; i < 500; i++) {
		if (hdmi_r8(S5L_HDMI_BASE + 0x1014) & 0x01)
			break;
		hdmi_udelay(100);
	}
	if (!(hdmi_r8(S5L_HDMI_BASE + 0x1014) & 0x01))
		return -1;

	hdmi_w32(S5L_HDMI_BASE + 0x04, hdmi_r32(S5L_HDMI_BASE + 0x04) & ~0x10U);
	hdmi_w8(S5L_HDMI_BASE + 0x1010, hdmi_r8(S5L_HDMI_BASE + 0x1010) & ~0x08U);
	hdmi_w32(S5L_HDMI_BASE + 0x04, hdmi_r32(S5L_HDMI_BASE + 0x04) & ~0x02U);
	hdmi_udelay(20);

	/* 1650 = 0x0672, 750 = 0x02ee, Hblank 370 = 0x172, Vblank 30 = 0x1e */
	hdmi_w8(S5L_HDMI_BASE + 0x110c, 0x72);
	hdmi_w8(S5L_HDMI_BASE + 0x110d, 0x06);
	hdmi_w8(S5L_HDMI_BASE + 0x1112, 0xee);
	hdmi_w8(S5L_HDMI_BASE + 0x1113, 0x02);
	hdmi_w8(S5L_HDMI_BASE + 0x110a, 0x72);
	hdmi_w8(S5L_HDMI_BASE + 0x110b, 0x01);
	hdmi_w8(S5L_HDMI_BASE + 0x1119, 0x1e);
	hdmi_w8(S5L_HDMI_BASE + 0x111a, 0xd2);
	hdmi_w8(S5L_HDMI_BASE + 0x111b, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1114, 0x96);
	hdmi_w8(S5L_HDMI_BASE + 0x1115, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1116, 0x28);
	hdmi_w8(S5L_HDMI_BASE + 0x1117, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x111c, 0x0a);
	hdmi_w8(S5L_HDMI_BASE + 0x111d, 0x05);
	hdmi_w8(S5L_HDMI_BASE + 0x110e, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x110f, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1110, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1111, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1125, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1126, 0x01);

	hdmi_w8(S5L_HDMI_BASE + 0x1040, 0x02);
	hdmi_w8(S5L_HDMI_BASE + 0x1041, 0x00);
	hdmi_w8(S5L_HDMI_BASE + 0x1044, 0x02);
	hdmi_w8(S5L_HDMI_BASE + 0x1050, 0x00);

	v = hdmi_r32(S5L_HDMI_BASE + 0x14);
	hdmi_w32(S5L_HDMI_BASE + 0x14, (v & ~0x3U) | 0x1U);
	hdmi_w8(S5L_HDMI_BASE + 0x1100, hdmi_r8(S5L_HDMI_BASE + 0x1100) | 0x01U);

	/* 5. Clear framebuffer to dark slate and draw top status bar */
	for (y = 0; y < S5L_FB_H; y++) {
		uint32_t color = (y < 8U) ? 0xff22c55eU : 0xff0f172aU;
		uint32_t *row = fb + y * S5L_FB_W;

		for (x = 0; x < S5L_FB_W; x++)
			row[x] = color;
	}
	__asm__ volatile("dsb sy" ::: "memory");

	/* 6. Enable pipe DMA and output scanout */
	hdmi_w32(S5L_PIPE_BASE + 0x1000, hdmi_r32(S5L_PIPE_BASE + 0x1000) | 3U);
	hdmi_w32(S5L_PIPE_BASE + 0x0000, hdmi_r32(S5L_PIPE_BASE + 0x0000) | (1U << 20));
	hdmi_w32(S5L_OUT_BASE + 0x00, hdmi_r32(S5L_OUT_BASE + 0x00) | 1U);
	return 0;
}

#endif /* VELLIANGIRI_HDMI_720P_H */
