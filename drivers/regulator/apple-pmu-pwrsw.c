// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dialog PMU GPIO power-switch regulator driver
 *
 * Controls PMU GPIO output registers (such as D1815 GPIO 8 at 0x58 for
 * LAN8742 Ethernet PHY reset and GPIO 7 at 0x57 for BCM4334 Wi-Fi REG_ON).
 * On D1815, register 0x50 + n configures GPIO n, and bit 1 (0x02) sets the
 * output level.
 */

#include <linux/apple-pmu-i2c.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

struct apple_pmu_pwrsw {
	struct device *dev;
	struct apple_pmu *pmu;
	struct regulator_desc desc;
	struct regulator_config config;
	struct regulator_init_data init_data;
	struct regulator_dev *rdev;
	u32 base;
	u32 mask;
	u32 enable;
	int was_enabled;
};

static int apple_pmu_pwrsw_enable(struct regulator_dev *rdev)
{
	struct apple_pmu_pwrsw *pwrsw = rdev_get_drvdata(rdev);
	int ret;

	ret = regulator_enable_regmap(rdev);
	if (!ret) {
		pwrsw->was_enabled = 1;
		mdelay(1);
	}
	return ret;
}

static int apple_pmu_pwrsw_disable(struct regulator_dev *rdev)
{
	return regulator_disable_regmap(rdev);
}

static int apple_pmu_pwrsw_is_enabled(struct regulator_dev *rdev)
{
	struct apple_pmu_pwrsw *pwrsw = rdev_get_drvdata(rdev);

	if (!pwrsw->was_enabled)
		return 0;
	return regulator_is_enabled_regmap(rdev);
}

static const struct regulator_ops apple_pmu_pwrsw_ops = {
	.enable		= apple_pmu_pwrsw_enable,
	.disable	= apple_pmu_pwrsw_disable,
	.is_enabled	= apple_pmu_pwrsw_is_enabled,
};

static int apple_pmu_pwrsw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apple_pmu_pwrsw *pwrsw;
	const char *supply_name = NULL;

	if (!dev->parent)
		return -EINVAL;

	pwrsw = devm_kzalloc(dev, sizeof(*pwrsw), GFP_KERNEL);
	if (!pwrsw)
		return -ENOMEM;

	pwrsw->pmu = dev_get_drvdata(dev->parent);
	if (!pwrsw->pmu)
		return -EINVAL;
	pwrsw->dev = dev;

	if (of_property_read_u32(np, "reg", &pwrsw->base))
		return -ENODEV;

	pwrsw->mask = 0x02;
	of_property_read_u32(np, "mask", &pwrsw->mask);

	pwrsw->enable = pwrsw->mask;
	if (of_property_read_u32(np, "enable", &pwrsw->enable))
		of_property_read_u32(np, "apple,active-val", &pwrsw->enable);

	of_property_read_string(np, "supply-name", &supply_name);

	platform_set_drvdata(pdev, pwrsw);

	pwrsw->desc.name = np->name;
	pwrsw->desc.supply_name = supply_name;
	pwrsw->desc.ops = &apple_pmu_pwrsw_ops;
	pwrsw->desc.type = REGULATOR_VOLTAGE;
	pwrsw->desc.owner = THIS_MODULE;
	pwrsw->desc.enable_reg = pwrsw->base;
	pwrsw->desc.enable_mask = pwrsw->mask;
	pwrsw->desc.enable_val = pwrsw->enable;
	pwrsw->desc.disable_val = pwrsw->enable ^ pwrsw->mask;

	pwrsw->init_data.constraints.valid_ops_mask = REGULATOR_CHANGE_STATUS;
	pwrsw->config.dev = dev;
	pwrsw->config.driver_data = pwrsw;
	pwrsw->config.init_data = &pwrsw->init_data;
	pwrsw->config.of_node = np;
	pwrsw->config.regmap = pwrsw->pmu->regmap;

	pwrsw->rdev = devm_regulator_register(dev, &pwrsw->desc, &pwrsw->config);
	if (IS_ERR(pwrsw->rdev))
		return PTR_ERR(pwrsw->rdev);

	dev_info(dev, "PMU power switch '%s' @ reg 0x%02x (mask=0x%02x, enable=0x%02x)\n",
		 pwrsw->desc.name, pwrsw->base, pwrsw->mask, pwrsw->enable);
	return 0;
}

static const struct of_device_id apple_pmu_pwrsw_of_match[] = {
	{ .compatible = "apple,pmu-pwrsw" },
	{ .compatible = "apple,pmu-power-switch" },
	{ },
};
MODULE_DEVICE_TABLE(of, apple_pmu_pwrsw_of_match);

static struct platform_driver apple_pmu_pwrsw_driver = {
	.probe = apple_pmu_pwrsw_probe,
	.driver = {
		.name		= "apple-pmu-pwrsw",
		.of_match_table	= apple_pmu_pwrsw_of_match,
	},
};
module_platform_driver(apple_pmu_pwrsw_driver);

MODULE_DESCRIPTION("Dialog PMU GPIO power-switch regulator driver");
MODULE_LICENSE("GPL");
