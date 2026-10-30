// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X central DMA controller (CDMA).
 *
 * The register model was measured on hardware (see the bring-up notes):
 *
 *   0x37000000 + 0x000     global page
 *     +0x00 / +0x04        channel enable set, channels 0-31 / 32-63
 *     +0x08 / +0x0c        channel enable clear
 *   0x37000000 + n * 0x1000  channel n, 1..42 (one interrupt each)
 *     +0x00 CSR            command / status
 *     +0x04 DCR            request line, width, burst, direction
 *     +0x08 DAR            device address, 24-bit offset in the 0x34000000 window
 *     +0x0c DBR            zero
 *     +0x10 MAR            memory read-ahead pointer (not the transfer position)
 *     +0x14 CAR            segment the channel is working on, 0 when finished
 *     +0x18 ERR            error code
 *
 * A transfer is a list of 32-byte segments { next, control, address, length }
 * with next = 0 ending the list; pointing the last segment back at the first
 * makes a ring. Bit 9 of the segment control raises the channel interrupt when
 * the segment has been read (CSR bit 20). CSR bit 3 enables the interrupt for
 * the end of the list (CSR bit 19).
 *
 * The status bits are write-1-to-clear, but every CSR write also replaces the
 * interrupt enables and the type field, so an acknowledge writes them back.
 *
 * Only memory-to-device transfers to peripherals in the 0x34000000 window are
 * supported: the other direction and the other windows have not been measured.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "virt-dma.h"

#define CDMA_NR_CHANNELS	42
#define CDMA_NR_REQUESTS	32

#define CDMA_ENABLE_SET(ch)	(((ch) >> 5) * 4)
#define CDMA_ENABLE_CLR(ch)	(0x08 + ((ch) >> 5) * 4)

#define CDMA_CH_STRIDE		0x1000
#define CDMA_CSR		0x00
#define CDMA_DCR		0x04
#define CDMA_DAR		0x08
#define CDMA_DBR		0x0c
#define CDMA_MAR		0x10
#define CDMA_CAR		0x14
#define CDMA_ERR		0x18

#define CDMA_CSR_RUN		BIT(0)
#define CDMA_CSR_ABORT		BIT(1)
#define CDMA_CSR_KICK		BIT(2)
#define CDMA_CSR_DONE_IE	BIT(3)
#define CDMA_CSR_ERR_IE		BIT(4)
#define CDMA_CSR_ACTIVE		BIT(16)
#define CDMA_CSR_ERR		BIT(18)
#define CDMA_CSR_DONE		BIT(19)
#define CDMA_CSR_SEG		BIT(20)
#define CDMA_CSR_READY		BIT(21)
#define CDMA_CSR_TYPE		GENMASK(25, 24)
#define CDMA_CSR_STATUS		(CDMA_CSR_ERR | CDMA_CSR_DONE | CDMA_CSR_SEG)
#define CDMA_CSR_KEEP		(CDMA_CSR_TYPE | CDMA_CSR_DONE_IE | CDMA_CSR_ERR_IE)

/* Transfer type 2, as used for every peripheral transfer on this SoC */
#define CDMA_TYPE		2
#define CDMA_CSR_START		(FIELD_PREP(CDMA_CSR_TYPE, CDMA_TYPE) | \
				 CDMA_CSR_DONE_IE | CDMA_CSR_ERR_IE | \
				 CDMA_CSR_ABORT | CDMA_CSR_RUN)

#define CDMA_DCR_TO_DEV		BIT(1)
#define CDMA_DCR_WIDTH		GENMASK(3, 2)	/* log2 of the bytes per request */
#define CDMA_DCR_BURST		GENMASK(7, 6)
#define CDMA_DCR_BURST_1	2
#define CDMA_DCR_BURST_16	3
#define CDMA_DCR_REQUEST	GENMASK(20, 16)

#define CDMA_DEV_WINDOW		0x34000000
#define CDMA_DEV_WINDOW_MASK	GENMASK(23, 0)

#define CDMA_SEG_CTL		(FIELD_PREP(GENMASK(25, 24), CDMA_TYPE) | 0x3)
#define CDMA_SEG_CTL_IRQ	BIT(9)
#define CDMA_SEG_MAX		SZ_4K	/* longest segment tested on hardware */

/* Microseconds to wait for the kick handshake and for an abort to land */
#define CDMA_POLL_US		1000

struct s5l_cdma_hw_seg {
	u32 next;
	u32 ctl;
	u32 addr;
	u32 len;
	u32 reserved[4];
};

struct s5l_cdma_seg {
	struct s5l_cdma_hw_seg *hw;
	dma_addr_t phys;
	u32 offset;		/* bytes before this segment in the transfer */
};

struct s5l_cdma_desc {
	struct virt_dma_desc vd;
	struct dma_pool *pool;
	u32 dcr;
	u32 dar;
	size_t len;
	bool cyclic;
	unsigned int nr_segs;
	struct s5l_cdma_seg segs[] __counted_by(nr_segs);
};

struct s5l_cdma_chan {
	struct virt_dma_chan vc;
	struct s5l_cdma *cdma;
	void __iomem *base;
	unsigned int id;
	unsigned int request;
	struct dma_slave_config cfg;
	struct s5l_cdma_desc *active;
};

struct s5l_cdma {
	struct dma_device dma;
	struct device *dev;
	void __iomem *regs;
	struct dma_pool *pool;
	struct s5l_cdma_chan chans[CDMA_NR_CHANNELS];
};

static inline struct s5l_cdma_chan *to_cdma_chan(struct dma_chan *c)
{
	return container_of(c, struct s5l_cdma_chan, vc.chan);
}

static inline struct s5l_cdma_desc *to_cdma_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct s5l_cdma_desc, vd);
}

static void s5l_cdma_desc_free(struct virt_dma_desc *vd)
{
	struct s5l_cdma_desc *d = to_cdma_desc(vd);
	unsigned int i;

	for (i = 0; i < d->nr_segs; i++)
		if (d->segs[i].hw)
			dma_pool_free(d->pool, d->segs[i].hw, d->segs[i].phys);
	kfree(d);
}

/* Stop the channel and wait until it no longer reports itself active. */
static void s5l_cdma_hw_abort(struct s5l_cdma_chan *c)
{
	u32 csr;

	writel(CDMA_CSR_ABORT, c->base + CDMA_CSR);
	if (readl_poll_timeout_atomic(c->base + CDMA_CSR, csr,
				      !(csr & CDMA_CSR_ACTIVE), 1,
				      CDMA_POLL_US))
		dev_warn(c->cdma->dev, "channel %u: abort timed out, csr %08x\n",
			 c->id, csr);
	writel(CDMA_CSR_STATUS, c->base + CDMA_CSR);
}

/* Called with the channel lock held. */
static void s5l_cdma_start_next(struct s5l_cdma_chan *c)
{
	struct virt_dma_desc *vd = vchan_next_desc(&c->vc);
	struct s5l_cdma_desc *d;
	u32 csr;

	c->active = NULL;
	if (!vd)
		return;
	list_del(&vd->node);
	d = to_cdma_desc(vd);
	c->active = d;

	csr = readl(c->base + CDMA_CSR);
	if (csr & CDMA_CSR_ACTIVE)
		s5l_cdma_hw_abort(c);
	else
		writel(CDMA_CSR_STATUS, c->base + CDMA_CSR);

	writel(d->dcr, c->base + CDMA_DCR);
	writel(d->dar, c->base + CDMA_DAR);
	writel(0, c->base + CDMA_DBR);
	writel(d->segs[0].phys, c->base + CDMA_CAR);

	writel(readl(c->base + CDMA_CSR) | CDMA_CSR_KICK, c->base + CDMA_CSR);
	if (readl_poll_timeout_atomic(c->base + CDMA_CSR, csr,
				      csr & CDMA_CSR_READY, 1, CDMA_POLL_US))
		dev_warn(c->cdma->dev, "channel %u: no kick acknowledge, csr %08x\n",
			 c->id, csr);
	writel(CDMA_CSR_START, c->base + CDMA_CSR);
}

static irqreturn_t s5l_cdma_irq(int irq, void *data)
{
	struct s5l_cdma_chan *c = data;
	struct s5l_cdma_desc *d;
	u32 csr, status;

	spin_lock(&c->vc.lock);
	csr = readl(c->base + CDMA_CSR);
	status = csr & CDMA_CSR_STATUS;
	if (!status) {
		spin_unlock(&c->vc.lock);
		return IRQ_NONE;
	}
	writel(status | (csr & CDMA_CSR_KEEP), c->base + CDMA_CSR);

	d = c->active;
	if (!d)
		goto out;

	/*
	 * A list that ran to its end can also report an error, with ERR still
	 * zero: the peripheral keeps requesting after the last word. Only an
	 * error without completion is a failed transfer.
	 */
	if ((status & CDMA_CSR_ERR) && !(status & CDMA_CSR_DONE)) {
		dev_err_ratelimited(c->cdma->dev,
				    "channel %u: transfer error %08x, csr %08x\n",
				    c->id, readl(c->base + CDMA_ERR), csr);
		s5l_cdma_hw_abort(c);
		d->vd.tx_result.result = DMA_TRANS_ABORTED;
		vchan_cookie_complete(&d->vd);
		s5l_cdma_start_next(c);
	} else if (d->cyclic) {
		if (status & CDMA_CSR_SEG)
			vchan_cyclic_callback(&d->vd);
	} else if (status & CDMA_CSR_DONE) {
		vchan_cookie_complete(&d->vd);
		s5l_cdma_start_next(c);
	}
out:
	spin_unlock(&c->vc.lock);
	return IRQ_HANDLED;
}

static int s5l_cdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	void __iomem *regs = c->cdma->regs;

	/* A disabled channel drops register writes. */
	writel(BIT(c->id & 31), regs + CDMA_ENABLE_SET(c->id));
	usleep_range(1000, 2000);
	return 0;
}

static void s5l_cdma_free_chan_resources(struct dma_chan *chan)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	void __iomem *regs = c->cdma->regs;

	vchan_free_chan_resources(&c->vc);
	writel(BIT(c->id & 31), regs + CDMA_ENABLE_CLR(c->id));
}

static int s5l_cdma_slave_config(struct dma_chan *chan,
				 struct dma_slave_config *cfg)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);

	c->cfg = *cfg;
	return 0;
}

/* Channel register values for a memory-to-device transfer. */
static int s5l_cdma_dev_regs(struct s5l_cdma_chan *c,
			     enum dma_transfer_direction dir,
			     u32 *dcr, u32 *dar)
{
	struct dma_slave_config *cfg = &c->cfg;
	unsigned int width, burst;

	if (dir != DMA_MEM_TO_DEV)
		return -EINVAL;

	if ((cfg->dst_addr & ~(phys_addr_t)CDMA_DEV_WINDOW_MASK) != CDMA_DEV_WINDOW) {
		dev_err(c->cdma->dev, "channel %u: device address %pa out of reach\n",
			c->id, &cfg->dst_addr);
		return -EINVAL;
	}

	switch (cfg->dst_addr_width) {
	case DMA_SLAVE_BUSWIDTH_1_BYTE:
		width = 0;
		break;
	case DMA_SLAVE_BUSWIDTH_2_BYTES:
		width = 1;
		break;
	case DMA_SLAVE_BUSWIDTH_UNDEFINED:
	case DMA_SLAVE_BUSWIDTH_4_BYTES:
		width = 2;
		break;
	default:
		return -EINVAL;
	}

	switch (cfg->dst_maxburst) {
	case 0:
	case 1:
		burst = CDMA_DCR_BURST_1;
		break;
	case 16:
		burst = CDMA_DCR_BURST_16;
		break;
	default:
		return -EINVAL;
	}

	*dcr = FIELD_PREP(CDMA_DCR_REQUEST, c->request) |
	       FIELD_PREP(CDMA_DCR_BURST, burst) |
	       FIELD_PREP(CDMA_DCR_WIDTH, width) | CDMA_DCR_TO_DEV;
	*dar = cfg->dst_addr & CDMA_DEV_WINDOW_MASK;
	return 0;
}

static struct s5l_cdma_desc *s5l_cdma_alloc_desc(struct s5l_cdma_chan *c,
						 unsigned int nr_segs)
{
	struct s5l_cdma_desc *d;
	unsigned int i;

	d = kzalloc(struct_size(d, segs, nr_segs), GFP_NOWAIT);
	if (!d)
		return NULL;
	d->nr_segs = nr_segs;
	d->pool = c->cdma->pool;
	for (i = 0; i < nr_segs; i++) {
		d->segs[i].hw = dma_pool_zalloc(d->pool, GFP_NOWAIT,
						&d->segs[i].phys);
		if (!d->segs[i].hw) {
			s5l_cdma_desc_free(&d->vd);
			return NULL;
		}
	}
	return d;
}

/*
 * Split [addr, addr + len) into segments starting at index *seg, linking each
 * one to the next. The last segment's link is left for the caller.
 */
static void s5l_cdma_fill(struct s5l_cdma_desc *d, unsigned int *seg,
			  dma_addr_t addr, size_t len, bool irq_at_end)
{
	while (len) {
		struct s5l_cdma_seg *s = &d->segs[*seg];
		u32 n = min_t(size_t, len, CDMA_SEG_MAX);

		s->offset = d->len;
		s->hw->ctl = CDMA_SEG_CTL;
		s->hw->addr = addr;
		s->hw->len = n;
		if (*seg + 1 < d->nr_segs)
			s->hw->next = d->segs[*seg + 1].phys;
		addr += n;
		len -= n;
		d->len += n;
		if (!len && irq_at_end)
			s->hw->ctl |= CDMA_SEG_CTL_IRQ;
		(*seg)++;
	}
}

static bool s5l_cdma_aligned(dma_addr_t addr, size_t len)
{
	return IS_ALIGNED(addr, 4) && IS_ALIGNED(len, 4) && len;
}

static struct dma_async_tx_descriptor *
s5l_cdma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
		       unsigned int sg_len, enum dma_transfer_direction dir,
		       unsigned long flags, void *context)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	struct s5l_cdma_desc *d;
	struct scatterlist *sg;
	unsigned int i, nr_segs = 0, seg = 0;
	u32 dcr, dar;

	if (s5l_cdma_dev_regs(c, dir, &dcr, &dar))
		return NULL;

	for_each_sg(sgl, sg, sg_len, i) {
		if (!s5l_cdma_aligned(sg_dma_address(sg), sg_dma_len(sg)))
			return NULL;
		nr_segs += DIV_ROUND_UP(sg_dma_len(sg), CDMA_SEG_MAX);
	}
	if (!nr_segs)
		return NULL;

	d = s5l_cdma_alloc_desc(c, nr_segs);
	if (!d)
		return NULL;
	d->dcr = dcr;
	d->dar = dar;
	for_each_sg(sgl, sg, sg_len, i)
		s5l_cdma_fill(d, &seg, sg_dma_address(sg), sg_dma_len(sg),
			      false);
	d->segs[nr_segs - 1].hw->next = 0;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

static struct dma_async_tx_descriptor *
s5l_cdma_prep_dma_cyclic(struct dma_chan *chan, dma_addr_t buf_addr,
			 size_t buf_len, size_t period_len,
			 enum dma_transfer_direction dir, unsigned long flags)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	unsigned int i, periods, nr_segs, seg = 0;
	struct s5l_cdma_desc *d;
	u32 dcr, dar;

	if (s5l_cdma_dev_regs(c, dir, &dcr, &dar))
		return NULL;
	if (!s5l_cdma_aligned(buf_addr, period_len) ||
	    buf_len % period_len)
		return NULL;

	periods = buf_len / period_len;
	nr_segs = periods * DIV_ROUND_UP(period_len, CDMA_SEG_MAX);

	d = s5l_cdma_alloc_desc(c, nr_segs);
	if (!d)
		return NULL;
	d->dcr = dcr;
	d->dar = dar;
	d->cyclic = true;
	for (i = 0; i < periods; i++)
		s5l_cdma_fill(d, &seg, buf_addr + i * period_len, period_len,
			      true);
	d->segs[nr_segs - 1].hw->next = d->segs[0].phys;

	return vchan_tx_prep(&c->vc, &d->vd, flags);
}

/*
 * Bytes left in a running transfer, at segment granularity. CAR holds the
 * segment the channel is reading, so everything before it has been consumed.
 */
static size_t s5l_cdma_residue(struct s5l_cdma_chan *c, struct s5l_cdma_desc *d)
{
	u32 car = readl(c->base + CDMA_CAR);
	unsigned int i;

	for (i = 0; i < d->nr_segs; i++)
		if (d->segs[i].phys == car)
			return d->len - d->segs[i].offset;
	/* Between segments, or past the end of a chain. */
	return d->cyclic ? d->len : 0;
}

static enum dma_status s5l_cdma_tx_status(struct dma_chan *chan,
					  dma_cookie_t cookie,
					  struct dma_tx_state *state)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	struct virt_dma_desc *vd;
	enum dma_status ret;
	unsigned long flags;
	size_t residue = 0;

	ret = dma_cookie_status(chan, cookie, state);
	if (ret == DMA_COMPLETE || !state)
		return ret;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (c->active && c->active->vd.tx.cookie == cookie) {
		residue = s5l_cdma_residue(c, c->active);
	} else {
		vd = vchan_find_desc(&c->vc, cookie);
		if (vd)
			residue = to_cdma_desc(vd)->len;
	}
	spin_unlock_irqrestore(&c->vc.lock, flags);

	dma_set_residue(state, residue);
	return ret;
}

static void s5l_cdma_issue_pending(struct dma_chan *chan)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	unsigned long flags;

	spin_lock_irqsave(&c->vc.lock, flags);
	if (vchan_issue_pending(&c->vc) && !c->active)
		s5l_cdma_start_next(c);
	spin_unlock_irqrestore(&c->vc.lock, flags);
}

static int s5l_cdma_terminate_all(struct dma_chan *chan)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&c->vc.lock, flags);
	s5l_cdma_hw_abort(c);
	if (c->active) {
		vchan_terminate_vdesc(&c->active->vd);
		c->active = NULL;
	}
	vchan_get_all_descriptors(&c->vc, &head);
	spin_unlock_irqrestore(&c->vc.lock, flags);

	vchan_dma_desc_free_list(&c->vc, &head);
	return 0;
}

static void s5l_cdma_synchronize(struct dma_chan *chan)
{
	struct s5l_cdma_chan *c = to_cdma_chan(chan);

	vchan_synchronize(&c->vc);
}

/* dmas = <&cdma channel request>; */
static struct dma_chan *s5l_cdma_of_xlate(struct of_phandle_args *args,
					  struct of_dma *ofdma)
{
	struct s5l_cdma *cdma = ofdma->of_dma_data;
	unsigned int id = args->args[0], request = args->args[1];
	struct dma_chan *chan;

	if (args->args_count != 2 || id < 1 || id > CDMA_NR_CHANNELS ||
	    request >= CDMA_NR_REQUESTS)
		return NULL;

	chan = dma_get_slave_channel(&cdma->chans[id - 1].vc.chan);
	if (chan)
		to_cdma_chan(chan)->request = request;
	return chan;
}

static int s5l_cdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s5l_cdma *cdma;
	struct dma_device *dd;
	struct clk *clk;
	unsigned int i;
	int ret, irq;

	cdma = devm_kzalloc(dev, sizeof(*cdma), GFP_KERNEL);
	if (!cdma)
		return -ENOMEM;
	cdma->dev = dev;

	cdma->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cdma->regs))
		return PTR_ERR(cdma->regs);

	clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable clock\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	cdma->pool = dmam_pool_create(dev_name(dev), dev,
				      sizeof(struct s5l_cdma_hw_seg),
				      sizeof(struct s5l_cdma_hw_seg), 0);
	if (!cdma->pool)
		return -ENOMEM;

	dd = &cdma->dma;
	dma_cap_set(DMA_SLAVE, dd->cap_mask);
	dma_cap_set(DMA_CYCLIC, dd->cap_mask);
	dma_cap_set(DMA_PRIVATE, dd->cap_mask);
	dd->dev = dev;
	dd->device_alloc_chan_resources = s5l_cdma_alloc_chan_resources;
	dd->device_free_chan_resources = s5l_cdma_free_chan_resources;
	dd->device_config = s5l_cdma_slave_config;
	dd->device_prep_slave_sg = s5l_cdma_prep_slave_sg;
	dd->device_prep_dma_cyclic = s5l_cdma_prep_dma_cyclic;
	dd->device_tx_status = s5l_cdma_tx_status;
	dd->device_issue_pending = s5l_cdma_issue_pending;
	dd->device_terminate_all = s5l_cdma_terminate_all;
	dd->device_synchronize = s5l_cdma_synchronize;
	dd->directions = BIT(DMA_MEM_TO_DEV);
	dd->dst_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
			      BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
			      BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	dd->residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
	dd->max_burst = 16;
	INIT_LIST_HEAD(&dd->channels);

	for (i = 0; i < CDMA_NR_CHANNELS; i++) {
		struct s5l_cdma_chan *c = &cdma->chans[i];

		c->cdma = cdma;
		c->id = i + 1;
		c->base = cdma->regs + c->id * CDMA_CH_STRIDE;
		c->vc.desc_free = s5l_cdma_desc_free;
		vchan_init(&c->vc, dd);

		irq = platform_get_irq(pdev, i);
		if (irq < 0)
			return irq;
		ret = devm_request_irq(dev, irq, s5l_cdma_irq, 0,
				       dev_name(dev), c);
		if (ret)
			return dev_err_probe(dev, ret, "channel %u irq\n", c->id);
	}

	ret = dmaenginem_async_device_register(dd);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register\n");

	ret = of_dma_controller_register(dev->of_node, s5l_cdma_of_xlate, cdma);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register OF xlate\n");

	platform_set_drvdata(pdev, cdma);
	dev_info(dev, "%u channels\n", CDMA_NR_CHANNELS);
	return 0;
}

static void s5l_cdma_remove(struct platform_device *pdev)
{
	struct s5l_cdma *cdma = platform_get_drvdata(pdev);
	unsigned int i;

	of_dma_controller_free(pdev->dev.of_node);
	for (i = 0; i < CDMA_NR_CHANNELS; i++) {
		struct s5l_cdma_chan *c = &cdma->chans[i];

		writel(CDMA_CSR_ABORT, c->base + CDMA_CSR);
		tasklet_kill(&c->vc.task);
	}
}

static const struct of_device_id s5l_cdma_of_match[] = {
	{ .compatible = "apple,s5l8947x-cdma" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_cdma_of_match);

static struct platform_driver s5l_cdma_driver = {
	.probe	= s5l_cdma_probe,
	.remove	= s5l_cdma_remove,
	.driver	= {
		.name		= "s5l8947x-cdma",
		.of_match_table	= s5l_cdma_of_match,
	},
};
module_platform_driver(s5l_cdma_driver);

MODULE_DESCRIPTION("S5L8947X central DMA controller");
MODULE_LICENSE("GPL");
