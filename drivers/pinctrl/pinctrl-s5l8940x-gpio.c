// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8930X / S5L8940X / S5L8947X GPIO and Pinctrl driver
 *
 * Interrupt controller layout inside the GPIO block (base 0x3f000000):
 *   0x800 + 4*g   IRQ disable (write 1 to mask)
 *   0x840 + 4*g   IRQ enable  (write 1 to unmask)
 *   0x880 + 4*g   IRQ status  (write 1 to ack)
 *   0xc00         group summary bitmask
 *   0xc48         NPL mode enable (write 1)
 */

#include <linux/clk.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "core.h"
#include "pinconf.h"
#include "pinmux.h"
#include "pinctrl-utils.h"

struct s5l8940x_gpio_pinctrl {
	struct device *dev;
	struct pinctrl_dev *pctldev;

	void __iomem *base;

	struct pinctrl_desc pinctrl_desc;
	struct pinctrl_pin_desc *pins;
	unsigned int npins;
	unsigned int nirqs;
	int *irqs;
	int nr_parent_irqs;

	raw_spinlock_t lock;

	struct gpio_chip gpio_chip;
	struct irq_chip irq_chip;
	u32 *irq_type;
};

#define REG_PIN(x)		(4 * (x))
#define  REG_PIN_DATA_MASK	1
#define  REG_PIN_MODE_MASK	(7 << 1)
#define  REG_PIN_MODE_INPUT	(0 << 1)
#define  REG_PIN_MODE_OUTPUT_1	(1 << 1)
#define  REG_PIN_MODE_IRQ_HI	(2 << 1)
#define  REG_PIN_MODE_IRQ_LO	(3 << 1)
#define  REG_PIN_MODE_IRQ_UP	(4 << 1)
#define  REG_PIN_MODE_IRQ_DN	(5 << 1)
#define  REG_PIN_MODE_IRQ_ANY	(6 << 1)
#define  REG_PIN_MODE_DISABLE	(7 << 1)
#define  REG_PIN_PERIPH_MASK	(3 << 5)
#define  REG_PIN_PERIPH_SHIFT	5
#define  REG_PIN_PULL_MASK	(3 << 7)
#define  REG_PIN_PULL_DISABLE	(0 << 7)
#define  REG_PIN_PULL_DOWN	(1 << 7)
#define  REG_PIN_PULL_UP_STRONG	(2 << 7)
#define  REG_PIN_PULL_UP	(3 << 7)
#define  REG_PIN_INPUT_ENABLE	(1 << 9)
#define  REG_PIN_DRIVE_SHIFT	10
#define  REG_PIN_DRIVE_MASK	(3 << 10)
#define  REG_PIN_SLEW_SHIFT	12
#define  REG_PIN_SLEW_MASK	(1 << 12)
#define  REG_PIN_SCHMITT_EN	(3 << 14)
#define REG_IRQ_DISABLE(g)	(0x800 + 4 * (g))
#define REG_IRQ_ENABLE(g)	(0x840 + 4 * (g))
#define REG_IRQ_STATUS(g)	(0x880 + 4 * (g))
#define REG_IRQ_GROUPS		0xc00
#define REG_NPL_ENABLE		0xc48

#define DEFAULT_PIN_STATE	(REG_PIN_MODE_DISABLE | REG_PIN_INPUT_ENABLE)

static void s5l8940x_set_bits(struct s5l8940x_gpio_pinctrl *agp,
			      unsigned int pin, u32 mask, u32 value)
{
	unsigned long flags;
	u32 cfg;

	raw_spin_lock_irqsave(&agp->lock, flags);
	cfg = readl(agp->base + REG_PIN(pin));
	cfg = (cfg & ~mask) | value;
	writel(cfg, agp->base + REG_PIN(pin));
	raw_spin_unlock_irqrestore(&agp->lock, flags);
}

static u32 s5l8940x_get_bits(struct s5l8940x_gpio_pinctrl *agp,
			     unsigned int pin)
{
	return readl(agp->base + REG_PIN(pin));
}

static int s5l8940x_gpio_direction_input(struct gpio_chip *chip,
					 unsigned int offset)
{
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(chip);

	s5l8940x_set_bits(agp, offset,
			  REG_PIN_PERIPH_MASK | REG_PIN_MODE_MASK |
			  REG_PIN_DATA_MASK | REG_PIN_INPUT_ENABLE,
			  REG_PIN_MODE_INPUT | REG_PIN_INPUT_ENABLE);
	return 0;
}

static int s5l8940x_gpio_direction_output(struct gpio_chip *chip,
					  unsigned int offset, int value)
{
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(chip);

	s5l8940x_set_bits(agp, offset,
			  REG_PIN_PERIPH_MASK | REG_PIN_MODE_MASK |
			  REG_PIN_DATA_MASK,
			  REG_PIN_MODE_OUTPUT_1 | (value ? 1 : 0));
	return 0;
}

static int s5l8940x_gpio_get_direction(struct gpio_chip *chip,
				       unsigned int offset)
{
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(chip);
	u32 cfg = s5l8940x_get_bits(agp, offset);

	if ((cfg & REG_PIN_MODE_MASK) == REG_PIN_MODE_OUTPUT_1)
		return 0;
	return 1;
}

static int s5l8940x_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(chip);

	return !!(s5l8940x_get_bits(agp, offset) & REG_PIN_DATA_MASK);
}

static void s5l8940x_gpio_set(struct gpio_chip *chip, unsigned int offset,
			      int value)
{
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(chip);

	s5l8940x_set_bits(agp, offset, REG_PIN_DATA_MASK, value ? 1 : 0);
}

static const struct pinctrl_ops s5l8940x_pinctrl_ops = {
	.get_groups_count	= pinctrl_generic_get_group_count,
	.get_group_name		= pinctrl_generic_get_group_name,
	.get_group_pins		= pinctrl_generic_get_group_pins,
	.dt_node_to_map		= pinconf_generic_dt_node_to_map_all,
	.dt_free_map		= pinctrl_utils_free_map,
};

static int s5l8940x_pinmux_set_mux(struct pinctrl_dev *pctldev,
				   unsigned int func, unsigned int group)
{
	return 0;
}

static int s5l8940x_pinmux_set_gpio(struct pinctrl_dev *pctldev,
				    struct pinctrl_gpio_range *range,
				    unsigned int pin)
{
	struct s5l8940x_gpio_pinctrl *agp = pinctrl_dev_get_drvdata(pctldev);

	s5l8940x_set_bits(agp, pin,
			  REG_PIN_PERIPH_MASK | REG_PIN_MODE_MASK |
			  REG_PIN_DATA_MASK,
			  DEFAULT_PIN_STATE);
	return 0;
}

static int s5l8940x_pinmux_set_direction(struct pinctrl_dev *pctldev,
					 struct pinctrl_gpio_range *range,
					 unsigned int pin, bool input)
{
	struct s5l8940x_gpio_pinctrl *agp = pinctrl_dev_get_drvdata(pctldev);

	if (input)
		s5l8940x_gpio_direction_input(&agp->gpio_chip, pin);
	else
		s5l8940x_gpio_direction_output(&agp->gpio_chip, pin,
					       s5l8940x_gpio_get(&agp->gpio_chip, pin));
	return 0;
}

static const struct pinmux_ops s5l8940x_pinmux_ops = {
	.get_functions_count	= pinmux_generic_get_function_count,
	.get_function_name	= pinmux_generic_get_function_name,
	.get_function_groups	= pinmux_generic_get_function_groups,
	.set_mux		= s5l8940x_pinmux_set_mux,
	.gpio_request_enable	= s5l8940x_pinmux_set_gpio,
	.gpio_set_direction	= s5l8940x_pinmux_set_direction,
	.strict			= true,
};

#define PIN_CONFIG_PERIPH	(PIN_CONFIG_END + 1)

static const struct pinconf_generic_params s5l8940x_pinconf_dt_params[] = {
	{ "apple,peripheral-function", PIN_CONFIG_PERIPH, 0 },
};

static int s5l8940x_pinconf_get(struct pinctrl_dev *pctldev, unsigned int pin,
				unsigned long *config)
{
	struct s5l8940x_gpio_pinctrl *agp = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param = pinconf_to_config_param(*config);
	u32 cfg = s5l8940x_get_bits(agp, pin);
	u32 arg = 0;

	switch ((u32)param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if ((cfg & REG_PIN_PULL_MASK) != REG_PIN_PULL_DISABLE)
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if ((cfg & REG_PIN_PULL_MASK) != REG_PIN_PULL_DOWN)
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if ((cfg & REG_PIN_PULL_MASK) == REG_PIN_PULL_UP)
			arg = 1;
		else if ((cfg & REG_PIN_PULL_MASK) == REG_PIN_PULL_UP_STRONG)
			arg = 2;
		else
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		arg = (cfg & REG_PIN_DRIVE_MASK) >> REG_PIN_DRIVE_SHIFT;
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		if (!(cfg & REG_PIN_INPUT_ENABLE))
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		if (!(cfg & REG_PIN_SCHMITT_EN))
			return -EINVAL;
		arg = 1;
		break;
	case PIN_CONFIG_OUTPUT:
		if ((cfg & REG_PIN_MODE_MASK) != REG_PIN_MODE_OUTPUT_1)
			return -EINVAL;
		arg = cfg & REG_PIN_DATA_MASK;
		break;
	case PIN_CONFIG_SLEW_RATE:
		arg = (cfg & REG_PIN_SLEW_MASK) >> REG_PIN_SLEW_SHIFT;
		break;
	case PIN_CONFIG_PERIPH:
		if ((cfg & REG_PIN_MODE_MASK) != REG_PIN_MODE_OUTPUT_1 &&
		    (cfg & REG_PIN_MODE_MASK) != REG_PIN_MODE_INPUT)
			return -EINVAL;
		arg = ((cfg & REG_PIN_PERIPH_MASK) >> REG_PIN_PERIPH_SHIFT) * 2 +
		      ((cfg & REG_PIN_MODE_MASK) == REG_PIN_MODE_OUTPUT_1);
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);
	return 0;
}

static int s5l8940x_pinconf_set(struct pinctrl_dev *pctldev, unsigned int pin,
				unsigned long *configs, unsigned int num_configs)
{
	struct s5l8940x_gpio_pinctrl *agp = pinctrl_dev_get_drvdata(pctldev);
	enum pin_config_param param;
	u32 arg, mask = 0, val = 0;
	unsigned int i;

	for (i = 0; i < num_configs; i++) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch ((u32)param) {
		case PIN_CONFIG_BIAS_DISABLE:
			mask |= REG_PIN_PULL_MASK;
			val = (val & ~REG_PIN_PULL_MASK) | REG_PIN_PULL_DISABLE;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			mask |= REG_PIN_PULL_MASK;
			val = (val & ~REG_PIN_PULL_MASK) | REG_PIN_PULL_DOWN;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			mask |= REG_PIN_PULL_MASK;
			if (arg > 1)
				val = (val & ~REG_PIN_PULL_MASK) | REG_PIN_PULL_UP_STRONG;
			else
				val = (val & ~REG_PIN_PULL_MASK) | REG_PIN_PULL_UP;
			break;
		case PIN_CONFIG_DRIVE_PUSH_PULL:
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			mask |= REG_PIN_DRIVE_MASK;
			val = (val & ~REG_PIN_DRIVE_MASK) |
			      ((arg << REG_PIN_DRIVE_SHIFT) & REG_PIN_DRIVE_MASK);
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			mask |= REG_PIN_INPUT_ENABLE;
			if (arg)
				val |= REG_PIN_INPUT_ENABLE;
			else
				val &= ~REG_PIN_INPUT_ENABLE;
			break;
		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			mask |= REG_PIN_SCHMITT_EN;
			if (arg)
				val |= REG_PIN_SCHMITT_EN;
			else
				val &= ~REG_PIN_SCHMITT_EN;
			break;
		case PIN_CONFIG_OUTPUT_ENABLE:
			mask |= REG_PIN_MODE_MASK;
			if (arg)
				val = (val & ~REG_PIN_MODE_MASK) | REG_PIN_MODE_OUTPUT_1;
			else
				val = (val & ~REG_PIN_MODE_MASK) | REG_PIN_MODE_INPUT;
			break;
		case PIN_CONFIG_OUTPUT:
			mask |= REG_PIN_MODE_MASK | REG_PIN_DATA_MASK;
			val = (val & ~(REG_PIN_MODE_MASK | REG_PIN_DATA_MASK)) |
			      REG_PIN_MODE_OUTPUT_1 | (arg ? 1 : 0);
			break;
		case PIN_CONFIG_SLEW_RATE:
			mask |= REG_PIN_SLEW_MASK;
			val = (val & ~REG_PIN_SLEW_MASK) |
			      ((arg << REG_PIN_SLEW_SHIFT) & REG_PIN_SLEW_MASK);
			break;
		case PIN_CONFIG_PERIPH:
			mask |= REG_PIN_MODE_MASK | REG_PIN_PERIPH_MASK;
			val = (val & ~(REG_PIN_MODE_MASK | REG_PIN_PERIPH_MASK)) |
			      (((arg >> 1) << REG_PIN_PERIPH_SHIFT) & REG_PIN_PERIPH_MASK) |
			      ((arg & 1) ? REG_PIN_MODE_OUTPUT_1 : REG_PIN_MODE_INPUT);
			break;
		default:
			return -ENOTSUPP;
		}
	}

	if (mask)
		s5l8940x_set_bits(agp, pin, mask, val);
	return 0;
}

static int s5l8940x_pinconf_group_get(struct pinctrl_dev *pctldev,
				      unsigned int group, unsigned long *config)
{
	const unsigned int *pins = NULL;
	unsigned int npins = 0, i;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, group, &pins, &npins);
	if (ret)
		return ret;
	for (i = 0; i < npins; i++) {
		ret = s5l8940x_pinconf_get(pctldev, pins[i], config);
		if (ret)
			return ret;
	}
	return 0;
}

static int s5l8940x_pinconf_group_set(struct pinctrl_dev *pctldev,
				      unsigned int group, unsigned long *configs,
				      unsigned int num_configs)
{
	const unsigned int *pins = NULL;
	unsigned int npins = 0, i;
	int ret;

	ret = pinctrl_generic_get_group_pins(pctldev, group, &pins, &npins);
	if (ret)
		return ret;
	for (i = 0; i < npins; i++) {
		ret = s5l8940x_pinconf_set(pctldev, pins[i], configs,
					   num_configs);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct pinconf_ops s5l8940x_pinconf_ops = {
	.is_generic			= true,
	.pin_config_get			= s5l8940x_pinconf_get,
	.pin_config_set			= s5l8940x_pinconf_set,
	.pin_config_group_get		= s5l8940x_pinconf_group_get,
	.pin_config_group_set		= s5l8940x_pinconf_group_set,
	.pin_config_config_dbg_show	= pinconf_generic_dump_config,
};

static void s5l8940x_gpio_irq_enable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	unsigned int irq = irqd_to_hwirq(data);
	unsigned long flags;

	raw_spin_lock_irqsave(&agp->lock, flags);
	writel(1U << (irq & 31), agp->base + REG_IRQ_STATUS(irq >> 5));
	writel(1U << (irq & 31), agp->base + REG_IRQ_ENABLE(irq >> 5));
	raw_spin_unlock_irqrestore(&agp->lock, flags);
}

static void s5l8940x_gpio_irq_disable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	unsigned int irq = irqd_to_hwirq(data);
	unsigned long flags;

	raw_spin_lock_irqsave(&agp->lock, flags);
	writel(1U << (irq & 31), agp->base + REG_IRQ_DISABLE(irq >> 5));
	raw_spin_unlock_irqrestore(&agp->lock, flags);
}

static void s5l8940x_gpio_irq_ack(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	unsigned int irq = irqd_to_hwirq(data);
	unsigned long flags;

	raw_spin_lock_irqsave(&agp->lock, flags);
	writel(1U << (irq & 31), agp->base + REG_IRQ_STATUS(irq >> 5));
	raw_spin_unlock_irqrestore(&agp->lock, flags);
}

static void s5l8940x_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	unsigned int irq = irqd_to_hwirq(data);

	s5l8940x_set_bits(agp, irq, REG_PIN_MODE_MASK, REG_PIN_MODE_INPUT);
}

static void s5l8940x_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	unsigned int irq = irqd_to_hwirq(data);

	s5l8940x_set_bits(agp, irq, REG_PIN_MODE_MASK, agp->irq_type[irq]);
}

static int s5l8940x_gpio_irq_set_type(struct irq_data *data,
				      unsigned int flow_type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	unsigned int irq = irqd_to_hwirq(data);
	u32 mode;

	if (irq >= agp->nirqs)
		return -EINVAL;

	switch (flow_type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		mode = REG_PIN_MODE_IRQ_UP;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		mode = REG_PIN_MODE_IRQ_DN;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		mode = REG_PIN_MODE_IRQ_ANY;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		mode = REG_PIN_MODE_IRQ_HI;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		mode = REG_PIN_MODE_IRQ_LO;
		break;
	default:
		return -EINVAL;
	}

	agp->irq_type[irq] = mode;
	if (flow_type & IRQ_TYPE_LEVEL_MASK)
		irq_set_handler_locked(data, handle_level_irq);
	else
		irq_set_handler_locked(data, handle_edge_irq);
	s5l8940x_set_bits(agp, irq, REG_PIN_MODE_MASK | REG_PIN_PERIPH_MASK,
			  mode);
	return 0;
}

static void s5l8940x_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *gc = irq_desc_get_handler_data(desc);
	struct s5l8940x_gpio_pinctrl *agp = gpiochip_get_data(gc);
	struct irq_chip *parent_chip = irq_desc_get_chip(desc);
	unsigned int group = 0, pinh;
	u32 grps, dis, sts;
	unsigned long flags;

	chained_irq_enter(parent_chip, desc);

	grps = readl(agp->base + REG_IRQ_GROUPS);
	for (group = 0; group * 32 < agp->nirqs; group++) {
		if (!(grps & (1U << group)))
			continue;

		/*
		 * The enable and disable registers both read back the
		 * disabled pins of the group, and the status register
		 * reports pending pins whether they are enabled or not.
		 */
		raw_spin_lock_irqsave(&agp->lock, flags);
		dis = readl(agp->base + REG_IRQ_DISABLE(group));
		sts = readl(agp->base + REG_IRQ_STATUS(group)) & ~dis;
		raw_spin_unlock_irqrestore(&agp->lock, flags);

		while (sts) {
			pinh = __ffs(sts);
			sts &= ~(1U << pinh);
			generic_handle_irq(irq_find_mapping(gc->irq.domain,
							    group * 32 + pinh));
		}
	}

	chained_irq_exit(parent_chip, desc);
}

static int s5l8940x_gpio_pinctrl_probe(struct platform_device *pdev)
{
	struct s5l8940x_gpio_pinctrl *agp;
	struct device_node *node = pdev->dev.of_node;
	struct gpio_irq_chip *girq;
	struct clk *clk;
	unsigned int i;
	int err;

	agp = devm_kzalloc(&pdev->dev, sizeof(*agp), GFP_KERNEL);
	if (!agp)
		return -ENOMEM;
	agp->dev = &pdev->dev;
	platform_set_drvdata(pdev, agp);

	err = of_property_read_u32(node, "apple,npins", &agp->npins);
	if (err)
		agp->npins = 224;
	err = of_property_read_u32(node, "apple,nirqs", &agp->nirqs);
	if (err || agp->nirqs > agp->npins)
		agp->nirqs = agp->npins;

	agp->pins = devm_kcalloc(&pdev->dev, agp->npins,
				 sizeof(struct pinctrl_pin_desc), GFP_KERNEL);
	if (!agp->pins)
		return -ENOMEM;

	agp->irq_type = devm_kcalloc(&pdev->dev, agp->nirqs, sizeof(u32),
				     GFP_KERNEL);
	if (!agp->irq_type)
		return -ENOMEM;

	agp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(agp->base))
		return PTR_ERR(agp->base);

	clk = devm_clk_get_optional_enabled(&pdev->dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(clk), "failed to enable clock\n");

	agp->nr_parent_irqs = of_irq_count(node);
	if (agp->nr_parent_irqs > 0) {
		agp->irqs = devm_kcalloc(&pdev->dev, agp->nr_parent_irqs,
					 sizeof(int), GFP_KERNEL);
		if (!agp->irqs)
			return -ENOMEM;
		for (i = 0; i < (unsigned int)agp->nr_parent_irqs; i++) {
			err = platform_get_irq(pdev, i);
			if (err < 0)
				return err;
			agp->irqs[i] = err;
		}
	}

	raw_spin_lock_init(&agp->lock);

	for (i = 0; i < agp->npins; i++) {
		agp->pins[i].number = i;
		agp->pins[i].name = devm_kasprintf(&pdev->dev, GFP_KERNEL,
						   "PIN%u", i);
		if (!agp->pins[i].name)
			return -ENOMEM;
	}

	agp->pinctrl_desc.name = dev_name(&pdev->dev);
	agp->pinctrl_desc.pins = agp->pins;
	agp->pinctrl_desc.npins = agp->npins;
	agp->pinctrl_desc.pctlops = &s5l8940x_pinctrl_ops;
	agp->pinctrl_desc.pmxops = &s5l8940x_pinmux_ops;
	agp->pinctrl_desc.confops = &s5l8940x_pinconf_ops;
	agp->pinctrl_desc.owner = THIS_MODULE;
	agp->pinctrl_desc.num_custom_params =
		ARRAY_SIZE(s5l8940x_pinconf_dt_params);
	agp->pinctrl_desc.custom_params = s5l8940x_pinconf_dt_params;

	err = devm_pinctrl_register_and_init(&pdev->dev, &agp->pinctrl_desc,
					     agp, &agp->pctldev);
	if (err)
		return err;

	err = pinctrl_enable(agp->pctldev);
	if (err)
		return err;

	agp->gpio_chip.owner = THIS_MODULE;
	agp->gpio_chip.label = dev_name(&pdev->dev);
	agp->gpio_chip.parent = &pdev->dev;
	agp->gpio_chip.request = gpiochip_generic_request;
	agp->gpio_chip.free = gpiochip_generic_free;
	agp->gpio_chip.get_direction = s5l8940x_gpio_get_direction;
	agp->gpio_chip.direction_input = s5l8940x_gpio_direction_input;
	agp->gpio_chip.direction_output = s5l8940x_gpio_direction_output;
	agp->gpio_chip.get = s5l8940x_gpio_get;
	agp->gpio_chip.set = s5l8940x_gpio_set;
	agp->gpio_chip.base = 0;
	agp->gpio_chip.ngpio = agp->npins;
	agp->gpio_chip.can_sleep = false;

	agp->irq_chip.name = dev_name(&pdev->dev);
	agp->irq_chip.irq_enable = s5l8940x_gpio_irq_enable;
	agp->irq_chip.irq_disable = s5l8940x_gpio_irq_disable;
	agp->irq_chip.irq_ack = s5l8940x_gpio_irq_ack;
	agp->irq_chip.irq_mask = s5l8940x_gpio_irq_mask;
	agp->irq_chip.irq_unmask = s5l8940x_gpio_irq_unmask;
	agp->irq_chip.irq_set_type = s5l8940x_gpio_irq_set_type;
	agp->irq_chip.flags = IRQCHIP_MASK_ON_SUSPEND;

	if (agp->nr_parent_irqs > 0) {
		for (i = 0; i * 32 < agp->nirqs; i++) {
			writel(0xffffffffU, agp->base + REG_IRQ_DISABLE(i));
			writel(0xffffffffU, agp->base + REG_IRQ_STATUS(i));
		}
		writel(1, agp->base + REG_NPL_ENABLE);
		for (i = 0; i < agp->nirqs; i++)
			agp->irq_type[i] = REG_PIN_MODE_IRQ_HI;

		girq = &agp->gpio_chip.irq;
		girq->chip = &agp->irq_chip;
		girq->parent_handler = s5l8940x_gpio_irq_handler;
		girq->num_parents = agp->nr_parent_irqs;
		girq->parents = agp->irqs;
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_bad_irq;
	}

	err = devm_gpiochip_add_data(&pdev->dev, &agp->gpio_chip, agp);
	if (err)
		return err;

	return gpiochip_add_pin_range(&agp->gpio_chip, dev_name(&pdev->dev),
				      0, 0, agp->npins);
}

static const struct of_device_id s5l8940x_gpio_pinctrl_match[] = {
	{ .compatible = "apple,s5l8930x-gpio" },
	{ .compatible = "apple,s5l8940x-gpio" },
	{ .compatible = "apple,s5l8947x-gpio" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8940x_gpio_pinctrl_match);

static struct platform_driver s5l8940x_gpio_pinctrl_driver = {
	.probe	= s5l8940x_gpio_pinctrl_probe,
	.driver	= {
		.name		= "s5l8940x-gpio-pinctrl",
		.of_match_table	= s5l8940x_gpio_pinctrl_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(s5l8940x_gpio_pinctrl_driver);

MODULE_DESCRIPTION("S5L8930X/S5L8940X/S5L8947X GPIO and Pinctrl driver");
MODULE_LICENSE("GPL");
