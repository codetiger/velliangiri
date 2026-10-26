// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Display Pipe (rgbout), Output Gamma Stage, HDMI 1.4 Transmitter,
 * and M2M Hardware Scaler Driver
 *
 * Hardware blocks:
 *   - rgbout  @ 0x3a200000: Display pipe & Plane 0 ARGB8888 scanout DMA
 *   - out     @ 0x39600000: Timing generator & 3x256 10-bit Gamma LUTs
 *   - hdmi    @ 0x39700000: HDMI 1.4 transmitter, PHY, AVI/Audio InfoFrames
 *   - scaler  @ 0x39200000: M2M YUV/RGB hardware scaler (PMGR gate 26)
 */

#define pr_fmt(fmt) "s5l8947x-display: " fmt

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/s5l8947x-display.h>
#include <linux/slab.h>

/* PMGR registers for display & scaler gates */
#define PMGR_PHYS_BASE		0x3f100000UL
#define PMGR_GATE_OFFSET(n)	(0x1008U + (n) * 4U)
#define PMGR_GATE_HPERF_RT_A	15
#define PMGR_GATE_RGBOUT_A	20
#define PMGR_GATE_HPERF_RT_B	21
#define PMGR_GATE_RGBOUT_B	25
#define PMGR_GATE_SCALER	26
#define PMGR_GATE_HDMI		80

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

/* HDMI controller registers (0x39700000) */
#define HDMI_HPD_STATUS		0x0000c
#define HDMI_PHY_STATUS_0	0x00020
#define HDMI_CON_0		0x10000
#define HDMI_MODE_SEL		0x10040
#define HDMI_ASP_CON		0x10160
#define HDMI_ASP_SP_FLAT	0x10164
#define HDMI_ASP_CHCFG0		0x10170
#define HDMI_ACR_CON		0x10180
#define HDMI_ACR_MCTS0		0x10184
#define HDMI_ACR_CTS0		0x10190
#define HDMI_ACR_N0		0x1019c
#define HDMI_AUI_CON		0x10800
#define HDMI_AUI_HEADER0	0x10810
#define HDMI_AUI_HEADER1	0x10814
#define HDMI_AUI_HEADER2	0x10818
#define HDMI_AUI_CHECK_SUM	0x1081c
#define HDMI_AUI_BYTE1		0x10820

#define S5L_FB_BASE_MIN		0x9ec00000U
#define S5L_FB_BASE_MAX		0x9f000000U
#define S5L_FB_PAGE_BYTES	(1280U * 720U * 4U)

struct s5l8947x_display {
	struct device *dev;
	void __iomem *rgbout;
	void __iomem *out;
	void __iomem *hdmi;
	void __iomem *scaler;
	void __iomem *pmgr;
	struct mutex lock;
	u32 plane_phys;
	unsigned int gamma_brightness;
	bool scaler_enabled;
	bool audio_enabled;
	unsigned int audio_rate;
};

static struct s5l8947x_display *g_s5l_display;

static int s5l8947x_pmgr_gate_set(struct s5l8947x_display *d, unsigned int gate,
				  bool on)
{
	u32 off = PMGR_GATE_OFFSET(gate);
	u32 v;
	int i;

	if (!d->pmgr)
		return -ENODEV;

	v = readl(d->pmgr + off);
	if (!v)
		return -ENODEV;
	writel((v & ~0x10fU) | (on ? 0x0fU : 0x00U), d->pmgr + off);
	for (i = 0; i < 10000; i++) {
		v = readl(d->pmgr + off);
		if (!((v ^ (v >> 4)) & 0x0fU))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

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

	/* Program Audio Clock Regeneration (ACR) N and CTS for 74.25 MHz TMDS */
	s5l8947x_hdmi_write_bytes(d->hdmi, HDMI_ACR_N0, n_val, 3);
	s5l8947x_hdmi_write_bytes(d->hdmi, HDMI_ACR_MCTS0, cts_val, 3);
	s5l8947x_hdmi_write_bytes(d->hdmi, HDMI_ACR_CTS0, cts_val, 3);
	writel(0x04, d->hdmi + HDMI_ACR_CON);

	/* Enable 2-channel LPCM Audio Sample Packet (ASP) */
	writel(0x00, d->hdmi + HDMI_ASP_SP_FLAT);
	writel(0x08, d->hdmi + HDMI_ASP_CHCFG0);
	writel(0x01, d->hdmi + HDMI_ASP_CON);

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
	u32 gate = d->pmgr ? readl(d->pmgr + PMGR_GATE_OFFSET(PMGR_GATE_SCALER)) : 0;

	return sysfs_emit(buf, "enabled=%u pmgr_gate=0x%08x mmio=0x39200000\n",
			  d->scaler_enabled ? 1U : 0U, gate);
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

	mutex_lock(&d->lock);
	ret = s5l8947x_pmgr_gate_set(d, PMGR_GATE_SCALER, on);
	if (!ret)
		d->scaler_enabled = on;
	mutex_unlock(&d->lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(scaler_status);

static struct attribute *s5l8947x_display_attrs[] = {
	&dev_attr_hpd_connected.attr,
	&dev_attr_phy_ready.attr,
	&dev_attr_plane_phys.attr,
	&dev_attr_gamma_brightness.attr,
	&dev_attr_scaler_status.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_display);

static int s5l8947x_display_probe(struct platform_device *pdev)
{
	struct s5l8947x_display *d;
	u32 wh, stride, hpd, phy;

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

	d->pmgr = devm_ioremap(&pdev->dev, PMGR_PHYS_BASE, 0x2000);
	if (!d->pmgr)
		return -ENOMEM;

	/* Ensure display pipe and M2M hardware scaler clock gates are active */
	s5l8947x_pmgr_gate_set(d, PMGR_GATE_HPERF_RT_A, true);
	s5l8947x_pmgr_gate_set(d, PMGR_GATE_HPERF_RT_B, true);
	s5l8947x_pmgr_gate_set(d, PMGR_GATE_RGBOUT_A, true);
	s5l8947x_pmgr_gate_set(d, PMGR_GATE_RGBOUT_B, true);
	s5l8947x_pmgr_gate_set(d, PMGR_GATE_HDMI, true);
	if (d->scaler && s5l8947x_pmgr_gate_set(d, PMGR_GATE_SCALER, true) == 0)
		d->scaler_enabled = true;

	d->plane_phys = readl(d->rgbout + RGB_PLANE0_BASE + RGB_PLANE_ADDR);
	wh = readl(d->rgbout + RGB_SIZE);
	stride = readl(d->rgbout + RGB_PLANE0_BASE + RGB_PLANE_STRIDE) & ~0x3fU;
	hpd = readl(d->hdmi + HDMI_HPD_STATUS) & 0x01U;
	phy = readl(d->hdmi + HDMI_PHY_STATUS_0) & 0x01U;

	g_s5l_display = d;
	(void)s5l8947x_hdmi_audio_configure(48000, true);

	dev_info(&pdev->dev,
		 "rgbout %ux%u (stride %u, fb 0x%08x), HDMI HPD=%u PHY=%u, M2M scaler=%s\n",
		 wh >> 16, wh & 0xffffU, stride, d->plane_phys, hpd, phy,
		 d->scaler_enabled ? "active" : "gated");
	return 0;
}

static void s5l8947x_display_remove(struct platform_device *pdev)
{
	if (g_s5l_display == platform_get_drvdata(pdev))
		g_s5l_display = NULL;
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
