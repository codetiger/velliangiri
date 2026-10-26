// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X PowerVR SGX543 GPU Power/Clock Domain & Telemetry Driver
 *
 * Manages the PowerVR SGX543 (0x38000000, IRQ 49) PMGR clock/power domain
 * gates and exposes safe hardware core identification and register inspection.
 */

#define pr_fmt(fmt) "s5l8947x-sgx: " fmt

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define PMGR_PHYS_BASE			0x3f100000UL
#define PMGR_GATE_OFFSET(n)		(0x1008U + (n) * 4U)

/* SGX543 EUR_CR core identification registers (offset from 0x38000000) */
#define EUR_CR_CLKGATECTL		0x0000
#define EUR_CR_CORE_ID			0x0014
#define EUR_CR_CORE_REVISION		0x0018
#define EUR_CR_EVENT_STATUS		0x012c
#define EUR_CR_SOFT_RESET		0x0080

#define SGX_MAX_GATES			8

struct s5l8947x_sgx {
	struct device *dev;
	void __iomem *regs;
	void __iomem *pmgr;
	struct miscdevice miscdev;
	struct mutex lock;
	int irq;
	u32 gates[SGX_MAX_GATES];
	unsigned int num_gates;
	bool powered;
	u32 core_id;
	u32 core_rev;
};

static const u32 s5l8947x_sgx_default_gates[] = { 7, 8, 9 };

static int s5l8947x_sgx_set_gate(struct s5l8947x_sgx *sgx, u32 gate, bool on)
{
	u32 off = PMGR_GATE_OFFSET(gate);
	u32 v;
	int i;

	if (!sgx->pmgr || gate > 85)
		return -EINVAL;

	v = readl(sgx->pmgr + off);
	writel((v & ~0x10fU) | (on ? 0x0fU : 0x00U), sgx->pmgr + off);
	for (i = 0; i < 10000; i++) {
		v = readl(sgx->pmgr + off);
		if (!((v ^ (v >> 4)) & 0x0fU))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static bool s5l8947x_sgx_gates_all_active(struct s5l8947x_sgx *sgx)
{
	unsigned int i;

	if (!sgx->pmgr || !sgx->num_gates)
		return false;

	for (i = 0; i < sgx->num_gates; i++) {
		u32 v = readl(sgx->pmgr + PMGR_GATE_OFFSET(sgx->gates[i]));

		if ((v & 0xffU) != 0xffU)
			return false;
	}
	return true;
}

static int s5l8947x_sgx_power_set(struct s5l8947x_sgx *sgx, bool on,
				  bool read_core_regs)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < sgx->num_gates; i++) {
		int rc = s5l8947x_sgx_set_gate(sgx, sgx->gates[i], on);

		if (rc && !ret)
			ret = rc;
	}

	sgx->powered = on && s5l8947x_sgx_gates_all_active(sgx);
	if (sgx->powered && read_core_regs && sgx->regs) {
		sgx->core_id = readl(sgx->regs + EUR_CR_CORE_ID);
		sgx->core_rev = readl(sgx->regs + EUR_CR_CORE_REVISION);
	}
	return ret;
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
	if (sysfs_streq(buf, "probe"))
		ret = s5l8947x_sgx_power_set(sgx, true, true);
	else if (sysfs_streq(buf, "on") || sysfs_streq(buf, "1"))
		ret = s5l8947x_sgx_power_set(sgx, true, false);
	else if (sysfs_streq(buf, "off") || sysfs_streq(buf, "0") ||
		 sysfs_streq(buf, "standby"))
		ret = s5l8947x_sgx_power_set(sgx, false, false);
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

static ssize_t gate_status_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s5l8947x_sgx *sgx = dev_get_drvdata(dev);
	unsigned int i;
	int pos = 0;

	for (i = 0; i < sgx->num_gates; i++) {
		u32 g = sgx->gates[i];
		u32 v = sgx->pmgr ?
			readl(sgx->pmgr + PMGR_GATE_OFFSET(g)) : 0;

		pos += sysfs_emit_at(buf, pos, "gate%u=0x%08x%s",
				     g, v,
				     (i + 1 < sgx->num_gates) ? " " : "\n");
	}
	return pos;
}
static DEVICE_ATTR_RO(gate_status);

static struct attribute *s5l8947x_sgx_attrs[] = {
	&dev_attr_power_state.attr,
	&dev_attr_core_info.attr,
	&dev_attr_gate_status.attr,
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
	int n, ret;

	sgx = devm_kzalloc(&pdev->dev, sizeof(*sgx), GFP_KERNEL);
	if (!sgx)
		return -ENOMEM;

	sgx->dev = &pdev->dev;
	mutex_init(&sgx->lock);
	platform_set_drvdata(pdev, sgx);

	sgx->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sgx->regs))
		return PTR_ERR(sgx->regs);

	sgx->pmgr = devm_ioremap(&pdev->dev, PMGR_PHYS_BASE, 0x2000);
	if (!sgx->pmgr)
		return -ENOMEM;

	sgx->irq = platform_get_irq_optional(pdev, 0);

	sgx->num_gates = ARRAY_SIZE(s5l8947x_sgx_default_gates);
	memcpy(sgx->gates, s5l8947x_sgx_default_gates,
	       sizeof(s5l8947x_sgx_default_gates));
	n = device_property_count_u32(&pdev->dev, "apple,clock-gates");
	if (n > 0 && n <= SGX_MAX_GATES &&
	    !device_property_read_u32_array(&pdev->dev, "apple,clock-gates",
					    sgx->gates, n))
		sgx->num_gates = n;

	(void)s5l8947x_sgx_power_set(sgx, true, true);

	sgx->miscdev.minor = MISC_DYNAMIC_MINOR;
	sgx->miscdev.name = "sgx0";
	sgx->miscdev.fops = &s5l8947x_sgx_fops;
	sgx->miscdev.parent = &pdev->dev;
	ret = misc_register(&sgx->miscdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev,
		 "PowerVR SGX543 registered (/dev/sgx0, irq=%d, gates=%u, state=%s, core_id=0x%08x)\n",
		 sgx->irq, sgx->num_gates, sgx->powered ? "on" : "standby",
		 sgx->core_id);
	return 0;
}

static void s5l8947x_sgx_remove(struct platform_device *pdev)
{
	struct s5l8947x_sgx *sgx = platform_get_drvdata(pdev);

	misc_deregister(&sgx->miscdev);
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
