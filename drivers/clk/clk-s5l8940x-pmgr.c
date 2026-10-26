// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8930X / S5L8940X / S5L8947X PMGR clock driver
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

struct s5l8940x_pmgr_gate {
	struct clk_hw hw;
	const char *name;
	void __iomem *base;
	u32 index;
};

#define to_s5l8940x_pmgr_gate(_hw) \
	container_of(_hw, struct s5l8940x_pmgr_gate, hw)

#define REG_GATE_MASK		0x1d
#define REG_GATE_ACTIVE		0x0d
#define REG_GATE_INACTIVE	0x00

static int s5l8940x_pmgr_gate_enable(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_gate *gate = to_s5l8940x_pmgr_gate(hw);
	u32 reg = readl(gate->base + gate->index * 4);

	if ((reg & REG_GATE_MASK) != REG_GATE_ACTIVE)
		writel((reg & ~REG_GATE_MASK) | REG_GATE_ACTIVE,
		       gate->base + gate->index * 4);
	return 0;
}

static void s5l8940x_pmgr_gate_disable(struct clk_hw *hw)
{
	/*
	 * Intentionally keep peripheral clock gates enabled once activated so
	 * late unused-clock cleanup does not gate critical SoC buses.
	 */
}

static int s5l8940x_pmgr_gate_is_enabled(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_gate *gate = to_s5l8940x_pmgr_gate(hw);

	return (readl(gate->base + gate->index * 4) & REG_GATE_MASK) !=
	       REG_GATE_INACTIVE;
}

static const struct clk_ops s5l8940x_pmgr_gate_ops = {
	.enable		= s5l8940x_pmgr_gate_enable,
	.disable	= s5l8940x_pmgr_gate_disable,
	.is_enabled	= s5l8940x_pmgr_gate_is_enabled,
};

struct s5l8940x_pmgr_pwm {
	struct clk_hw hw;
	const char *name;
	void __iomem *base;
	u32 index;
	u32 config[3];
};

#define to_s5l8940x_pmgr_pwm(_hw) \
	container_of(_hw, struct s5l8940x_pmgr_pwm, hw)

static int s5l8940x_pmgr_pwm_enable(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_pwm *pwm = to_s5l8940x_pmgr_pwm(hw);

	writel(pwm->config[2], pwm->base + pwm->index * 12 + 8);
	writel(pwm->config[1], pwm->base + pwm->index * 12 + 4);
	writel(pwm->config[0] | 1, pwm->base + pwm->index * 12);
	return 0;
}

static void s5l8940x_pmgr_pwm_disable(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_pwm *pwm = to_s5l8940x_pmgr_pwm(hw);

	writel(pwm->config[0] & ~1U, pwm->base + pwm->index * 12);
}

static int s5l8940x_pmgr_pwm_is_enabled(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_pwm *pwm = to_s5l8940x_pmgr_pwm(hw);

	return readl(pwm->base + pwm->index * 12) & 1;
}

static const struct clk_ops s5l8940x_pmgr_pwm_ops = {
	.enable		= s5l8940x_pmgr_pwm_enable,
	.disable	= s5l8940x_pmgr_pwm_disable,
	.is_enabled	= s5l8940x_pmgr_pwm_is_enabled,
};

static void __init s5l8940x_pmgr_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	struct clk_init_data init;
	const char *parent_name;
	void __iomem *base;
	int num, idx, err;
	struct s5l8940x_pmgr_gate *gate;
	struct s5l8940x_pmgr_pwm *pwm;
	u32 index;

	parent_name = of_clk_get_parent_name(node, 0);
	if (!parent_name)
		return;

	base = of_iomap(node, 0);
	if (!base) {
		pr_err("%pOFn: failed to map MMIO\n", node);
		return;
	}

	num = of_property_count_strings(node, "clock-output-names");
	if (num <= 0) {
		pr_err("%pOFn: invalid clock-output-names\n", node);
		return;
	}

	clk_data = kzalloc(struct_size(clk_data, hws, num), GFP_KERNEL);
	if (!clk_data)
		return;
	clk_data->num = num;

	for (idx = 0; idx < num; idx++) {
		if (!of_property_read_u32_index(node, "clock-indices", idx,
						&index)) {
			gate = kzalloc(sizeof(*gate), GFP_KERNEL);
			if (!gate)
				break;

			if (of_property_read_string_index(node,
							  "clock-output-names",
							  idx, &gate->name) < 0) {
				kfree(gate);
				break;
			}

			init.name = gate->name;
			init.ops = &s5l8940x_pmgr_gate_ops;
			init.flags = 0;
			init.parent_names = &parent_name;
			init.num_parents = 1;

			gate->base = base;
			gate->index = index;
			gate->hw.init = &init;

			err = clk_hw_register(NULL, &gate->hw);
			if (err) {
				kfree(gate);
				break;
			}
			clk_data->hws[idx] = &gate->hw;
		} else if (!of_property_read_u32_index(node, "pwm-indices", idx,
						       &index)) {
			pwm = kzalloc(sizeof(*pwm), GFP_KERNEL);
			if (!pwm)
				break;

			if (of_property_read_string_index(node,
							  "clock-output-names",
							  idx, &pwm->name) < 0) {
				kfree(pwm);
				break;
			}

			if (of_property_read_u32_index(node, "pwm-config",
						       idx * 3,
						       &pwm->config[0]) < 0 ||
			    of_property_read_u32_index(node, "pwm-config",
						       idx * 3 + 1,
						       &pwm->config[1]) < 0 ||
			    of_property_read_u32_index(node, "pwm-config",
						       idx * 3 + 2,
						       &pwm->config[2]) < 0) {
				kfree(pwm);
				break;
			}

			init.name = pwm->name;
			init.ops = &s5l8940x_pmgr_pwm_ops;
			init.flags = 0;
			init.parent_names = &parent_name;
			init.num_parents = 1;

			pwm->base = base;
			pwm->index = index;
			pwm->hw.init = &init;

			err = clk_hw_register(NULL, &pwm->hw);
			if (err) {
				kfree(pwm);
				break;
			}
			clk_data->hws[idx] = &pwm->hw;
		} else {
			break;
		}
	}

	of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
}

CLK_OF_DECLARE_DRIVER(s5l8930x_pmgr, "apple,s5l8930x-pmgr-clocks", s5l8940x_pmgr_init);
CLK_OF_DECLARE_DRIVER(s5l8940x_pmgr, "apple,s5l8940x-pmgr-clocks", s5l8940x_pmgr_init);
CLK_OF_DECLARE_DRIVER(s5l8947x_pmgr, "apple,s5l8947x-pmgr-clocks", s5l8940x_pmgr_init);
