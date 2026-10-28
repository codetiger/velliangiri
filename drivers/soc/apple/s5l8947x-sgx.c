// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X PowerVR SGX543 GPU Power/Clock Domain & Telemetry Driver
 *
 * Switches the PowerVR SGX543 (0x38000000, IRQ 49) clock/power domain and
 * exposes the core identification registers.
 */

#define pr_fmt(fmt) "s5l8947x-sgx: " fmt

#include <linux/clk.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* SGX543 EUR_CR core identification registers (offset from 0x38000000) */
#define EUR_CR_CORE_ID			0x0014
#define EUR_CR_CORE_REVISION		0x0018

struct s5l8947x_sgx {
	struct device *dev;
	void __iomem *regs;
	struct clk_bulk_data *clks;
	int num_clks;
	struct miscdevice miscdev;
	struct mutex lock;
	int irq;
	bool powered;
	u32 core_id;
	u32 core_rev;
};

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
	sgx->core_id = readl(sgx->regs + EUR_CR_CORE_ID);
	sgx->core_rev = readl(sgx->regs + EUR_CR_CORE_REVISION);
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
			  "model=PowerVR-SGX543 mmio=0x38000000 irq=%d core_id=0x%08x core_rev=0x%08x\n",
			  sgx->irq, sgx->core_id, sgx->core_rev);
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
			"SGX543 mmio=0x38000000 irq=%d state=%s core_id=0x%08x core_rev=0x%08x\n",
			sgx->irq, sgx->powered ? "on" : "standby",
			sgx->core_id, sgx->core_rev);
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
		 "PowerVR SGX543 registered (/dev/sgx0, irq=%d, core_id=0x%08x rev=0x%08x)\n",
		 sgx->irq, sgx->core_id, sgx->core_rev);
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
