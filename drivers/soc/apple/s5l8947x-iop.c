// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X IOP coprocessor bring-up.
 *
 * The IOP is a separate ARM core with its own firmware.  The stock OS uses it
 * to drive the NAND controller (AppleIOPFMI); this port drives NAND directly
 * from the main CPU, so the IOP has no client here.  This driver provides what
 * is needed to bring it up and observe it:
 *
 *   - the PMGR clock gate for the block,
 *   - the firmware image, carved from the device's own stock kernel image by
 *     iopcarve at first boot,
 *   - the image patches the stock loader applies (heap and page-table physical
 *     addresses and sizes, configuration flags),
 *   - the CPU start/stop sequence from the A5IOP driver (control register,
 *     image address, configuration word).
 *
 * The message-channel protocol (EmbeddedIOP endpoints, control messages and
 * the panic/NMI path) is not implemented; there is nothing on this port that
 * would use it.
 */

#define pr_fmt(fmt) "s5l8947x-iop: " fmt

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/kstrtox.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

/* IOP register block (mapped from the device tree) */
#define IOP_CTRL		0x00	/* 0 = hold, 1 = start */
#define IOP_IMAGE		0x04	/* firmware image physical address */
#define IOP_CONFIG		0x34	/* coherent/configuration word */

/* A5IOP uses this when the IOP is not coherent with the CPU */
#define IOP_CONFIG_NONCOHERENT	0x01ff01ffU

/* Firmware image layout (all offsets are into the 0x24000-byte image) */
#define IOPFW_SIZE		0x24000U
#define IOPFW_BUILD_OFF		0x210U		/* "s5l8942xiop" */
#define IOPFW_CONFIG_OFF	0x13000U	/* "gfnc" + flags */
#define IOPFW_CONFIG_MAGIC	0x636e6667U
#define IOPFW_CONFIG_FLAGS	0x04U
#define IOPFW_CONFIG_MESSAGE	0x0cU
#define IOPFW_HEAP_BASE_OFF	0x1318cU
#define IOPFW_HEAP_SIZE_OFF	0x13190U
#define IOPFW_PT_SIZE_OFF	0x13360U
#define IOPFW_PT_BASE_OFF	0x13364U
#define IOPFW_HEAP_REQUIRED	0xa000U

/* The stock loader sets bit 0 for a non-coherent IOP */
#define IOPFW_FLAG_NONCOHERENT	0x1U

struct s5l8947x_iop {
	struct device *dev;
	void __iomem *regs;
	struct clk *clk;
	const char *firmware_name;

	void *image;
	dma_addr_t image_phys;
	void *heap;
	dma_addr_t heap_phys;
	void *page_table;
	dma_addr_t page_table_phys;
	size_t page_table_size;

	struct mutex lock;
	bool started;
};

static bool s5l8947x_iop_valid_image(const u8 *p)
{
	u32 pt_size = get_unaligned_le32(p + IOPFW_PT_SIZE_OFF);

	if (get_unaligned_le32(p) != 0xea00000eU)
		return false;
	if (memcmp(p + IOPFW_BUILD_OFF, "s5l8942xiop", 11))
		return false;
	if (get_unaligned_le32(p + IOPFW_CONFIG_OFF) != IOPFW_CONFIG_MAGIC)
		return false;
	return pt_size >= 0x1000 && pt_size <= 0x100000 &&
	       is_power_of_2(pt_size);
}

static void s5l8947x_iop_free(struct s5l8947x_iop *iop)
{
	if (iop->image)
		dma_free_coherent(iop->dev, IOPFW_SIZE, iop->image,
				  iop->image_phys);
	if (iop->heap)
		dma_free_coherent(iop->dev, IOPFW_HEAP_REQUIRED, iop->heap,
				  iop->heap_phys);
	if (iop->page_table)
		dma_free_coherent(iop->dev, iop->page_table_size,
				  iop->page_table, iop->page_table_phys);
	iop->image = NULL;
	iop->heap = NULL;
	iop->page_table = NULL;
	iop->page_table_size = 0;
}

static int s5l8947x_iop_alloc(struct s5l8947x_iop *iop, size_t pt_size)
{
	struct device *dev = iop->dev;

	if (iop->image && iop->page_table_size == pt_size)
		return 0;
	if (iop->image || iop->heap || iop->page_table)
		s5l8947x_iop_free(iop);

	iop->image = dma_alloc_coherent(dev, IOPFW_SIZE, &iop->image_phys,
					GFP_KERNEL);
	iop->heap = dma_alloc_coherent(dev, IOPFW_HEAP_REQUIRED,
				       &iop->heap_phys, GFP_KERNEL);
	iop->page_table = dma_alloc_coherent(dev, pt_size,
					     &iop->page_table_phys,
					     GFP_KERNEL);
	iop->page_table_size = pt_size;
	if (!iop->image || !iop->heap || !iop->page_table) {
		s5l8947x_iop_free(iop);
		return -ENOMEM;
	}
	return 0;
}

static int s5l8947x_iop_start(struct s5l8947x_iop *iop)
{
	const struct firmware *fw;
	u8 *image = iop->image;
	size_t pt_size;
	u32 flags;
	int ret;

	if (iop->started)
		return 0;

	ret = request_firmware(&fw, iop->firmware_name, iop->dev);
	if (ret)
		return ret;
	if (fw->size != IOPFW_SIZE || !s5l8947x_iop_valid_image(fw->data)) {
		dev_err(iop->dev, "firmware is not a known IOP image\n");
		ret = -EINVAL;
		goto out_release;
	}

	pt_size = get_unaligned_le32(fw->data + IOPFW_PT_SIZE_OFF);
	ret = s5l8947x_iop_alloc(iop, pt_size);
	if (ret)
		goto out_release;

	image = iop->image;
	memcpy(image, fw->data, IOPFW_SIZE);
	memset(iop->heap, 0, IOPFW_HEAP_REQUIRED);
	memset(iop->page_table, 0, pt_size);

	/* The stock loader patches the heap and page-table variables in place */
	put_unaligned_le32(iop->heap_phys, image + IOPFW_HEAP_BASE_OFF);
	put_unaligned_le32(IOPFW_HEAP_REQUIRED, image + IOPFW_HEAP_SIZE_OFF);
	put_unaligned_le32(iop->page_table_phys, image + IOPFW_PT_BASE_OFF);

	flags = get_unaligned_le32(image + IOPFW_CONFIG_OFF +
				   IOPFW_CONFIG_FLAGS);
	flags |= IOPFW_FLAG_NONCOHERENT;
	put_unaligned_le32(flags, image + IOPFW_CONFIG_OFF +
			   IOPFW_CONFIG_FLAGS);
	put_unaligned_le32(0, image + IOPFW_CONFIG_OFF +
			   IOPFW_CONFIG_MESSAGE);

	writel(0, iop->regs + IOP_CTRL);
	wmb();
	writel(lower_32_bits(iop->image_phys), iop->regs + IOP_IMAGE);
	writel(IOP_CONFIG_NONCOHERENT, iop->regs + IOP_CONFIG);
	wmb();
	writel(1, iop->regs + IOP_CTRL);
	wmb();
	msleep(20);

	if (readl(iop->regs + IOP_CTRL) != 1) {
		dev_err(iop->dev,
			"control register did not latch (ctrl=0x%08x); the IOP is still held in reset or its power gate is not configured\n",
			readl(iop->regs + IOP_CTRL));
		s5l8947x_iop_free(iop);
		ret = -EIO;
		goto out_release;
	}

	iop->started = true;
	dev_info(iop->dev,
		 "started: ctrl=0x%08x image=0x%08x heap=0x%08x+0x%x page-table=0x%08x+0x%zx\n",
		 readl(iop->regs + IOP_CTRL), (u32)iop->image_phys,
		 (u32)iop->heap_phys, IOPFW_HEAP_REQUIRED,
		 (u32)iop->page_table_phys, pt_size);
	ret = 0;

out_release:
	release_firmware(fw);
	return ret;
}

static void s5l8947x_iop_stop(struct s5l8947x_iop *iop)
{
	if (!iop->started)
		return;
	writel(0, iop->regs + IOP_CTRL);
	wmb();
	iop->started = false;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct s5l8947x_iop *iop = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&iop->lock);
	ret = sysfs_emit(buf, "%s ctrl=0x%08x image=0x%08x\n",
			 iop->started ? "started" : "stopped",
			 readl(iop->regs + IOP_CTRL),
			 iop->image ? (u32)iop->image_phys : 0);
	mutex_unlock(&iop->lock);
	return ret;
}
static DEVICE_ATTR_RO(state);

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct s5l8947x_iop *iop = dev_get_drvdata(dev);
	bool on;
	int ret = 0;

	if (kstrtobool(buf, &on))
		return -EINVAL;
	mutex_lock(&iop->lock);
	if (on)
		ret = s5l8947x_iop_start(iop);
	else
		s5l8947x_iop_stop(iop);
	mutex_unlock(&iop->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(start);

static struct attribute *s5l8947x_iop_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_start.attr,
	NULL,
};

static const struct attribute_group s5l8947x_iop_group = {
	.attrs = s5l8947x_iop_attrs,
};

static int s5l8947x_iop_probe(struct platform_device *pdev)
{
	struct s5l8947x_iop *iop;
	struct resource *res;
	int ret;

	iop = devm_kzalloc(&pdev->dev, sizeof(*iop), GFP_KERNEL);
	if (!iop)
		return -ENOMEM;
	iop->dev = &pdev->dev;
	mutex_init(&iop->lock);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	iop->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(iop->regs))
		return PTR_ERR(iop->regs);

	iop->clk = devm_clk_get(&pdev->dev, "iop");
	if (IS_ERR(iop->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(iop->clk),
				     "failed to get the IOP clock\n");
	ret = clk_prepare_enable(iop->clk);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to enable the IOP clock\n");

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_clk;

	iop->firmware_name = "s5l8947x/iop.fw";
	of_property_read_string(pdev->dev.of_node, "firmware-name",
				&iop->firmware_name);
	platform_set_drvdata(pdev, iop);

	ret = devm_device_add_group(&pdev->dev, &s5l8947x_iop_group);
	if (ret)
		goto err_clk;

	dev_info(&pdev->dev, "registered %s (firmware %s, ctrl=0x%08x)\n",
		 dev_name(&pdev->dev), iop->firmware_name,
		 readl(iop->regs + IOP_CTRL));
	return 0;

err_clk:
	clk_disable_unprepare(iop->clk);
	return ret;
}

static void s5l8947x_iop_remove(struct platform_device *pdev)
{
	struct s5l8947x_iop *iop = platform_get_drvdata(pdev);

	s5l8947x_iop_stop(iop);
	s5l8947x_iop_free(iop);
	clk_disable_unprepare(iop->clk);
}

static const struct of_device_id s5l8947x_iop_of_match[] = {
	{ .compatible = "apple,s5l8947x-iop" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l8947x_iop_of_match);

static struct platform_driver s5l8947x_iop_driver = {
	.probe	= s5l8947x_iop_probe,
	.remove	= s5l8947x_iop_remove,
	.driver	= {
		.name		= "s5l8947x-iop",
		.of_match_table	= s5l8947x_iop_of_match,
	},
};
module_platform_driver(s5l8947x_iop_driver);

MODULE_DESCRIPTION("S5L8947X IOP coprocessor power and firmware bring-up");
MODULE_LICENSE("GPL");
