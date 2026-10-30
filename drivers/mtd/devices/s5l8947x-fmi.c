// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X flash memory interface (FMI) with a managed NAND package.
 *
 * The register model was measured on hardware (see the bring-up notes). Each
 * FMI channel has three 4 KB blocks:
 *
 *   host       transfer configuration, start and status, the data FIFO
 *              (+0x14) and the byte-wide metadata FIFO (+0x18)
 *   sequencer  chip enable, command and address bytes, NAND status polling
 *   ecc        BCH engine (unused: the package corrects its own errors, but
 *              the engine still queues a result per sector, and a full
 *              queue stalls the next transfer, so it is emptied per page)
 *
 * The package is a managed NAND: it hides ECC and bad blocks and reports a
 * status byte per operation. It is identified by a three-byte signature at
 * the start of its READ ID answer and describes its geometry in a 512-byte
 * parameter page. After a reset it sits in a low-power interface state and
 * must be moved to the asynchronous state before page operations.
 *
 * A row address is cau << (block_bits + page_bits) | block << page_bits |
 * page. A page holds four 4 KiB logical pages, each carrying 16 bytes of
 * metadata; on the bus a logical page is 1 KiB of data, its metadata and the
 * other 3 KiB. With host configuration 3 the controller splits each logical
 * page into the data FIFO and the metadata FIFO, and two central DMA channels
 * drain them.
 *
 * This driver is read-only.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/unaligned.h>

/* host block */
#define FMI_H_CFG		0x00
#define FMI_H_CMD		0x04
#define FMI_H_STAT		0x0c
#define FMI_H_IRQEN		0x10
#define FMI_H_DATA		0x14
#define FMI_H_META		0x18
#define FMI_H_FIFO		0x1c
#define FMI_H_SHAPE		0x34

#define FMI_H_CMD_START		3
#define FMI_H_CMD_WRITE		5
#define FMI_H_CMD_RESET		6
#define FMI_H_STAT_DONE		BIT(1)
#define FMI_H_STAT_STATUS	BIT(8)
#define FMI_H_STAT_ALL		0xf
#define FMI_H_FIFO_DATA		GENMASK(4, 3)
#define FMI_H_CFG_PIO		5
#define FMI_H_CFG_SPLIT		3	/* logical pages: data + metadata FIFOs */
#define FMI_H_SHAPE_COUNT	GENMASK(7, 0)
#define FMI_H_SHAPE_SIZE	GENMASK(18, 8)
#define FMI_H_SHAPE_META	GENMASK(24, 19)
#define FMI_H_SHAPE_META2	GENMASK(30, 25)

/* sequencer block */
#define FMI_S_CTRL		0x00
#define FMI_S_TIMING		0x08
#define FMI_S_CE		0x0c
#define FMI_S_GO		0x10
#define FMI_S_CMD		0x14
#define FMI_S_ADDR0		0x18
#define FMI_S_ADDR1		0x1c
#define FMI_S_NADDR		0x20
#define FMI_S_24		0x24
#define FMI_S_IRQEN		0x40
#define FMI_S_STAT		0x44
#define FMI_S_NSTAT		0x48
#define FMI_S_MATCH		0x4c

#define FMI_S_CTRL_ENABLE	1
#define FMI_S_TIMING_SLOW	0xffff	/* reset value: every interface phase at maximum */
#define FMI_S_TIMING_RE_LOW	GENMASK(15, 12)	/* read enable low, clocks - 1 */
#define FMI_S_TIMING_RE_HIGH	GENMASK(11, 8)	/* read enable high, clocks - 1 */
#define FMI_S_TIMING_WRITE	GENMASK(7, 0)	/* write phases (left at maximum) */

/*
 * Interface clock. Stepping the read phases one clock at a time changes the
 * read time by about 10 ns a byte, so it runs near 100 MHz; timings are
 * computed for 110 MHz to keep some headroom.
 */
#define FMI_CLK_PS		9091
#define FMI_READ_MARGIN_NS	6	/* added to the read access time */
#define FMI_S_TIMING_NOPOLL	BIT(20)
#define FMI_S_GO_CMD1		BIT(0)
#define FMI_S_GO_CMD2		BIT(1)
#define FMI_S_GO_ADDR		BIT(3)
#define FMI_S_GO_POLL		0x50
#define FMI_S_STAT_MATCH	BIT(5)
#define FMI_S_STAT_ALL		0x31ffff
#define FMI_S_MATCH_READY	0x4040	/* status bit 6 set */

/* ecc block */
#define FMI_E_SECT		0x0c	/* per-sector result FIFO; write 1 to empty it */
#define FMI_E_14		0x14

/* managed NAND commands, as cmd1 | cmd2 << 8 */
#define MN_CMD_RESET		0xff
#define MN_CMD_READ_ID		0x90
#define MN_CMD_PARAMS		0x9792
#define MN_CMD_SET_FEATURE	0xef
#define MN_CMD_CONFIRM		0xe7
#define MN_CMD_READ		0x370a
#define MN_CMD_OP_STATUS	0x7d77
#define MN_CMD_DATA_OUT		0x7a
#define MN_CMD_END		0x77

#define MN_FEATURE_POWER	0x180
#define MN_POWER_ASYNC		2	/* low power -> asynchronous */

/* operation status */
#define MN_ST_OK		0x40
#define MN_ST_REFRESH		0x42	/* data good, block should be refreshed */
#define MN_ST_BLANK		0x49	/* erased page, reads as 0xff */
#define MN_ST_GENERAL_ERROR	BIT(4)	/* sticky until a reset */

/* parameter page */
#define MN_PARAMS_SIZE		512
#define MN_P_CAUS		0x10
#define MN_P_CAU_BITS		0x14
#define MN_P_BLOCKS		0x18
#define MN_P_BLOCK_BITS		0x1c
#define MN_P_PAGES		0x20
#define MN_P_PAGE_BITS		0x28
#define MN_P_PAGE_SIZE		0x34
#define MN_P_TRST_MS		0xf0
#define MN_P_TRC		0xa0	/* timings, one byte each, in ns */
#define MN_P_TREA		0xa1
#define MN_P_TREH		0xa2
#define MN_P_TRP		0xa6

#define FMI_LP_SIZE		4096
#define FMI_LP_META		16
#define FMI_SECTOR		1024
#define FMI_POLL_US		200000

static const u8 mn_signature[3] = { 0x50, 0x50, 0x4e };


struct s5l_fmi {
	struct device *dev;
	void __iomem *host;
	void __iomem *seq;
	void __iomem *ecc;
	phys_addr_t host_phys;
	struct dma_chan *data_chan;
	struct dma_chan *meta_chan;
	struct device *dma_dev;
	void *buf;		/* one page of data, then its metadata */
	dma_addr_t buf_dma;
	size_t buf_size;
	struct completion data_done;
	struct completion meta_done;
	struct mutex lock;
	struct mtd_info mtd;

	u32 caus;
	u32 blocks;		/* per CAU */
	u32 pages;		/* per block */
	u32 block_bits;
	u32 page_bits;
	u32 page_size;		/* data bytes per page */
	u32 lps;		/* logical pages per page */
	u32 trst_ms;
	u32 timing;		/* sequencer interface timing */
};

static int s5l_fmi_wait(struct s5l_fmi *fmi, void __iomem *reg, u32 mask, const char *what)
{
	u32 v;

	if (readl_poll_timeout(reg, v, (v & mask) == mask, 2, FMI_POLL_US)) {
		dev_err_ratelimited(fmi->dev, "timeout waiting for %s (%08x)\n", what, v);
		return -ETIMEDOUT;
	}
	writel(mask, reg);
	return 0;
}

static void s5l_fmi_quiesce(struct s5l_fmi *fmi)
{
	writel(0, fmi->seq + FMI_S_IRQEN);
	writel(0, fmi->host + FMI_H_IRQEN);
	writel(FMI_S_STAT_ALL, fmi->seq + FMI_S_STAT);
	writel(FMI_H_STAT_ALL, fmi->host + FMI_H_STAT);
}

/* Reset the controller's host and sequencer state, keeping the slow timing. */
static void s5l_fmi_ctl_reset(struct s5l_fmi *fmi)
{
	writel(FMI_H_CMD_RESET, fmi->host + FMI_H_CMD);
	writel(FMI_S_CTRL_ENABLE, fmi->seq + FMI_S_CTRL);
	writel(fmi->timing, fmi->seq + FMI_S_TIMING);
	s5l_fmi_quiesce(fmi);
}

static void s5l_fmi_ce(struct s5l_fmi *fmi, bool on)
{
	writel(on ? BIT(0) : 0, fmi->seq + FMI_S_CE);
}

/* Send cmd1 [address bytes] [cmd2] and wait for the sequencer to finish. */
static int s5l_fmi_cmd(struct s5l_fmi *fmi, u32 cmds, u32 go, u32 addr0, u32 addr1,
		       unsigned int naddr)
{
	writel(cmds, fmi->seq + FMI_S_CMD);
	if (go & FMI_S_GO_ADDR) {
		writel(addr0, fmi->seq + FMI_S_ADDR0);
		if (naddr > 4)
			writel(addr1, fmi->seq + FMI_S_ADDR1);
		writel(naddr - 1, fmi->seq + FMI_S_NADDR);
	}
	writel(go, fmi->seq + FMI_S_GO);
	return s5l_fmi_wait(fmi, fmi->seq + FMI_S_STAT, go, "command");
}

static u32 s5l_fmi_status_ready(struct s5l_fmi *fmi)
{
	return (readl(fmi->seq + FMI_S_STAT) & FMI_S_STAT_MATCH) |
	       (readl(fmi->host + FMI_H_STAT) & FMI_H_STAT_STATUS);
}

/* Operation status: 0x77 0x7d, then poll the status byte until ready. */
static int s5l_fmi_op_status(struct s5l_fmi *fmi)
{
	u32 v;
	int st;

	if (s5l_fmi_cmd(fmi, MN_CMD_OP_STATUS, FMI_S_GO_CMD1 | FMI_S_GO_CMD2, 0, 0, 0))
		return -ETIMEDOUT;
	s5l_fmi_quiesce(fmi);
	writel(readl(fmi->seq + FMI_S_TIMING) & ~FMI_S_TIMING_NOPOLL, fmi->seq + FMI_S_TIMING);
	writel(FMI_S_MATCH_READY, fmi->seq + FMI_S_MATCH);
	writel(0, fmi->seq + FMI_S_24);
	writel(FMI_S_STAT_MATCH, fmi->seq + FMI_S_IRQEN);
	writel(FMI_S_GO_POLL, fmi->seq + FMI_S_GO);
	if (read_poll_timeout(s5l_fmi_status_ready, v, v, 2, FMI_POLL_US, false, fmi)) {
		dev_err_ratelimited(fmi->dev, "timeout waiting for the NAND status\n");
		st = -ETIMEDOUT;
	} else {
		st = readl(fmi->seq + FMI_S_NSTAT) & 0xff;
	}
	writel(0, fmi->seq + FMI_S_GO);
	s5l_fmi_quiesce(fmi);
	return st;
}

/* End a managed NAND operation and release the chip enable. */
static void s5l_fmi_op_end(struct s5l_fmi *fmi)
{
	s5l_fmi_cmd(fmi, MN_CMD_END, FMI_S_GO_CMD1, 0, 0, 0);
	writel(FMI_H_CMD_RESET, fmi->host + FMI_H_CMD);
	s5l_fmi_ce(fmi, false);
	s5l_fmi_quiesce(fmi);
}

/* PIO transfer of len bytes (a multiple of 4, at most one 2 KiB burst). */
static int s5l_fmi_pio(struct s5l_fmi *fmi, void *buf, unsigned int len, bool write)
{
	u32 *p = buf;
	unsigned int i;
	u32 v;

	writel(FMI_H_CFG_PIO, fmi->host + FMI_H_CFG);
	writel(FIELD_PREP(FMI_H_SHAPE_SIZE, len) | FIELD_PREP(FMI_H_SHAPE_COUNT, 1),
	       fmi->host + FMI_H_SHAPE);
	writel(write ? FMI_H_CMD_WRITE : FMI_H_CMD_START, fmi->host + FMI_H_CMD);
	if (readl_poll_timeout(fmi->host + FMI_H_FIFO, v, v & FMI_H_FIFO_DATA, 2, FMI_POLL_US)) {
		dev_err(fmi->dev, "PIO FIFO timeout (%08x)\n", v);
		return -ETIMEDOUT;
	}
	for (i = 0; i < len / 4; i++) {
		if (write)
			writel(p[i], fmi->host + FMI_H_DATA);
		else
			p[i] = readl(fmi->host + FMI_H_DATA);
	}
	return s5l_fmi_wait(fmi, fmi->host + FMI_H_STAT, FMI_H_STAT_DONE, "PIO transfer");
}

static int s5l_fmi_reset_chip(struct s5l_fmi *fmi)
{
	int ret;

	s5l_fmi_ctl_reset(fmi);
	s5l_fmi_ce(fmi, true);
	ret = s5l_fmi_cmd(fmi, MN_CMD_RESET, FMI_S_GO_CMD1, 0, 0, 0);
	s5l_fmi_ce(fmi, false);
	msleep(fmi->trst_ms);
	return ret;
}

static int s5l_fmi_read_id(struct s5l_fmi *fmi, u8 id[8])
{
	int ret;

	s5l_fmi_ce(fmi, true);
	ret = s5l_fmi_cmd(fmi, MN_CMD_READ_ID, FMI_S_GO_CMD1 | FMI_S_GO_ADDR, 0, 0, 1);
	if (!ret)
		ret = s5l_fmi_pio(fmi, id, 8, false);
	s5l_fmi_ce(fmi, false);
	s5l_fmi_ctl_reset(fmi);
	return ret;
}

static int s5l_fmi_set_feature(struct s5l_fmi *fmi, u16 feature, u32 value)
{
	int ret, st;

	s5l_fmi_ce(fmi, true);
	s5l_fmi_quiesce(fmi);
	writel(FMI_H_CMD_RESET, fmi->host + FMI_H_CMD);
	ret = s5l_fmi_cmd(fmi, MN_CMD_SET_FEATURE, FMI_S_GO_CMD1 | FMI_S_GO_ADDR, feature, 0, 2);
	if (!ret) {
		s5l_fmi_quiesce(fmi);
		ret = s5l_fmi_pio(fmi, &value, sizeof(value), true);
	}
	if (!ret)
		ret = s5l_fmi_cmd(fmi, MN_CMD_CONFIRM, FMI_S_GO_CMD1, 0, 0, 0);
	st = ret ? ret : s5l_fmi_op_status(fmi);
	s5l_fmi_op_end(fmi);
	if (st != MN_ST_OK) {
		dev_err(fmi->dev, "set feature %#x: status %#x\n", feature, st);
		return st < 0 ? st : -EIO;
	}
	return 0;
}

/* Reset the package and bring it to the state page reads need. */
static int s5l_fmi_recover(struct s5l_fmi *fmi)
{
	int ret = s5l_fmi_reset_chip(fmi);

	return ret ? ret : s5l_fmi_set_feature(fmi, MN_FEATURE_POWER, MN_POWER_ASYNC);
}

static int s5l_fmi_read_params(struct s5l_fmi *fmi, u8 *p)
{
	int ret, st;

	s5l_fmi_ce(fmi, true);
	s5l_fmi_quiesce(fmi);
	ret = s5l_fmi_cmd(fmi, MN_CMD_PARAMS, FMI_S_GO_CMD1 | FMI_S_GO_ADDR | FMI_S_GO_CMD2,
			  0, 0, 1);
	st = ret ? ret : s5l_fmi_op_status(fmi);
	if (st == MN_ST_OK)
		ret = s5l_fmi_cmd(fmi, MN_CMD_DATA_OUT, FMI_S_GO_CMD1, 0, 0, 0);
	else
		ret = st < 0 ? st : -EIO;
	if (!ret)
		ret = s5l_fmi_pio(fmi, p, MN_PARAMS_SIZE, false);
	s5l_fmi_op_end(fmi);
	return ret;
}

static void s5l_fmi_dma_done(void *arg)
{
	complete(arg);
}

static int s5l_fmi_dma_prep(struct s5l_fmi *fmi, struct dma_chan *chan, dma_addr_t addr,
			    size_t len, struct completion *done)
{
	struct dma_async_tx_descriptor *tx;

	tx = dmaengine_prep_slave_single(chan, addr, len, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT);
	if (!tx)
		return -EIO;
	reinit_completion(done);
	tx->callback = s5l_fmi_dma_done;
	tx->callback_param = done;
	if (dma_submit_error(dmaengine_submit(tx)))
		return -EIO;
	dma_async_issue_pending(chan);
	return 0;
}

/*
 * Read one page into fmi->buf (data, then lps * 16 bytes of metadata).
 * Returns the operation status byte or a negative error.
 */
static int s5l_fmi_read_page(struct s5l_fmi *fmi, u32 row)
{
	size_t meta_len = fmi->lps * FMI_LP_META;
	u32 shape;
	int st, ret;
	unsigned int i;

	s5l_fmi_ce(fmi, true);
	s5l_fmi_quiesce(fmi);
	ret = s5l_fmi_cmd(fmi, MN_CMD_READ, FMI_S_GO_CMD1 | FMI_S_GO_ADDR | FMI_S_GO_CMD2,
			  row, 0, 4);
	st = ret ? ret : s5l_fmi_op_status(fmi);
	if (st < 0 || (st & MN_ST_GENERAL_ERROR))
		goto end;

	ret = s5l_fmi_cmd(fmi, MN_CMD_DATA_OUT, FMI_S_GO_CMD1, 0, 0, 0);
	if (!ret)
		ret = s5l_fmi_dma_prep(fmi, fmi->data_chan, fmi->buf_dma, fmi->page_size,
				       &fmi->data_done);
	if (!ret)
		ret = s5l_fmi_dma_prep(fmi, fmi->meta_chan, fmi->buf_dma + fmi->page_size,
				       meta_len, &fmi->meta_done);
	if (ret)
		goto abort;

	writel(1, fmi->ecc + FMI_E_SECT);
	shape = FIELD_PREP(FMI_H_SHAPE_COUNT, FMI_LP_SIZE / FMI_SECTOR) |
		FIELD_PREP(FMI_H_SHAPE_SIZE, FMI_SECTOR) |
		FIELD_PREP(FMI_H_SHAPE_META, FMI_LP_META) |
		FIELD_PREP(FMI_H_SHAPE_META2, FMI_LP_META);
	for (i = 0; i < fmi->lps && !ret; i++) {
		writel(FMI_H_CFG_SPLIT, fmi->host + FMI_H_CFG);
		writel(shape, fmi->host + FMI_H_SHAPE);
		writel(FMI_H_CMD_START, fmi->host + FMI_H_CMD);
		ret = s5l_fmi_wait(fmi, fmi->host + FMI_H_STAT, FMI_H_STAT_DONE, "page transfer");
		if (ret) {
			struct dma_tx_state ds, ms;

			dmaengine_tx_status(fmi->data_chan, fmi->data_chan->cookie, &ds);
			dmaengine_tx_status(fmi->meta_chan, fmi->meta_chan->cookie, &ms);
			dev_err_ratelimited(fmi->dev,
					    "row %#x: logical page %u stalled, fifo %08x, residue %u/%u\n",
					    row, i, readl(fmi->host + FMI_H_FIFO), ds.residue,
					    ms.residue);
		}
	}
	if (!ret &&
	    (!wait_for_completion_timeout(&fmi->data_done, msecs_to_jiffies(100)) ||
	     !wait_for_completion_timeout(&fmi->meta_done, msecs_to_jiffies(100)))) {
		dev_err_ratelimited(fmi->dev, "row %#x: DMA timeout\n", row);
		ret = -ETIMEDOUT;
	}
abort:
	if (ret) {
		dmaengine_terminate_sync(fmi->data_chan);
		dmaengine_terminate_sync(fmi->meta_chan);
		st = ret;
	}
end:
	s5l_fmi_op_end(fmi);
	return st;
}

static u32 s5l_fmi_row(struct s5l_fmi *fmi, u64 page)
{
	u32 pg = do_div(page, fmi->pages);
	u32 block = do_div(page, fmi->blocks);
	u32 cau = page;

	return cau << (fmi->block_bits + fmi->page_bits) | block << fmi->page_bits | pg;
}

/*
 * Read a page with one recovery attempt on a stuck package. Returns the
 * bitflips to report (1 when the package asks for a refresh), -EBADMSG with
 * the data still in the buffer, or another error.
 */
static int s5l_fmi_read_one(struct s5l_fmi *fmi, u64 page)
{
	u32 row = s5l_fmi_row(fmi, page);
	int st, tries;

	for (tries = 0; tries < 2; tries++) {
		st = s5l_fmi_read_page(fmi, row);
		if (st >= 0 && !(st & MN_ST_GENERAL_ERROR))
			break;
		dev_warn_ratelimited(fmi->dev, "row %#x: status %d, resetting the package\n",
				     row, st);
		if (s5l_fmi_recover(fmi))
			return -EIO;
	}
	switch (st) {
	case MN_ST_OK:
	case MN_ST_BLANK:
		return 0;
	case MN_ST_REFRESH:
		fmi->mtd.ecc_stats.corrected++;
		return 1;
	default:
		if (st < 0 || (st & MN_ST_GENERAL_ERROR))
			return -EIO;
		fmi->mtd.ecc_stats.failed++;
		return -EBADMSG;
	}
}

static int s5l_fmi_mtd_read_oob(struct mtd_info *mtd, loff_t from, struct mtd_oob_ops *ops)
{
	struct s5l_fmi *fmi = mtd->priv;
	u64 page = from >> mtd->writesize_shift;
	u32 col = from & mtd->writesize_mask;
	size_t dlen = ops->datbuf ? ops->len : 0;
	size_t olen = ops->oobbuf ? ops->ooblen : 0;
	unsigned int max_bitflips = 0;
	bool failed = false;
	int ret = 0, err;

	ops->retlen = 0;
	ops->oobretlen = 0;
	if (olen && ops->ooboffs >= mtd->oobsize)
		return -EINVAL;

	mutex_lock(&fmi->lock);
	while (dlen || olen) {
		size_t n;

		if (page >= mtd_div_by_ws(mtd->size, mtd)) {
			ret = -EINVAL;
			break;
		}
		err = s5l_fmi_read_one(fmi, page);
		if (err == -EBADMSG) {
			failed = true;
		} else if (err < 0) {
			ret = err;
			break;
		} else {
			max_bitflips = max_t(unsigned int, max_bitflips, err);
		}
		if (dlen) {
			n = min_t(size_t, dlen, mtd->writesize - col);
			memcpy(ops->datbuf + ops->retlen, fmi->buf + col, n);
			ops->retlen += n;
			dlen -= n;
			col = 0;
		}
		if (olen) {
			n = min_t(size_t, olen, mtd->oobsize - ops->ooboffs);
			memcpy(ops->oobbuf + ops->oobretlen,
			       fmi->buf + fmi->page_size + ops->ooboffs, n);
			ops->oobretlen += n;
			olen -= n;
			if (!dlen && olen)
				ops->ooboffs = 0;
		}
		page++;
	}
	mutex_unlock(&fmi->lock);
	if (ret)
		return ret;
	return failed ? -EBADMSG : max_bitflips;
}

/* The metadata is all free bytes: there is no ECC in the out-of-band area. */
static int s5l_fmi_ooblayout_ecc(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *region)
{
	return -ERANGE;
}

static int s5l_fmi_ooblayout_free(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *region)
{
	if (section)
		return -ERANGE;
	region->offset = 0;
	region->length = mtd->oobsize;
	return 0;
}

static const struct mtd_ooblayout_ops s5l_fmi_ooblayout = {
	.ecc = s5l_fmi_ooblayout_ecc,
	.free = s5l_fmi_ooblayout_free,
};

static int s5l_fmi_mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	/* The package retires bad blocks itself. */
	return 0;
}

static u32 s5l_fmi_le32(const u8 *p, unsigned int off)
{
	return get_unaligned_le32(p + off);
}

static unsigned int s5l_fmi_clocks(unsigned int ns)
{
	return max(1U, DIV_ROUND_UP(ns * 1000, FMI_CLK_PS));
}

/* Read phases from the package's own timings; write phases stay slowest. */
static u32 s5l_fmi_read_timing(const u8 *p)
{
	unsigned int low, high;

	low = s5l_fmi_clocks(max(p[MN_P_TREA] + FMI_READ_MARGIN_NS, (unsigned int)p[MN_P_TRP]));
	high = s5l_fmi_clocks(p[MN_P_TREH]);
	if ((low + high) * FMI_CLK_PS < p[MN_P_TRC] * 1000)
		high = DIV_ROUND_UP(p[MN_P_TRC] * 1000, FMI_CLK_PS) - low;
	low = min(low, 16U);
	high = min(high, 16U);
	return FIELD_PREP(FMI_S_TIMING_RE_LOW, low - 1) |
	       FIELD_PREP(FMI_S_TIMING_RE_HIGH, high - 1) | FMI_S_TIMING_WRITE;
}

static int s5l_fmi_identify(struct s5l_fmi *fmi)
{
	u8 id[8] __aligned(4);
	u8 *p;
	int ret;

	fmi->trst_ms = 20;
	ret = s5l_fmi_reset_chip(fmi);
	if (!ret)
		ret = s5l_fmi_read_id(fmi, id);
	if (ret)
		return ret;
	if (memcmp(id, mn_signature, sizeof(mn_signature)))
		return dev_err_probe(fmi->dev, -ENODEV, "no managed NAND on CE0 (id %8ph)\n", id);

	ret = s5l_fmi_set_feature(fmi, MN_FEATURE_POWER, MN_POWER_ASYNC);
	if (ret)
		return ret;

	p = kzalloc(MN_PARAMS_SIZE, GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	ret = s5l_fmi_read_params(fmi, p);
	if (ret)
		goto out;

	fmi->caus = s5l_fmi_le32(p, MN_P_CAUS);
	fmi->blocks = s5l_fmi_le32(p, MN_P_BLOCKS);
	fmi->pages = s5l_fmi_le32(p, MN_P_PAGES);
	fmi->block_bits = s5l_fmi_le32(p, MN_P_BLOCK_BITS);
	fmi->page_bits = s5l_fmi_le32(p, MN_P_PAGE_BITS);
	fmi->page_size = rounddown_pow_of_two(s5l_fmi_le32(p, MN_P_PAGE_SIZE));
	fmi->trst_ms = clamp(s5l_fmi_le32(p, MN_P_TRST_MS), 1U, 100U);

	if (!fmi->caus || fmi->caus > 64 || !fmi->blocks || fmi->blocks > BIT(fmi->block_bits) ||
	    fmi->pages != BIT(fmi->page_bits) || fmi->block_bits > 15 ||
	    fmi->page_size < FMI_LP_SIZE || fmi->page_size % FMI_LP_SIZE ||
	    fmi->page_size > SZ_16K * 2 ||
	    fmi->block_bits + fmi->page_bits + order_base_2(fmi->caus) > 31) {
		ret = dev_err_probe(fmi->dev, -EINVAL,
				    "unexpected geometry: %u CAUs, %u blocks, %u pages of %u bytes\n",
				    fmi->caus, fmi->blocks, fmi->pages, fmi->page_size);
		goto out;
	}
	fmi->lps = fmi->page_size / FMI_LP_SIZE;
	fmi->timing = s5l_fmi_read_timing(p);
	dev_info(fmi->dev,
		 "managed NAND %5ph: %u CAUs x %u blocks x %u pages x %u bytes, timing %#x\n",
		 id + 3, fmi->caus, fmi->blocks, fmi->pages, fmi->page_size, fmi->timing);
out:
	kfree(p);
	return ret;
}

static void s5l_fmi_release_chan(void *chan)
{
	dma_release_channel(chan);
}

static struct dma_chan *s5l_fmi_request_chan(struct s5l_fmi *fmi, const char *name)
{
	struct dma_chan *chan = dma_request_chan(fmi->dev, name);
	int ret;

	if (IS_ERR(chan))
		return chan;
	ret = devm_add_action_or_reset(fmi->dev, s5l_fmi_release_chan, chan);
	return ret ? ERR_PTR(ret) : chan;
}

static int s5l_fmi_dma_init(struct s5l_fmi *fmi)
{
	struct dma_slave_config cfg = {
		.direction = DMA_DEV_TO_MEM,
		.src_maxburst = 1,
	};
	int ret;

	fmi->data_chan = s5l_fmi_request_chan(fmi, "data");
	if (IS_ERR(fmi->data_chan))
		return dev_err_probe(fmi->dev, PTR_ERR(fmi->data_chan), "data DMA channel\n");
	fmi->meta_chan = s5l_fmi_request_chan(fmi, "meta");
	if (IS_ERR(fmi->meta_chan))
		return dev_err_probe(fmi->dev, PTR_ERR(fmi->meta_chan), "metadata DMA channel\n");

	/* A transfer left armed on a channel would drain the FIFOs under PIO. */
	dmaengine_terminate_sync(fmi->data_chan);
	dmaengine_terminate_sync(fmi->meta_chan);

	cfg.src_addr = fmi->host_phys + FMI_H_DATA;
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	ret = dmaengine_slave_config(fmi->data_chan, &cfg);
	if (ret)
		return ret;
	cfg.src_addr = fmi->host_phys + FMI_H_META;
	cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_1_BYTE;
	ret = dmaengine_slave_config(fmi->meta_chan, &cfg);
	if (ret)
		return ret;

	init_completion(&fmi->data_done);
	init_completion(&fmi->meta_done);
	return 0;
}

static void s5l_fmi_free_buf(void *data)
{
	struct s5l_fmi *fmi = data;

	dma_free_coherent(fmi->dma_dev, fmi->buf_size, fmi->buf, fmi->buf_dma);
}

static int s5l_fmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtd_erase_region_info *regions;
	struct clk_bulk_data *clks;
	struct resource *res;
	struct s5l_fmi *fmi;
	struct mtd_info *mtd;
	unsigned int i;
	int ret;

	fmi = devm_kzalloc(dev, sizeof(*fmi), GFP_KERNEL);
	if (!fmi)
		return -ENOMEM;
	fmi->dev = dev;
	mutex_init(&fmi->lock);

	fmi->host = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(fmi->host))
		return PTR_ERR(fmi->host);
	fmi->host_phys = res->start;
	fmi->seq = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(fmi->seq))
		return PTR_ERR(fmi->seq);
	fmi->ecc = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(fmi->ecc))
		return PTR_ERR(fmi->ecc);

	ret = devm_clk_bulk_get_all_enable(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to enable clocks\n");

	ret = s5l_fmi_dma_init(fmi);
	if (ret)
		return ret;

	fmi->timing = FMI_S_TIMING_SLOW;
	s5l_fmi_ctl_reset(fmi);
	writel(0, fmi->ecc + FMI_E_14);

	ret = s5l_fmi_identify(fmi);
	if (ret)
		return ret;
	writel(fmi->timing, fmi->seq + FMI_S_TIMING);

	fmi->dma_dev = fmi->data_chan->device->dev;
	fmi->buf_size = fmi->page_size + fmi->lps * FMI_LP_META;
	fmi->buf = dma_alloc_coherent(fmi->dma_dev, fmi->buf_size, &fmi->buf_dma, GFP_KERNEL);
	if (!fmi->buf)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, s5l_fmi_free_buf, fmi);
	if (ret)
		return ret;

	mtd = &fmi->mtd;
	mtd->priv = fmi;
	mtd->dev.parent = dev;
	mtd->owner = THIS_MODULE;
	mtd->name = dev_name(dev);
	mtd->type = MTD_MLCNANDFLASH;
	mtd->flags = MTD_NO_ERASE;	/* read-only: no MTD_WRITEABLE, no erase */
	mtd->writesize = fmi->page_size;
	mtd->writebufsize = fmi->page_size;
	mtd->erasesize = fmi->page_size * fmi->pages;
	mtd->size = (u64)mtd->erasesize * fmi->blocks * fmi->caus;
	mtd->oobsize = fmi->lps * FMI_LP_META;
	mtd->oobavail = mtd->oobsize;
	mtd->ecc_step_size = FMI_LP_SIZE;
	mtd->ecc_strength = 1;
	mtd->bitflip_threshold = 1;
	mtd_set_ooblayout(mtd, &s5l_fmi_ooblayout);
	mtd->_read_oob = s5l_fmi_mtd_read_oob;

	/* One erase region per CAU, so users can find a block's twin in each. */
	regions = devm_kcalloc(dev, fmi->caus, sizeof(*regions), GFP_KERNEL);
	if (!regions)
		return -ENOMEM;
	for (i = 0; i < fmi->caus; i++) {
		regions[i].offset = (u64)i * fmi->blocks * mtd->erasesize;
		regions[i].erasesize = mtd->erasesize;
		regions[i].numblocks = fmi->blocks;
	}
	mtd->eraseregions = regions;
	mtd->numeraseregions = fmi->caus;
	mtd->_block_isbad = s5l_fmi_mtd_block_isbad;

	ret = mtd_device_register(mtd, NULL, 0);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, fmi);
	return 0;
}

static void s5l_fmi_remove(struct platform_device *pdev)
{
	struct s5l_fmi *fmi = platform_get_drvdata(pdev);

	WARN_ON(mtd_device_unregister(&fmi->mtd));
}

static const struct of_device_id s5l_fmi_of_match[] = {
	{ .compatible = "apple,s5l8947x-fmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_fmi_of_match);

static struct platform_driver s5l_fmi_driver = {
	.probe	= s5l_fmi_probe,
	.remove	= s5l_fmi_remove,
	.driver	= {
		.name		= "s5l8947x-fmi",
		.of_match_table	= s5l_fmi_of_match,
	},
};
module_platform_driver(s5l_fmi_driver);

MODULE_DESCRIPTION("S5L8947X flash memory interface (managed NAND, read-only)");
MODULE_LICENSE("GPL");
