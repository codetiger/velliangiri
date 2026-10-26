// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dialog PMU GPIO power-switch regulator driver
 *
 * Controls PMU GPIO output registers (such as D1815 GPIO 8 at 0x58 for
 * LAN8742 Ethernet PHY reset and GPIO 7 at 0x57 for BCM4334 Wi-Fi REG_ON).
 */

#include <linux/apple-pmu-i2c.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/slab.h>

struct apple_pmu_pwrsw {
	struct device *dev;
	struct apple_pmu *pmu;
	struct regulator_desc desc;
	struct regulator_dev *rdev;
};

static const struct regulator_ops apple_pmu_pwrsw_ops = {
	.enable		= regulator_enable_regmap,
	.disable	= regulator_disable_regmap,
	.is_enabled	= regulator_is_enabled_regmap,
};

static int apple_pmu_pwrsw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct apple_pmu_pwrsw *pwrsw;
	struct regulator_config config = { };
	const struct regulator_init_data *init_data;
	u32 reg, mask = 1, active_val, inactive_val;
	int ret;

	if (!dev->parent)
		return -EINVAL;

	pwrsw = devm_kzalloc(dev, sizeof(*pwrsw), GFP_KERNEL);
	if (!pwrsw)
		return -ENOMEM;

	pwrsw->pmu = dev_get_drvdata(dev->parent);
	if (!pwrsw->pmu)
		return -EINVAL;
	pwrsw->dev = dev;

	init_data = of_get_regulator_init_data(dev, np, &pwrsw->desc);
	if (!init_data)
		return -ENOMEM;

	ret = of_property_read_u32(np, "reg", &reg);
	if (ret)
		return ret;
	of_property_read_u32(np, "mask", &mask);

	pwrsw->desc.name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn", np);
	pwrsw->desc.ops = &apple_pmu_pwrsw_ops;
	pwrsw->desc.type = REGULATOR_VOLTAGE;
	pwrsw->desc.owner = THIS_MODULE;
	pwrsw->desc.enable_reg = reg;
	pwrsw->desc.enable_mask = mask;

	if (!of_property_read_u32(np, "apple,active-val", &active_val)) {
		if (of_property_read_u32(np, "apple,inactive-val", &inactive_val))
			inactive_val = 0;
		pwrsw->desc.enable_val = active_val;
		pwrsw->desc.disable_val = inactive_val;
	}

	config.dev = dev;
	config.init_data = init_data;
	config.driver_data = pwrsw;
	config.of_node = np;
	config.regmap = pwrsw->pmu->regmap;

	pwrsw->rdev = devm_regulator_register(dev, &pwrsw->desc, &config);
	if (IS_ERR(pwrsw->rdev))
		return PTR_ERR(pwrsw->rdev);

	platform_set_drvdata(pdev, pwrsw);
	dev_info(dev, "PMU power switch '%s' @ reg 0x%02x (mask=0x%02x, active=0x%02x)\n",
		 pwrsw->desc.name, reg, mask, pwrsw->desc.enable_val);
	return 0;
}

static const struct of_device_id apple_pmu_pwrsw_of_match[] = {
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
