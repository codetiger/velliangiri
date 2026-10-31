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
 *
 * The PMGR also has seven numerically controlled oscillators, the audio
 * master clocks (channel 0 clocks the I2S master, channel 4 the S/PDIF
 * transmitter). Each is three registers: control, increment, decrement. A
 * phase accumulator adds the increment (2 * f) on every reference cycle and
 * the decrement (2 * f - fref) when it overflows, toggling the output, so the
 * output is f to the hertz. The reference measures 342 MHz; it is the first
 * clock of the node, the second and third are the reference gates.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/math64.h>
#include <linux/slab.h>

#define PMGR_GATE_REQ		GENMASK(3, 0)
#define PMGR_GATE_STATUS	GENMASK(7, 4)
#define PMGR_GATE_CLEAR		(PMGR_GATE_REQ | BIT(8))
#define PMGR_GATE_TIMEOUT_US	10000

#define PMGR_NCO_CHANNELS	7
#define PMGR_NCO_STRIDE		0x10
#define PMGR_NCO_CTL		0x0
#define PMGR_NCO_INC		0x4
#define PMGR_NCO_DEC		0x8
#define PMGR_NCO_CTL_ON		(BIT(31) | BIT(28))	/* stopped, accepts updates */
#define PMGR_NCO_CTL_STOPPING	BIT(30)
#define PMGR_NCO_CTL_BUSY	BIT(9)			/* register update pending */
#define PMGR_NCO_CTL_LOAD	BIT(10)
#define PMGR_NCO_CTL_RUN	BIT(11)
#define PMGR_NCO_TIMEOUT_US	1000

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

struct s5l8940x_pmgr_nco {
	struct clk_hw hw;
	void __iomem *reg;
	struct clk *ref_gate[2];
	u32 inc, dec;
};

#define to_pmgr_nco(_hw) container_of(_hw, struct s5l8940x_pmgr_nco, hw)

static int s5l8940x_pmgr_nco_wait(struct s5l8940x_pmgr_nco *nco, u32 bits)
{
	u32 v;

	return readl_poll_timeout_atomic(nco->reg + PMGR_NCO_CTL, v, !(v & bits),
					 1, PMGR_NCO_TIMEOUT_US);
}

static int s5l8940x_pmgr_nco_prepare(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);
	int ret;

	ret = clk_prepare_enable(nco->ref_gate[0]);
	if (ret)
		return ret;
	ret = clk_prepare_enable(nco->ref_gate[1]);
	if (ret)
		clk_disable_unprepare(nco->ref_gate[0]);
	return ret;
}

static void s5l8940x_pmgr_nco_unprepare(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);

	clk_disable_unprepare(nco->ref_gate[1]);
	clk_disable_unprepare(nco->ref_gate[0]);
}

static int s5l8940x_pmgr_nco_enable(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);
	int ret;

	writel(PMGR_NCO_CTL_ON, nco->reg + PMGR_NCO_CTL);
	ret = s5l8940x_pmgr_nco_wait(nco, PMGR_NCO_CTL_STOPPING);
	if (ret)
		goto err;
	writel(nco->inc, nco->reg + PMGR_NCO_INC);
	ret = s5l8940x_pmgr_nco_wait(nco, PMGR_NCO_CTL_BUSY);
	if (ret)
		goto err;
	writel(nco->dec, nco->reg + PMGR_NCO_DEC);
	ret = s5l8940x_pmgr_nco_wait(nco, PMGR_NCO_CTL_BUSY);
	if (ret)
		goto err;
	writel(PMGR_NCO_CTL_ON | PMGR_NCO_CTL_LOAD, nco->reg + PMGR_NCO_CTL);
	ret = s5l8940x_pmgr_nco_wait(nco, PMGR_NCO_CTL_BUSY);
	if (ret)
		goto err;
	writel(PMGR_NCO_CTL_ON | PMGR_NCO_CTL_LOAD | PMGR_NCO_CTL_RUN,
	       nco->reg + PMGR_NCO_CTL);
	ret = s5l8940x_pmgr_nco_wait(nco, PMGR_NCO_CTL_BUSY);
	if (!ret)
		return 0;
err:
	pr_err("%s: stuck at 0x%08x\n", clk_hw_get_name(hw),
	       readl(nco->reg + PMGR_NCO_CTL));
	writel(PMGR_NCO_CTL_ON, nco->reg + PMGR_NCO_CTL);
	return ret;
}

static void s5l8940x_pmgr_nco_disable(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);

	writel(PMGR_NCO_CTL_ON, nco->reg + PMGR_NCO_CTL);
}

static int s5l8940x_pmgr_nco_is_enabled(struct clk_hw *hw)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);

	return !!(readl(nco->reg + PMGR_NCO_CTL) & PMGR_NCO_CTL_RUN);
}

static unsigned long s5l8940x_pmgr_nco_recalc_rate(struct clk_hw *hw,
						   unsigned long parent_rate)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);
	u32 span = nco->inc - nco->dec;	/* the reference the registers assume */

	if (!span)
		return 0;
	return div_u64((u64)parent_rate * nco->inc, 2ULL * span);
}

static int s5l8940x_pmgr_nco_determine_rate(struct clk_hw *hw,
					    struct clk_rate_request *req)
{
	req->rate = clamp(req->rate, 1UL, req->best_parent_rate / 2);
	return 0;
}

static int s5l8940x_pmgr_nco_set_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long parent_rate)
{
	struct s5l8940x_pmgr_nco *nco = to_pmgr_nco(hw);

	if (!rate || rate > parent_rate / 2)
		return -EINVAL;
	/* Takes effect on the next enable (CLK_SET_RATE_GATE) */
	nco->inc = 2 * rate;
	nco->dec = 2 * rate - parent_rate;
	return 0;
}

static const struct clk_ops s5l8940x_pmgr_nco_ops = {
	.prepare	= s5l8940x_pmgr_nco_prepare,
	.unprepare	= s5l8940x_pmgr_nco_unprepare,
	.enable		= s5l8940x_pmgr_nco_enable,
	.disable	= s5l8940x_pmgr_nco_disable,
	.is_enabled	= s5l8940x_pmgr_nco_is_enabled,
	.recalc_rate	= s5l8940x_pmgr_nco_recalc_rate,
	.determine_rate	= s5l8940x_pmgr_nco_determine_rate,
	.set_rate	= s5l8940x_pmgr_nco_set_rate,
};

static void __init s5l8940x_pmgr_nco_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clk_data;
	struct s5l8940x_pmgr_nco *ncos;
	struct clk_init_data init = {};
	struct clk *gate[2];
	const char *parent_name;
	void __iomem *base;
	unsigned int i;
	int err;

	parent_name = of_clk_get_parent_name(node, 0);
	gate[0] = of_clk_get(node, 1);
	gate[1] = of_clk_get(node, 2);
	base = of_iomap(node, 0);
	clk_data = kzalloc(struct_size(clk_data, hws, PMGR_NCO_CHANNELS), GFP_KERNEL);
	ncos = kcalloc(PMGR_NCO_CHANNELS, sizeof(*ncos), GFP_KERNEL);
	if (!parent_name || IS_ERR(gate[0]) || IS_ERR(gate[1]) || !base ||
	    !clk_data || !ncos) {
		pr_err("%pOFn: missing reference clock, gates or reg\n", node);
		goto err_free;
	}
	clk_data->num = PMGR_NCO_CHANNELS;

	init.ops = &s5l8940x_pmgr_nco_ops;
	init.flags = CLK_SET_RATE_GATE;
	init.parent_names = &parent_name;
	init.num_parents = 1;

	for (i = 0; i < PMGR_NCO_CHANNELS; i++) {
		struct s5l8940x_pmgr_nco *nco = &ncos[i];

		nco->reg = base + i * PMGR_NCO_STRIDE;
		nco->ref_gate[0] = gate[0];
		nco->ref_gate[1] = gate[1];
		nco->inc = readl(nco->reg + PMGR_NCO_INC);
		nco->dec = readl(nco->reg + PMGR_NCO_DEC);
		nco->hw.init = &init;
		init.name = kasprintf(GFP_KERNEL, "pmgr-nco%u", i);
		err = init.name ? clk_hw_register(NULL, &nco->hw) : -ENOMEM;
		kfree(init.name);
		if (err) {
			pr_err("%pOFn: channel %u: %d\n", node, i, err);
			clk_data->hws[i] = ERR_PTR(err);
			continue;
		}
		clk_data->hws[i] = &nco->hw;
	}

	err = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clk_data);
	if (err)
		pr_err("%pOFn: failed to add clock provider: %d\n", node, err);
	return;

err_free:
	kfree(ncos);
	kfree(clk_data);
	if (base)
		iounmap(base);
	if (!IS_ERR(gate[0]))
		clk_put(gate[0]);
	if (!IS_ERR(gate[1]))
		clk_put(gate[1]);
}

CLK_OF_DECLARE(s5l8940x_pmgr_gates, "apple,s5l8940x-pmgr-gates", s5l8940x_pmgr_gates_init);
CLK_OF_DECLARE(s5l8947x_pmgr_gates, "apple,s5l8947x-pmgr-gates", s5l8940x_pmgr_gates_init);
CLK_OF_DECLARE(s5l8947x_pmgr_nco, "apple,s5l8947x-pmgr-nco", s5l8940x_pmgr_nco_init);
