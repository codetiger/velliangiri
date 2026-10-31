// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dialog D1815 / D1946 / D1974 / D2045 I2C PMU MFD driver
 *
 * On S5L8947X (j33iap), the Dialog D1815 PMU sits on i2c0 at address 0x74
 * and uses 8-bit register addressing (reg_bits = 8, val_bits = 8).
 *
 * The D1815 latches events (buttons, charger, RTC, its GPIO inputs) in six
 * read-to-clear registers from 0x01 and drives its interrupt line low while
 * an unmasked one is pending; the matching masks (1 = masked) start at 0x0c.
 * With an "interrupts" property the PMU becomes an interrupt controller with
 * one interrupt per event bit: register 0x01 + n / 8, bit n % 8. Its GPIOs
 * 0..7 (configured at 0x50 + n) are events 16..23, and GPIOs 8..9 are
 * events 40..41.
 */

#include <linux/apple-pmu-i2c.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
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

#define D1815_EVENT_BASE	0x01
#define D1815_MASK_BASE		0x0c
#define D1815_EVENT_REGS	6
#define D1815_NUM_EVENTS	(D1815_EVENT_REGS * 8)

static int apple_pmu_add_irq_chip(struct apple_pmu *pmu, int irq)
{
	struct regmap_irq_chip *chip;
	struct regmap_irq *irqs;
	struct regmap_irq_chip_data *data;
	unsigned int i;

	chip = devm_kzalloc(pmu->dev, sizeof(*chip), GFP_KERNEL);
	irqs = devm_kcalloc(pmu->dev, D1815_NUM_EVENTS, sizeof(*irqs), GFP_KERNEL);
	if (!chip || !irqs)
		return -ENOMEM;
	for (i = 0; i < D1815_NUM_EVENTS; i++) {
		irqs[i].reg_offset = i / 8;
		irqs[i].mask = BIT(i % 8);
	}
	chip->name = dev_name(pmu->dev);
	chip->status_base = D1815_EVENT_BASE;	/* reading clears the events */
	chip->mask_base = D1815_MASK_BASE;
	chip->num_regs = D1815_EVENT_REGS;
	chip->irqs = irqs;
	chip->num_irqs = D1815_NUM_EVENTS;

	return devm_regmap_add_irq_chip(pmu->dev, pmu->regmap, irq, IRQF_ONESHOT,
					0, chip, &data);
}

static int apple_pmu_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct apple_pmu *pmu;
	const struct regmap_config *cfg = &apple_pmu_regmap_8bit;
	u32 reg_bits = 8;

	if (!of_property_read_u32(node, "apple,reg-bits", &reg_bits) &&
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

	if (client->irq > 0 && reg_bits == 8) {
		int ret = apple_pmu_add_irq_chip(pmu, client->irq);

		if (ret)
			return dev_err_probe(dev, ret, "cannot register PMU interrupts\n");
	}

	dev_info(dev, "Dialog PMU I2C MFD initialized (%u-bit regmap @ 0x%02x%s)\n",
		 reg_bits, client->addr, client->irq > 0 ? ", interrupts" : "");

	/* Child nodes (power switches) become platform devices on the regmap */
	return devm_of_platform_populate(dev);
}

static const struct of_device_id apple_pmu_of_match[] = {
	{ .compatible = "apple,pmu-d1815" },
	{ .compatible = "apple,d1815-pmu" },
	{ .compatible = "apple,pmu-d1946" },
	{ .compatible = "apple,pmu-i2c" },
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

static int __init apple_pmu_i2c_init(void)
{
	return i2c_add_driver(&apple_pmu_i2c_driver);
}
subsys_initcall(apple_pmu_i2c_init);

MODULE_DESCRIPTION("Dialog D1815/D1946/D1974/D2045 I2C PMU MFD driver");
MODULE_LICENSE("GPL");
