// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-only translation layer for the managed NAND of S5L8947X devices, as
 * written by the stock firmware, and its volume map.
 *
 * The NAND is reached through an MTD whose pages hold several 4 KiB logical
 * pages, each with a 16-byte metadata record in the out-of-band area, and
 * whose erase regions are the package's CAUs (one block per CAU forms a
 * superblock). The format was worked out from the on-flash data:
 *
 *   metadata    u8 type, u8 flags, u16 sequence, u32 generation, u32 lba,
 *               u32 generation first written; (generation, sequence) is a
 *               48-bit write counter and lba counts 4 KiB logical blocks.
 *               Type 0x01 is data, 0x1c a table of contents.
 *
 *   superblock  block b of every CAU whose block has page 0 programmed,
 *               written page by page, then CAU by CAU, then logical page.
 *
 *   table of    the last one or two pages of a closed superblock, in write
 *   contents    order. Its metadata gives the generation, a sequence, its
 *               size in logical pages (word 3 bits 15:0) and bit 0 of the
 *               flags, set when the superblock was filled by garbage
 *               collection. Its data is a list of runs {x, generation,
 *               lba, count} covering the superblock's logical pages in
 *               order. In a garbage-collection superblock the second word
 *               is not a generation and the copies rank at the
 *               superblock's own generation.
 *
 * The newest copy of an lba is the one with the highest (run generation,
 * superblock generation and sequence, position). Superblocks without a table
 * (still open when the firmware stopped) are read page by page.
 *
 * Logical block 0 holds the volume map: a type GUID, the media size at 0x20,
 * the volume count at 0x28, 128-byte volume records from 0x200 (type, GUID,
 * begin, end, attributes, UTF-16 name) and a map of 1024 u16 chunk entries at
 * 0x800 (volume << 12 | chunk; 0xfnnn is the header or free). The disk is cut
 * into 1024 power-of-two chunks; map index i is disk chunk i.
 *
 * The whole logical disk is exposed as nand0 and each volume as nand0pN,
 * all read-only. Volumes are separate disks, not partitions, because their
 * chunks need not be contiguous on the logical disk.
 */

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#define FTL_LP_SHIFT		12
#define FTL_LP_SIZE		BIT(FTL_LP_SHIFT)
#define FTL_META		16
#define FTL_T_DATA		0x01
#define FTL_T_TOC		0x1c
#define FTL_F_GC		BIT(0)
#define FTL_TOC_MAX_PAGES	4
#define FTL_MAX_CAUS		8
#define FTL_UNMAPPED		U32_MAX

#define LV_MEDIA_SIZE		0x20
#define LV_COUNT		0x28
#define LV_RECORDS		0x200
#define LV_RECORD_SIZE		0x80
#define LV_R_BEGIN		0x20
#define LV_R_END		0x28
#define LV_R_ATTR		0x30
#define LV_R_NAME		0x38
#define LV_R_NAME_LEN		36
#define LV_MAP			0x800
#define LV_MAP_ENTRIES		1024
#define LV_MAX_VOLUMES		8

static const u8 lv_magic[2][16] = {
	{ 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a,
	  0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b },
	{ 0xb1, 0x89, 0xa5, 0x19, 0x4f, 0x59, 0x4b, 0x1d,
	  0xad, 0x44, 0x1e, 0x12, 0x7a, 0xaf, 0x45, 0x39 },
};

struct ftl;

struct ftl_disk {
	struct ftl *ftl;
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;
	u64 size;		/* bytes */
	u16 *chunks;		/* disk chunk of each volume chunk, NULL = whole disk */
	unsigned int nchunks;
};

struct ftl {
	struct mtd_info *mtd;
	struct list_head node;
	struct work_struct work;
	struct mutex lock;	/* the MTD and the page cache */

	u32 caus;
	u32 blocks;		/* per CAU */
	u32 pages;		/* per block */
	u32 lps;		/* logical pages per page */
	u32 nlba;		/* map entries */
	u32 *l2p;		/* lba -> page * lps + logical page */

	u8 *page;		/* cached page data */
	u8 *oob;
	u32 cached;		/* page number in the cache, or FTL_UNMAPPED */

	unsigned int chunk_shift;
	struct ftl_disk whole;
	struct ftl_disk vols[LV_MAX_VOLUMES];
	unsigned int nvols;
};

static LIST_HEAD(ftl_list);
static DEFINE_MUTEX(ftl_list_lock);
static atomic_t ftl_count = ATOMIC_INIT(0);

/* --- reading the MTD --- */

static u32 ftl_page(struct ftl *f, u32 cau, u32 block, u32 page)
{
	return (cau * f->blocks + block) * f->pages + page;
}

/* Read a page's metadata and, if data is set, its data. 0, or -EBADMSG / -EIO. */
static int ftl_read_page(struct ftl *f, u32 page, u8 *data, u8 *oob)
{
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB,
		.len = data ? f->mtd->writesize : 0,
		.datbuf = data,
		.ooblen = f->mtd->oobsize,
		.oobbuf = oob,
	};
	int ret;

	ret = mtd_read_oob(f->mtd, (loff_t)page * f->mtd->writesize, &ops);
	return ret == -EUCLEAN ? 0 : ret;
}

/* --- building the map --- */

struct ftl_build {
	u64 *keys;		/* rank of the copy in l2p */
	u8 *toc;		/* table of contents data */
	u32 members[FTL_MAX_CAUS];
	u32 nmembers;
	u32 total;		/* logical pages in the superblock */
};

/* Position in a superblock's write order to (page number, logical page). */
static void ftl_pos(struct ftl *f, struct ftl_build *b, u32 block, u32 idx,
		    u32 *page, u32 *lp)
{
	u32 per_page = f->lps * b->nmembers;

	*lp = idx % f->lps;
	*page = ftl_page(f, b->members[(idx / f->lps) % b->nmembers], block,
			 idx / per_page);
}

static void ftl_apply(struct ftl *f, struct ftl_build *b, u32 lba, u64 key, u32 phys)
{
	if (lba >= f->nlba)
		return;
	if (f->l2p[lba] == FTL_UNMAPPED || key > b->keys[lba]) {
		b->keys[lba] = key;
		f->l2p[lba] = phys;
	}
}

static u64 ftl_key(u32 run_gen, u32 sb_gen, u32 sb_seq, u32 idx)
{
	return (u64)(run_gen & 0xffff) << 48 | (u64)(sb_gen & 0xffff) << 32 |
	       (sb_seq & 0xffff) << 16 | (idx & 0xffff);
}

static int ftl_build_toc(struct ftl *f, struct ftl_build *b, u32 block, const u8 *meta)
{
	u32 sb_seq = get_unaligned_le16(meta + 2);
	u32 sb_gen = get_unaligned_le32(meta + 4);
	u32 toc_lps = get_unaligned_le32(meta + 12) & 0xffff;
	bool gc = meta[1] & FTL_F_GC;
	u32 i, k, idx, page, lp, words;
	int ret;

	if (!toc_lps || toc_lps % f->lps || toc_lps > FTL_TOC_MAX_PAGES * f->lps ||
	    toc_lps >= b->total)
		return -EINVAL;
	for (i = 0; i < toc_lps / f->lps; i++) {
		ftl_pos(f, b, block, b->total - toc_lps + i * f->lps, &page, &lp);
		ret = ftl_read_page(f, page, b->toc + i * f->mtd->writesize, f->oob);
		if (ret)
			return ret;
	}

	words = toc_lps / f->lps * f->mtd->writesize / 4;
	for (k = 0, idx = 0; k + 4 <= words; k += 4) {
		const u8 *r = b->toc + k * 4;
		u32 gen = get_unaligned_le32(r + 4);
		u32 lba = get_unaligned_le32(r + 8);
		u32 cnt = get_unaligned_le32(r + 12);
		u32 j;

		if (!cnt || cnt == U32_MAX || idx + cnt > b->total - toc_lps)
			break;
		if (gc)
			gen = sb_gen;
		for (j = 0; j < cnt; j++, idx++) {
			ftl_pos(f, b, block, idx, &page, &lp);
			ftl_apply(f, b, lba + j, ftl_key(gen, sb_gen, sb_seq, idx),
				  page * f->lps + lp);
		}
	}
	return 0;
}

/* A superblock without a table: read its metadata page by page. */
static int ftl_build_open(struct ftl *f, struct ftl_build *b, u32 block)
{
	u32 *ent;		/* per position: lba, generation; FTL_UNMAPPED = none */
	u32 sb_gen = 0, sb_seq = 0, idx, page, lp, n = 0;
	int ret = 0;

	ent = kvmalloc_array(b->total, 2 * sizeof(*ent), GFP_KERNEL);
	if (!ent)
		return -ENOMEM;
	for (idx = 0; idx < b->total; idx += f->lps) {
		bool any = false;

		ftl_pos(f, b, block, idx, &page, &lp);
		ret = ftl_read_page(f, page, NULL, f->oob);
		if (ret == -EBADMSG)
			ret = 0;
		if (ret)
			break;
		for (lp = 0; lp < f->lps; lp++) {
			const u8 *m = f->oob + lp * FTL_META;
			u32 gen = get_unaligned_le32(m + 4);
			u32 seq = get_unaligned_le16(m + 2);

			ent[2 * (idx + lp)] = FTL_UNMAPPED;
			if (m[0] == 0xff)
				continue;
			any = true;
			if (m[0] != FTL_T_DATA)
				continue;
			ent[2 * (idx + lp)] = get_unaligned_le32(m + 8);
			ent[2 * (idx + lp) + 1] = gen;
			if (gen > sb_gen || (gen == sb_gen && seq > sb_seq)) {
				sb_gen = gen;
				sb_seq = seq;
			}
		}
		n = idx + f->lps;
		if (!any)
			break;	/* first blank page: the rest is unwritten */
	}
	for (idx = 0; idx < n && !ret; idx++) {
		if (ent[2 * idx] == FTL_UNMAPPED)
			continue;
		ftl_pos(f, b, block, idx, &page, &lp);
		ftl_apply(f, b, ent[2 * idx], ftl_key(ent[2 * idx + 1], sb_gen, sb_seq, idx),
			  page * f->lps + lp);
	}
	kvfree(ent);
	return ret;
}

static int ftl_build(struct ftl *f)
{
	struct ftl_build b = {};
	u32 block, c, page, lp, mapped = 0, tocs = 0, open = 0;
	unsigned long start = jiffies;
	int ret = 0;

	b.keys = vmalloc(array_size(f->nlba, sizeof(*b.keys)));
	b.toc = kvmalloc(FTL_TOC_MAX_PAGES * f->mtd->writesize, GFP_KERNEL);
	if (!b.keys || !b.toc) {
		ret = -ENOMEM;
		goto out;
	}

	/* Block 0 of the first CAU holds the boot pages, not translated data. */
	for (block = 1; block < f->blocks; block++) {
		b.nmembers = 0;
		for (c = 0; c < f->caus; c++) {
			ret = ftl_read_page(f, ftl_page(f, c, block, 0), NULL, f->oob);
			if (!ret && f->oob[0] != 0xff)
				b.members[b.nmembers++] = c;
			else if (ret && ret != -EBADMSG)
				goto out;
		}
		ret = 0;
		if (!b.nmembers)
			continue;
		b.total = f->pages * f->lps * b.nmembers;

		/* The table, if any, ends the superblock; its first record describes it. */
		ftl_pos(f, &b, block, b.total - f->lps, &page, &lp);
		ret = ftl_read_page(f, page, NULL, f->oob);
		if (!ret && f->oob[0] == FTL_T_TOC && !ftl_build_toc(f, &b, block, f->oob)) {
			tocs++;
		} else {
			ret = ftl_build_open(f, &b, block);
			if (ret)
				goto out;
			open++;
		}
		cond_resched();
	}

	for (block = 0; block < f->nlba; block++)
		mapped += f->l2p[block] != FTL_UNMAPPED;
	dev_info(&f->mtd->dev,
		 "translation map: %u blocks mapped, %u tables, %u open superblocks, %u ms\n",
		 mapped, tocs, open, jiffies_to_msecs(jiffies - start));
out:
	kvfree(b.toc);
	vfree(b.keys);
	return ret;
}

/* --- reading logical blocks --- */

/* Copy n bytes at offset off of logical block lba. Caller holds f->lock. */
static int ftl_copy(struct ftl *f, u32 lba, unsigned int off, void *dst, unsigned int n)
{
	u32 phys, page;
	int ret;

	phys = lba < f->nlba ? f->l2p[lba] : FTL_UNMAPPED;
	if (phys == FTL_UNMAPPED) {
		memset(dst, 0, n);
		return 0;
	}
	page = phys / f->lps;
	if (f->cached != page) {
		f->cached = FTL_UNMAPPED;
		ret = ftl_read_page(f, page, f->page, f->oob);
		if (ret)
			return ret;
		f->cached = page;
	}
	memcpy(dst, f->page + (phys % f->lps) * FTL_LP_SIZE + off, n);
	return 0;
}

/* Byte offset on a disk to byte offset on the logical disk. */
static u64 ftl_disk_offset(struct ftl_disk *d, u64 pos)
{
	u64 mask = BIT_ULL(d->ftl->chunk_shift) - 1;

	if (!d->chunks)
		return pos;
	return ((u64)d->chunks[pos >> d->ftl->chunk_shift] << d->ftl->chunk_shift) | (pos & mask);
}

static blk_status_t ftl_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
{
	struct ftl_disk *d = hctx->queue->queuedata;
	struct request *rq = bd->rq;
	struct ftl *f = d->ftl;
	u64 pos = (u64)blk_rq_pos(rq) << SECTOR_SHIFT;
	struct req_iterator iter;
	blk_status_t status = BLK_STS_OK;
	struct bio_vec bv;

	blk_mq_start_request(rq);
	if (req_op(rq) != REQ_OP_READ) {
		status = BLK_STS_NOTSUPP;
		goto out;
	}

	mutex_lock(&f->lock);
	rq_for_each_segment(bv, rq, iter) {
		u8 *dst = bvec_kmap_local(&bv);
		unsigned int done = 0;

		while (done < bv.bv_len && status == BLK_STS_OK) {
			u64 at = ftl_disk_offset(d, pos);
			unsigned int off = at & (FTL_LP_SIZE - 1);
			unsigned int n = min_t(unsigned int, bv.bv_len - done, FTL_LP_SIZE - off);

			if (pos + n > d->size ||
			    ftl_copy(f, at >> FTL_LP_SHIFT, off, dst + done, n))
				status = BLK_STS_IOERR;
			done += n;
			pos += n;
		}
		kunmap_local(dst);
		if (status != BLK_STS_OK)
			break;
	}
	mutex_unlock(&f->lock);
out:
	blk_mq_end_request(rq, status);
	return BLK_STS_OK;
}

static const struct blk_mq_ops ftl_mq_ops = {
	.queue_rq = ftl_queue_rq,
};

static const struct block_device_operations ftl_fops = {
	.owner = THIS_MODULE,
};

static int ftl_add_disk(struct ftl *f, struct ftl_disk *d, const char *name)
{
	struct queue_limits lim = {
		.logical_block_size = FTL_LP_SIZE,
		.physical_block_size = FTL_LP_SIZE,
		.max_hw_sectors = 256,
	};
	int ret;

	d->ftl = f;
	d->tag_set.ops = &ftl_mq_ops;
	d->tag_set.nr_hw_queues = 1;
	d->tag_set.queue_depth = 16;
	d->tag_set.numa_node = NUMA_NO_NODE;
	d->tag_set.flags = BLK_MQ_F_BLOCKING;
	ret = blk_mq_alloc_tag_set(&d->tag_set);
	if (ret)
		return ret;

	d->disk = blk_mq_alloc_disk(&d->tag_set, &lim, d);
	if (IS_ERR(d->disk)) {
		ret = PTR_ERR(d->disk);
		d->disk = NULL;
		blk_mq_free_tag_set(&d->tag_set);
		return ret;
	}
	d->disk->fops = &ftl_fops;
	d->disk->private_data = d;
	d->disk->flags |= GENHD_FL_NO_PART;
	strscpy(d->disk->disk_name, name, sizeof(d->disk->disk_name));
	set_capacity(d->disk, d->size >> SECTOR_SHIFT);
	set_disk_ro(d->disk, true);

	ret = add_disk(d->disk);
	if (ret) {
		put_disk(d->disk);
		d->disk = NULL;
		blk_mq_free_tag_set(&d->tag_set);
	}
	return ret;
}

static void ftl_del_disk(struct ftl_disk *d)
{
	if (!d->disk)
		return;
	del_gendisk(d->disk);
	put_disk(d->disk);
	blk_mq_free_tag_set(&d->tag_set);
	kfree(d->chunks);
	d->disk = NULL;
}

/* --- volume map --- */

/* The volume map header (8 KiB), or NULL when logical block 0 holds none. */
static u8 *ftl_volume_header(struct ftl *f)
{
	u8 *hdr = kmalloc(2 * FTL_LP_SIZE, GFP_KERNEL);
	u64 media;
	int ret;

	if (!hdr)
		return NULL;
	mutex_lock(&f->lock);
	ret = ftl_copy(f, 0, 0, hdr, FTL_LP_SIZE) ?: ftl_copy(f, 1, 0, hdr + FTL_LP_SIZE, FTL_LP_SIZE);
	mutex_unlock(&f->lock);
	media = get_unaligned_le64(hdr + LV_MEDIA_SIZE);
	if (ret || (memcmp(hdr, lv_magic[0], 16) && memcmp(hdr, lv_magic[1], 16)) ||
	    !media || media > (u64)f->nlba << FTL_LP_SHIFT) {
		kfree(hdr);
		return NULL;
	}
	f->chunk_shift = order_base_2(DIV_ROUND_UP_ULL(media, LV_MAP_ENTRIES));
	return hdr;
}

static void ftl_volumes(struct ftl *f, const u8 *hdr, const char *base)
{
	unsigned int i, n, c;

	n = min_t(u32, get_unaligned_le32(hdr + LV_COUNT), LV_MAX_VOLUMES);

	for (i = 0; i < n; i++) {
		const u8 *r = hdr + LV_RECORDS + i * LV_RECORD_SIZE;
		struct ftl_disk *d = &f->vols[i];
		u64 begin = get_unaligned_le64(r + LV_R_BEGIN);
		u64 end = get_unaligned_le64(r + LV_R_END);
		char label[LV_R_NAME_LEN + 1], name[DISK_NAME_LEN];
		unsigned int k, found = 0;

		for (k = 0; k < LV_R_NAME_LEN; k++) {
			u16 ch = get_unaligned_le16(r + LV_R_NAME + 2 * k);

			label[k] = ch && ch < 0x80 ? ch : (ch ? '?' : 0);
			if (!ch)
				break;
		}
		label[k] = 0;
		if (end <= begin)
			continue;
		d->size = end - begin;
		d->nchunks = DIV_ROUND_UP_ULL(d->size, BIT_ULL(f->chunk_shift));
		d->chunks = kcalloc(d->nchunks, sizeof(*d->chunks), GFP_KERNEL);
		if (!d->chunks)
			break;
		for (c = 0; c < LV_MAP_ENTRIES; c++) {
			u16 e = get_unaligned_le16(hdr + LV_MAP + 2 * c);

			if (e >> 12 == i && (e & 0xfff) < d->nchunks) {
				d->chunks[e & 0xfff] = c;
				found++;
			}
		}
		if (found != d->nchunks) {
			dev_warn(&f->mtd->dev, "volume %u (%s): %u of %u chunks mapped, skipped\n",
				 i, label, found, d->nchunks);
			kfree(d->chunks);
			d->chunks = NULL;
			continue;
		}
		snprintf(name, sizeof(name), "%sp%u", base, i + 1);
		if (ftl_add_disk(f, d, name)) {
			kfree(d->chunks);
			d->chunks = NULL;
			continue;
		}
		f->nvols = i + 1;
		dev_info(&f->mtd->dev, "%s: volume \"%s\", %llu bytes, attributes %#llx\n",
			 name, label, d->size, get_unaligned_le64(r + LV_R_ATTR));
	}
}

/* --- MTD binding --- */

static bool ftl_usable(struct mtd_info *mtd)
{
	u32 lps = mtd->writesize >> FTL_LP_SHIFT;
	int i;

	if (mtd->type != MTD_MLCNANDFLASH || !lps || mtd->writesize % FTL_LP_SIZE ||
	    mtd->oobsize != lps * FTL_META || !mtd->numeraseregions ||
	    mtd->numeraseregions > FTL_MAX_CAUS)
		return false;
	for (i = 0; i < mtd->numeraseregions; i++)
		if (mtd->eraseregions[i].erasesize != mtd->erasesize ||
		    mtd->eraseregions[i].numblocks != mtd->eraseregions[0].numblocks)
			return false;
	return true;
}

static void ftl_work(struct work_struct *work)
{
	struct ftl *f = container_of(work, struct ftl, work);
	char base[DISK_NAME_LEN];
	u8 *hdr;
	int ret;

	ret = ftl_build(f);
	if (ret) {
		dev_err(&f->mtd->dev, "translation map failed: %d\n", ret);
		return;
	}
	snprintf(base, sizeof(base), "nand%d", atomic_inc_return(&ftl_count) - 1);
	hdr = ftl_volume_header(f);
	f->whole.size = hdr ? get_unaligned_le64(hdr + LV_MEDIA_SIZE) :
			      (u64)f->nlba << FTL_LP_SHIFT;
	if (!ftl_add_disk(f, &f->whole, base) && hdr)
		ftl_volumes(f, hdr, base);
	kfree(hdr);
}

static void ftl_notify_add(struct mtd_info *mtd)
{
	struct ftl *f;

	if (!ftl_usable(mtd))
		return;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return;
	f->mtd = mtd;
	f->caus = mtd->numeraseregions;
	f->blocks = mtd->eraseregions[0].numblocks;
	f->pages = mtd->erasesize / mtd->writesize;
	f->lps = mtd->writesize >> FTL_LP_SHIFT;
	f->nlba = f->caus * f->blocks * f->pages * f->lps;
	f->cached = FTL_UNMAPPED;
	mutex_init(&f->lock);
	f->l2p = vmalloc(array_size(f->nlba, sizeof(*f->l2p)));
	f->page = kmalloc(mtd->writesize, GFP_KERNEL);
	f->oob = kmalloc(mtd->oobsize, GFP_KERNEL);
	if (!f->l2p || !f->page || !f->oob) {
		vfree(f->l2p);
		kfree(f->page);
		kfree(f->oob);
		kfree(f);
		return;
	}
	memset32(f->l2p, FTL_UNMAPPED, f->nlba);
	__get_mtd_device(mtd);
	INIT_WORK(&f->work, ftl_work);
	mutex_lock(&ftl_list_lock);
	list_add(&f->node, &ftl_list);
	mutex_unlock(&ftl_list_lock);
	schedule_work(&f->work);
}

static void ftl_free(struct ftl *f)
{
	unsigned int i;

	flush_work(&f->work);
	for (i = 0; i < f->nvols; i++)
		ftl_del_disk(&f->vols[i]);
	ftl_del_disk(&f->whole);
	__put_mtd_device(f->mtd);
	vfree(f->l2p);
	kfree(f->page);
	kfree(f->oob);
	kfree(f);
}

static void ftl_notify_remove(struct mtd_info *mtd)
{
	struct ftl *f, *tmp;

	mutex_lock(&ftl_list_lock);
	list_for_each_entry_safe(f, tmp, &ftl_list, node) {
		if (f->mtd == mtd) {
			list_del(&f->node);
			ftl_free(f);
		}
	}
	mutex_unlock(&ftl_list_lock);
}

static struct mtd_notifier ftl_notifier = {
	.add = ftl_notify_add,
	.remove = ftl_notify_remove,
};

static int __init ftl_init(void)
{
	register_mtd_user(&ftl_notifier);
	return 0;
}

static void __exit ftl_exit(void)
{
	unregister_mtd_user(&ftl_notifier);
}

module_init(ftl_init);
module_exit(ftl_exit);
MODULE_DESCRIPTION("S5L8947X managed NAND translation layer and volume map (read-only)");
MODULE_LICENSE("GPL");
