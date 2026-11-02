// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X display: rgbout pipe, output stage and HDMI 1.4 transmitter
 *
 * Hardware blocks:
 *   - rgbout @ 0x3a200000: display pipe and plane 0 (ARGB8888 scanout DMA)
 *   - out    @ 0x39600000: output stage, 3 x 256 10-bit gamma LUTs
 *   - hdmi   @ 0x39700000: HDMI transmitter, timing generator, PHY bridge
 *   - scaler @ 0x39200000: memory-to-memory scaler (clock only)
 *
 * The pipe has no clock of its own: it runs from the HDMI PHY, whose PLL is
 * set from a table of 30-byte configurations indexed by pixel clock. Modes
 * come from the sink's EDID, read over the board's DDC bus, and are kept
 * when a PHY configuration lies within 0.5 % of their pixel clock (the
 * table tops out at 162 MHz, so 1080p60 is the largest mode).
 *
 * HDMI timing registers, per axis with CEA porches:
 *   +0x0a0 H blank, +0x0b0/+0x0c0 V total, +0x0b8 V blank, +0x0c8 H total,
 *   +0x110 V total + V blank + 1, +0x118 V total,
 *   +0x120/+0x128 H sync start/end = H front porch - 2 (+ sync width),
 *   +0x138/+0x130 V sync start/end = V front porch (+ sync width), and the
 *   same plus V total at +0x148/+0x140 for the second field,
 *   +0x0e0/+0x0e4 H/V sync active low, +0x0e8 interlaced;
 *   the timing generator at +0x50000 holds totals, blanks and actives.
 *
 * The audio bridge (ACR, I2S receiver, audio InfoFrame) is exported to the
 * ALSA driver through <linux/s5l8947x-display.h>.
 */

#include <linux/aperture.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

/* rgbout (0x3a200000) */
#define RGB_SIZE		0x1030
#define RGB_CTL			0x1038
#define RGB_CFG			0x104c
#define RGB_105C		0x105c
#define RGB_COMMIT		0x2040
#define RGB_205C		0x205c
#define RGB_2060		0x2060
#define RGB_2064		0x2064
#define RGB_2068		0x2068
#define RGB_2078		0x2078
#define RGB_2088		0x2088
/* Interrupt registers decoded from the stock driver; see irq_probe below */
#define RGB_INT_STATUS		0x1014
#define RGB_PLANE0		0x4000
#define  PLANE_CTL		0x40
#define  PLANE_ADDR		0x44
#define  PLANE_STRIDE		0x48
#define RGB_COMMIT_VAL		0xffff0202U

/* Output stage (0x39600000) */
#define OUT_START		0x00
#define OUT_CFG			0x04
#define OUT_RESET		0x08
#define OUT_INT_EN		0x10
#define OUT_INT_STATUS		0x14
#define OUT_1C			0x1c
#define OUT_24			0x24
#define OUT_VSIZE		0x34
#define OUT_HSIZE		0x40
#define OUT_GAMMA_EN		0x4c
#define OUT_GAMMA_IDX		0x54
#define OUT_GAMMA_VAL		0x58
#define OUT_VSYNC		0x6c

/* HDMI (0x39700000): control, core (+0x10000), TG (+0x50000), PHY (+0x80000) */
#define HDMI_HPD_STATUS		0x0000c
#define HDMI_PHY_STATUS_0	0x00020
#define HDMI_PHY_CON_0		0x00030
#define HDMI_AUDIO_CLKSEL	0x00070
#define HDMI_PHY_RSTOUT		0x00074
#define HDMI_CON_0		0x10000
#define HDMI_CON_1		0x10004
#define HDMI_CON_2		0x10008
#define HDMI_MODE_SEL		0x10040
#define HDMI_GCP_CON		0x10200
#define HDMI_GCP_BYTE1		0x10210
#define HDMI_GCP_BYTE2		0x10214
#define HDMI_AVI_CON		0x10700
#define HDMI_AVI_HEADER0	0x10710
#define HDMI_AVI_CHECK_SUM	0x1071c
#define HDMI_AVI_BYTE1		0x10720
#define HDMI_DC_CONTROL		0x10d00
#define HDMI_VIDEO_PATTERN_GEN	0x10d04
#define HDMI_CORE(x)		(0x10000 + (x))
#define HDMI_TG(x)		(0x50000 + (x))
#define HDMI_PHYB(x)		(0x80000 + (x))
#define HDMI_PHY_BUSY		HDMI_PHYB(0x84)
#define HDMI_PHY_CFG(w)		HDMI_PHYB(0x88 + 4 * (w))

/* Audio bridge */
#define HDMI_ACR_CON		0x10400
#define HDMI_ACR_CTS0		0x10420
#define HDMI_ACR_N0		0x10430
#define HDMI_ASP_CON		0x10300
#define HDMI_AUI_CON		0x10800
#define HDMI_AUI_HEADER0	0x10810
#define HDMI_AUI_HEADER1	0x10814
#define HDMI_AUI_HEADER2	0x10818
#define HDMI_AUI_CHECK_SUM	0x1081c
#define HDMI_AUI_BYTE1		0x10820
#define HDMI_SPDIF_RX_CLK	0x30000
#define HDMI_SPDIF_RX_OP	0x30004
#define HDMI_I2S_CLK_CON	0x40000
#define HDMI_I2S_CON_1		0x40004
#define HDMI_I2S_CON_2		0x40008
#define HDMI_I2S_PIN_SEL_0	0x4000c
#define HDMI_I2S_PIN_SEL_1	0x40010
#define HDMI_I2S_PIN_SEL_2	0x40014
#define HDMI_I2S_PIN_SEL_3	0x40018
#define HDMI_I2S_DSD_CON	0x4001c
#define HDMI_I2S_MUX_CON	0x40020
#define HDMI_I2S_CH_ST_CON	0x40024
#define HDMI_I2S_CH_ST_0	0x40028
#define HDMI_I2S_CH_ST_1	0x4002c
#define HDMI_I2S_CH_ST_2	0x40030
#define HDMI_I2S_CH_ST_3	0x40034
#define HDMI_I2S_CH_ST_4	0x40038
#define HDMI_I2S_MUX_CH		0x40054
#define HDMI_I2S_MUX_CUV	0x40058

#define S5L_MAX_W		4096
#define S5L_MAX_H		2048

#define S5L_IRQ_PROBE_MS	1000
#define S5L_IRQ_PROBE_MAX	4096

/*
 * One-shot interrupt probe. The display node carries three interrupts, but
 * no frame/vblank source is known: the stock driver only enables the
 * output-stage error bits and the display-pipe interrupts are all error
 * conditions. On j33iap all three lines stay silent while the display runs
 * (measured 2026-11-01, with the output-stage enable at 1 and its status at
 * 0), so the probe is kept to look for error events and for any board whose
 * wiring differs. A line that keeps firing is disabled after
 * S5L_IRQ_PROBE_MAX hits, so an unacknowledged level interrupt cannot lock
 * up the CPU.
 */
struct s5l_irq_probe {
	struct mutex lock;
	int irq;
	atomic_t hits;
	u64 first_ns;
	u64 last_ns;
	u32 int_en;
	u32 int_status;
	u32 rgbout_status;
	u32 hpd;
	bool storm;
	bool valid;
};

/*
 * Highest pixel clock with a clean HDMI link on this board. 1280x1024@60
 * (108 MHz) is clean; at 121.75 MHz the sink sees bit errors and from
 * 146.25 MHz it does not lock, although the PHY reports lock with the
 * stock configuration. 1080p is therefore offered at 30 Hz and below.
 */
#define S5L_MAX_PIXCLK_KHZ	108000

static const char * const s5l_clk_names[] = {
	"hperf-rt-a", "rgbout-a", "hperf-rt-b", "rgbout-b", "hdmi",
};

/*
 * HDMI PHY PLL configurations (register bytes 1..30) by pixel clock, for
 * 8 bits per component, up to S5L_MAX_PIXCLK_KHZ
 */
struct s5l_phy_cfg {
	u32 clock;
	u8 cfg[30];
};

static const struct s5l_phy_cfg s5l_phy_cfgs[] = {
	{  23750000, { 0xd1, 0x28, 0x71, 0x40, 0x30, 0xd8, 0x00, 0x80, 0x80, 0xee,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x03, 0x25, 0x00, 0x00, 0x00, 0x01 } },
	{  23856000, { 0xd1, 0x28, 0x71, 0x40, 0x4b, 0xdc, 0x00, 0x80, 0x80, 0xef,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x02, 0x25, 0x00, 0x00, 0x00, 0x01 } },
	{  25174825, { 0xd1, 0x2a, 0x74, 0x40, 0x60, 0xd5, 0x00, 0x00, 0x80, 0xfc,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf4, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  25175000, { 0xd1, 0x2a, 0x72, 0x40, 0x30, 0xfc, 0x04, 0x83, 0xa0, 0xfc,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf4, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  27000000, { 0x51, 0x2d, 0x70, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x0e,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xe4, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  29201000, { 0xd4, 0xc3, 0x74, 0x40, 0x4b, 0xcf, 0x20, 0x1d, 0xa0, 0x24,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xd2, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  29250000, { 0xd1, 0x31, 0x74, 0x40, 0x20, 0xf0, 0x08, 0x87, 0xa0, 0x25,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xd2, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  30722000, { 0xd1, 0x33, 0x74, 0x40, 0x4b, 0x3d, 0x08, 0x45, 0xa0, 0x33,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xc8, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  30750000, { 0xd1, 0x33, 0x74, 0x40, 0x20, 0x10, 0x08, 0x85, 0xa0, 0x34,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xc8, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  31500000, { 0x52, 0x69, 0x70, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x3b,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xc3, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  33000000, { 0x51, 0x37, 0x70, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x4a,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xba, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  33750000, { 0xd1, 0x38, 0x74, 0x40, 0x20, 0x10, 0x00, 0x80, 0x80, 0x52,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xb6, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  35000000, { 0xd1, 0x3a, 0x75, 0x40, 0x30, 0x20, 0x04, 0x83, 0xa0, 0x5e,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xb0, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  35500000, { 0xd1, 0x3b, 0x75, 0x40, 0x30, 0x10, 0x08, 0x85, 0xa0, 0x63,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xad, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  35714000, { 0xd2, 0x77, 0x75, 0x40, 0x4b, 0x07, 0x10, 0x49, 0xa0, 0x65,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xac, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  36000000, { 0x51, 0x3c, 0x70, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x68,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xab, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  38000000, { 0x52, 0x5f, 0x50, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x1d,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa2, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  38216000, { 0xd1, 0x30, 0x52, 0x40, 0x64, 0xd2, 0x00, 0x80, 0x80, 0x1f,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa1, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  38250000, { 0xd1, 0x30, 0x52, 0x40, 0x20, 0xf4, 0x00, 0x80, 0x80, 0x1f,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa1, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  40000000, { 0x51, 0x32, 0x50, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x2c,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x9a, 0x24, 0x00, 0x00, 0x00, 0x01 } },
	{  48906000, { 0xd1, 0x3d, 0x55, 0x40, 0x64, 0x35, 0x08, 0x43, 0xa0, 0x6f,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xfb, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  49000000, { 0xd1, 0x3d, 0x55, 0x40, 0x20, 0x10, 0x08, 0x83, 0xa0, 0x70,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xfb, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  49500000, { 0xd1, 0x3e, 0x55, 0x40, 0x20, 0xf8, 0x04, 0x81, 0xa0, 0x73,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf8, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  50000000, { 0x52, 0x7d, 0x50, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x77,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf6, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  54000000, { 0x51, 0x2d, 0x30, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x0e,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xe4, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  56250000, { 0xd1, 0x2f, 0x32, 0x40, 0x20, 0xf8, 0x08, 0x81, 0xa0, 0x19,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xda, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  56549000, { 0xd1, 0x2f, 0x32, 0x40, 0x7f, 0x3f, 0x08, 0x41, 0xa0, 0x1b,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xd9, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  56750000, { 0xd1, 0x2f, 0x32, 0x40, 0x30, 0x1c, 0x08, 0x81, 0xa0, 0x1c,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xd9, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  63500000, { 0xd1, 0x35, 0x34, 0x40, 0x30, 0xf8, 0x08, 0x83, 0xa0, 0x3e,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xc2, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  64108999, { 0xd2, 0x6b, 0x34, 0x40, 0x7f, 0xb3, 0x10, 0x05, 0xa0, 0x41,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xc0, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  65000000, { 0xd1, 0x36, 0x34, 0x40, 0x30, 0x10, 0x04, 0x81, 0xa0, 0x45,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xbd, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  68250000, { 0xd1, 0x39, 0x35, 0x40, 0x20, 0xf8, 0x08, 0x87, 0xa0, 0x55,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xb4, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  71000000, { 0xd1, 0x3b, 0x35, 0x40, 0x30, 0x10, 0x08, 0x85, 0xa0, 0x63,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xad, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  72000000, { 0x51, 0x3c, 0x30, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x68,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xab, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  73250000, { 0xd1, 0x3d, 0x35, 0x40, 0x30, 0x04, 0x08, 0x83, 0xa0, 0x6e,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa8, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  74175824, { 0xd1, 0x3e, 0x35, 0x40, 0x5b, 0xde, 0x04, 0x81, 0xa0, 0x73,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa6, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  74250000, { 0xd1, 0x3e, 0x35, 0x40, 0x20, 0xf8, 0x04, 0x81, 0xa0, 0x73,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa5, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  74481000, { 0xd3, 0xba, 0x35, 0x40, 0x64, 0x1b, 0x04, 0x41, 0xa0, 0x74,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa5, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  74500000, { 0xd1, 0x3e, 0x35, 0x40, 0x30, 0x08, 0x04, 0x81, 0xa0, 0x75,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa5, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  75000000, { 0x52, 0x7d, 0x30, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x77,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xa4, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  78750000, { 0xd1, 0x21, 0x11, 0x40, 0x20, 0xf4, 0x08, 0x87, 0xa0, 0xc5,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x9c, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  79500000, { 0xd1, 0x21, 0x11, 0x40, 0x20, 0x08, 0x08, 0x87, 0xa0, 0xc7,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x9b, 0x24, 0x01, 0x00, 0x00, 0x01 } },
	{  80136000, { 0xd1, 0x21, 0x11, 0x40, 0x64, 0x4e, 0x08, 0x87, 0xa0, 0xc8,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x33, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  81804000, { 0xd1, 0x22, 0x11, 0x40, 0x64, 0x11, 0x04, 0x83, 0xa0, 0xcd,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x2c, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  82000000, { 0xd1, 0x22, 0x11, 0x40, 0x30, 0x10, 0x04, 0x83, 0xa0, 0xcd,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x2c, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  83462000, { 0xd4, 0x8b, 0x16, 0x40, 0x4b, 0x1f, 0x10, 0x05, 0xa0, 0xd1,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x26, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  83500000, { 0xd1, 0x23, 0x11, 0x40, 0x30, 0xec, 0x08, 0x85, 0xa0, 0xd1,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x26, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  84715000, { 0xd2, 0x47, 0x16, 0x40, 0x78, 0x9f, 0x08, 0x81, 0xa0, 0xd4,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x22, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  84750000, { 0xd1, 0x23, 0x11, 0x40, 0x20, 0x14, 0x08, 0x85, 0xa0, 0xd4,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x22, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  85500000, { 0xd1, 0x24, 0x11, 0x40, 0x20, 0xe8, 0x02, 0x81, 0xa0, 0xd6,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x1f, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  85860000, { 0xd1, 0x24, 0x11, 0x40, 0x3c, 0xe5, 0x02, 0x81, 0xa0, 0xd7,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x1e, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  88750000, { 0xd1, 0x25, 0x11, 0x40, 0x30, 0xfe, 0x08, 0x83, 0xa0, 0xde,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x15, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  94387000, { 0xd1, 0x27, 0x14, 0x40, 0x7f, 0x6f, 0x02, 0x81, 0xa0, 0xec,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x04, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{  94500000, { 0xd1, 0x27, 0x11, 0x40, 0x20, 0x18, 0x08, 0x81, 0xa0, 0xec,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0x04, 0x25, 0x02, 0x00, 0x00, 0x01 } },
	{ 101000000, { 0xd1, 0x2a, 0x12, 0x40, 0x30, 0x08, 0x04, 0x83, 0xa0, 0xfd,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf3, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 101250000, { 0xd1, 0x2a, 0x12, 0x40, 0x20, 0x0c, 0x04, 0x83, 0xa0, 0xfd,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf3, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 102104000, { 0xd2, 0x55, 0x12, 0x40, 0x4b, 0x0d, 0x10, 0x4b, 0xa0, 0xff,
		       0xd8, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf1, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 102250000, { 0xd1, 0x2b, 0x12, 0x40, 0x30, 0xda, 0x08, 0x85, 0xa0, 0x00,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xf0, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 102977000, { 0xd1, 0x2b, 0x12, 0x40, 0x78, 0xa7, 0x08, 0x05, 0xa0, 0x01,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xef, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 104000000, { 0xd1, 0x2b, 0x12, 0x40, 0x30, 0x20, 0x08, 0x85, 0xa0, 0x04,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xec, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 104993000, { 0xd4, 0xaf, 0x12, 0x40, 0x7f, 0xfd, 0x20, 0x11, 0xa0, 0x06,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xea, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 106472000, { 0xd2, 0x59, 0x12, 0x40, 0x4b, 0xd7, 0x10, 0x47, 0xa0, 0x0a,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xe7, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 106500000, { 0xd1, 0x2c, 0x12, 0x40, 0x20, 0x18, 0x02, 0x81, 0xa0, 0x0a,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xe7, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 107214000, { 0xd2, 0x59, 0x12, 0x40, 0x64, 0x45, 0x10, 0x47, 0xa0, 0x0c,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xe5, 0x24, 0x02, 0x00, 0x00, 0x01 } },
	{ 108000000, { 0x51, 0x2d, 0x10, 0x40, 0x00, 0x00, 0x00, 0x80, 0x80, 0x0e,
		       0xd9, 0x45, 0xa0, 0xac, 0x80, 0x07, 0x80, 0x11, 0x04, 0x02,
		       0x22, 0x44, 0x82, 0x54, 0xe4, 0x24, 0x02, 0x00, 0x00, 0x01 } },
};

struct s5l_display {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct device *dev;
	void __iomem *rgbout;
	void __iomem *out;
	void __iomem *hdmi;
	struct clk_bulk_data clks[ARRAY_SIZE(s5l_clk_names)];
	struct clk *scaler_clk;
	struct i2c_adapter *ddc;
	struct mutex lock;	/* HDMI registers shared with the audio bridge */
	struct delayed_work hpd_work;
	bool hpd_present;
	bool scaler_enabled;
	bool audio_enabled;
	unsigned int audio_rate;
	unsigned int audio_channels;
	unsigned int gamma_brightness;
	unsigned long tmds_hz;
	struct hrtimer vblank_timer;
	ktime_t vblank_period;
	struct s5l_irq_probe probe;
	/* the buffer the plane is scanning out now, for the GPU bring-up path */
	phys_addr_t fb_pa;
	u32 fb_stride, fb_w, fb_h;
};

static struct s5l_display *g_s5l_display;

/*
 * Forced display mode, "WxH" or "WxH@Hz". Empty keeps the sink's native
 * mode. This exists for workloads that need a higher refresh rate than the
 * sink's largest drivable mode offers: a 4K sink is driven at 1080p30
 * because the PHY tops out at 108 MHz, while 1280x720@60 is within reach.
 * Writing a value re-probes the connector, so the mode can be chosen at
 * runtime without rebuilding the device tree.
 */
static char s5l_preferred_mode[32];
static struct kparam_string s5l_preferred_mode_param = {
	.maxlen = sizeof(s5l_preferred_mode),
	.string = s5l_preferred_mode,
};

static int s5l_preferred_mode_set(const char *val, const struct kernel_param *kp)
{
	char buf[sizeof(s5l_preferred_mode)];
	char *mode;
	unsigned int w, h;
	int hz = 0;

	if (!val || strlen(val) >= sizeof(buf))
		return -ENOSPC;

	/* sysfs appends a newline; accept surrounding whitespace */
	strscpy(buf, val, sizeof(buf));
	mode = strim(buf);

	/* An empty value clears the override; anything else must parse */
	if (*mode && sscanf(mode, "%ux%u@%d", &w, &h, &hz) < 2)
		return -EINVAL;

	strscpy(s5l_preferred_mode, mode, sizeof(s5l_preferred_mode));

	if (g_s5l_display)
		drm_kms_helper_hotplug_event(&g_s5l_display->drm);
	return 0;
}

static const struct kernel_param_ops s5l_preferred_mode_ops = {
	.set	= s5l_preferred_mode_set,
	.get	= param_get_string,
};
module_param_cb(preferred_mode, &s5l_preferred_mode_ops,
		&s5l_preferred_mode_param, 0644);
MODULE_PARM_DESC(preferred_mode,
	"Force a display mode as WxH[@Hz], for example 1280x720@60");

/*
 * The display path has no frame interrupt: the output-stage status only
 * carries error bits and all three display lines stay silent while the
 * display runs. Report a vblank at the mode's refresh rate from an hrtimer
 * instead, so page flips complete on time and FBIO_WAITFORVSYNC blocks.
 * This is pacing, not phase: the timer is not locked to the scanout, so it
 * reduces tearing without guaranteeing it. Off gives the old behaviour of
 * completing every flip immediately.
 */
static bool vblank_sim = true;
module_param(vblank_sim, bool, 0444);
MODULE_PARM_DESC(vblank_sim,
	"synthesize a vblank at the mode refresh rate (default on)");

static inline struct s5l_display *to_s5l(struct drm_device *drm)
{
	return container_of(drm, struct s5l_display, drm);
}

/* The HDMI core registers are byte-wide, one per word. */
static void hdmi_wb(struct s5l_display *d, u32 off, u32 val, int nbytes)
{
	while (nbytes-- > 0) {
		writel(val & 0xffU, d->hdmi + off);
		off += 4;
		val >>= 8;
	}
}

static u32 hdmi_rb(struct s5l_display *d, u32 off, int nbytes)
{
	u32 val = 0;

	while (nbytes-- > 0)
		val |= (readl(d->hdmi + off + nbytes * 4) & 0xffU) << (nbytes * 8);
	return val;
}

static void hdmi_mask(struct s5l_display *d, u32 off, u32 clear, u32 set)
{
	writel(((readl(d->hdmi + off) & 0xffU) & ~clear) | set, d->hdmi + off);
}

/* --- HDMI PHY --- */

/* The nearest PHY configuration, if it is within 0.5% of the pixel clock */
static const struct s5l_phy_cfg *s5l_phy_find(unsigned long hz)
{
	const struct s5l_phy_cfg *best = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s5l_phy_cfgs); i++) {
		unsigned long c = s5l_phy_cfgs[i].clock;

		if (abs_diff(c, hz) <= c / 200 &&
		    (!best || abs_diff(c, hz) < abs_diff((unsigned long)best->clock, hz)))
			best = &s5l_phy_cfgs[i];
	}
	return best;
}

static int s5l_phy_wait_idle(struct s5l_display *d)
{
	u32 v;

	return readl_poll_timeout(d->hdmi + HDMI_PHY_BUSY, v, !(v & 1), 1, 100);
}

static int s5l_phy_write(struct s5l_display *d, unsigned int reg, u8 val)
{
	void __iomem *word = d->hdmi + HDMI_PHY_CFG(reg >> 2);
	unsigned int sh = (reg & 3) * 8;
	int ret;

	ret = s5l_phy_wait_idle(d);
	if (ret)
		return ret;
	writel((readl(word) & ~(0xffU << sh)) | ((u32)val << sh), word);
	return s5l_phy_wait_idle(d);
}

static u8 s5l_phy_byte(struct s5l_display *d, unsigned int reg)
{
	return readl(d->hdmi + HDMI_PHY_CFG(reg >> 2)) >> ((reg & 3) * 8);
}

static void s5l_phy_power(struct s5l_display *d, bool on)
{
	u8 r28 = s5l_phy_byte(d, 28) >> 1;

	s5l_phy_write(d, 29, on ? (r28 & 0x6b) : (r28 | 0x94));
}

static int s5l_phy_wait_ready(struct s5l_display *d)
{
	u32 v;

	return readl_poll_timeout(d->hdmi + HDMI_PHY_STATUS_0, v, v & 1,
				  100, 200000);
}

static int s5l_phy_program(struct s5l_display *d, const struct s5l_phy_cfg *p)
{
	u8 img[32];
	int i, ret;

	img[0] = s5l_phy_byte(d, 0);
	memcpy(&img[1], p->cfg, sizeof(p->cfg));
	img[31] = 0;

	ret = s5l_phy_write(d, 31, 0x00);
	for (i = 0; !ret && i < 32; i++)
		ret = s5l_phy_write(d, i, img[i]);
	if (!ret)
		ret = s5l_phy_write(d, 31, 0x80);
	if (!ret)
		ret = s5l_phy_wait_ready(d);
	return ret;
}

/* --- audio bridge (shared with sound/arm/s5l8947x-audio.c) --- */

static u32 s5l_acr_n(unsigned int rate)
{
	/* HDMI-recommended N for these rates at the TMDS clocks in use */
	switch (rate) {
	case 32000:
		return 4096;
	case 44100:
		return 6272;
	default:
		return 6144;
	}
}

static u32 s5l_acr_cts(struct s5l_display *d, unsigned int rate)
{
	/* CTS = fTMDS * N / (128 * fs), rounded */
	u64 num = (u64)d->tmds_hz * s5l_acr_n(rate);
	u64 den = 128ULL * rate;

	return (u32)div64_u64(num + den / 2, den);
}

static void s5l_audio_off(struct s5l_display *d)
{
	writel(0x00, d->hdmi + HDMI_ACR_CON);
	writel(0x00, d->hdmi + HDMI_I2S_CLK_CON);
	writel(0x00, d->hdmi + HDMI_ASP_CON);
	writel(0x00, d->hdmi + HDMI_AUI_CON);
	hdmi_mask(d, HDMI_CON_0, 0x04, 0);
}

static void s5l_audio_on(struct s5l_display *d, unsigned int rate,
			 unsigned int channels)
{
	u8 aui[5] = { 0x01, 0x00, 0x00, 0x00, 0x00 };
	u32 csum;
	int i;

	/* Audio InfoFrame: channel count in byte 1, default allocation */
	aui[0] = (channels - 1) & 0x07;

	/* HDMI mode with guard bands and preambles for the data islands */
	writel(0x02, d->hdmi + HDMI_MODE_SEL);
	writel(0x00, d->hdmi + HDMI_CON_2);
	writel(0x00, d->hdmi + HDMI_AUDIO_CLKSEL);

	hdmi_wb(d, HDMI_ACR_N0, s5l_acr_n(rate), 3);
	hdmi_wb(d, HDMI_ACR_CTS0, s5l_acr_cts(d, rate), 3);
	writel(0x04, d->hdmi + HDMI_ACR_CON);

	/* Audio comes in over I2S, not the S/PDIF receiver */
	writel(0, d->hdmi + HDMI_SPDIF_RX_OP);
	writel(2, d->hdmi + HDMI_SPDIF_RX_CLK);

	/*
	 * I2S receiver: reset pin selection (all 7s), basic I2S (left-justified
	 * reads every sample one bit late), 16-bit samples in 32-bit frames.
	 */
	writel(0x01, d->hdmi + HDMI_I2S_CLK_CON);
	writel(readl(d->hdmi + HDMI_I2S_DSD_CON) | 0x01U, d->hdmi + HDMI_I2S_DSD_CON);
	writel(0x77, d->hdmi + HDMI_I2S_PIN_SEL_0);
	writel(0x77, d->hdmi + HDMI_I2S_PIN_SEL_1);
	writel(0x77, d->hdmi + HDMI_I2S_PIN_SEL_2);
	writel(0x07, d->hdmi + HDMI_I2S_PIN_SEL_3);
	writel(0x00, d->hdmi + HDMI_I2S_CON_1);
	writel(0x04, d->hdmi + HDMI_I2S_CON_2);
	writel(0x00, d->hdmi + HDMI_I2S_CH_ST_0);
	writel(0x00, d->hdmi + HDMI_I2S_CH_ST_1);
	writel(0x00, d->hdmi + HDMI_I2S_CH_ST_2);
	writel(rate == 44100 ? 0x00 : rate == 32000 ? 0x03 : 0x02,
	       d->hdmi + HDMI_I2S_CH_ST_3);
	writel(0x02, d->hdmi + HDMI_I2S_CH_ST_4);
	writel(0x01, d->hdmi + HDMI_I2S_CH_ST_CON);
	/* One mux bit per channel (I2S0 = 0/1, I2S1 = 2/3, ...) */
	writel((1U << channels) - 1, d->hdmi + HDMI_I2S_MUX_CH);
	writel(0x03, d->hdmi + HDMI_I2S_MUX_CUV);
	writel(0x77, d->hdmi + HDMI_I2S_MUX_CON);

	/* Audio InfoFrame, version 1 (type 0x84 in the header is dropped) */
	writel(0x04, d->hdmi + HDMI_AUI_HEADER0);
	writel(0x01, d->hdmi + HDMI_AUI_HEADER1);
	writel(0x0a, d->hdmi + HDMI_AUI_HEADER2);
	csum = 0x04 + 0x01 + 0x0a;
	for (i = 0; i < 10; i++) {
		u8 b = i < 5 ? aui[i] : 0;

		csum += b;
		writel(b, d->hdmi + HDMI_AUI_BYTE1 + i * 4);
	}
	writel((0x100U - (csum & 0xffU)) & 0xffU, d->hdmi + HDMI_AUI_CHECK_SUM);
	writel(0x02, d->hdmi + HDMI_AUI_CON);

	hdmi_mask(d, HDMI_CON_0, 0, 0x05);
}

/*
 * The ACR drops N/CTS if it is armed before an audio clock is present, so
 * the ALSA driver re-arms it once the clock runs. Register writes only.
 */
static void s5l_audio_arm(struct s5l_display *d, unsigned int rate)
{
	writel(0x00, d->hdmi + HDMI_ACR_CON);
	hdmi_wb(d, HDMI_ACR_N0, s5l_acr_n(rate), 3);
	hdmi_wb(d, HDMI_ACR_CTS0, s5l_acr_cts(d, rate), 3);
	writel(0x04, d->hdmi + HDMI_ACR_CON);
}

int s5l8947x_hdmi_audio_configure(unsigned int sample_rate,
				  unsigned int channels, bool enable)
{
	struct s5l_display *d = g_s5l_display;

	if (!d)
		return -ENODEV;
	if (enable && (!sample_rate || !channels || channels > 8 ||
		       (channels & 1)))
		return -EINVAL;

	mutex_lock(&d->lock);
	if (enable)
		s5l_audio_on(d, sample_rate, channels);
	else
		s5l_audio_off(d);
	d->audio_enabled = enable;
	if (enable) {
		d->audio_rate = sample_rate;
		d->audio_channels = channels;
	}
	mutex_unlock(&d->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(s5l8947x_hdmi_audio_configure);

void s5l8947x_hdmi_audio_arm(unsigned int sample_rate)
{
	struct s5l_display *d = g_s5l_display;

	if (d && sample_rate)
		s5l_audio_arm(d, sample_rate);
}
EXPORT_SYMBOL_GPL(s5l8947x_hdmi_audio_arm);

/* --- display pipe and output stage --- */

/*
 * Scale the output by pct percent through the gamma LUT. The LUT as loaded
 * here (256 entries per component) posterizes the picture and crushes the
 * darks, so at full brightness it is bypassed and the framebuffer goes out
 * unchanged.
 */
static void s5l_gamma(struct s5l_display *d, unsigned int pct)
{
	u32 base = readl(d->out + OUT_GAMMA_IDX) & 0xfffe0f00U;
	int c, i;

	if (pct >= 100) {
		writel(0, d->out + OUT_GAMMA_EN);
		return;
	}
	writel(base, d->out + OUT_GAMMA_IDX);
	for (c = 0; c < 3; c++)
		for (i = 0; i < 256; i++) {
			u32 v = min_t(u32, (u32)i * 1023U * pct / (255U * 100U), 1023U);

			writel(base | i | (0x14000U + c * 0x1000U), d->out + OUT_GAMMA_IDX);
			writel(v, d->out + OUT_GAMMA_VAL);
		}
	writel(9, d->out + OUT_GAMMA_EN);
}

static void s5l_pipe_setup(struct s5l_display *d, const struct drm_display_mode *m,
			   dma_addr_t addr, u32 pitch)
{
	void __iomem *p = d->rgbout + RGB_PLANE0;
	u32 wh = m->vdisplay | (m->hdisplay << 16);
	u32 v;
	int i;

	/* rgbout */
	writel(readl(d->rgbout + RGB_CFG) | 0x10U, d->rgbout + RGB_CFG);
	v = (readl(d->rgbout + RGB_CFG) & ~0x700U) | 0x100U;
	writel(v, d->rgbout + RGB_CFG);
	v = (readl(d->rgbout + RGB_CFG) & ~0x07ff0000U) | 0x04000000U;
	writel(v, d->rgbout + RGB_CFG);
	writel(wh, d->rgbout + RGB_SIZE);
	writel(0x01c001f0U, d->rgbout + RGB_205C);
	writel(0x00000100U, d->rgbout + RGB_2060);
	writel(0x00400001U, d->rgbout + RGB_105C);
	writel(0x1000U, d->rgbout + RGB_2068);
	writel(0x1000U, d->rgbout + RGB_2078);
	writel(0x1000U, d->rgbout + RGB_2088);
	writel(0xbff00000U, d->rgbout + RGB_2064);

	/* output stage: V sync position is sync width + back porch - 2 */
	writel(1, d->out + OUT_RESET);
	for (i = 0; i < 200000 && (readl(d->out + OUT_RESET) & 1); i++)
		;
	writel(0, d->out + OUT_1C);
	writel(0, d->out + OUT_24);
	writel(m->hdisplay - 1, d->out + OUT_HSIZE);
	writel(m->vdisplay - 1, d->out + OUT_VSIZE);
	writel(0x23, d->out + OUT_CFG);
	writel((m->vsync_end - m->vsync_start) + (m->vtotal - m->vsync_end) - 2,
	       d->out + OUT_VSYNC);

	/* plane 0: ARGB8888, whole screen */
	writel(1, p + PLANE_CTL);
	writel(addr, p + PLANE_ADDR);
	writel((readl(p + PLANE_STRIDE) & 0x3fU) | (pitch & ~0x3fU), p + PLANE_STRIDE);
	writel(0, p + 0x4c);
	writel(wh, p + 0x50);
	writel(0, p + 0x54);
	writel(wh, p + 0x58);
	writel(1, p + 0x5c);
	writel(0, p + 0x90);
	writel(0, p + 0x80);
	writel(0, p + 0x84);
	writel(0x100000U, p + 0x88);
	writel(0x100000U, p + 0x8c);
	writel(0, p + 0x60);
	writel(wh, p + 0x70);
	writel(RGB_COMMIT_VAL, d->rgbout + RGB_COMMIT);

	writel(readl(d->rgbout + RGB_CTL) | 0x100U, d->rgbout + RGB_CTL);
}

/* --- HDMI transmitter --- */

static void s5l_hdmi_stop(struct s5l_display *d)
{
	hdmi_mask(d, HDMI_TG(0x00), 0x01, 0);
	hdmi_mask(d, HDMI_CON_0, 0x01, 0);
	s5l_phy_power(d, false);
}

static void s5l_hdmi_controller_start(struct s5l_display *d)
{
	int i;

	writel(readl(d->hdmi + HDMI_PHY_CON_0) & ~1U, d->hdmi + HDMI_PHY_CON_0);
	writel(readl(d->hdmi + HDMI_PHY_RSTOUT) | 1U, d->hdmi + HDMI_PHY_RSTOUT);
	usleep_range(1000, 1200);
	writel(readl(d->hdmi + HDMI_PHY_RSTOUT) & ~1U, d->hdmi + HDMI_PHY_RSTOUT);
	writel(readl(d->hdmi + HDMI_CON_0) & ~0x10U, d->hdmi + HDMI_CON_0);

	writel(1, d->hdmi + HDMI_PHYB(0x2c));
	for (i = 0; i < 1000; i++) {
		writel(0x70, d->hdmi + HDMI_PHYB(0x80));
		if (readl(d->hdmi + HDMI_PHYB(0x80)) == 0x70)
			break;
	}
	writel(0x3f00U, d->hdmi + HDMI_PHYB(0x20));
	writel(0x281U, d->hdmi + HDMI_PHYB(0x00));
	writel(0, d->hdmi + HDMI_PHYB(0x18));
	writel(0, d->hdmi + HDMI_PHYB(0x30));
	writel(0xffffffffU, d->hdmi + HDMI_PHYB(0x34));
	writel(0xd0U, d->hdmi + HDMI_PHYB(0x04));
	s5l_phy_write(d, 29, (s5l_phy_byte(d, 28) >> 1) | 0xb7);
}

static void s5l_hdmi_timing(struct s5l_display *d, const struct drm_display_mode *m)
{
	u32 ht = m->htotal, vt = m->vtotal;
	u32 hblank = ht - m->hdisplay, vblank = vt - m->vdisplay;
	u32 hfp = m->hsync_start - m->hdisplay, hsw = m->hsync_end - m->hsync_start;
	u32 vfp = m->vsync_start - m->vdisplay, vsw = m->vsync_end - m->vsync_start;

	hdmi_wb(d, HDMI_CORE(0x0a0), hblank, 2);
	hdmi_wb(d, HDMI_CORE(0x0b8), vblank, 2);
	hdmi_wb(d, HDMI_CORE(0x0b0), vt, 2);
	hdmi_wb(d, HDMI_CORE(0x0c0), vt, 2);
	hdmi_wb(d, HDMI_CORE(0x0c8), ht, 2);
	hdmi_wb(d, HDMI_CORE(0x0e8), 0, 1);
	hdmi_wb(d, HDMI_CORE(0x110), vt + vblank + 1, 2);
	hdmi_wb(d, HDMI_CORE(0x118), vt, 2);
	hdmi_wb(d, HDMI_CORE(0x0e0), !!(m->flags & DRM_MODE_FLAG_NHSYNC), 1);
	hdmi_wb(d, HDMI_CORE(0x120), hfp - 2, 2);
	hdmi_wb(d, HDMI_CORE(0x128), hfp - 2 + hsw, 2);
	hdmi_wb(d, HDMI_CORE(0x0e4), !!(m->flags & DRM_MODE_FLAG_NVSYNC), 1);
	hdmi_wb(d, HDMI_CORE(0x138), vfp, 2);
	hdmi_wb(d, HDMI_CORE(0x130), vfp + vsw, 2);
	hdmi_wb(d, HDMI_CORE(0x148), vfp + vt, 2);
	hdmi_wb(d, HDMI_CORE(0x140), vfp + vsw + vt, 2);
	hdmi_wb(d, HDMI_CORE(0x158), hfp + ht / 2, 2);
	hdmi_wb(d, HDMI_CORE(0x150), hfp + ht / 2, 2);

	hdmi_wb(d, HDMI_TG(0x00), 0, 1);
	hdmi_wb(d, HDMI_TG(0x18), ht, 2);
	hdmi_wb(d, HDMI_TG(0x20), hblank, 2);
	hdmi_wb(d, HDMI_TG(0x28), m->hdisplay, 2);
	hdmi_wb(d, HDMI_TG(0x30), vt, 2);
	hdmi_wb(d, HDMI_TG(0x38), 1, 2);
	hdmi_wb(d, HDMI_TG(0x40), vt + 1, 2);
	hdmi_wb(d, HDMI_TG(0x48), vblank, 2);
	hdmi_wb(d, HDMI_TG(0x50), m->vdisplay, 2);
	hdmi_wb(d, HDMI_TG(0x58), vt + 1, 2);
	hdmi_wb(d, HDMI_TG(0x60), vblank + vt + 1, 2);
	hdmi_wb(d, HDMI_TG(0x78), 1, 2);
	hdmi_wb(d, HDMI_TG(0x80), vt + 1, 2);
	hdmi_wb(d, HDMI_TG(0x88), 1, 2);
	hdmi_wb(d, HDMI_TG(0x90), vt + 1, 2);
}

/* RGB 4:4:4, full range, no colour conversion, no test pattern */
static void s5l_hdmi_color(struct s5l_display *d)
{
	hdmi_mask(d, HDMI_CON_0, 0x08, 0);
	hdmi_wb(d, HDMI_CORE(0xd520), 0, 1);
	hdmi_wb(d, HDMI_CORE(0xd528), 0, 1);
	hdmi_wb(d, HDMI_CORE(0xd530), 0, 1);
	hdmi_wb(d, HDMI_CORE(0xd524), 0, 1);
	hdmi_wb(d, HDMI_CORE(0xd52c), 0, 1);
	hdmi_wb(d, HDMI_CORE(0xd534), 0, 1);
	hdmi_mask(d, HDMI_CON_1, 0x60, 0);
	hdmi_mask(d, HDMI_DC_CONTROL, 0x03, 0);
	hdmi_mask(d, HDMI_VIDEO_PATTERN_GEN, 0x01, 0);
}

static void s5l_hdmi_avi(struct s5l_display *d, const struct drm_display_mode *m)
{
	struct hdmi_avi_infoframe frame;
	u8 buf[HDMI_INFOFRAME_SIZE(AVI)];
	ssize_t len;
	int i;

	if (drm_hdmi_avi_infoframe_from_display_mode(&frame, &d->connector, m))
		return;
	/*
	 * The pipe sends full-range RGB; say so when the sink lets us, or it
	 * takes CE modes as limited range and clips and stretches the levels.
	 */
	drm_hdmi_avi_infoframe_quant_range(&frame, &d->connector, m,
					   HDMI_QUANTIZATION_RANGE_FULL);
	len = hdmi_avi_infoframe_pack(&frame, buf, sizeof(buf));
	if (len < 0)
		return;
	/* header (type, version, length), checksum, then the payload */
	for (i = 0; i < 3; i++)
		writel(buf[i], d->hdmi + HDMI_AVI_HEADER0 + 4 * i);
	writel(buf[3], d->hdmi + HDMI_AVI_CHECK_SUM);
	for (i = HDMI_INFOFRAME_HEADER_SIZE; i < len; i++)
		writel(buf[i], d->hdmi + HDMI_AVI_BYTE1 +
		       4 * (i - HDMI_INFOFRAME_HEADER_SIZE));
	hdmi_mask(d, HDMI_AVI_CON, 0x03, 0x02);
}

static int s5l_hdmi_start(struct s5l_display *d, const struct drm_display_mode *m,
			  const struct s5l_phy_cfg *phy, bool hdmi_mode)
{
	unsigned int frame_us = DIV_ROUND_UP(1000000U, max(drm_mode_vrefresh(m), 1));
	int ret;

	s5l_hdmi_controller_start(d);
	s5l_hdmi_timing(d, m);
	s5l_hdmi_color(d);
	hdmi_mask(d, HDMI_MODE_SEL, 0x03, hdmi_mode ? 2 : 1);
	hdmi_mask(d, HDMI_CON_2, 0x22, 0);

	ret = s5l_phy_program(d, phy);
	if (ret)
		return ret;
	s5l_phy_power(d, false);
	hdmi_mask(d, HDMI_TG(0x00), 0x01, 0x01);

	if (hdmi_mode) {
		s5l_hdmi_avi(d, m);
		/* General Control Packet: clear AV mute */
		hdmi_wb(d, HDMI_GCP_BYTE1, 0, 3);
		hdmi_mask(d, HDMI_GCP_BYTE2, 0x07, 0);
		hdmi_mask(d, HDMI_GCP_CON, 0, 0x0c);
		hdmi_mask(d, HDMI_GCP_CON, 0x03, 0x02);
		hdmi_mask(d, HDMI_GCP_BYTE1, 0x11, 0x01);
		fsleep(frame_us);
	}
	hdmi_mask(d, HDMI_CON_0, 0, 0x01);
	fsleep(100);
	s5l_phy_power(d, true);
	ret = s5l_phy_wait_ready(d);
	fsleep(60);
	s5l_hdmi_color(d);
	hdmi_mask(d, HDMI_GCP_BYTE1, 0x11, 0x10);
	return ret;
}

/* --- KMS --- */

static enum drm_mode_status s5l_mode_ok(const struct drm_display_mode *m)
{
	if (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN |
			DRM_MODE_FLAG_DBLCLK))
		return MODE_NO_INTERLACE;
	if (m->hdisplay > S5L_MAX_W || m->vdisplay > S5L_MAX_H)
		return MODE_BAD;
	if (m->hsync_start - m->hdisplay < 2)
		return MODE_HSYNC;
	if (m->clock > S5L_MAX_PIXCLK_KHZ)
		return MODE_CLOCK_HIGH;
	if (!s5l_phy_find((unsigned long)m->clock * 1000))
		return MODE_CLOCK_RANGE;
	return MODE_OK;
}

static enum drm_mode_status s5l_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
						const struct drm_display_mode *mode)
{
	return s5l_mode_ok(mode);
}

/*
 * Complete the commit's event. With a synthesized vblank the event is armed
 * so it is sent on the next tick; the reference keeps vblanking on until
 * then. Otherwise, and when vblanking is unavailable, send it now.
 */
static void s5l_complete_event(struct drm_crtc *crtc, bool wait_vblank)
{
	struct drm_pending_vblank_event *event = crtc->state->event;
	unsigned long flags;

	if (!event)
		return;
	crtc->state->event = NULL;

	if (wait_vblank && vblank_sim && drm_crtc_vblank_get(crtc) == 0) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_arm_vblank_event(crtc, event);
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		return;
	}

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static enum hrtimer_restart s5l_vblank_timer(struct hrtimer *timer)
{
	struct s5l_display *d = container_of(timer, struct s5l_display,
					     vblank_timer);

	drm_crtc_handle_vblank(&d->pipe.crtc);
	hrtimer_forward_now(timer, d->vblank_period);
	return HRTIMER_RESTART;
}

static void s5l_pipe_enable(struct drm_simple_display_pipe *pipe,
			    struct drm_crtc_state *crtc_state,
			    struct drm_plane_state *plane_state)
{
	struct s5l_display *d = to_s5l(pipe->crtc.dev);
	const struct drm_display_mode *m = &crtc_state->adjusted_mode;
	struct drm_framebuffer *fb = plane_state->fb;
	const struct s5l_phy_cfg *phy = s5l_phy_find((unsigned long)m->clock * 1000);
	bool hdmi_mode = d->connector.display_info.is_hdmi;
	int ret;

	if (WARN_ON(!phy || !fb))
		return;

	mutex_lock(&d->lock);
	s5l_hdmi_stop(d);
	s5l_pipe_setup(d, m, drm_fb_dma_get_gem_addr(fb, plane_state, 0),
		       fb->pitches[0]);
	d->tmds_hz = phy->clock;
	ret = s5l_hdmi_start(d, m, phy, hdmi_mode);
	writel(9, d->out + OUT_START);
	s5l_gamma(d, d->gamma_brightness);
	if (hdmi_mode && d->audio_enabled) {
		s5l_audio_on(d, d->audio_rate,
			    d->audio_channels ?: 2);
		s5l_audio_arm(d, d->audio_rate);
	}
	mutex_unlock(&d->lock);

	if (vblank_sim) {
		int refresh = drm_mode_vrefresh(m);

		if (refresh > 0) {
			d->vblank_period = ns_to_ktime(
				DIV_ROUND_CLOSEST(NSEC_PER_SEC, refresh));
			drm_crtc_vblank_on(&d->pipe.crtc);
			hrtimer_start(&d->vblank_timer, d->vblank_period,
				      HRTIMER_MODE_REL);
		}
	}

	if (ret)
		drm_err(&d->drm, "PHY not ready for %s\n", m->name);
	else
		drm_info(&d->drm, "%ux%u@%u, %u kHz, %s\n", m->hdisplay, m->vdisplay,
			 drm_mode_vrefresh(m), m->clock, hdmi_mode ? "HDMI" : "DVI");
	s5l_complete_event(&pipe->crtc, false);
}

static void s5l_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct s5l_display *d = to_s5l(pipe->crtc.dev);

	if (vblank_sim)
		hrtimer_cancel(&d->vblank_timer);

	mutex_lock(&d->lock);
	s5l_hdmi_stop(d);
	writel(1, d->out + OUT_START);
	mutex_unlock(&d->lock);
	s5l_complete_event(&pipe->crtc, false);

	/* Flushes any flip that was still waiting for a tick */
	if (vblank_sim)
		drm_crtc_vblank_off(&d->pipe.crtc);
}

static void s5l_pipe_update(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *old_state)
{
	struct s5l_display *d = to_s5l(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;

	if (fb && pipe->crtc.state->active) {
		void __iomem *p = d->rgbout + RGB_PLANE0;

		d->fb_pa = drm_fb_dma_get_gem_addr(fb, state, 0);
		d->fb_stride = fb->pitches[0];
		d->fb_w = fb->width;
		d->fb_h = fb->height;
		writel(d->fb_pa, p + PLANE_ADDR);
		writel((readl(p + PLANE_STRIDE) & 0x3fU) | (fb->pitches[0] & ~0x3fU),
		       p + PLANE_STRIDE);
		writel(RGB_COMMIT_VAL, d->rgbout + RGB_COMMIT);
	}

	s5l_complete_event(&pipe->crtc, pipe->crtc.state->active);
}

static const struct drm_simple_display_pipe_funcs s5l_pipe_funcs = {
	.mode_valid	= s5l_pipe_mode_valid,
	.enable		= s5l_pipe_enable,
	.disable	= s5l_pipe_disable,
	.update		= s5l_pipe_update,
};

static const u32 s5l_formats[] = { DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888 };

static bool s5l_same_aspect(const struct drm_display_mode *a,
			     const struct drm_display_mode *b)
{
	u32 x = a->hdisplay * b->vdisplay, y = b->hdisplay * a->vdisplay;

	return abs_diff(x, y) * 100 <= max(x, y);	/* within 1% */
}

/* true if a is a better choice than b for a sink whose native mode is n */
static bool s5l_mode_better(const struct drm_display_mode *a,
			    const struct drm_display_mode *b,
			    const struct drm_display_mode *n)
{
	u32 aa = a->hdisplay * a->vdisplay, ba = b->hdisplay * b->vdisplay;
	int nr = drm_mode_vrefresh(n);

	if (aa != ba)
		return aa > ba;
	return abs(drm_mode_vrefresh(a) - nr) < abs(drm_mode_vrefresh(b) - nr);
}

/*
 * Mark the mode to use by default. That is the sink's native (EDID
 * preferred) mode when the hardware can drive it; otherwise the largest
 * mode we can drive with the native aspect ratio and the refresh rate
 * closest to the native one, and only failing that the largest mode of
 * any shape.
 */
static void s5l_pick_preferred(struct drm_connector *conn)
{
	struct drm_display_mode *m, *native = NULL, *same = NULL, *any = NULL;

	list_for_each_entry(m, &conn->probed_modes, head) {
		if (m->type & DRM_MODE_TYPE_PREFERRED) {
			native = m;
			break;
		}
	}
	if (!native || s5l_mode_ok(native) == MODE_OK)
		return;

	list_for_each_entry(m, &conn->probed_modes, head) {
		if (s5l_mode_ok(m) != MODE_OK)
			continue;
		if (s5l_same_aspect(m, native) &&
		    (!same || s5l_mode_better(m, same, native)))
			same = m;
		if (!any || s5l_mode_better(m, any, native))
			any = m;
	}
	m = same ?: any;
	if (!m)
		return;

	native->type &= ~DRM_MODE_TYPE_PREFERRED;
	m->type |= DRM_MODE_TYPE_PREFERRED;
	drm_info(conn->dev, "native %dx%d@%d not supported, using %dx%d@%d\n",
		    native->hdisplay, native->vdisplay, drm_mode_vrefresh(native),
		    m->hdisplay, m->vdisplay, drm_mode_vrefresh(m));
}

/* True if the requested mode was found and marked preferred */
static bool s5l_force_preferred(struct drm_connector *conn)
{
	struct drm_display_mode *m, *pick = NULL;
	unsigned int w, h;
	int hz = 0, best = INT_MAX;

	if (sscanf(s5l_preferred_mode, "%ux%u@%d", &w, &h, &hz) < 2)
		return false;

	list_for_each_entry(m, &conn->probed_modes, head) {
		int delta;

		if (m->hdisplay != w || m->vdisplay != h)
			continue;
		if (s5l_mode_ok(m) != MODE_OK)
			continue;
		delta = hz ? abs(drm_mode_vrefresh(m) - hz) : 0;
		if (!pick || delta < best) {
			pick = m;
			best = delta;
		}
	}
	if (!pick)
		return false;

	list_for_each_entry(m, &conn->probed_modes, head)
		m->type &= ~DRM_MODE_TYPE_PREFERRED;
	pick->type |= DRM_MODE_TYPE_PREFERRED;
	drm_info(conn->dev, "forced mode %s\n", pick->name);
	return true;
}

static int s5l_conn_get_modes(struct drm_connector *conn)
{
	const struct drm_edid *edid = drm_edid_read(conn);
	int n;

	drm_edid_connector_update(conn, edid);
	n = drm_edid_connector_add_modes(conn);
	drm_edid_free(edid);
	if (n)
		s5l_pick_preferred(conn);
	if (!n) {
		/* No EDID: offer 720p, which every HDMI sink takes */
		n = drm_add_modes_noedid(conn, 1280, 720);
		drm_set_preferred_mode(conn, 1280, 720);
	}

	if (s5l_preferred_mode[0] && !s5l_force_preferred(conn)) {
		unsigned int w, h;

		/*
		 * The sink need not list the requested mode: add the
		 * standard 60 Hz modes the pipe can drive and look again.
		 */
		if (sscanf(s5l_preferred_mode, "%ux%u", &w, &h) == 2)
			n += drm_add_modes_noedid(conn, w, h);
		if (n && !s5l_force_preferred(conn))
			drm_warn(conn->dev, "preferred_mode %s not available\n",
				 s5l_preferred_mode);
	}
	return n;
}

static enum drm_mode_status s5l_conn_mode_valid(struct drm_connector *conn,
						struct drm_display_mode *mode)
{
	return s5l_mode_ok(mode);
}

static const struct drm_connector_helper_funcs s5l_conn_helper_funcs = {
	.get_modes	= s5l_conn_get_modes,
	.mode_valid	= s5l_conn_mode_valid,
};

static enum drm_connector_status s5l_conn_detect(struct drm_connector *conn,
						 bool force)
{
	struct s5l_display *d = to_s5l(conn->dev);

	return (readl(d->hdmi + HDMI_HPD_STATUS) & 1) ?
		connector_status_connected : connector_status_disconnected;
}

static const struct drm_connector_funcs s5l_conn_funcs = {
	.detect			= s5l_conn_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= drm_connector_cleanup,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs s5l_mode_config_funcs = {
	.fb_create	= drm_gem_fb_create,
	.atomic_check	= drm_atomic_helper_check,
	.atomic_commit	= drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_DMA_FOPS(s5l_fops);

static const struct drm_driver s5l_drm_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops			= &s5l_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	.name			= "s5l8947x",
	.desc			= "S5L8947X display pipe and HDMI",
	.date			= "20261031",
	.major			= 1,
	.minor			= 0,
};

/*
 * Hot-plug: the HPD register is polled. A change re-probes the connector,
 * and the audio packets stop while the sink is away and come back with it.
 */
static void s5l_hpd_poll(struct work_struct *work)
{
	struct s5l_display *d = container_of(work, struct s5l_display, hpd_work.work);
	bool present = readl(d->hdmi + HDMI_HPD_STATUS) & 1;

	if (present != d->hpd_present) {
		d->hpd_present = present;
		drm_info(&d->drm, "HDMI hot-plug %s\n", present ? "connected" : "lost");
		mutex_lock(&d->lock);
		if (!present) {
			writel(0x00, d->hdmi + HDMI_AUI_CON);
			hdmi_mask(d, HDMI_CON_0, 0x04, 0);
			writel(0x00, d->hdmi + HDMI_ACR_CON);
		} else if (d->audio_enabled) {
			s5l_audio_on(d, d->audio_rate,
			    d->audio_channels ?: 2);
			s5l_audio_arm(d, d->audio_rate);
		}
		mutex_unlock(&d->lock);
		drm_kms_helper_hotplug_event(&d->drm);
	}
	schedule_delayed_work(&d->hpd_work, msecs_to_jiffies(500));
}

/* --- sysfs (platform device) --- */

static ssize_t hpd_connected_show(struct device *dev, struct device_attribute *a,
				  char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(d->hdmi + HDMI_HPD_STATUS) & 1);
}
static DEVICE_ATTR_RO(hpd_connected);

static ssize_t phy_ready_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", readl(d->hdmi + HDMI_PHY_STATUS_0) & 1);
}
static DEVICE_ATTR_RO(phy_ready);

static ssize_t plane_phys_show(struct device *dev, struct device_attribute *a,
			       char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x\n", readl(d->rgbout + RGB_PLANE0 + PLANE_ADDR));
}
static DEVICE_ATTR_RO(plane_phys);

/*
 * The current scanout buffer as "physical stride width height".  The SGX
 * bring-up path maps this physical range into the GPU and lets the 2D engine
 * draw into the screen without a copy.
 */
static ssize_t plane_info_show(struct device *dev, struct device_attribute *a,
			       char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "0x%08x %u %u %u\n", (u32)d->fb_pa,
			  d->fb_stride, d->fb_w, d->fb_h);
}
static DEVICE_ATTR_RO(plane_info);

static ssize_t gamma_brightness_show(struct device *dev, struct device_attribute *a,
				     char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", d->gamma_brightness);
}

static ssize_t gamma_brightness_store(struct device *dev, struct device_attribute *a,
				      const char *buf, size_t count)
{
	struct s5l_display *d = dev_get_drvdata(dev);
	unsigned int val;

	if (kstrtouint(buf, 0, &val) || val > 100)
		return -EINVAL;
	mutex_lock(&d->lock);
	s5l_gamma(d, val);
	d->gamma_brightness = val;
	mutex_unlock(&d->lock);
	return count;
}
static DEVICE_ATTR_RW(gamma_brightness);

/*
 * irq_probe: write one of the display interrupt numbers, wait a second, read
 * the result. Used to look for a frame/vblank source on real hardware. The
 * status words of the three candidate blocks are captured on the first hit;
 * nothing is written, so a pending error interrupt is not cleared and shows
 * up as a storm.
 *
 * A second value sets the output-stage interrupt-enable register for the
 * duration of the probe and restores it afterwards, which is needed when the
 * source is masked: "echo 44 1 > irq_probe".
 */
static irqreturn_t s5l_irq_probe_irq(int irq, void *data)
{
	struct s5l_display *d = data;
	struct s5l_irq_probe *p = &d->probe;
	u64 now = ktime_get_ns();

	if (atomic_inc_return(&p->hits) == 1) {
		p->first_ns = now;
		p->int_status = readl(d->out + OUT_INT_STATUS);
		p->rgbout_status = readl(d->rgbout + RGB_INT_STATUS);
		p->hpd = readl(d->hdmi + HDMI_HPD_STATUS);
	}
	p->last_ns = now;

	if (atomic_read(&p->hits) > S5L_IRQ_PROBE_MAX && !p->storm) {
		p->storm = true;
		disable_irq_nosync(irq);
	}
	return IRQ_HANDLED;
}

static ssize_t irq_probe_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);
	struct platform_device *pdev = to_platform_device(dev);
	struct s5l_irq_probe *p = &d->probe;
	ssize_t len = 0;
	unsigned int hits;
	u64 span, avg;
	int i, nirq, irq;

	nirq = platform_irq_count(pdev);
	for (i = 0; i < nirq; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq >= 0)
			len += sysfs_emit_at(buf, len, "%s%d", i ? " " : "",
					     irq);
	}
	len += sysfs_emit_at(buf, len, "\n");

	mutex_lock(&p->lock);
	if (p->valid) {
		hits = atomic_read(&p->hits);
		span = (hits > 1 && p->last_ns > p->first_ns) ?
		       p->last_ns - p->first_ns : 0;
		avg = hits > 1 ? div_u64(span, hits - 1) : 0;
		len += sysfs_emit_at(buf, len,
			"last: irq=%d hits=%u storm=%u span_us=%llu avg_us=%llu "
			"en=0x%08x status=0x%08x rgbout=0x%08x hpd=0x%08x\n",
			p->irq, hits, p->storm,
			div_u64(span, NSEC_PER_USEC), div_u64(avg, NSEC_PER_USEC),
			p->int_en, p->int_status, p->rgbout_status, p->hpd);
	}
	mutex_unlock(&p->lock);
	return len;
}

static ssize_t irq_probe_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t count)
{
	struct s5l_display *d = dev_get_drvdata(dev);
	struct platform_device *pdev = to_platform_device(dev);
	struct s5l_irq_probe *p = &d->probe;
	unsigned int want, en = 0;
	int i, nirq, irq = -1, parsed, ret;

	parsed = sscanf(buf, "%u %x", &want, &en);
	if (parsed < 1)
		return -EINVAL;

	nirq = platform_irq_count(pdev);
	for (i = 0; i < nirq; i++) {
		int n = platform_get_irq(pdev, i);

		if (n >= 0 && n == (int)want) {
			irq = n;
			break;
		}
	}
	if (irq < 0)
		return -EINVAL;

	mutex_lock(&p->lock);
	p->irq = irq;
	atomic_set(&p->hits, 0);
	p->first_ns = 0;
	p->last_ns = 0;
	p->storm = false;
	p->valid = false;
	/*
	 * Snapshot the blocks, so a silent line still reports what it saw.
	 * The enable/status pair lives in the output stage.
	 */
	p->int_en = readl(d->out + OUT_INT_EN);
	p->int_status = readl(d->out + OUT_INT_STATUS);
	p->rgbout_status = readl(d->rgbout + RGB_INT_STATUS);
	p->hpd = readl(d->hdmi + HDMI_HPD_STATUS);
	if (parsed == 2)
		writel(en, d->out + OUT_INT_EN);

	ret = request_irq(irq, s5l_irq_probe_irq, 0, "s5l-irqprobe", d);
	if (ret)
		goto out_restore;

	msleep(S5L_IRQ_PROBE_MS);

	/* Balance the disable the handler did within the probe window */
	if (p->storm)
		enable_irq(irq);
	free_irq(irq, d);
	p->valid = true;

out_restore:
	if (parsed == 2)
		writel(p->int_en, d->out + OUT_INT_EN);
	mutex_unlock(&p->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(irq_probe);

static ssize_t scaler_status_show(struct device *dev, struct device_attribute *a,
				  char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "enabled=%u mmio=0x39200000\n", d->scaler_enabled);
}

static ssize_t scaler_status_store(struct device *dev, struct device_attribute *a,
				   const char *buf, size_t count)
{
	struct s5l_display *d = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	if (!d->scaler_clk)
		return -ENODEV;
	mutex_lock(&d->lock);
	if (on && !d->scaler_enabled)
		ret = clk_prepare_enable(d->scaler_clk);
	else if (!on && d->scaler_enabled)
		clk_disable_unprepare(d->scaler_clk);
	if (!ret)
		d->scaler_enabled = on;
	mutex_unlock(&d->lock);
	return ret ?: count;
}
static DEVICE_ATTR_RW(scaler_status);

static ssize_t hdmi_audio_show(struct device *dev, struct device_attribute *a,
			       char *buf)
{
	struct s5l_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "enabled=%u rate=%u channels=%u con0=0x%02x acr_con=0x%02x n=%u cts=%u asp_con=0x%02x aui_con=0x%02x i2s_mux=0x%02x mux_ch=0x%02x\n",
			  d->audio_enabled, d->audio_rate, d->audio_channels,
			  readl(d->hdmi + HDMI_CON_0) & 0xffU,
			  readl(d->hdmi + HDMI_ACR_CON) & 0xffU,
			  hdmi_rb(d, HDMI_ACR_N0, 3), hdmi_rb(d, HDMI_ACR_CTS0, 3),
			  readl(d->hdmi + HDMI_ASP_CON) & 0xffU,
			  readl(d->hdmi + HDMI_AUI_CON) & 0xffU,
			  readl(d->hdmi + HDMI_I2S_MUX_CON) & 0xffU,
			  readl(d->hdmi + HDMI_I2S_MUX_CH) & 0xffU);
}

static ssize_t hdmi_audio_store(struct device *dev, struct device_attribute *a,
				const char *buf, size_t count)
{
	unsigned int rate, channels = 2;
	char tmp[32];
	const char *sep;
	int ret;

	if (sysfs_streq(buf, "off")) {
		rate = 0;
	} else {
		/* Accept "rate" or "rate:channels" (channels 2..8, even) */
		sep = strchr(buf, ':');
		if (sep) {
			size_t len = min_t(size_t, sep - buf, sizeof(tmp) - 1);

			memcpy(tmp, buf, len);
			tmp[len] = '\0';
			if (kstrtouint(tmp, 0, &rate) ||
			    kstrtouint(sep + 1, 0, &channels))
				return -EINVAL;
		} else if (kstrtouint(buf, 0, &rate)) {
			return -EINVAL;
		}
		if (rate && (!channels || channels > 8 || (channels & 1)))
			return -EINVAL;
	}
	ret = s5l8947x_hdmi_audio_configure(rate, channels, rate != 0);
	if (ret)
		return ret;
	if (rate)
		s5l8947x_hdmi_audio_arm(rate);
	return count;
}
static DEVICE_ATTR_RW(hdmi_audio);

static struct attribute *s5l_attrs[] = {
	&dev_attr_hpd_connected.attr,
	&dev_attr_phy_ready.attr,
	&dev_attr_plane_phys.attr,
	&dev_attr_plane_info.attr,
	&dev_attr_gamma_brightness.attr,
	&dev_attr_irq_probe.attr,
	&dev_attr_scaler_status.attr,
	&dev_attr_hdmi_audio.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l);

/* --- probe --- */

static void s5l_clks_off(void *data)
{
	struct s5l_display *d = data;

	if (d->scaler_enabled)
		clk_disable_unprepare(d->scaler_clk);
	clk_bulk_disable_unprepare(ARRAY_SIZE(d->clks), d->clks);
}

static void s5l_put_ddc(void *data)
{
	i2c_put_adapter(data);
}

static int s5l_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *ddc_node;
	struct s5l_display *d;
	struct drm_device *drm;
	const char *preferred;
	unsigned int i;
	int ret;

	ddc_node = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (!ddc_node)
		return dev_err_probe(dev, -ENODEV, "no ddc-i2c-bus\n");

	d = devm_drm_dev_alloc(dev, &s5l_drm_driver, struct s5l_display, drm);
	if (IS_ERR(d)) {
		of_node_put(ddc_node);
		return PTR_ERR(d);
	}
	drm = &d->drm;
	d->dev = dev;
	d->gamma_brightness = 100;
	d->tmds_hz = 74250000;
	d->audio_channels = 2;
	mutex_init(&d->lock);
	mutex_init(&d->probe.lock);
	platform_set_drvdata(pdev, d);

	d->ddc = of_get_i2c_adapter_by_node(ddc_node);
	of_node_put(ddc_node);
	if (!d->ddc)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(dev, s5l_put_ddc, d->ddc);
	if (ret)
		return ret;

	d->rgbout = devm_platform_ioremap_resource_byname(pdev, "rgbout");
	if (IS_ERR(d->rgbout))
		return PTR_ERR(d->rgbout);
	d->out = devm_platform_ioremap_resource_byname(pdev, "out");
	if (IS_ERR(d->out))
		return PTR_ERR(d->out);
	d->hdmi = devm_platform_ioremap_resource_byname(pdev, "hdmi");
	if (IS_ERR(d->hdmi))
		return PTR_ERR(d->hdmi);

	for (i = 0; i < ARRAY_SIZE(d->clks); i++)
		d->clks[i].id = s5l_clk_names[i];
	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(d->clks), d->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");
	d->scaler_clk = devm_clk_get_optional(dev, "scaler");
	if (IS_ERR(d->scaler_clk))
		return dev_err_probe(dev, PTR_ERR(d->scaler_clk), "scaler clock\n");
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(d->clks), d->clks);
	if (ret)
		return ret;
	if (d->scaler_clk && !clk_prepare_enable(d->scaler_clk))
		d->scaler_enabled = true;
	ret = devm_add_action_or_reset(dev, s5l_clks_off, d);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/*
	 * The board may name the mode to start in; a value given on the
	 * command line or written to the module parameter takes precedence.
	 */
	if (!s5l_preferred_mode[0] &&
	    !device_property_read_string(dev, "apple,preferred-mode", &preferred))
		strscpy(s5l_preferred_mode, preferred, sizeof(s5l_preferred_mode));

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = 640;
	drm->mode_config.min_height = 480;
	drm->mode_config.max_width = S5L_MAX_W;
	drm->mode_config.max_height = S5L_MAX_H;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &s5l_mode_config_funcs;

	ret = drm_connector_init_with_ddc(drm, &d->connector, &s5l_conn_funcs,
					  DRM_MODE_CONNECTOR_HDMIA, d->ddc);
	if (ret)
		return ret;
	drm_connector_helper_add(&d->connector, &s5l_conn_helper_funcs);
	d->connector.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_simple_display_pipe_init(drm, &d->pipe, &s5l_pipe_funcs,
					   s5l_formats, ARRAY_SIZE(s5l_formats),
					   NULL, &d->connector);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);

	hrtimer_init(&d->vblank_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	d->vblank_timer.function = s5l_vblank_timer;
	if (vblank_sim) {
		ret = drm_vblank_init(drm, 1);
		if (ret)
			return ret;
	}

	/* Take over from the boot framebuffer the loader set up */
	ret = aperture_remove_all_conflicting_devices(s5l_drm_driver.name);
	if (ret)
		return ret;

	g_s5l_display = d;
	d->hpd_present = readl(d->hdmi + HDMI_HPD_STATUS) & 1;
	INIT_DELAYED_WORK(&d->hpd_work, s5l_hpd_poll);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		g_s5l_display = NULL;
		return ret;
	}
	schedule_delayed_work(&d->hpd_work, msecs_to_jiffies(500));
	drm_fbdev_dma_setup(drm, 32);
	return 0;
}

static void s5l_remove(struct platform_device *pdev)
{
	struct s5l_display *d = platform_get_drvdata(pdev);

	hrtimer_cancel(&d->vblank_timer);
	cancel_delayed_work_sync(&d->hpd_work);
	drm_dev_unplug(&d->drm);
	drm_atomic_helper_shutdown(&d->drm);
	g_s5l_display = NULL;
}

static void s5l_shutdown(struct platform_device *pdev)
{
	struct s5l_display *d = platform_get_drvdata(pdev);

	hrtimer_cancel(&d->vblank_timer);
	drm_atomic_helper_shutdown(&d->drm);
}

static const struct of_device_id s5l_of_match[] = {
	{ .compatible = "apple,s5l8947x-display-pipe" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_of_match);

static struct platform_driver s5l_platform_driver = {
	.probe		= s5l_probe,
	.remove		= s5l_remove,
	.shutdown	= s5l_shutdown,
	.driver	= {
		.name		= "s5l8947x-display",
		.of_match_table	= s5l_of_match,
		.dev_groups	= s5l_groups,
	},
};
module_platform_driver(s5l_platform_driver);

MODULE_DESCRIPTION("S5L8947X display pipe and HDMI DRM driver");
MODULE_LICENSE("GPL");
