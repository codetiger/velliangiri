// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X USB 2.0 OTG PHY and EHCI/HSIC Host PHY driver
 *
 * Provides two PHYs via #phy-cells = <1>:
 *   - index 0: USB 2.0 Synopsys OTG PHY at 0x36000000 (paired with dwc2 @ 0x36100000)
 *   - index 1: USB 2.0 Host / HSIC PHY (paired with ehci @ 0x37000000 / ohci @ 0x37100000)
 */

#define pr_fmt(fmt) "s5l-usb-phy: " fmt

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define OPHYPWR			0x00
#define OPHYCLK			0x04
#define ORSTCON			0x08
#define OPHYUNK1		0x1c
#define OPHYUNK2		0x20
#define OPHYUNK4		0x48
#define OPHYTUNE1		0x50
#define OPHYTUNE2		0x54

/* PMGR registers for S5L8947X USB clocks and host PHY power */
#define S5L_PMGR_PHYS		0x3f100000UL
#define S5L_PMGR_SIZE		0x2000UL
#define PMGR_GATE(g)		(0x01c0 + ((g) * 4))
#define  PMGR_GATE_ON		0x0000000fU
#define PMGR_USB_HOST_PHY_CFG0	0x1088
#define PMGR_USB_HOST_PHY_CFG1	0x108c

/* DWC2 PCGCCTL register */
#define S5L_OTG_PCGCCTL_PHYS	0x36100e00UL

struct s5l_usb_phy {
	void __iomem *base;
	struct device *dev;
	u32 clk_sel;
	u32 tune1;
	u32 tune2;
};

static void s5l_usb_pmgr_ungate_otg(void)
{
	void __iomem *pmgr;
	void __iomem *pcgcctl;

	pmgr = ioremap(S5L_PMGR_PHYS, S5L_PMGR_SIZE);
	if (pmgr) {
		/* Enable usb-complex (30), usb-otg (31), and usb20phy (36) */
		writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(30));
		writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(31));
		writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(36));
		dsb(sy);
		udelay(10);
		iounmap(pmgr);
	}

	pcgcctl = ioremap(S5L_OTG_PCGCCTL_PHYS, 0x10);
	if (pcgcctl) {
		writel_relaxed(0, pcgcctl);
		dsb(sy);
		iounmap(pcgcctl);
	}
}

static int s5l_usb_phy_power_on(struct phy *phy)
{
	struct s5l_usb_phy *uphy = phy_get_drvdata(phy);
	void __iomem *b = uphy->base;

	s5l_usb_pmgr_ungate_otg();

	writel_relaxed(0x200, b + OPHYUNK4);
	/* Assert PHY reset, configure power/clock/tuning, then release reset */
	writel_relaxed(1, b + ORSTCON);
	writel_relaxed(0x6, b + OPHYPWR);
	writel_relaxed(uphy->clk_sel, b + OPHYCLK);
	writel_relaxed(uphy->tune1, b + OPHYTUNE1);
	writel_relaxed(uphy->tune2, b + OPHYTUNE2);
	udelay(20);

	writel_relaxed(0, b + ORSTCON);
	mdelay(1);

	writel_relaxed(0x600, b + OPHYUNK2);
	writel_relaxed(readl_relaxed(b + OPHYUNK1) | BIT(1), b + OPHYUNK1);
	mdelay(3);

	dev_info(uphy->dev, "OTG PHY powered on (PWR=%#x CLK=%#x TUNE1=%#x TUNE2=%#x)\n",
		 readl_relaxed(b + OPHYPWR), readl_relaxed(b + OPHYCLK),
		 readl_relaxed(b + OPHYTUNE1), readl_relaxed(b + OPHYTUNE2));
	return 0;
}

static int s5l_usb_phy_power_off(struct phy *phy)
{
	struct s5l_usb_phy *uphy = phy_get_drvdata(phy);
	void __iomem *b = uphy->base;

	writel_relaxed(0xff, b + OPHYPWR);
	udelay(10);
	writel_relaxed(7, b + ORSTCON);
	writel_relaxed(readl_relaxed(b + OPHYUNK1) & ~BIT(1), b + OPHYUNK1);
	return 0;
}

static const struct phy_ops s5l_usb_phy_ops = {
	.power_on	= s5l_usb_phy_power_on,
	.power_off	= s5l_usb_phy_power_off,
	.owner		= THIS_MODULE,
};

static int s5l_hostphy_power_on(struct phy *phy)
{
	struct s5l_usb_phy *uphy = phy_get_drvdata(phy);
	void __iomem *pmgr;
	u32 v;

	pmgr = ioremap(S5L_PMGR_PHYS, S5L_PMGR_SIZE);
	if (!pmgr)
		return -ENOMEM;

	/* Enable usb-complex (30), usb-ehci (32), usb-ohci (33), usb20phy (36), hsic0phy (90) */
	writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(30));
	writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(32));
	writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(33));
	writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(36));
	writel_relaxed(PMGR_GATE_ON, pmgr + PMGR_GATE(90));
	dsb(sy);
	udelay(20);

	v = readl_relaxed(pmgr + PMGR_USB_HOST_PHY_CFG0);
	v &= ~0x000000ffU;
	v |= 0x00000008U;
	writel_relaxed(v, pmgr + PMGR_USB_HOST_PHY_CFG0);
	dsb(sy);
	udelay(200);

	v = readl_relaxed(uphy->base + 0x80);
	if ((v & 0x4) == 0) {
		writel_relaxed(v | 0x4, uphy->base + 0x80);
		dsb(sy);
		udelay(50);
	}

	iounmap(pmgr);
	return 0;
}

static int s5l_hostphy_power_off(struct phy *phy)
{
	return 0;
}

static const struct phy_ops s5l_hostphy_ops = {
	.power_on	= s5l_hostphy_power_on,
	.power_off	= s5l_hostphy_power_off,
	.owner		= THIS_MODULE,
};

static struct phy *s5l_usb_phy_xlate(struct device *dev,
				     const struct of_phandle_args *args)
{
	struct phy **phys = dev_get_drvdata(dev);
	unsigned int idx = 0;

	if (args->args_count >= 1)
		idx = args->args[0];
	if (idx > 1 || !phys[idx])
		return ERR_PTR(-ENODEV);
	return phys[idx];
}

static int s5l_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct s5l_usb_phy *uphy;
	struct phy_provider *provider;
	struct phy **phys;

	uphy = devm_kzalloc(dev, sizeof(*uphy), GFP_KERNEL);
	if (!uphy)
		return -ENOMEM;

	phys = devm_kcalloc(dev, 2, sizeof(*phys), GFP_KERNEL);
	if (!phys)
		return -ENOMEM;

	uphy->dev = dev;
	uphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(uphy->base))
		return PTR_ERR(uphy->base);

	/* Defaults verified on S5L8947X (j33iap) */
	uphy->clk_sel = 0x0;
	uphy->tune1 = 0x349;
	uphy->tune2 = 0x6ff3;

	of_property_read_u32(np, "apple,phy-clk-sel", &uphy->clk_sel);
	of_property_read_u32(np, "apple,phy-tune1", &uphy->tune1);
	of_property_read_u32(np, "apple,phy-tune2", &uphy->tune2);

	phys[0] = devm_phy_create(dev, np, &s5l_usb_phy_ops);
	if (IS_ERR(phys[0]))
		return PTR_ERR(phys[0]);
	phy_set_drvdata(phys[0], uphy);

	phys[1] = devm_phy_create(dev, np, &s5l_hostphy_ops);
	if (IS_ERR(phys[1]))
		return PTR_ERR(phys[1]);
	phy_set_drvdata(phys[1], uphy);

	dev_set_drvdata(dev, phys);

	provider = devm_of_phy_provider_register(dev, s5l_usb_phy_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id s5l_usb_phy_of_match[] = {
	{ .compatible = "apple,s5l-usb-phy" },
	{ .compatible = "apple,s5l8940x-usb-phy" },
	{ .compatible = "apple,s5l8947x-usb-phy" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l_usb_phy_of_match);

static struct platform_driver s5l_usb_phy_driver = {
	.probe	= s5l_usb_phy_probe,
	.driver	= {
		.name		= "apple-s5l-usb-phy",
		.of_match_table	= s5l_usb_phy_of_match,
	},
};
module_platform_driver(s5l_usb_phy_driver);

MODULE_DESCRIPTION("S5L8940X/S5L8947X USB 2.0 OTG and Host PHY driver");
MODULE_LICENSE("GPL");
