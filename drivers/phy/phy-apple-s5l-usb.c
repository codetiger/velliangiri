// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X USB 2.0 OTG PHY and EHCI/HSIC Host PHY driver
 *
 * Provides two PHYs via #phy-cells = <1>:
 *   - index 0: USB 2.0 Synopsys OTG PHY at 0x36000000 (paired with dwc2 @ 0x36100000)
 *   - index 1: USB 2.0 Host / HSIC PHY (paired with ehci / ohci)
 *
 * The OTG PHY clock gates come from the PMGR clock provider and stay on
 * while the driver is bound. The host PHY still switches its PMGR power
 * domains directly; nothing uses it until the HSIC host is enabled.
 */

#define pr_fmt(fmt) "s5l-usb-phy: " fmt

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#define OPHYPWR				0x00
#define OPHYCLK				0x04
#define ORSTCON				0x08
#define OPHYUNK1			0x1c
#define OPHY_UOTGTUNE1			0x40
#define OPHY_UOTGTUNE2			0x44
#define OPHYUNK4			0x60

#define OPHYCLK_CLKSEL_MASK		0x3
#define ORSTCON_PHYSWRESET		BIT(0)
#define OPHYUNK4_START			0x200

/* PMGR registers for the S5L8947X USB host PHY power domains */
#define PMGR_BASE			0x3f100000UL
#define PMGR_GATE_ON			0x0fU
#define PMGR_USB20_HOST			0x1088U
#define PMGR_GATE_90			0x1140U
#define USBCPLX_HSIC_EN			BIT(2)

/* Defaults for the j33iap board when the DT carries no tuning */
#define OPHY_TUNE1_DEFAULT		0x349
#define OPHY_TUNE2_DEFAULT		0x6ff3

struct s5l_usbphy {
	void __iomem *base;
	void __iomem *pmgr;
	void __iomem *usbcplx;
	void __iomem *pcgcctl;
	struct regulator *hsic_supply;
	bool hsic_supply_on;
	struct regulator *lpo_supply;
	bool lpo_supply_on;
	struct device *dev;
	struct phy *phys[2];
	u32 tune1;
	u32 tune2;
	int ref_clock_sel;
};

static int s5l_pmgr_on(struct s5l_usbphy *p, u32 off)
{
	u32 v = readl(p->pmgr + off);
	int n;

	writel((v & ~0x10fU) | PMGR_GATE_ON, p->pmgr + off);
	for (n = 0; n < 1000; n++) {
		v = readl(p->pmgr + off);
		if (!((v ^ (v >> 4)) & 0xfU))
			return 0;
		udelay(1);
	}
	dev_err(p->dev, "PMGR +0x%x stuck at 0x%08x\n", off, v);
	return -ETIMEDOUT;
}

static int s5l_usbphy_init(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	u32 v;

	if (p->pcgcctl) {
		v = readl(p->pcgcctl);
		writel(v & ~0x3U, p->pcgcctl);
	}
	udelay(100);

	writel(OPHYUNK4_START, p->base + OPHYUNK4);
	writel(0x6, p->base + OPHYPWR);
	writel(0x4, p->base + OPHYUNK1);
	writel(p->tune1, p->base + OPHY_UOTGTUNE1);
	writel(p->tune2, p->base + OPHY_UOTGTUNE2);
	udelay(10);

	v = readl(p->base + OPHYCLK);
	if (p->ref_clock_sel >= 0)
		v = (v & ~OPHYCLK_CLKSEL_MASK) |
		    (p->ref_clock_sel & OPHYCLK_CLKSEL_MASK);
	writel(v, p->base + OPHYCLK);

	writel(readl(p->base + ORSTCON) | ORSTCON_PHYSWRESET, p->base + ORSTCON);
	udelay(20);
	writel(readl(p->base + ORSTCON) & ~ORSTCON_PHYSWRESET, p->base + ORSTCON);
	mdelay(1);
	writel(readl(p->base + OPHYUNK1) | BIT(1), p->base + OPHYUNK1);

	dev_info(p->dev, "OTG PHY initialized\n");
	return 0;
}

static int s5l_usbphy_exit(struct phy *phy)
{
	return 0;
}

static const struct phy_ops s5l_usbphy_ops = {
	.init	= s5l_usbphy_init,
	.exit	= s5l_usbphy_exit,
	.owner	= THIS_MODULE,
};

static int s5l_hostphy_power_on(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	int ret;

	if (!p->lpo_supply) {
		struct regulator *r = devm_regulator_get_optional(p->dev, "lpo");

		if (IS_ERR(r)) {
			if (PTR_ERR(r) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
		} else {
			p->lpo_supply = r;
		}
	}
	if (p->lpo_supply && !p->lpo_supply_on) {
		ret = regulator_enable(p->lpo_supply);
		if (ret)
			return ret;
		p->lpo_supply_on = true;
		msleep(10);
	}

	if (!p->hsic_supply) {
		struct regulator *r = devm_regulator_get_optional(p->dev, "hsic");

		if (IS_ERR(r)) {
			if (PTR_ERR(r) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
		} else {
			p->hsic_supply = r;
		}
	}
	if (p->hsic_supply && !p->hsic_supply_on) {
		ret = regulator_enable(p->hsic_supply);
		if (ret)
			return ret;
		p->hsic_supply_on = true;
		msleep(10);
	}

	ret = s5l_pmgr_on(p, PMGR_USB20_HOST);
	if (ret)
		return ret;
	s5l_pmgr_on(p, PMGR_GATE_90);
	if (p->usbcplx)
		writel(readl(p->usbcplx) | USBCPLX_HSIC_EN, p->usbcplx);

	return 0;
}

static int s5l_hostphy_power_off(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);

	if (p->hsic_supply_on) {
		regulator_disable(p->hsic_supply);
		p->hsic_supply_on = false;
	}
	if (p->lpo_supply_on) {
		regulator_disable(p->lpo_supply);
		p->lpo_supply_on = false;
	}
	return 0;
}

static const struct phy_ops s5l_hostphy_ops = {
	.power_on	= s5l_hostphy_power_on,
	.power_off	= s5l_hostphy_power_off,
	.owner		= THIS_MODULE,
};

static struct phy *s5l_usbphy_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct s5l_usbphy *p = dev_get_drvdata(dev);
	unsigned int idx = args->args_count ? args->args[0] : 0;

	if (idx >= ARRAY_SIZE(p->phys))
		return ERR_PTR(-EINVAL);
	return p->phys[idx];
}

static int s5l_usbphy_probe(struct platform_device *pdev)
{
	struct s5l_usbphy *p;
	struct phy *phy;
	struct phy_provider *provider;
	struct clk_bulk_data *clks;
	struct resource *r;
	u32 sel;
	int ret;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = &pdev->dev;
	p->tune1 = OPHY_TUNE1_DEFAULT;
	p->tune2 = OPHY_TUNE2_DEFAULT;
	p->ref_clock_sel = -1;

	device_property_read_u32(&pdev->dev, "apple,uotgtune1-device", &p->tune1);
	device_property_read_u32(&pdev->dev, "apple,uotgtune2-device", &p->tune2);
	if (!device_property_read_u32(&pdev->dev, "apple,ref-clock-sel", &sel))
		p->ref_clock_sel = sel & OPHYCLK_CLKSEL_MASK;

	ret = devm_clk_bulk_get_all_enable(&pdev->dev, &clks);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to enable clocks\n");

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	p->pmgr = devm_ioremap(&pdev->dev, PMGR_BASE, 0x2000);
	if (!p->pmgr)
		return -ENOMEM;

	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		p->usbcplx = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(p->usbcplx))
			p->usbcplx = NULL;
	}

	/* dwc2 PCGCCTL sits inside the dwc2 register block: map, don't request */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (r)
		p->pcgcctl = devm_ioremap(&pdev->dev, r->start, resource_size(r));

	platform_set_drvdata(pdev, p);

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_usbphy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, p);
	p->phys[0] = phy;

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_hostphy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, p);
	p->phys[1] = phy;

	provider = devm_of_phy_provider_register(&pdev->dev, s5l_usbphy_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id s5l_usbphy_of_match[] = {
	{ .compatible = "apple,s5l8947x-otgphy" },
	{ .compatible = "apple,s5l-usb-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l_usbphy_of_match);

static struct platform_driver s5l_usbphy_driver = {
	.probe	= s5l_usbphy_probe,
	.driver	= {
		.name		= "phy-apple-s5l-usb",
		.of_match_table	= s5l_usbphy_of_match,
	},
};
module_platform_driver(s5l_usbphy_driver);

MODULE_DESCRIPTION("S5L8940X/S5L8947X USB 2.0 OTG and Host PHY driver");
MODULE_LICENSE("GPL");
