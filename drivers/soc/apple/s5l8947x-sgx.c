// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X PowerVR SGX543 GPU Power/Clock Domain & Telemetry Driver
 *
 * Switches the PowerVR SGX543 (0x35100000, IRQ 49) clock/power domain,
 * releases the cores from reset with automatic clock gating, and exposes the
 * master configuration registers.
 */

#define pr_fmt(fmt) "s5l8947x-sgx: " fmt

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/*
 * SGX543 registers (offset from 0x35100000). The master block at 0x4000 stays
 * readable while the cores are clock gated; the per-core registers at the
 * bottom of the window hang the bus until the clock gating below has been
 * programmed, and not every offset there is implemented. Only offsets known to
 * answer are read.
 */
#define SGX_CLKGATE0			0x0000
#define SGX_CLKGATE1			0x0004
#define SGX_CLKGATE_EXT			0x0310
#define SGX_MASTER_CORES		0x4000
#define SGX_MASTER_CORE_EN		0x4004
#define SGX_MASTER_CFG0			0x4010
#define SGX_MASTER_CFG1			0x4014
#define SGX_MASTER_CLKGATE		0x4020
#define SGX_MASTER_RESET		0x4080
#define SGX_MASTER_MEM0			0x4d00
#define SGX_MASTER_MEM1			0x4d04
#define SGX_MASTER_CFG0_CORES		GENMASK(11, 8)

#define SGX_MASTER_CLKGATE_AUTO		0x00000155
#define SGX_MASTER_RESET_ALL		0x000004f0
#define SGX_MASTER_RESET_EXT		BIT(8)
#define SGX_MASTER_MEM0_VAL		0x0044c000
#define SGX_MASTER_MEM1_VAL		0x04001e40
#define SGX_CLKGATE0_AUTO		0x10155555
#define SGX_CLKGATE1_AUTO		0x05454555

struct s5l8947x_sgx {
	struct device *dev;
	void __iomem *regs;
	struct clk_bulk_data *clks;
	int num_clks;
	struct miscdevice miscdev;
	struct mutex lock;
	int irq;
	bool powered;
	u32 cfg0;
	u32 cfg1;
	u32 clkgate0;
};

/*
 * Release the cores from reset with automatic clock gating, in the order the
 * hardware expects. Called with the clocks running.
 */
static void s5l8947x_sgx_hw_init(struct s5l8947x_sgx *sgx)
{
	u32 cores, en = 0, rst = SGX_MASTER_RESET_ALL | SGX_MASTER_RESET_EXT;
	unsigned int i;

	sgx->cfg0 = readl(sgx->regs + SGX_MASTER_CFG0);
	sgx->cfg1 = readl(sgx->regs + SGX_MASTER_CFG1);
	cores = max_t(u32, FIELD_GET(SGX_MASTER_CFG0_CORES, sgx->cfg0), 1);

	for (i = 0; i < cores; i++) {
		en |= BIT(2 * i);
		rst |= BIT(i);
	}

	writel(cores - 1, sgx->regs + SGX_MASTER_CORES);
	writel(SGX_MASTER_CLKGATE_AUTO, sgx->regs + SGX_MASTER_CLKGATE);
	writel(en, sgx->regs + SGX_MASTER_CORE_EN);

	writel(rst, sgx->regs + SGX_MASTER_RESET);
	writel(SGX_MASTER_MEM0_VAL, sgx->regs + SGX_MASTER_MEM0);
	writel(SGX_MASTER_MEM1_VAL, sgx->regs + SGX_MASTER_MEM1);
	writel(0, sgx->regs + SGX_MASTER_RESET);

	writel(SGX_CLKGATE0_AUTO, sgx->regs + SGX_CLKGATE0);
	writel(SGX_CLKGATE1_AUTO, sgx->regs + SGX_CLKGATE1);
	writel(1, sgx->regs + SGX_CLKGATE_EXT);
	sgx->clkgate0 = readl(sgx->regs + SGX_CLKGATE0);
}

/* Called with sgx->lock held */
static int s5l8947x_sgx_power_set(struct s5l8947x_sgx *sgx, bool on)
{
	int ret;

	if (on == sgx->powered)
		return 0;

	if (!on) {
		clk_bulk_disable_unprepare(sgx->num_clks, sgx->clks);
		sgx->powered = false;
		return 0;
	}

	ret = clk_bulk_prepare_enable(sgx->num_clks, sgx->clks);
	if (ret)
		return ret;
	sgx->powered = true;
	s5l8947x_sgx_hw_init(sgx);
	return 0;
}

static ssize_t power_state_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", sgx->powered ? "on" : "standby");
}

static ssize_t power_state_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&sgx->lock);
	if (sysfs_streq(buf, "on") || sysfs_streq(buf, "1"))
		ret = s5l8947x_sgx_power_set(sgx, true);
	else if (sysfs_streq(buf, "off") || sysfs_streq(buf, "0") ||
		 sysfs_streq(buf, "standby"))
		ret = s5l8947x_sgx_power_set(sgx, false);
	else
		ret = -EINVAL;
	mutex_unlock(&sgx->lock);

	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(power_state);

static ssize_t core_info_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);

	return sysfs_emit(buf,
			  "model=PowerVR-SGX543 mmio=0x35100000 irq=%d cfg0=0x%08x cfg1=0x%08x cores=%lu clkgate=0x%08x\n",
			  sgx->irq, sgx->cfg0, sgx->cfg1,
			  FIELD_GET(SGX_MASTER_CFG0_CORES, sgx->cfg0),
			  sgx->clkgate0);
}
static DEVICE_ATTR_RO(core_info);

static struct attribute *s5l8947x_sgx_attrs[] = {
	&dev_attr_power_state.attr,
	&dev_attr_core_info.attr,
	NULL,
};
ATTRIBUTE_GROUPS(s5l8947x_sgx);

static ssize_t s5l8947x_sgx_misc_read(struct file *file, char __user *ubuf,
				      size_t count, loff_t *ppos)
{
	struct s5l8947x_sgx *sgx =
		container_of(file->private_data, struct s5l8947x_sgx, miscdev);
	char info[160];
	int len;

	len = scnprintf(info, sizeof(info),
			"SGX543 mmio=0x35100000 irq=%d state=%s cfg0=0x%08x cfg1=0x%08x clkgate=0x%08x\n",
			sgx->irq, sgx->powered ? "on" : "standby",
			sgx->cfg0, sgx->cfg1, sgx->clkgate0);
	return simple_read_from_buffer(ubuf, count, ppos, info, len);
}

static const struct file_operations s5l8947x_sgx_fops = {
	.owner	= THIS_MODULE,
	.read	= s5l8947x_sgx_misc_read,
	.llseek	= default_llseek,
};

static int s5l8947x_sgx_probe(struct platform_device *pdev)
{
	struct s5l8947x_sgx *sgx;
	int ret;

	sgx = devm_kzalloc(&pdev->dev, sizeof(*sgx), GFP_KERNEL);
	if (!sgx)
		return -ENOMEM;

	sgx->dev = &pdev->dev;
	mutex_init(&sgx->lock);
	platform_set_drvdata(pdev, sgx);

	sgx->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sgx->regs))
		return PTR_ERR(sgx->regs);

	sgx->irq = platform_get_irq_optional(pdev, 0);

	sgx->num_clks = devm_clk_bulk_get_all(&pdev->dev, &sgx->clks);
	if (sgx->num_clks <= 0)
		return dev_err_probe(&pdev->dev, sgx->num_clks ?: -ENODEV,
				     "failed to get clocks\n");

	ret = s5l8947x_sgx_power_set(sgx, true);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to power on\n");

	sgx->miscdev.minor = MISC_DYNAMIC_MINOR;
	sgx->miscdev.name = "sgx0";
	sgx->miscdev.fops = &s5l8947x_sgx_fops;
	sgx->miscdev.parent = &pdev->dev;
	ret = misc_register(&sgx->miscdev);
	if (ret) {
		s5l8947x_sgx_power_set(sgx, false);
		return ret;
	}

	dev_info(&pdev->dev,
		 "PowerVR SGX543 registered (/dev/sgx0, irq=%d, cfg0=0x%08x cfg1=0x%08x)\n",
		 sgx->irq, sgx->cfg0, sgx->cfg1);
	return 0;
}

static void s5l8947x_sgx_remove(struct platform_device *pdev)
{
	struct s5l8947x_sgx *sgx = platform_get_drvdata(pdev);

	misc_deregister(&sgx->miscdev);
	s5l8947x_sgx_power_set(sgx, false);
}

static const struct of_device_id s5l8947x_sgx_of_match[] = {
	{ .compatible = "apple,s5l8947x-sgx" },
	{ .compatible = "img,sgx543" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l8947x_sgx_of_match);

static struct platform_driver s5l8947x_sgx_driver = {
	.probe	= s5l8947x_sgx_probe,
	.remove	= s5l8947x_sgx_remove,
	.driver	= {
		.name		= "s5l8947x-sgx",
		.of_match_table	= s5l8947x_sgx_of_match,
		.dev_groups	= s5l8947x_sgx_groups,
	},
};
module_platform_driver(s5l8947x_sgx_driver);

MODULE_DESCRIPTION("S5L8947X PowerVR SGX543 GPU Power/Clock Domain & Telemetry Driver");
MODULE_LICENSE("GPL");
