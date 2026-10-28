// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8940X / S5L8947X PMGR clock gate driver
 *
 * Each peripheral clock gate is one 32-bit register in a linear table. The
 * requested state is written to bits 3:0 (0xf on, 0x0 off) and the gate
 * reports the state it has reached in bits 7:4. The single clock specifier
 * cell is the gate's index in the table.
 *
 * Gates are flagged CLK_IGNORE_UNUSED: the boot loader leaves the console,
 * display and bus gates running for devices Linux may never claim.
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define PMGR_GATE_REQ		GENMASK(3, 0)
#define PMGR_GATE_STATUS	GENMASK(7, 4)
#define PMGR_GATE_CLEAR		(PMGR_GATE_REQ | BIT(8))
#define PMGR_GATE_TIMEOUT_US	10000

struct s5l8940x_pmgr_gate {
	struct clk_hw hw;
	void __iomem *reg;
};

#define to_pmgr_gate(_hw) container_of(_hw, struct s5l8940x_pmgr_gate, hw)

static int s5l8940x_pmgr_gate_set(struct clk_hw *hw, bool on)
{
	struct s5l8940x_pmgr_gate *gate = to_pmgr_gate(hw);
	u32 want = on ? PMGR_GATE_REQ : 0;
	u32 v = readl(gate->reg);
	int ret;

	writel((v & ~PMGR_GATE_CLEAR) | want, gate->reg);
	ret = readl_poll_timeout_atomic(gate->reg, v,
					FIELD_GET(PMGR_GATE_STATUS, v) == want,
					1, PMGR_GATE_TIMEOUT_US);
	if (ret)
		pr_err("%s: stuck at 0x%08x turning %s\n", clk_hw_get_name(hw),
		       v, on ? "on" : "off");
	return ret;
}

static int s5l8940x_pmgr_gate_enable(struct clk_hw *hw)
{
	return s5l8940x_pmgr_gate_set(hw, true);
}

static void s5l8940x_pmgr_gate_disable(struct clk_hw *hw)
{
	s5l8940x_pmgr_gate_set(hw, false);
}

static int s5l8940x_pmgr_gate_is_enabled(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_gate *gate = to_pmgr_gate(hw);

	return FIELD_GET(PMGR_GATE_STATUS, readl(gate->reg)) == PMGR_GATE_REQ;
}

static const struct clk_ops s5l8940x_pmgr_gate_ops = {
	.enable		= s5l8940x_pmgr_gate_enable,
	.disable	= s5l8940x_pmgr_gate_disable,
	.is_enabled	= s5l8940x_pmgr_gate_is_enabled,
};

static void __init s5l8940x_pmgr_gates_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	struct s5l8940x_pmgr_gate *gates;
	struct clk_init_data init = {};
	const char *parent_name;
	struct resource res;
	void __iomem *base;
	unsigned int i, num;
	int err;

	parent_name = of_clk_get_parent_name(node, 0);
	if (!parent_name || of_address_to_resource(node, 0, &res)) {
		pr_err("%pOFn: missing parent clock or reg\n", node);
		return;
	}

	num = resource_size(&res) / 4;
	base = of_iomap(node, 0);
	clk_data = kzalloc(struct_size(clk_data, hws, num), GFP_KERNEL);
	gates = kcalloc(num, sizeof(*gates), GFP_KERNEL);
	if (!base || !clk_data || !gates)
		goto err_free;
	clk_data->num = num;

	init.ops = &s5l8940x_pmgr_gate_ops;
	init.flags = CLK_IGNORE_UNUSED;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	for (i = 0; i < num; i++) {
		init.name = kasprintf(GFP_KERNEL, "pmgr-gate%u", i);
		gates[i].reg = base + i * 4;
		gates[i].hw.init = &init;
		err = init.name ? clk_hw_register(NULL, &gates[i].hw) : -ENOMEM;
		kfree(init.name);
		if (err) {
			pr_err("%pOFn: gate %u: %d\n", node, i, err);
			clk_data->hws[i] = ERR_PTR(err);
			continue;
		}
		clk_data->hws[i] = &gates[i].hw;
	}

	err = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (err)
		pr_err("%pOFn: failed to add clock provider: %d\n", node, err);
	return;

err_free:
	kfree(gates);
	kfree(clk_data);
	if (base)
		iounmap(base);
}

CLK_OF_DECLARE(s5l8940x_pmgr_gates, "apple,s5l8940x-pmgr-gates", s5l8940x_pmgr_gates_init);
CLK_OF_DECLARE(s5l8947x_pmgr_gates, "apple,s5l8947x-pmgr-gates", s5l8940x_pmgr_gates_init);
