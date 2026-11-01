// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X VXD video decoder: power, firmware upload and microkernel handshake
 *
 * The decoder at 0x38100000 is a microcontroller running an image that is not
 * shipped with Linux: stage2 carves it from the device's own kernel image with
 * vxdcarve into /lib/firmware/s5l8947x/vxd390.fw. Starting it means
 *
 *   - enabling its PMGR power state (Linux clock gate 10, the same register
 *     the stock driver writes: request bits 3:0, status bits 7:4),
 *   - resetting the block (0x600 = 0x11110100, wait for bit 8, then 0),
 *   - building the 40 KiB upload image (0xcd fill, the two carved segments,
 *     0x10101010 end marker) in coherent memory and handing it to the
 *     decoder's DMA engine (0x500..0x514, 0x340..0x348),
 *   - polling the handshake word (0x2fe0) until the microkernel writes
 *     0xa5a5a5a5 and publishes the command/response ring bases.
 *
 * Nothing beyond that handshake is provided: no bitstream interface, no
 * V4L2 mem2mem node, no output path. EXPERIMENTAL: on 2026-11-01 the block
 * accepts the power state and stays readable, but its registers still read
 * zero and drop writes, so the media clock source has not been raised yet and
 * the handshake times out. The register window must never be read while the
 * clock gate is off; that stalls the bus.
 */

#define pr_fmt(fmt) "s5l8947x-vxd: " fmt

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#define VXD_FW_NAME		"s5l8947x/vxd390.fw"
#define VXD_IMAGE_SIZE		0xa000
#define VXD_FILL		0xcd
#define VXD_END_MARKER		0x10101010
#define VXD_HANDSHAKE_MAGIC	0xa5a5a5a5

/* Registers, offsets from 0x38100000 (see research/docs/vxd-recon.md) */
#define VXD_CTRL		0x000
#define VXD_NOTIFY		0x080
#define VXD_UPLOAD_WORDS	0x340
#define VXD_UPLOAD_OFFSET	0x344
#define VXD_UPLOAD_STATUS	0x348
#define VXD_DMA_ADDR		0x500
#define VXD_DMA_SIZE		0x504
#define VXD_DMA_CFG		0x508
#define VXD_DMA_STATUS		0x50c
#define VXD_DMA_EXTRA		0x514
#define VXD_CLK_CTRL		0x600
#define VXD_EVENT_STATUS	0x608
#define VXD_EVENT_CLEAR		0x60c
#define VXD_EVENT_ENABLE	0x610
#define VXD_CORE_CFG		0x620
#define VXD_AUX0		0x680
#define VXD_AUX1		0x688
#define VXD_AUX2		0x690
#define VXD_AUX3		0x694
#define VXD_AUX4		0x6d4
#define VXD_MEM_CTL		0x6f0
#define VXD_HANDSHAKE		0x2fe0
#define VXD_RSP_RING		0x2fe4
#define VXD_RSP_READ		0x2fe8
#define VXD_RSP_WRITE		0x2fec
#define VXD_CMD_RING		0x2ff0
#define VXD_CMD_READ		0x2ff4
#define VXD_IRQ_CFG		0x2ff8
#define VXD_CMD_WRITE		0x2ffc

#define VXD_CLK_RESET_VAL	0x11110100
#define VXD_CLK_RESET_BIT	BIT(8)
#define VXD_CORE_CFG_VAL	0x3e0041
#define VXD_EVENT_HOST		BIT(14)
#define VXD_DMA_SIZE_TAG	0x10000
#define VXD_DMA_CFG_VAL		0x01000000
#define VXD_DMA_EXTRA_VAL	0x350
#define VXD_DMA_DONE		BIT(17)
#define VXD_UPLOAD_DONE		BIT(0)
#define VXD_MEM_RUN		4

/* Firmware container written by vxdcarve (all little endian) */
#define VXDF_MAGIC		0x46445856u	/* "VXDF" */
#define VXDF_VERSION		1
#define VXDF_MAX_SEGMENTS	8
#define VXDF_HEADER		32

struct vxd_segment {
	u32 dst;
	u32 size;
};

struct s5l8947x_vxd {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	const char *fw_name;

	void *image;
	dma_addr_t image_dma;

	struct mutex lock;		/* serialises start/stop */
	bool running;
	bool clocked;
	int last_error;
	u32 ctl0;
	u32 cmd_ring;
	u32 rsp_ring;
	u8 fw_uuid[16];
};

static inline void vxd_wr(struct s5l8947x_vxd *vxd, u32 reg, u32 val)
{
	writel(val, vxd->base + reg);
}

static inline u32 vxd_rd(struct s5l8947x_vxd *vxd, u32 reg)
{
	return readl(vxd->base + reg);
}

static int vxd_reset(struct s5l8947x_vxd *vxd)
{
	u32 val;
	int ret;

	vxd_wr(vxd, VXD_CLK_CTRL, VXD_CLK_RESET_VAL);
	ret = readl_poll_timeout(vxd->base + VXD_CLK_CTRL, val,
				 val & VXD_CLK_RESET_BIT, 100, 100000);
	if (ret)
		return ret;
	vxd_wr(vxd, VXD_CLK_CTRL, 0);
	vxd_wr(vxd, VXD_EVENT_CLEAR, 0xffffffff);
	return 0;
}

static void vxd_init_regs(struct s5l8947x_vxd *vxd)
{
	vxd_wr(vxd, VXD_CORE_CFG, VXD_CORE_CFG_VAL);
	vxd_wr(vxd, VXD_EVENT_CLEAR, VXD_EVENT_HOST);
	vxd_wr(vxd, VXD_AUX0, 0);
	vxd_wr(vxd, VXD_AUX4, 4);
	vxd_wr(vxd, VXD_AUX2, 0);
	vxd_wr(vxd, VXD_CMD_WRITE, 0);
}

static int vxd_parse_fw(struct s5l8947x_vxd *vxd, const struct firmware *fw)
{
	struct vxd_segment seg[VXDF_MAX_SEGMENTS];
	const u8 *buf = fw->data;
	u32 image_size, count, i;
	size_t off;
	u32 prev_end;

	if (fw->size < VXDF_HEADER)
		return -EINVAL;
	if (get_unaligned_le32(buf) != VXDF_MAGIC ||
	    get_unaligned_le32(buf + 4) != VXDF_VERSION)
		return -EINVAL;

	image_size = get_unaligned_le32(buf + 8);
	count = get_unaligned_le32(buf + 12);
	if (image_size != VXD_IMAGE_SIZE || !count || count > VXDF_MAX_SEGMENTS)
		return -EINVAL;
	if (VXDF_HEADER + count * 16 > fw->size)
		return -EINVAL;

	memcpy(vxd->fw_uuid, buf + 16, sizeof(vxd->fw_uuid));

	prev_end = 0;
	off = VXDF_HEADER + count * 16;
	for (i = 0; i < count; i++) {
		const u8 *e = buf + VXDF_HEADER + i * 16;

		seg[i].dst = get_unaligned_le32(e);
		seg[i].size = get_unaligned_le32(e + 4);
		if (!seg[i].size || seg[i].dst < prev_end ||
		    seg[i].dst + seg[i].size > image_size)
			return -EINVAL;
		prev_end = seg[i].dst + seg[i].size;
	}

	memset(vxd->image, VXD_FILL, image_size);
	for (i = 0; i < count; i++) {
		off = ALIGN(off, 64);
		if (off + seg[i].size > fw->size)
			return -EINVAL;
		memcpy(vxd->image + seg[i].dst, buf + off, seg[i].size);
		off += seg[i].size;
	}
	put_unaligned_le32(VXD_END_MARKER, vxd->image + image_size - 4);
	return 0;
}

static int vxd_upload(struct s5l8947x_vxd *vxd, u32 offset, u32 words)
{
	u32 val;
	int ret;

	vxd_wr(vxd, VXD_UPLOAD_OFFSET, offset);
	vxd_wr(vxd, VXD_UPLOAD_WORDS, words | VXD_DMA_SIZE_TAG);
	vxd_wr(vxd, VXD_CLK_CTRL, vxd_rd(vxd, VXD_CLK_CTRL) & ~BIT(12));
	vxd_wr(vxd, VXD_DMA_STATUS, 0);
	vxd_wr(vxd, VXD_AUX1, 0);
	vxd_wr(vxd, VXD_AUX3, vxd->image_dma);
	vxd_wr(vxd, VXD_DMA_ADDR, vxd->image_dma);
	vxd_wr(vxd, VXD_DMA_SIZE, words | VXD_DMA_SIZE_TAG);
	vxd_wr(vxd, VXD_DMA_CFG, VXD_DMA_CFG_VAL);
	vxd_wr(vxd, VXD_DMA_EXTRA, VXD_DMA_EXTRA_VAL);
	vxd_wr(vxd, VXD_DMA_SIZE, words | VXD_DMA_SIZE_TAG);

	ret = readl_poll_timeout(vxd->base + VXD_DMA_STATUS, val,
				 val & VXD_DMA_DONE, 100, 100000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(vxd->base + VXD_UPLOAD_STATUS, val,
				 val & VXD_UPLOAD_DONE, 100, 100000);
	vxd_wr(vxd, VXD_MEM_CTL, VXD_MEM_RUN);
	return ret;
}

static int vxd_handshake(struct s5l8947x_vxd *vxd)
{
	u32 val;
	int ret;

	vxd_wr(vxd, VXD_CTRL, 1);
	ret = readl_poll_timeout(vxd->base + VXD_HANDSHAKE, val,
				 val == VXD_HANDSHAKE_MAGIC, 1000, 200000);
	vxd->ctl0 = val;
	if (ret)
		return -ETIMEDOUT;

	vxd->cmd_ring = vxd_rd(vxd, VXD_CMD_RING);
	vxd->rsp_ring = vxd_rd(vxd, VXD_RSP_RING);
	vxd_wr(vxd, VXD_EVENT_ENABLE, VXD_EVENT_HOST);
	return 0;
}

static int vxd_start(struct s5l8947x_vxd *vxd)
{
	const struct firmware *fw;
	u32 size = VXD_IMAGE_SIZE;
	int ret;

	lockdep_assert_held(&vxd->lock);
	if (vxd->running)
		return -EBUSY;

	ret = clk_prepare_enable(vxd->clk);
	if (ret)
		return ret;
	vxd->clocked = true;

	ret = request_firmware(&fw, vxd->fw_name, vxd->dev);
	if (ret)
		goto err_clock;

	ret = vxd_parse_fw(vxd, fw);
	release_firmware(fw);
	if (ret) {
		dev_err(vxd->dev, "bad firmware image (%d)\n", ret);
		goto err_clock;
	}

	vxd_wr(vxd, 0x200, 1);
	vxd_wr(vxd, VXD_HANDSHAKE, 0);
	vxd_wr(vxd, VXD_RSP_READ, 0);
	vxd_wr(vxd, VXD_RSP_WRITE, 0);
	vxd_wr(vxd, VXD_CMD_READ, 0);
	vxd_wr(vxd, VXD_CMD_WRITE, 0);
	vxd_wr(vxd, VXD_IRQ_CFG, 8);

	ret = vxd_reset(vxd);
	if (ret) {
		dev_err(vxd->dev, "reset timed out\n");
		goto err_clock;
	}
	vxd_init_regs(vxd);
	ret = vxd_upload(vxd, 0, size / 4);
	if (ret) {
		dev_err(vxd->dev, "firmware upload timed out\n");
		goto err_clock;
	}
	ret = vxd_handshake(vxd);
	if (ret) {
		dev_err(vxd->dev, "no handshake (ctl0=0x%08x, events 0x%08x)\n",
			vxd->ctl0, vxd_rd(vxd, VXD_EVENT_STATUS));
		goto err_clock;
	}

	dev_info(vxd->dev, "decoder up: cmd ring 0x%08x, rsp ring 0x%08x\n",
		 vxd->cmd_ring, vxd->rsp_ring);
	vxd->running = true;
	return 0;

err_clock:
	vxd->last_error = ret;
	clk_disable_unprepare(vxd->clk);
	vxd->clocked = false;
	return ret;
}

static void vxd_stop(struct s5l8947x_vxd *vxd)
{
	lockdep_assert_held(&vxd->lock);

	if (vxd->clocked) {
		/* Back to the state the stock driver leaves on stop */
		if (vxd->running)
			vxd_wr(vxd, VXD_CORE_CFG, 1);
		vxd_wr(vxd, VXD_EVENT_ENABLE, 0);
		clk_disable_unprepare(vxd->clk);
		vxd->clocked = false;
	}
	vxd->running = false;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct s5l8947x_vxd *vxd = dev_get_drvdata(dev);
	const char *state;

	if (vxd->running)
		state = "running";
	else if (vxd->clocked)
		state = "clocked";
	else if (vxd->last_error)
		state = "failed";
	else
		state = "idle";
	return sysfs_emit(buf, "%s ctl0=0x%08x cmd=0x%08x rsp=0x%08x err=%d\n",
			  state, vxd->ctl0, vxd->cmd_ring, vxd->rsp_ring,
			  vxd->last_error);
}
static DEVICE_ATTR_RO(state);

static ssize_t start_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct s5l8947x_vxd *vxd = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	mutex_lock(&vxd->lock);
	if (on)
		ret = vxd_start(vxd);
	else
		vxd_stop(vxd), ret = 0;
	mutex_unlock(&vxd->lock);
	return ret ?: count;
}
static DEVICE_ATTR_WO(start);

static ssize_t firmware_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct s5l8947x_vxd *vxd = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s %*phN\n", vxd->fw_name, 16, vxd->fw_uuid);
}
static DEVICE_ATTR_RO(firmware);

static struct attribute *vxd_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_start.attr,
	&dev_attr_firmware.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vxd);

static int s5l8947x_vxd_probe(struct platform_device *pdev)
{
	struct s5l8947x_vxd *vxd;
	int ret;

	vxd = devm_kzalloc(&pdev->dev, sizeof(*vxd), GFP_KERNEL);
	if (!vxd)
		return -ENOMEM;

	vxd->dev = &pdev->dev;
	mutex_init(&vxd->lock);
	platform_set_drvdata(pdev, vxd);

	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	vxd->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vxd->base))
		return PTR_ERR(vxd->base);

	/* Only claimed, not enabled: the window must not be touched until start */
	vxd->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(vxd->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(vxd->clk),
				     "failed to get clock\n");

	vxd->fw_name = VXD_FW_NAME;
	of_property_read_string(pdev->dev.of_node, "firmware-name",
				&vxd->fw_name);

	vxd->image = dmam_alloc_coherent(&pdev->dev, VXD_IMAGE_SIZE,
					 &vxd->image_dma, GFP_KERNEL);
	if (!vxd->image)
		return -ENOMEM;

	dev_info(&pdev->dev, "VXD decoder registered (firmware %s)\n",
		 vxd->fw_name);
	return 0;
}

static void s5l8947x_vxd_remove(struct platform_device *pdev)
{
	struct s5l8947x_vxd *vxd = platform_get_drvdata(pdev);

	mutex_lock(&vxd->lock);
	vxd_stop(vxd);
	mutex_unlock(&vxd->lock);
}

static const struct of_device_id s5l8947x_vxd_of_match[] = {
	{ .compatible = "apple,s5l8947x-vxd" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l8947x_vxd_of_match);

static struct platform_driver s5l8947x_vxd_driver = {
	.probe = s5l8947x_vxd_probe,
	.remove = s5l8947x_vxd_remove,
	.driver = {
		.name = "s5l8947x-vxd",
		.of_match_table = s5l8947x_vxd_of_match,
		.dev_groups = vxd_groups,
	},
};
module_platform_driver(s5l8947x_vxd_driver);

MODULE_DESCRIPTION("S5L8947X VXD video decoder power, firmware upload and handshake");
MODULE_FIRMWARE(VXD_FW_NAME);
MODULE_LICENSE("GPL");
