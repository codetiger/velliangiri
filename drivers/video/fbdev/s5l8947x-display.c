// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Display Pipe (rgbout), Output Gamma Stage, HDMI 1.4 Transmitter,
 * and M2M Hardware Scaler Driver
 *
 * Hardware blocks:
 *   - rgbout  @ 0x3a200000: Display pipe & Plane 0 ARGB8888 scanout DMA
 *   - out     @ 0x39600000: Timing generator & 3x256 10-bit Gamma LUTs
 *   - hdmi    @ 0x39700000: HDMI 1.4 transmitter, PHY, AVI/Audio InfoFrames
 *   - scaler  @ 0x39200000: M2M YUV/RGB hardware scaler
 *
 * The pipe and HDMI clocks stay on while the driver is bound; the scaler
 * clock is switched through the scaler_status attribute.
 */

#define pr_fmt(fmt) "s5l8947x-display: " fmt

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <linux/slab.h>

/* rgbout registers (0x3a200000) */
#define RGB_SIZE		0x1030
#define RGB_CTL			0x1038
#define RGB_CFG			0x104c
#define RGB_COMMIT		0x2040
#define RGB_PLANE0_BASE		0x4000
#define RGB_PLANE_CTL		0x0040
#define RGB_PLANE_ADDR		0x0044
#define RGB_PLANE_STRIDE	0x0048
#define RGB_PLANE_WH		0x0050
#define RGB_COMMIT_VAL		0xffff0202U

/* Output stage registers (0x39600000) */
#define OUT_START		0x0000
#define OUT_CFG			0x0004
#define OUT_VSIZE		0x0034
#define OUT_HSIZE		0x0040
#define OUT_GAMMA_EN		0x004c
#define OUT_GAMMA_IDX		0x0054
#define OUT_GAMMA_VAL		0x0058

/* HDMI 1.4 controller registers (0x39700000) */
#define HDMI_HPD_STATUS		0x0000c
#define HDMI_PHY_STATUS_0	0x00020
#define HDMI_AUDIO_CLKSEL	0x00070
#define HDMI_CON_0		0x10000
#define HDMI_CON_1		0x10004
#define HDMI_CON_2		0x10008
#define HDMI_MODE_SEL		0x10040
#define HDMI_GCP_CON		0x10200
#define HDMI_ASP_CON		0x10300
#define HDMI_ASP_SP_FLAT	0x10304
#define HDMI_ASP_CHCFG0		0x10310
#define HDMI_ASP_CHCFG1		0x10314
#define HDMI_ASP_CHCFG2		0x10318
#define HDMI_ASP_CHCFG3		0x1031c
#define HDMI_ACR_CON		0x10400
#define HDMI_ACR_MCTS0		0x10410
#define HDMI_ACR_CTS0		0x10420
#define HDMI_ACR_N0		0x10430
#define HDMI_AUI_CON		0x10800
#define HDMI_AUI_HEADER0	0x10810
#define HDMI_AUI_HEADER1	0x10814
#define HDMI_AUI_HEADER2	0x10818
#define HDMI_AUI_CHECK_SUM	0x1081c
#define HDMI_AUI_BYTE1		0x10820

/* HDMI internal I2S receiver & subpacket mux (0x39740000) */
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

#define S5L_FB_BASE_MIN		0x9ec00000U
#define S5L_FB_BASE_MAX		0x9f000000U
#define S5L_FB_PAGE_BYTES	(1280U * 720U * 4U)

static const char * const s5l8947x_display_clk_names[] = {
	"hperf-rt-a", "rgbout-a", "hperf-rt-b", "rgbout-b", "hdmi",
};

struct s5l8947x_display {
	struct device *dev;
	void __iomem *rgbout;
	void __iomem *out;
	void __iomem *hdmi;
	void __iomem *scaler;
	struct clk_bulk_data clks[ARRAY_SIZE(s5l8947x_display_clk_names)];
	struct clk *scaler_clk;
	struct mutex lock;
	u32 plane_phys;
	unsigned int gamma_brightness;
	bool scaler_enabled;
	bool audio_enabled;
	unsigned int audio_rate;
};

static struct s5l8947x_display *g_s5l_display;

static void s5l8947x_hdmi_write_bytes(void __iomem *base, u32 off, u32 val,
				      int nbytes)
{
	while (nbytes-- > 0) {
		writel(val & 0xffU, base + off);
		off += 4;
		val >>= 8;
	}
}

static void s5l8947x_display_program_gamma(struct s5l8947x_display *d,
					   unsigned int pct)
{
	u32 base_idx;
	int c, i;

	if (!d->out)
		return;
	if (pct > 100)
		pct = 100;

	base_idx = readl(d->out + OUT_GAMMA_IDX) & 0xfffe0f00U;
	writel(base_idx, d->out + OUT_GAMMA_IDX);
	for (c = 0; c < 3; c++) {
		for (i = 0; i < 256; i++) {
			u32 v = ((u32)i * 1023U * pct) / (255U * 100U);

			if (v > 1023U)
				v = 1023U;
			writel(base_idx | (u32)i | (0x14000U + c * 0x1000U),
			       d->out + OUT_GAMMA_IDX);
			writel(v & 0xfffffU, d->out + OUT_GAMMA_VAL);
		}
	}
	writel(9, d->out + OUT_GAMMA_EN);
}

int s5l8947x_hdmi_audio_configure(unsigned int sample_rate, bool enable)
{
	struct s5l8947x_display *d = g_s5l_display;
	u32 n_val, cts_val, con0;
	u8 aui[5] = { 0x01, 0x00, 0x00, 0x00, 0x00 };
	u32 csum;
	int i;

	if (!d || !d->hdmi)
		return -ENODEV;

	switch (sample_rate) {
	case 44100:
		n_val = 6272U;
		cts_val = 82500U;
		break;
	case 32000:
		n_val = 4096U;
		cts_val = 74250U;
		break;
	case 48000:
	default:
		sample_rate = 48000;
		n_val = 6144U;
		cts_val = 74250U;
		break;
	}

	mutex_lock(&d->lock);
	if (!enable) {
		writel(0x00, d->hdmi + HDMI_ACR_CON);
		writel(0x00, d->hdmi + HDMI_ASP_CON);
		writel(0x00, d->hdmi + HDMI_AUI_CON);
		con0 = readl(d->hdmi + HDMI_CON_0) & 0xffU;
		writel(con0 & ~0x04U, d->hdmi + HDMI_CON_0);
		d->audio_enabled = false;
		mutex_unlock(&d->lock);
		return 0;
	}

	/* Ensure HDMI mode (not DVI) and guard-band/preamble are enabled for data islands */
	writel(0x02, d->hdmi + HDMI_MODE_SEL);
	writel(0x00, d->hdmi + HDMI_CON_2);
	writel(0x00, d->hdmi + HDMI_AUDIO_CLKSEL);

	/* Program HDMI 1.4 Audio Clock Regeneration (ACR) N and CTS for 74.25 MHz TMDS */
	s5l8947x_hdmi_write_bytes(d->hdmi, HDMI_ACR_N0, n_val, 3);
	s5l8947x_hdmi_write_bytes(d->hdmi, HDMI_ACR_MCTS0, cts_val, 3);
	s5l8947x_hdmi_write_bytes(d->hdmi, HDMI_ACR_CTS0, cts_val, 3);
	writel(0x04, d->hdmi + HDMI_ACR_CON);

	/* Enable 2-channel LPCM Audio Sample Packet (ASP) at HDMI 1.4 offset 0x10300 */
	writel(0x00, d->hdmi + HDMI_ASP_SP_FLAT);
	writel(0x08, d->hdmi + HDMI_ASP_CHCFG0);
	writel(0x1a, d->hdmi + HDMI_ASP_CHCFG1);
	writel(0x2c, d->hdmi + HDMI_ASP_CHCFG2);
	writel(0x3e, d->hdmi + HDMI_ASP_CHCFG3);
	writel(0x00, d->hdmi + HDMI_ASP_CON);

	/* Enable HDMI 1.4 internal I2S receiver and subpacket mux (0x39740000) */
	writel(0x00, d->hdmi + HDMI_I2S_CLK_CON);
	writel(0x01, d->hdmi + HDMI_I2S_CLK_CON);
	writel(readl(d->hdmi + HDMI_I2S_DSD_CON) | 0x01U, d->hdmi + HDMI_I2S_DSD_CON);
	writel(0x56, d->hdmi + HDMI_I2S_PIN_SEL_0); /* SCLK=5, LRCK=6 */
	writel(0x34, d->hdmi + HDMI_I2S_PIN_SEL_1); /* SDATA1=3, SDATA0=4 */
	writel(0x12, d->hdmi + HDMI_I2S_PIN_SEL_2); /* SDATA3=1, SDATA2=2 */
	writel(0x00, d->hdmi + HDMI_I2S_PIN_SEL_3); /* DSD=0 */
	writel(0x00, d->hdmi + HDMI_I2S_CON_1);     /* Falling edge SCLK, low=L */
	writel(0x04, d->hdmi + HDMI_I2S_CON_2);     /* MSB first, 32fs, 16-bit, basic I2S */
	writel(0x00, d->hdmi + HDMI_I2S_CH_ST_0);   /* Consumer LPCM */
	writel(0x00, d->hdmi + HDMI_I2S_CH_ST_1);
	writel(0x00, d->hdmi + HDMI_I2S_CH_ST_2);
	writel((sample_rate == 44100) ? 0x00 : 0x02, d->hdmi + HDMI_I2S_CH_ST_3);
	writel(0x02, d->hdmi + HDMI_I2S_CH_ST_4);   /* 16-bit word length */
	writel(0x01, d->hdmi + HDMI_I2S_CH_ST_CON); /* Reload channel status */
	writel(0xff, d->hdmi + HDMI_I2S_MUX_CH);    /* Enable all channels */
	writel(0x03, d->hdmi + HDMI_I2S_MUX_CUV);   /* Enable L+R CUV */
	writel(0x07, d->hdmi + HDMI_I2S_MUX_CON);   /* IN_ENABLE | AUD_I2S | CUV_I2S | MUX_EN */

	/* Program CEA-861 Audio InfoFrame (0x84, ver 0x01, len 0x0a) */
	writel(0x84, d->hdmi + HDMI_AUI_HEADER0);
	writel(0x01, d->hdmi + HDMI_AUI_HEADER1);
	writel(0x0a, d->hdmi + HDMI_AUI_HEADER2);
	csum = 0x84U + 0x01U + 0x0aU;
	for (i = 0; i < 5; i++) {
		csum += aui[i];
		writel(aui[i], d->hdmi + HDMI_AUI_BYTE1 + i * 4);
	}
	for (i = 5; i < 10; i++)
		writel(0x00, d->hdmi + HDMI_AUI_BYTE1 + i * 4);
	writel((0x100U - (csum & 0xffU)) & 0xffU, d->hdmi + HDMI_AUI_CHECK_SUM);
	writel(0x02, d->hdmi + HDMI_AUI_CON);

	con0 = readl(d->hdmi + HDMI_CON_0) & 0xffU;
	writel(con0 | 0x05U, d->hdmi + HDMI_CON_0);

	d->audio_enabled = true;
	d->audio_rate = sample_rate;
	mutex_unlock(&d->lock);
	return 0;
}
EXPORT_SYMBOL_GPL(s5l8947x_hdmi_audio_configure);

static ssize_t hpd_connected_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);
	u32 hpd = d->hdmi ? (readl(d->hdmi + HDMI_HPD_STATUS) & 0x01U) : 0;

	return sysfs_emit(buf, "%u\n", hpd);
}
static DEVICE_ATTR_RO(hpd_connected);

static ssize_t phy_ready_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);
	u32 rdy = d->hdmi ? (readl(d->hdmi + HDMI_PHY_STATUS_0) & 0x01U) : 0;

	return sysfs_emit(buf, "%u\n", rdy);
}
static DEVICE_ATTR_RO(phy_ready);

static ssize_t plane_phys_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);
	u32 addr = d->rgbout ?
		   readl(d->rgbout + RGB_PLANE0_BASE + RGB_PLANE_ADDR) :
		   d->plane_phys;

	return sysfs_emit(buf, "0x%08x\n", addr);
}

static ssize_t plane_phys_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);
	u32 addr;
	int ret;

	ret = kstrtou32(buf, 0, &addr);
	if (ret)
		return ret;
	if ((addr & 0x3fU) || addr < S5L_FB_BASE_MIN ||
	    addr + S5L_FB_PAGE_BYTES > S5L_FB_BASE_MAX)
		return -EINVAL;

	mutex_lock(&d->lock);
	writel(addr, d->rgbout + RGB_PLANE0_BASE + RGB_PLANE_ADDR);
	writel(RGB_COMMIT_VAL, d->rgbout + RGB_COMMIT);
	d->plane_phys = addr;
	mutex_unlock(&d->lock);
	return count;
}
static DEVICE_ATTR_RW(plane_phys);

static ssize_t gamma_brightness_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", d->gamma_brightness);
}

static ssize_t gamma_brightness_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret || val > 100)
		return -EINVAL;

	mutex_lock(&d->lock);
	s5l8947x_display_program_gamma(d, val);
	d->gamma_brightness = val;
	mutex_unlock(&d->lock);
	return count;
}
static DEVICE_ATTR_RW(gamma_brightness);

static ssize_t scaler_status_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf, "enabled=%u mmio=0x39200000\n",
			  d->scaler_enabled ? 1U : 0U);
}

static ssize_t scaler_status_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);
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
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(scaler_status);

static u32 s5l8947x_hdmi_read_bytes(void __iomem *base, u32 off, int nbytes)
{
	u32 val = 0;

	while (nbytes-- > 0)
		val |= (readl(base + off + nbytes * 4) & 0xffU) << (nbytes * 8);
	return val;
}

static ssize_t hdmi_audio_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct s5l8947x_display *d = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "enabled=%u rate=%u con0=0x%02x acr_con=0x%02x n=%u cts=%u asp_con=0x%02x aui_con=0x%02x i2s_mux=0x%02x\n",
			  d->audio_enabled ? 1U : 0U, d->audio_rate,
			  readl(d->hdmi + HDMI_CON_0) & 0xffU,
			  readl(d->hdmi + HDMI_ACR_CON) & 0xffU,
			  s5l8947x_hdmi_read_bytes(d->hdmi, HDMI_ACR_N0, 3),
			  s5l8947x_hdmi_read_bytes(d->hdmi, HDMI_ACR_CTS0, 3),
			  readl(d->hdmi + HDMI_ASP_CON) & 0xffU,
			  readl(d->hdmi + HDMI_AUI_CON) & 0xffU,
			  readl(d->hdmi + HDMI_I2S_MUX_CON) & 0xffU);
}

static ssize_t hdmi_audio_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	unsigned int rate;
	int ret;

	if (sysfs_streq(buf, "off"))
		rate = 0;
	else if (kstrtouint(buf, 0, &rate))
		return -EINVAL;

	ret = s5l8947x_hdmi_audio_configure(rate, rate != 0);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(hdmi_audio);

static struct attribute *s5l8947x_display_attrs[] = {
	&dev_attr_hpd_connected.attr,
	&dev_attr_phy_ready.attr,
	&dev_attr_plane_phys.attr,
	&dev_attr_gamma_brightness.attr,
	&dev_attr_scaler_status.attr,
	&dev_attr_hdmi_audio.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_display);

static int s5l8947x_display_probe(struct platform_device *pdev)
{
	struct s5l8947x_display *d;
	u32 wh, stride, hpd, phy;
	unsigned int i;
	int ret;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	d->dev = &pdev->dev;
	mutex_init(&d->lock);
	d->gamma_brightness = 100;
	platform_set_drvdata(pdev, d);

	d->rgbout = devm_platform_ioremap_resource_byname(pdev, "rgbout");
	if (IS_ERR(d->rgbout))
		return PTR_ERR(d->rgbout);

	d->out = devm_platform_ioremap_resource_byname(pdev, "out");
	if (IS_ERR(d->out))
		return PTR_ERR(d->out);

	d->hdmi = devm_platform_ioremap_resource_byname(pdev, "hdmi");
	if (IS_ERR(d->hdmi))
		return PTR_ERR(d->hdmi);

	d->scaler = devm_platform_ioremap_resource_byname(pdev, "scaler");
	if (IS_ERR(d->scaler))
		d->scaler = NULL;

	for (i = 0; i < ARRAY_SIZE(d->clks); i++)
		d->clks[i].id = s5l8947x_display_clk_names[i];
	ret = devm_clk_bulk_get(&pdev->dev, ARRAY_SIZE(d->clks), d->clks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to get clocks\n");
	ret = clk_bulk_prepare_enable(ARRAY_SIZE(d->clks), d->clks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to enable clocks\n");

	d->scaler_clk = devm_clk_get_optional(&pdev->dev, "scaler");
	if (IS_ERR(d->scaler_clk)) {
		clk_bulk_disable_unprepare(ARRAY_SIZE(d->clks), d->clks);
		return dev_err_probe(&pdev->dev, PTR_ERR(d->scaler_clk),
				     "failed to get scaler clock\n");
	}
	if (d->scaler && d->scaler_clk && !clk_prepare_enable(d->scaler_clk))
		d->scaler_enabled = true;

	d->plane_phys = readl(d->rgbout + RGB_PLANE0_BASE + RGB_PLANE_ADDR);
	wh = readl(d->rgbout + RGB_SIZE);
	stride = readl(d->rgbout + RGB_PLANE0_BASE + RGB_PLANE_STRIDE) & ~0x3fU;
	hpd = readl(d->hdmi + HDMI_HPD_STATUS) & 0x01U;
	phy = readl(d->hdmi + HDMI_PHY_STATUS_0) & 0x01U;

	g_s5l_display = d;

	dev_info(&pdev->dev,
		 "rgbout %ux%u (stride %u, fb 0x%08x), HDMI HPD=%u PHY=%u, M2M scaler=%s\n",
		 wh >> 16, wh & 0xffffU, stride, d->plane_phys, hpd, phy,
		 d->scaler_enabled ? "active" : "gated");
	return 0;
}

static void s5l8947x_display_remove(struct platform_device *pdev)
{
	struct s5l8947x_display *d = platform_get_drvdata(pdev);

	if (g_s5l_display == d)
		g_s5l_display = NULL;
	if (d->scaler_enabled)
		clk_disable_unprepare(d->scaler_clk);
	clk_bulk_disable_unprepare(ARRAY_SIZE(d->clks), d->clks);
}

static const struct of_device_id s5l8947x_display_of_match[] = {
	{ .compatible = "apple,s5l8947x-display-pipe" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_display_of_match);

static struct platform_driver s5l8947x_display_driver = {
	.probe	= s5l8947x_display_probe,
	.remove	= s5l8947x_display_remove,
	.driver	= {
		.name		= "s5l8947x-display",
		.of_match_table	= s5l8947x_display_of_match,
		.dev_groups	= s5l8947x_display_groups,
	},
};
module_platform_driver(s5l8947x_display_driver);

MODULE_DESCRIPTION("S5L8947X Display Pipe, HDMI Transmitter, and M2M Scaler Driver");
MODULE_LICENSE("GPL");
