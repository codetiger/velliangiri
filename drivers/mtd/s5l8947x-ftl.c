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
 * chunks need not be contiguous on the logical disk. Volumes with attribute
 * bit 48 set are encrypted per 4 KiB logical block with AES-256-CBC and a
 * 4-word Galois LFSR (0x80000061) IV seeded from the block's lba on nand0;
 * their 32-byte keys are unwrapped from the key store in the reserved SLC
 * blocks at the top of each CAU using the hardware device key.
 */

#include <crypto/aes.h>
#include <crypto/sha1.h>
#include <crypto/skcipher.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/s5l8947x-aes.h>
#include <linux/scatterlist.h>
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

#define LV_UUID			0x10
#define LV_MEDIA_SIZE		0x20
#define LV_COUNT		0x28
#define LV_RECORDS		0x200
#define LV_RECORD_SIZE		0x80
#define LV_R_UUID		0x10
#define LV_R_BEGIN		0x20
#define LV_R_END		0x28
#define LV_R_ATTR		0x30
#define LV_R_NAME		0x38
#define LV_R_NAME_LEN		36
#define LV_MAP			0x800
#define LV_MAP_ENTRIES		1024
#define LV_MAX_VOLUMES		8
#define LV_ATTR_ENCRYPTED	BIT_ULL(48)

#define KS_RESERVED_BLOCKS	16
#define KS_OPEN_MAGIC		0x6f70656e
#define KS_DATA_PAGE		16
#define KS_SIZE			0x400
#define KS_MASK_LEN		16
#define KS_MAGIC		0x46616365
#define KS_VERSION		1
#define KS_GEN			0x38
#define KS_LOCKERS		0x40
#define KS_L_MAGIC		0x4c6b
#define KS_L_KEYS		0x4c77564d
#define KS_L_FKEY		0xc46b6579
#define KS_L_DONE		0x444f4e45
#define KS_K_COUNT		0x0c
#define KS_K_UUID		0x10
#define KS_K_RECORDS		0x20
#define KS_K_RECORD_SIZE	0x30
#define KS_FKEY_WRAP_LEN	40

#define RFC3394_IV		0xa6a6a6a6a6a6a6a6ULL
#define CP_V4_VERSION		4
#define CP_CLASS_OPEN		4
#define CP_V4_HEADER_LEN	36
#define CP_WRAP_LEN		40

static const u8 lv_magic[2][16] = {
	{ 0x6a, 0x90, 0x88, 0xcf, 0x8a, 0xfd, 0x63, 0x0a,
	  0xe3, 0x51, 0xe2, 0x48, 0x87, 0xe0, 0xb9, 0x8b },
	{ 0xb1, 0x89, 0xa5, 0x19, 0x4f, 0x59, 0x4b, 0x1d,
	  0xad, 0x44, 0x1e, 0x12, 0x7a, 0xaf, 0x45, 0x39 },
};

static const u8 ftl_wrap_seed[AES_BLOCK_SIZE] = {
	0x18, 0x3e, 0x99, 0x67, 0x6b, 0xb0, 0x3c, 0x54,
	0x6f, 0xa4, 0x68, 0xf5, 0x1c, 0x0c, 0xbd, 0x49,
};

static const u8 ftl_fkey_seed[AES_BLOCK_SIZE] = {
	[0 ... AES_BLOCK_SIZE - 1] = 0x01,
};

struct ftl;

struct ftl_disk {
	struct ftl *ftl;
	struct gendisk *disk;
	struct blk_mq_tag_set tag_set;
	u64 size;		/* bytes */
	u16 *chunks;		/* disk chunk of each volume chunk, NULL = whole disk */
	unsigned int nchunks;
	struct crypto_skcipher *tfm;
	struct skcipher_request *req;
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
	u8 *dec;		/* decrypted 4 KiB logical block */
	u32 cached;		/* page number in the cache, or FTL_UNMAPPED */

	unsigned int chunk_shift;
	struct ftl_disk whole;
	struct ftl_disk vols[LV_MAX_VOLUMES];
	unsigned int nvols;
};

static LIST_HEAD(ftl_list);
static DEFINE_MUTEX(ftl_list_lock);

static DEFINE_MUTEX(ftl_file_lock);
static struct crypto_skcipher *ftl_file_tfm;
static struct skcipher_request *ftl_file_req;
static u8 *ftl_file_buf;
static u8 ftl_file_master_key[AES_KEYSIZE_256];
static bool ftl_file_master_valid;

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

/* Read len bytes from an SLC-programmed reserved block page. */
static int ftl_read_slc(struct ftl *f, u32 page, u8 *data, size_t len)
{
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_RAW,
		.len = len,
		.datbuf = data,
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

static void ftl_block_iv(u32 lba, __le32 iv[4])
{
	u32 x = lba;
	int i;

	for (i = 0; i < 4; i++) {
		x = (x >> 1) ^ (x & 1 ? 0x80000061U : 0);
		iv[i] = cpu_to_le32(x);
	}
}

static int ftl_decrypt(struct skcipher_request *req, const void *src, void *dst,
		       size_t len, void *iv)
{
	DECLARE_CRYPTO_WAIT(wait);
	struct scatterlist sg_src, sg_dst;

	sg_init_one(&sg_src, src, len);
	sg_init_one(&sg_dst, dst, len);
	skcipher_request_set_callback(req,
				      CRYPTO_TFM_REQ_MAY_SLEEP |
				      CRYPTO_TFM_REQ_MAY_BACKLOG,
				      crypto_req_done, &wait);
	skcipher_request_set_crypt(req, &sg_src, &sg_dst, len, iv);
	return crypto_wait_req(crypto_skcipher_decrypt(req), &wait);
}

/* Copy n bytes at offset off of logical block lba. Caller holds f->lock. */
static int ftl_copy(struct ftl *f, struct ftl_disk *d, u32 lba,
		    unsigned int off, void *dst, unsigned int n)
{
	u32 phys, page;
	u8 *src;
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
	src = f->page + (phys % f->lps) * FTL_LP_SIZE;
	if (d && d->req) {
		__le32 iv[4];

		ftl_block_iv(lba, iv);
		ret = ftl_decrypt(d->req, src, f->dec, FTL_LP_SIZE, iv);
		if (ret)
			return ret;
		src = f->dec;
	}
	memcpy(dst, src + off, n);
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
			    ftl_copy(f, d, at >> FTL_LP_SHIFT, off, dst + done, n))
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
	skcipher_request_free(d->req);
	crypto_free_skcipher(d->tfm);
	kfree(d->chunks);
	d->req = NULL;
	d->tfm = NULL;
	d->disk = NULL;
}

/* --- key store and volume map --- */

static bool ftl_read_keystore(struct ftl *f, u8 *store)
{
	u32 start = f->blocks > KS_RESERVED_BLOCKS ? f->blocks - KS_RESERVED_BLOCKS : 0;
	u8 *buf = kmalloc(KS_SIZE, GFP_KERNEL);
	u32 cau, blk, best_gen = 0;
	bool found = false;
	int i;

	if (!buf)
		return false;
	for (cau = 0; cau < f->caus; cau++) {
		for (blk = start; blk < f->blocks; blk++) {
			u32 gen;

			if (ftl_read_slc(f, ftl_page(f, cau, blk, 0), buf, 4) ||
			    get_unaligned_le32(buf) != KS_OPEN_MAGIC ||
			    ftl_read_slc(f, ftl_page(f, cau, blk, KS_DATA_PAGE), buf, KS_SIZE))
				continue;
			for (i = 0; i < KS_MASK_LEN; i++) {
				buf[i] ^= buf[KS_MASK_LEN + i];
				buf[KS_MASK_LEN + i] = 0;
			}
			if (get_unaligned_le32(buf) != KS_MAGIC ||
			    get_unaligned_le32(buf + 4) != KS_VERSION)
				continue;
			gen = get_unaligned_le32(buf + KS_GEN);
			if (!found || gen > best_gen) {
				memcpy(store, buf, KS_SIZE);
				best_gen = gen;
				found = true;
			}
		}
	}
	kfree(buf);
	return found;
}

/* RFC 3394 AES Key Unwrap for a 32-byte key (40-byte wrapped input). */
static int ftl_rfc3394_unwrap(const u8 *kek, unsigned int kek_len,
			      const u8 in[CP_WRAP_LEN], u8 out[AES_KEYSIZE_256])
{
	struct crypto_aes_ctx ctx;
	u8 blk[AES_BLOCK_SIZE];
	u64 a, r[4];
	int i, j, ret;

	ret = aes_expandkey(&ctx, kek, kek_len);
	if (ret)
		return ret;
	a = get_unaligned_be64(in);
	for (i = 0; i < 4; i++)
		r[i] = get_unaligned_be64(in + 8 + 8 * i);
	for (j = 5; j >= 0; j--) {
		for (i = 3; i >= 0; i--) {
			put_unaligned_be64(a ^ (u64)(4 * j + i + 1), blk);
			put_unaligned_be64(r[i], blk + 8);
			aes_decrypt(&ctx, blk, blk);
			a = get_unaligned_be64(blk);
			r[i] = get_unaligned_be64(blk + 8);
		}
	}
	memzero_explicit(&ctx, sizeof(ctx));
	memzero_explicit(blk, sizeof(blk));
	if (a != RFC3394_IV) {
		memzero_explicit(r, sizeof(r));
		return -EBADMSG;
	}
	for (i = 0; i < 4; i++)
		put_unaligned_be64(r[i], out + 8 * i);
	memzero_explicit(r, sizeof(r));
	return 0;
}

/* Unwrap the volume key locker (0x4c77564d) for media hdr; caller frees with kfree_sensitive. */
static u8 *ftl_unwrap_keys(struct ftl *f, const u8 *hdr, u32 *nkeys)
{
	u8 wkey[AES_BLOCK_SIZE], iv[AES_BLOCK_SIZE] = {};
	struct skcipher_request *req = NULL;
	struct crypto_skcipher *tfm = NULL;
	u8 *store, *locker = NULL;
	unsigned int off;
	u32 count;

	*nkeys = 0;
	store = kmalloc(KS_SIZE, GFP_KERNEL);
	if (!store)
		return NULL;
	if (!ftl_read_keystore(f, store))
		goto out;

	for (off = KS_LOCKERS; off + 8 <= KS_SIZE; ) {
		u16 magic = get_unaligned_le16(store + off);
		u16 len = get_unaligned_le16(store + off + 2);
		u32 tag = get_unaligned_le32(store + off + 4);

		if (magic != KS_L_MAGIC || tag == KS_L_DONE || off + 8 + len > KS_SIZE)
			break;
		if (!locker && tag == KS_L_KEYS && len >= KS_K_RECORDS &&
		    !(len % AES_BLOCK_SIZE)) {
			memset(iv, 0, sizeof(iv));
			if (s5l8947x_aes_hwkey_crypt(S5L8947X_AES_KEY_DEVICE, true, NULL,
						     ftl_wrap_seed, wkey, sizeof(wkey)))
				break;
			tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
			if (IS_ERR(tfm)) {
				tfm = NULL;
				break;
			}
			req = skcipher_request_alloc(tfm, GFP_KERNEL);
			locker = kmemdup(store + off + 8, len, GFP_KERNEL);
			if (!req || !locker ||
			    crypto_skcipher_setkey(tfm, wkey, sizeof(wkey)) ||
			    ftl_decrypt(req, locker, locker, len, iv) ||
			    memcmp(locker + KS_K_UUID, hdr + LV_UUID, 16)) {
				kfree_sensitive(locker);
				locker = NULL;
				break;
			}
			count = get_unaligned_le32(locker + KS_K_COUNT);
			*nkeys = min_t(u32, count, (len - KS_K_RECORDS) / KS_K_RECORD_SIZE);
		} else if (tag == KS_L_FKEY && len == KS_FKEY_WRAP_LEN) {
			if (!s5l8947x_aes_hwkey_crypt(S5L8947X_AES_KEY_DEVICE, true, NULL,
						      ftl_fkey_seed, wkey, sizeof(wkey)) &&
			    !ftl_rfc3394_unwrap(wkey, sizeof(wkey), store + off + 8,
						ftl_file_master_key))
				ftl_file_master_valid = true;
		}
		off += 8 + len;
	}
out:
	memzero_explicit(wkey, sizeof(wkey));
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	kfree_sensitive(store);
	return locker;
}

int s5l8947x_file_key_unwrap(const void *attr, size_t len, u8 fkey[32], u8 iv_key[16])
{
	u32 digest[SHA1_DIGEST_WORDS], ws[SHA1_WORKSPACE_WORDS];
	u8 blk[SHA1_BLOCK_SIZE] = {};
	const u8 *p = attr;
	int i, ret;

	if (!ftl_file_master_valid)
		return -ENOKEY;
	if (len < CP_V4_HEADER_LEN + CP_WRAP_LEN)
		return -EINVAL;
	if (get_unaligned_le16(p) != CP_V4_VERSION ||
	    (get_unaligned_le32(p + 8) & 0x1f) != CP_CLASS_OPEN ||
	    get_unaligned_le32(p + 12) != CP_WRAP_LEN)
		return -EOPNOTSUPP;
	ret = ftl_rfc3394_unwrap(ftl_file_master_key, sizeof(ftl_file_master_key),
				 p + CP_V4_HEADER_LEN, fkey);
	if (ret)
		return ret;

	memcpy(blk, fkey, AES_KEYSIZE_256);
	blk[AES_KEYSIZE_256] = 0x80;
	put_unaligned_be64(AES_KEYSIZE_256 * 8, blk + SHA1_BLOCK_SIZE - 8);
	sha1_init(digest);
	sha1_transform(digest, blk, ws);
	for (i = 0; i < 4; i++)
		put_unaligned_be32(digest[i], iv_key + 4 * i);
	memzero_explicit(blk, sizeof(blk));
	memzero_explicit(digest, sizeof(digest));
	memzero_explicit(ws, sizeof(ws));
	return 0;
}
EXPORT_SYMBOL_GPL(s5l8947x_file_key_unwrap);

int s5l8947x_file_block_decrypt(struct block_device *bdev, u64 blocknr,
				const u8 fkey[32], const u8 iv_key[16],
				u64 file_off, void *dst)
{
	struct ftl_disk *d = bdev ? bdev->bd_disk->private_data : NULL;
	struct crypto_aes_ctx aes;
	u8 iv[AES_BLOCK_SIZE];
	__le32 lfsr[4];
	struct ftl *f;
	u64 pos, at;
	int ret;

	if (!d || !d->ftl || (file_off & (FTL_LP_SIZE - 1)))
		return -EINVAL;
	f = d->ftl;
	pos = blocknr << FTL_LP_SHIFT;
	if (pos + FTL_LP_SIZE > d->size)
		return -EIO;
	ret = aes_expandkey(&aes, iv_key, AES_KEYSIZE_128);
	if (ret)
		return ret;

	mutex_lock(&ftl_file_lock);
	if (!ftl_file_tfm) {
		struct crypto_skcipher *tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);

		if (IS_ERR(tfm)) {
			ret = PTR_ERR(tfm);
			goto out;
		}
		ftl_file_tfm = tfm;
	}
	if (!ftl_file_req) {
		ftl_file_req = skcipher_request_alloc(ftl_file_tfm, GFP_KERNEL);
		if (!ftl_file_req) {
			ret = -ENOMEM;
			goto out;
		}
	}
	if (!ftl_file_buf) {
		ftl_file_buf = kmalloc(FTL_LP_SIZE, GFP_KERNEL);
		if (!ftl_file_buf) {
			ret = -ENOMEM;
			goto out;
		}
	}

	at = ftl_disk_offset(d, pos);
	mutex_lock(&f->lock);
	ret = ftl_copy(f, NULL, at >> FTL_LP_SHIFT, 0, ftl_file_buf, FTL_LP_SIZE);
	mutex_unlock(&f->lock);
	if (ret)
		goto out;

	ret = crypto_skcipher_setkey(ftl_file_tfm, fkey, AES_KEYSIZE_256);
	if (!ret) {
		ftl_block_iv((u32)file_off, lfsr);
		aes_encrypt(&aes, iv, (const u8 *)lfsr);
		ret = ftl_decrypt(ftl_file_req, ftl_file_buf, ftl_file_buf,
				  FTL_LP_SIZE, iv);
		if (!ret)
			memcpy(dst, ftl_file_buf, FTL_LP_SIZE);
	}
out:
	mutex_unlock(&ftl_file_lock);
	memzero_explicit(&aes, sizeof(aes));
	memzero_explicit(iv, sizeof(iv));
	return ret;
}
EXPORT_SYMBOL_GPL(s5l8947x_file_block_decrypt);

static int ftl_setup_cipher(struct ftl_disk *d, const u8 *uuid, const u8 *locker, u32 nkeys)
{
	u32 i;
	int ret;

	if (!locker)
		return -ENOKEY;
	for (i = 0; i < nkeys; i++) {
		const u8 *ent = locker + KS_K_RECORDS + i * KS_K_RECORD_SIZE;

		if (memcmp(ent, uuid, 16))
			continue;
		d->tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
		if (IS_ERR(d->tfm)) {
			ret = PTR_ERR(d->tfm);
			d->tfm = NULL;
			return ret;
		}
		ret = crypto_skcipher_setkey(d->tfm, ent + 16, AES_KEYSIZE_256);
		if (!ret) {
			d->req = skcipher_request_alloc(d->tfm, GFP_KERNEL);
			if (!d->req)
				ret = -ENOMEM;
		}
		if (ret) {
			crypto_free_skcipher(d->tfm);
			d->tfm = NULL;
		}
		return ret;
	}
	return -ENOKEY;
}

/* The volume map header (8 KiB), or NULL when logical block 0 holds none. */
static u8 *ftl_volume_header(struct ftl *f)
{
	u8 *hdr = kmalloc(2 * FTL_LP_SIZE, GFP_KERNEL);
	u64 media;
	int ret;

	if (!hdr)
		return NULL;
	mutex_lock(&f->lock);
	ret = ftl_copy(f, NULL, 0, 0, hdr, FTL_LP_SIZE) ?:
	      ftl_copy(f, NULL, 1, 0, hdr + FTL_LP_SIZE, FTL_LP_SIZE);
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
	u8 *locker = NULL;
	u32 nkeys = 0;
	unsigned int i, n, c;

	n = min_t(u32, get_unaligned_le32(hdr + LV_COUNT), LV_MAX_VOLUMES);
	for (i = 0; i < n; i++) {
		const u8 *r = hdr + LV_RECORDS + i * LV_RECORD_SIZE;

		if (get_unaligned_le64(r + LV_R_ATTR) & LV_ATTR_ENCRYPTED) {
			locker = ftl_unwrap_keys(f, hdr, &nkeys);
			break;
		}
	}

	for (i = 0; i < n; i++) {
		const u8 *r = hdr + LV_RECORDS + i * LV_RECORD_SIZE;
		struct ftl_disk *d = &f->vols[i];
		u64 begin = get_unaligned_le64(r + LV_R_BEGIN);
		u64 end = get_unaligned_le64(r + LV_R_END);
		u64 attr = get_unaligned_le64(r + LV_R_ATTR);
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
		if ((attr & LV_ATTR_ENCRYPTED) &&
		    ftl_setup_cipher(d, r + LV_R_UUID, locker, nkeys))
			dev_warn(&f->mtd->dev, "volume %u (%s): encryption key unavailable\n",
				 i, label);
		snprintf(name, sizeof(name), "%sp%u", base, i + 1);
		if (ftl_add_disk(f, d, name)) {
			skcipher_request_free(d->req);
			crypto_free_skcipher(d->tfm);
			kfree(d->chunks);
			d->req = NULL;
			d->tfm = NULL;
			d->chunks = NULL;
			continue;
		}
		f->nvols = i + 1;
		dev_info(&f->mtd->dev, "%s: volume \"%s\", %llu bytes, attributes %#llx%s\n",
			 name, label, d->size, attr, d->req ? " (decrypted)" : "");
	}
	kfree_sensitive(locker);
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
	snprintf(base, sizeof(base), "nand%d", f->mtd->index);
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
	f->dec = kmalloc(FTL_LP_SIZE, GFP_KERNEL);
	if (!f->l2p || !f->page || !f->oob || !f->dec) {
		vfree(f->l2p);
		kfree(f->page);
		kfree(f->oob);
		kfree(f->dec);
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
	kfree_sensitive(f->dec);
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
	skcipher_request_free(ftl_file_req);
	crypto_free_skcipher(ftl_file_tfm);
	kfree_sensitive(ftl_file_buf);
	memzero_explicit(ftl_file_master_key, sizeof(ftl_file_master_key));
	ftl_file_master_valid = false;
}

module_init(ftl_init);
module_exit(ftl_exit);
MODULE_DESCRIPTION("S5L8947X managed NAND translation layer and volume map (read-only)");
MODULE_LICENSE("GPL");
