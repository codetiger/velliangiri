// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dialog D1815 / D1974 / D2045 I2C PMU MFD driver
 *
 * On S5L8947X (j33iap), the Dialog D1815 PMU sits on i2c0 at address 0x74
 * and uses 8-bit register addressing (reg_bits = 8, val_bits = 8).
 * Boards with 16-bit register addressing (such as D1974/D2045) may set
 * "apple,reg-bits = <16>" in the device tree.
 */

#include <linux/apple-pmu-i2c.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>

static const struct regmap_config apple_pmu_regmap_8bit = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 0xff,
};

static const struct regmap_config apple_pmu_regmap_16bit = {
	.reg_bits	= 16,
	.val_bits	= 8,
	.max_register	= 0xffff,
};

static int apple_pmu_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct apple_pmu *pmu;
	const struct regmap_config *cfg = &apple_pmu_regmap_8bit;
	u32 reg_bits = 8;

	if (!of_property_read_u32(dev->of_node, "apple,reg-bits", &reg_bits) &&
	    reg_bits == 16)
		cfg = &apple_pmu_regmap_16bit;

	pmu = devm_kzalloc(dev, sizeof(*pmu), GFP_KERNEL);
	if (!pmu)
		return -ENOMEM;

	pmu->dev = dev;
	i2c_set_clientdata(client, pmu);

	pmu->regmap = devm_regmap_init_i2c(client, cfg);
	if (IS_ERR(pmu->regmap))
		return PTR_ERR(pmu->regmap);

	dev_info(dev, "Dialog PMU I2C MFD initialized (%u-bit regmap @ 0x%02x)\n",
		 reg_bits, client->addr);

	return devm_of_platform_populate(dev);
}

static const struct of_device_id apple_pmu_of_match[] = {
	{ .compatible = "apple,pmu-i2c" },
	{ .compatible = "apple,d1815-pmu" },
	{ },
};
MODULE_DEVICE_TABLE(of, apple_pmu_of_match);

static const struct i2c_device_id apple_pmu_i2c_id[] = {
	{ "apple-pmu-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, apple_pmu_i2c_id);

static struct i2c_driver apple_pmu_i2c_driver = {
	.driver = {
		.name		= "apple-pmu-i2c",
		.of_match_table	= apple_pmu_of_match,
	},
	.probe		= apple_pmu_i2c_probe,
	.id_table	= apple_pmu_i2c_id,
};
module_i2c_driver(apple_pmu_i2c_driver);

MODULE_DESCRIPTION("Dialog D1815/D1974/D2045 I2C PMU MFD driver");
MODULE_LICENSE("GPL");
