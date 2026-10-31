// SPDX-License-Identifier: GPL-2.0-only
/*
 * Read-write translation layer for the managed NAND of S5L8947X devices, as
 * formatted by the stock firmware, and its volume map.
 *
 * The NAND is reached through an MTD whose pages hold several 4 KiB logical
 * pages, each with a 16-byte metadata record in the out-of-band area, and
 * whose erase regions are the package's CAUs (one block per CAU forms a
 * superblock). The format was worked out from the on-flash data:
 *
 *   metadata    u8 type, u8 flags, u16 sequence, u32 generation, u32 lba,
 *               u32 generation first written; (generation, sequence) is a
 *               48-bit write counter and lba counts 4 KiB logical blocks.
 *               Type 0x01 is data, 0x02 is padding, 0x1c a table of contents.
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
 * The whole logical disk is exposed as nand0 and each volume as nand0pN.
 * Volumes are separate disks, not partitions, because their chunks need not
 * be contiguous on the logical disk. Volumes with attribute bit 48 set are
 * encrypted per 4 KiB logical block with AES-256-CBC and a 4-word Galois LFSR
 * (0x80000061) IV seeded from the block's lba on nand0; their 32-byte keys
 * are unwrapped from the key store in the reserved SLC blocks at the top of
 * each CAU using the hardware device key. Factory board identity and MAC
 * addresses (SCfg) are also read from the reserved SLC blocks and exposed on
 * /sys/block/nand0/{serial,model,emac,wmac,bmac}.
 */

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/hash.h>
#include <crypto/sha1.h>
#include <crypto/skcipher.h>
#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/etherdevice.h>
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mutex.h>
#include <linux/of.h>
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
#define FTL_T_PAD		0x02
#define FTL_T_TOC		0x1c
#define FTL_F_GC		BIT(0)
#define FTL_F_TOC_STD		0x08
#define FTL_TOC_MAX_PAGES	4
#define FTL_TOC_WRITE_PAGES	2
#define FTL_MAX_CAUS		8
#define FTL_MAX_LPS		16
#define FTL_GC_LOW_SB		2
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
#define KS_L_BAG		0x42414731

#define RFC3394_IV		0xa6a6a6a6a6a6a6a6ULL
#define CP_V4_VERSION		4
#define CP_CLASS_FIRST		1
#define CP_CLASS_OPEN		4
#define CP_CLASSES		(CP_CLASS_OPEN + 1)
#define CP_V4_HEADER_LEN	36
#define CP_WRAP_LEN		40

#define SCFG_HDR_LEN		0x80
#define SCFG_HDR_VERSION	0x20
#define SCFG_HDR_COPY_VER	0x28
#define SCFG_MAGIC		0x53436667
#define SCFG_COUNT		0x14
#define SCFG_ENTRIES		0x18
#define SCFG_ENTRY_SIZE		0x14
#define SCFG_TAG_CNTB		0x434e5442
#define SCFG_TAG_SRNM		0x53724e6d
#define SCFG_TAG_MODN		0x4d6f6423
#define SCFG_TAG_EMAC		0x454d6163
#define SCFG_TAG_WMAC		0x574d6163
#define SCFG_TAG_BMAC		0x424d6163

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

enum ftl_sb_state {
	FTL_SB_BAD = 0,
	FTL_SB_FREE,
	FTL_SB_OPEN,
	FTL_SB_TOC,
	FTL_SB_SPECIAL,
};

struct ftl_sb {
	u8 state;
	u8 members;
	u16 valid_lps;
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
	bool ro;
};

struct ftl {
	struct mtd_info *mtd;
	struct list_head node;
	struct work_struct work;
	struct mutex lock;	/* the MTD, page cache, and write staging buffer */

	u32 caus;
	u32 blocks;		/* per CAU */
	u32 data_blocks;	/* excluding reserved SLC blocks at top of CAU */
	u32 pages;		/* per block */
	u32 lps;		/* logical pages per page */
	u32 nlba;		/* map entries */
	u32 *l2p;		/* lba -> page * lps + logical page */
	struct ftl_sb *sbs;
	u32 free_sbs;
	u32 next_gen;
	u16 next_seq;

	u8 *page;		/* cached page data */
	u8 *oob;
	u8 *dec;		/* decrypted/encrypted 4 KiB logical block */
	u32 cached;		/* page number in the cache, or FTL_UNMAPPED */

	u32 wsb_block;		/* active open superblock for writes, or 0 */
	u32 wsb_gen;
	u16 wsb_first_seq;
	u32 wsb_idx;
	u32 wsb_total;
	u32 *wsb_lbas;
	u8 *wbuf_data;
	u8 *wbuf_oob;
	u32 wbuf_lbas[FTL_MAX_LPS];
	u32 wbuf_count;

	unsigned int chunk_shift;
	struct ftl_disk whole;
	struct ftl_disk vols[LV_MAX_VOLUMES];
	unsigned int nvols;

	char serial[17];
	char model[17];
	u8 emac[ETH_ALEN];
	u8 wmac[ETH_ALEN];
	u8 bmac[ETH_ALEN];
};

static LIST_HEAD(ftl_list);
static DEFINE_MUTEX(ftl_list_lock);

static DEFINE_MUTEX(ftl_file_lock);
static struct crypto_skcipher *ftl_file_tfm;
static struct skcipher_request *ftl_file_req;
static u8 *ftl_file_buf;
static u8 ftl_file_master_key[AES_KEYSIZE_256];
static bool ftl_file_master_valid;
/* system keybag: payload key from the key store, class keys it unlocks */
static DEFINE_MUTEX(ftl_keybag_lock);
static u8 ftl_bag_key[AES_KEYSIZE_256];
static bool ftl_bag_valid;
static u8 ftl_class_key[CP_CLASS_OPEN][AES_KEYSIZE_256];
static unsigned long ftl_class_valid;
static atomic_t ftl_class_files[CP_CLASSES];

/* --- reading and writing the MTD --- */

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

static int ftl_write_page(struct ftl *f, u32 page, const u8 *data, const u8 *oob)
{
	struct mtd_oob_ops ops = {
		.mode = MTD_OPS_PLACE_OOB,
		.len = f->mtd->writesize,
		.datbuf = (u8 *)data,
		.ooblen = f->mtd->oobsize,
		.oobbuf = (u8 *)oob,
	};

	if (f->cached == page)
		f->cached = FTL_UNMAPPED;
	return mtd_write_oob(f->mtd, (loff_t)page * f->mtd->writesize, &ops);
}

static int ftl_erase_sb(struct ftl *f, u32 block)
{
	u32 c;
	int ret;

	for (c = 0; c < f->caus; c++) {
		struct erase_info instr = {
			.addr = (loff_t)ftl_page(f, c, block, 0) * f->mtd->writesize,
			.len = f->mtd->erasesize,
		};

		ret = mtd_erase(f->mtd, &instr);
		if (ret) {
			f->sbs[block].state = FTL_SB_BAD;
			return ret;
		}
	}
	if (f->cached != FTL_UNMAPPED &&
	    ((f->cached / f->pages) % f->blocks) == block)
		f->cached = FTL_UNMAPPED;
	f->sbs[block].state = FTL_SB_FREE;
	f->sbs[block].members = GENMASK(f->caus - 1, 0);
	f->sbs[block].valid_lps = 0;
	f->free_sbs++;
	return 0;
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
	u32 max_gen;
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

static void ftl_wsb_pos(struct ftl *f, u32 block, u32 idx, u32 *page, u32 *lp)
{
	u32 per_page = f->lps * f->caus;
	u32 cau = (idx / f->lps) % f->caus;

	*lp = idx % f->lps;
	*page = ftl_page(f, cau, block, idx / per_page);
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

static void ftl_map_update(struct ftl *f, u32 lba, u32 new_phys)
{
	u32 old_phys, blk;

	if (lba >= f->nlba)
		return;
	old_phys = f->l2p[lba];
	if (old_phys != FTL_UNMAPPED) {
		blk = ((old_phys / f->lps) / f->pages) % f->blocks;
		if (blk < f->blocks && f->sbs[blk].valid_lps > 0)
			f->sbs[blk].valid_lps--;
	}
	f->l2p[lba] = new_phys;
	if (new_phys != FTL_UNMAPPED) {
		blk = ((new_phys / f->lps) / f->pages) % f->blocks;
		if (blk < f->blocks)
			f->sbs[blk].valid_lps++;
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
	if (sb_gen != U32_MAX && sb_gen > b->max_gen)
		b->max_gen = sb_gen;
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
		if (gen != U32_MAX && gen > b->max_gen)
			b->max_gen = gen;
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
			if (gen != U32_MAX && gen > b->max_gen)
				b->max_gen = gen;
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

	/* Block 0 holds boot pages; top KS_RESERVED_BLOCKS hold SLC special data. */
	for (block = 1; block < f->data_blocks; block++) {
		u32 erased = 0;
		u8 members_mask = 0;
		u8 first_type = 0xff;
		bool bad = false;

		b.nmembers = 0;
		for (c = 0; c < f->caus; c++) {
			ret = ftl_read_page(f, ftl_page(f, c, block, 0), NULL, f->oob);
			if (!ret && f->oob[0] != 0xff) {
				if (!b.nmembers)
					first_type = f->oob[0];
				b.members[b.nmembers++] = c;
				members_mask |= BIT(c);
			} else if (!ret && f->oob[0] == 0xff) {
				erased++;
			} else if (ret == -EBADMSG) {
				bad = true;
			} else if (ret) {
				goto out;
			}
		}
		ret = 0;
		if (!b.nmembers) {
			if (!bad && erased == f->caus) {
				f->sbs[block].state = FTL_SB_FREE;
				f->sbs[block].members = GENMASK(f->caus - 1, 0);
				f->free_sbs++;
			} else {
				f->sbs[block].state = FTL_SB_BAD;
			}
			continue;
		}
		f->sbs[block].members = members_mask;
		b.total = f->pages * f->lps * b.nmembers;

		/* The table, if any, ends the superblock; its first record describes it. */
		ftl_pos(f, &b, block, b.total - f->lps, &page, &lp);
		ret = ftl_read_page(f, page, NULL, f->oob);
		if (!ret && f->oob[0] == FTL_T_TOC && !ftl_build_toc(f, &b, block, f->oob)) {
			f->sbs[block].state = FTL_SB_TOC;
			tocs++;
		} else {
			ret = ftl_build_open(f, &b, block);
			if (ret)
				goto out;
			f->sbs[block].state = (first_type == FTL_T_DATA) ?
					      FTL_SB_OPEN : FTL_SB_SPECIAL;
			open++;
		}
		cond_resched();
	}

	for (block = 0; block < f->nlba; block++) {
		u32 phys = f->l2p[block];

		if (phys == FTL_UNMAPPED)
			continue;
		mapped++;
		c = ((phys / f->lps) / f->pages) % f->blocks;
		if (c < f->blocks)
			f->sbs[c].valid_lps++;
	}
	f->next_gen = b.max_gen + 1;
	f->next_seq = 0;
	dev_info(&f->mtd->dev,
		 "translation map: %u blocks mapped, %u tables, %u open, %u free superblocks, next gen %u, %u ms\n",
		 mapped, tocs, open, f->free_sbs, f->next_gen,
		 jiffies_to_msecs(jiffies - start));
out:
	kvfree(b.toc);
	vfree(b.keys);
	return ret;
}

/* --- reading and writing logical blocks --- */

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

static int ftl_encrypt(struct skcipher_request *req, const void *src, void *dst,
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
	return crypto_wait_req(crypto_skcipher_encrypt(req), &wait);
}

/* Copy n bytes at offset off of logical block lba. Caller holds f->lock. */
static int ftl_copy(struct ftl *f, struct ftl_disk *d, u32 lba,
		    unsigned int off, void *dst, unsigned int n)
{
	u32 phys, page, i;
	u8 *src;
	int ret;

	for (i = f->wbuf_count; i > 0; i--) {
		if (f->wbuf_lbas[i - 1] == lba) {
			src = f->wbuf_data + (i - 1) * FTL_LP_SIZE;
			goto decrypt;
		}
	}

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
decrypt:
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

static void ftl_fill_meta(u8 *m, u8 type, u8 flags, u16 seq, u32 gen,
			  u32 lba, u32 word3)
{
	m[0] = type;
	m[1] = flags;
	put_unaligned_le16(seq, m + 2);
	put_unaligned_le32(gen, m + 4);
	put_unaligned_le32(lba, m + 8);
	put_unaligned_le32(word3, m + 12);
}

static int ftl_close_wsb(struct ftl *f)
{
	u32 toc_pages = FTL_TOC_WRITE_PAGES;
	u32 toc_lps = toc_pages * f->lps;
	u32 data_lps = f->wsb_total - toc_lps;
	u32 i = 0, run = 0, page, lp;
	u8 *toc;
	int ret = 0;

	if (!f->wsb_block)
		return 0;

	/* Pad any remaining data pages up to data_lps so the TOC sits at the end. */
	memset(f->wbuf_data, 0xff, f->mtd->writesize);
	while (f->wsb_idx < data_lps) {
		for (lp = 0; lp < f->lps; lp++) {
			ftl_fill_meta(f->wbuf_oob + lp * FTL_META, FTL_T_PAD, 0,
				      f->next_seq++, f->wsb_gen, FTL_UNMAPPED,
				      f->wsb_gen);
			f->wsb_lbas[f->wsb_idx + lp] = FTL_UNMAPPED;
		}
		ftl_wsb_pos(f, f->wsb_block, f->wsb_idx, &page, &lp);
		ret = ftl_write_page(f, page, f->wbuf_data, f->wbuf_oob);
		if (ret)
			return ret;
		f->wsb_idx += f->lps;
	}

	toc = kvzalloc(toc_pages * f->mtd->writesize, GFP_NOIO);
	if (!toc)
		return -ENOMEM;

	while (i < data_lps && (run + 1) * 16 <= toc_pages * f->mtd->writesize) {
		u32 lba = f->wsb_lbas[i];
		u32 cnt = 1;
		u8 *r = toc + run * 16;

		if (lba < f->nlba) {
			while (i + cnt < data_lps &&
			       f->wsb_lbas[i + cnt] == lba + cnt)
				cnt++;
		} else {
			lba = f->nlba;
		}
		put_unaligned_le32(i, r);
		put_unaligned_le32(f->wsb_gen, r + 4);
		put_unaligned_le32(lba, r + 8);
		put_unaligned_le32(cnt, r + 12);
		i += cnt;
		run++;
	}

	for (i = 0; i < toc_pages; i++) {
		for (lp = 0; lp < f->lps; lp++)
			ftl_fill_meta(f->wbuf_oob + lp * FTL_META, FTL_T_TOC,
				      FTL_F_TOC_STD, f->wsb_first_seq,
				      f->wsb_gen, f->wsb_total,
				      0xffff0000U | toc_lps);
		ftl_wsb_pos(f, f->wsb_block, data_lps + i * f->lps, &page, &lp);
		ret = ftl_write_page(f, page, toc + i * f->mtd->writesize,
				     f->wbuf_oob);
		if (ret)
			break;
	}
	kvfree(toc);
	if (!ret) {
		f->sbs[f->wsb_block].state = FTL_SB_TOC;
		f->wsb_block = 0;
		f->wsb_idx = 0;
	}
	return ret;
}

static int ftl_commit_wbuf(struct ftl *f)
{
	u32 page, lp, i;
	int ret;

	if (!f->wbuf_count)
		return 0;
	for (i = f->wbuf_count; i < f->lps; i++) {
		memset(f->wbuf_data + i * FTL_LP_SIZE, 0xff, FTL_LP_SIZE);
		ftl_fill_meta(f->wbuf_oob + i * FTL_META, FTL_T_PAD, 0,
			      f->next_seq++, f->wsb_gen, FTL_UNMAPPED,
			      f->wsb_gen);
		f->wbuf_lbas[i] = FTL_UNMAPPED;
	}

	ftl_wsb_pos(f, f->wsb_block, f->wsb_idx, &page, &lp);
	ret = ftl_write_page(f, page, f->wbuf_data, f->wbuf_oob);
	if (ret)
		return ret;

	for (i = 0; i < f->lps; i++) {
		u32 lba = f->wbuf_lbas[i];

		f->wsb_lbas[f->wsb_idx + i] = lba;
		if (lba < f->nlba)
			ftl_map_update(f, lba, page * f->lps + i);
	}
	f->wsb_idx += f->lps;
	f->wbuf_count = 0;
	if (!f->next_seq)
		f->wsb_gen = f->next_gen++;

	if (f->wsb_idx >= f->wsb_total - FTL_TOC_WRITE_PAGES * f->lps)
		return ftl_close_wsb(f);
	return 0;
}

/* Flush any staged writes to NAND; ensure page 0 of every CAU in wsb is written. */
static int ftl_flush_wbuf(struct ftl *f)
{
	int ret;

	if (f->wbuf_count) {
		ret = ftl_commit_wbuf(f);
		if (ret)
			return ret;
	}
	while (f->wsb_block && f->wsb_idx > 0 &&
	       f->wsb_idx < f->caus * f->lps) {
		f->wbuf_count = 0;
		memset(f->wbuf_data, 0xff, f->mtd->writesize);
		for (u32 i = 0; i < f->lps; i++) {
			ftl_fill_meta(f->wbuf_oob + i * FTL_META, FTL_T_PAD, 0,
				      f->next_seq++, f->wsb_gen, FTL_UNMAPPED,
				      f->wsb_gen);
			f->wbuf_lbas[i] = FTL_UNMAPPED;
		}
		f->wbuf_count = f->lps;
		ret = ftl_commit_wbuf(f);
		if (ret)
			return ret;
	}
	return 0;
}

static int ftl_open_free_sb(struct ftl *f)
{
	u32 b;

	for (b = 1; b < f->data_blocks; b++) {
		if (f->sbs[b].state == FTL_SB_FREE) {
			f->sbs[b].state = FTL_SB_OPEN;
			f->sbs[b].valid_lps = 0;
			f->free_sbs--;
			f->wsb_block = b;
			f->wsb_gen = f->next_gen++;
			f->next_seq = 0;
			f->wsb_first_seq = 0;
			f->wsb_idx = 0;
			f->wsb_total = f->pages * f->lps * f->caus;
			return 0;
		}
	}
	return -ENOSPC;
}

static int ftl_stage_raw(struct ftl *f, u32 lba, const void *raw_lp);

static int ftl_gc(struct ftl *f)
{
	u32 b, victim = 0, best_valid = U32_MAX, lba;
	u8 *buf;
	int ret;

	for (b = 1; b < f->data_blocks; b++) {
		if (f->sbs[b].state == FTL_SB_TOC &&
		    f->sbs[b].valid_lps < best_valid) {
			victim = b;
			best_valid = f->sbs[b].valid_lps;
			if (!best_valid)
				break;
		}
	}
	if (!victim)
		return -ENOSPC;
	if (!best_valid)
		return ftl_erase_sb(f, victim);

	if (!f->wsb_block) {
		ret = ftl_open_free_sb(f);
		if (ret)
			return ret;
	}

	buf = kmalloc(FTL_LP_SIZE, GFP_NOIO);
	if (!buf)
		return -ENOMEM;

	for (lba = 0; lba < f->nlba && f->sbs[victim].valid_lps > 0; lba++) {
		u32 phys = f->l2p[lba];

		if (phys == FTL_UNMAPPED)
			continue;
		if ((((phys / f->lps) / f->pages) % f->blocks) != victim)
			continue;
		ret = ftl_copy(f, NULL, lba, 0, buf, FTL_LP_SIZE);
		if (ret)
			goto out;
		ret = ftl_stage_raw(f, lba, buf);
		if (ret)
			goto out;
	}
	ret = ftl_flush_wbuf(f);
	if (!ret)
		ret = ftl_erase_sb(f, victim);
out:
	kfree(buf);
	return ret;
}

static int ftl_stage_raw(struct ftl *f, u32 lba, const void *raw_lp)
{
	u32 i, slot;
	u16 seq;
	int ret;

	for (i = 0; i < f->wbuf_count; i++) {
		if (f->wbuf_lbas[i] == lba) {
			memcpy(f->wbuf_data + i * FTL_LP_SIZE, raw_lp, FTL_LP_SIZE);
			return 0;
		}
	}

	if (!f->wsb_block) {
		if (f->free_sbs <= FTL_GC_LOW_SB) {
			ret = ftl_gc(f);
			if (ret && !f->free_sbs)
				return ret;
		}
		if (!f->wsb_block) {
			ret = ftl_open_free_sb(f);
			if (ret)
				return ret;
		}
	}

	slot = f->wbuf_count++;
	seq = f->next_seq++;
	if (!f->wsb_idx && !slot)
		f->wsb_first_seq = seq;
	memcpy(f->wbuf_data + slot * FTL_LP_SIZE, raw_lp, FTL_LP_SIZE);
	ftl_fill_meta(f->wbuf_oob + slot * FTL_META, FTL_T_DATA, 0, seq,
		      f->wsb_gen, lba, f->wsb_gen);
	f->wbuf_lbas[slot] = lba;

	if (f->wbuf_count == f->lps)
		return ftl_commit_wbuf(f);
	return 0;
}

/* Write n bytes at offset off of logical block lba. Caller holds f->lock. */
static int ftl_write(struct ftl *f, struct ftl_disk *d, u32 lba,
		     unsigned int off, const void *src, unsigned int n)
{
	const void *raw = src;
	int ret;

	if (lba >= f->nlba)
		return -EIO;
	if (off != 0 || n != FTL_LP_SIZE) {
		ret = ftl_copy(f, d, lba, 0, f->dec, FTL_LP_SIZE);
		if (ret)
			return ret;
		memcpy(f->dec + off, src, n);
		raw = f->dec;
	}
	if (d && d->req) {
		__le32 iv[4];

		ftl_block_iv(lba, iv);
		ret = ftl_encrypt(d->req, raw, f->dec, FTL_LP_SIZE, iv);
		if (ret)
			return ret;
		raw = f->dec;
	}
	return ftl_stage_raw(f, lba, raw);
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
	enum req_op op = req_op(rq);
	struct req_iterator iter;
	blk_status_t status = BLK_STS_OK;
	struct bio_vec bv;

	blk_mq_start_request(rq);
	if (op == REQ_OP_FLUSH) {
		mutex_lock(&f->lock);
		if (ftl_flush_wbuf(f))
			status = BLK_STS_IOERR;
		mutex_unlock(&f->lock);
		goto out;
	}
	if (op != REQ_OP_READ && op != REQ_OP_WRITE) {
		status = BLK_STS_NOTSUPP;
		goto out;
	}
	if (op == REQ_OP_WRITE && d->ro) {
		status = BLK_STS_IOERR;
		goto out;
	}

	mutex_lock(&f->lock);
	rq_for_each_segment(bv, rq, iter) {
		u8 *buf = bvec_kmap_local(&bv);
		unsigned int done = 0;

		while (done < bv.bv_len && status == BLK_STS_OK) {
			u64 at = ftl_disk_offset(d, pos);
			unsigned int off = at & (FTL_LP_SIZE - 1);
			unsigned int n = min_t(unsigned int, bv.bv_len - done, FTL_LP_SIZE - off);
			int err;

			if (pos + n > d->size) {
				status = BLK_STS_IOERR;
				break;
			}
			if (op == REQ_OP_WRITE)
				err = ftl_write(f, d, at >> FTL_LP_SHIFT, off, buf + done, n);
			else
				err = ftl_copy(f, d, at >> FTL_LP_SHIFT, off, buf + done, n);
			if (err)
				status = BLK_STS_IOERR;
			done += n;
			pos += n;
		}
		kunmap_local(buf);
		if (status != BLK_STS_OK)
			break;
	}
	if (status == BLK_STS_OK && op == REQ_OP_WRITE && (rq->cmd_flags & REQ_FUA)) {
		if (ftl_flush_wbuf(f))
			status = BLK_STS_IOERR;
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

static ssize_t serial_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ftl_disk *d = dev_to_disk(dev)->private_data;

	return sysfs_emit(buf, "%s\n", d->ftl->serial);
}
static DEVICE_ATTR_RO(serial);

static ssize_t model_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ftl_disk *d = dev_to_disk(dev)->private_data;

	return sysfs_emit(buf, "%s\n", d->ftl->model);
}
static DEVICE_ATTR_RO(model);

static ssize_t emac_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ftl_disk *d = dev_to_disk(dev)->private_data;

	if (!is_valid_ether_addr(d->ftl->emac))
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%pM\n", d->ftl->emac);
}
static DEVICE_ATTR_RO(emac);

static ssize_t wmac_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ftl_disk *d = dev_to_disk(dev)->private_data;

	if (!is_valid_ether_addr(d->ftl->wmac))
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%pM\n", d->ftl->wmac);
}
static DEVICE_ATTR_RO(wmac);

static ssize_t bmac_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ftl_disk *d = dev_to_disk(dev)->private_data;

	if (!is_valid_ether_addr(d->ftl->bmac))
		return sysfs_emit(buf, "\n");
	return sysfs_emit(buf, "%pM\n", d->ftl->bmac);
}
static DEVICE_ATTR_RO(bmac);

/*
 * System keybag (/sys/block/nand0/keybag)
 *
 * The keys of protection classes 1-3 sit in the system keybag file on the
 * Data volume, a binary property list holding an IV and a payload that is
 * AES-256-CBC encrypted with the last 32 bytes of the key store's bag
 * locker. The payload is a property list whose "KeyBagKeys" data is a
 * tag/length/value stream ("DATA", then "SIGN"); inside "DATA" each class
 * entry starts with a "UUID" tag and carries "CLAS", "WRAP" and "WPKY".
 * A class key is unwrapped in the order it was wrapped: with WRAP bit 1
 * (passcode) by RFC 3394 under the passcode key, then with WRAP bit 0
 * (device) by AES-CBC under the device-derived key. This box has no
 * passcode, so the passcode key is derived from an empty one; a class whose
 * key fails the RFC 3394 integrity check stays locked. Userspace writes the
 * file in; reading lists each class as unlocked or locked and how many
 * files have used its key.
 */
#define KB_TAG(a, b, c, d)	((a) << 24 | (b) << 16 | (c) << 8 | (d))
#define KB_DATA			KB_TAG('D', 'A', 'T', 'A')
#define KB_UUID			KB_TAG('U', 'U', 'I', 'D')
#define KB_CLAS			KB_TAG('C', 'L', 'A', 'S')
#define KB_WRAP			KB_TAG('W', 'R', 'A', 'P')
#define KB_WPKY			KB_TAG('W', 'P', 'K', 'Y')
#define KB_WRAP_DEVICE		1
#define KB_MAX_SIZE		SZ_16K

struct ftl_bplist {
	const u8 *buf;
	size_t len;
	u8 off_size, ref_size;
	u64 nobjs, table;
};

static u64 ftl_be(const u8 *p, unsigned int n)
{
	u64 v = 0;

	while (n--)
		v = v << 8 | *p++;
	return v;
}

/* Offset of object @idx, or 0 when it is out of range. */
static size_t ftl_bplist_obj(const struct ftl_bplist *bp, u64 idx)
{
	u64 at = bp->table + idx * bp->off_size, off;

	if (idx >= bp->nobjs || at + bp->off_size > bp->len - 32)
		return 0;
	off = ftl_be(bp->buf + at, bp->off_size);
	return off >= 8 && off < bp->len - 32 ? off : 0;
}

/* Marker type of the object at @off and its payload; 0 when malformed. */
static u8 ftl_bplist_item(const struct ftl_bplist *bp, size_t off,
			  const u8 **data, u64 *count)
{
	u8 m = bp->buf[off], type = m >> 4;
	size_t p = off + 1;

	*count = m & 0xf;
	if (*count == 0xf) {
		u8 im = bp->buf[p], n;

		if (p >= bp->len - 32 || im >> 4 != 1 || (im & 0xf) > 3)
			return 0;
		n = 1 << (im & 0xf);
		if (p + 1 + n > bp->len - 32)
			return 0;
		*count = ftl_be(bp->buf + p + 1, n);
		p += 1 + n;
	}
	*data = bp->buf + p;
	if (*count > bp->len - 32 - p)
		return 0;
	return type;
}

/*
 * Find the data value stored under @key in the top-level dictionary of a
 * binary property list.
 */
static const u8 *ftl_bplist_data(const u8 *buf, size_t len, const char *key,
				 u64 *dlen)
{
	struct ftl_bplist bp = { .buf = buf, .len = len };
	const u8 *t = buf + len - 32, *refs, *k, *v;
	size_t klen = strlen(key), off;
	u64 n, i, cnt;

	if (len < 8 + 32 || memcmp(buf, "bplist00", 8))
		return NULL;
	bp.off_size = t[6];
	bp.ref_size = t[7];
	bp.nobjs = ftl_be(t + 8, 8);
	bp.table = ftl_be(t + 24, 8);
	if (!bp.off_size || bp.off_size > 8 || !bp.ref_size || bp.ref_size > 8 ||
	    bp.table >= len - 32)
		return NULL;

	off = ftl_bplist_obj(&bp, ftl_be(t + 16, 8));
	if (!off || ftl_bplist_item(&bp, off, &refs, &n) != 0xd ||
	    2 * n * bp.ref_size > len - 32 - (refs - buf))
		return NULL;
	for (i = 0; i < n; i++) {
		off = ftl_bplist_obj(&bp, ftl_be(refs + i * bp.ref_size, bp.ref_size));
		if (!off || ftl_bplist_item(&bp, off, &k, &cnt) != 0x5 ||
		    cnt != klen || memcmp(k, key, klen))
			continue;
		off = ftl_bplist_obj(&bp, ftl_be(refs + (n + i) * bp.ref_size,
						 bp.ref_size));
		if (!off || ftl_bplist_item(&bp, off, &v, &cnt) != 0x4)
			return NULL;
		*dlen = cnt;
		return v;
	}
	return NULL;
}

static int ftl_rfc3394_unwrap(const u8 *kek, unsigned int kek_len,
			      const u8 in[CP_WRAP_LEN], u8 out[AES_KEYSIZE_256]);

#define KB_VERS			KB_TAG('V', 'E', 'R', 'S')
#define KB_TYPE			KB_TAG('T', 'Y', 'P', 'E')
#define KB_SALT			KB_TAG('S', 'A', 'L', 'T')
#define KB_ITER			KB_TAG('I', 'T', 'E', 'R')
#define KB_WRAP_PASSCODE	2
#define KB_TYPE_SEP_KEY		BIT(30)
#define KB_MAX_CLASSES		16
#define KB_MAX_ITER		10000000
#define KB_TANGLE_SIZE		SZ_4K

struct ftl_kb_class {
	u32 class, wrap, wpky_len;
	const u8 *wpky;
};

struct ftl_kb {
	u32 vers, type, iter, salt_len, nclasses;
	const u8 *salt;
	struct ftl_kb_class c[KB_MAX_CLASSES];
};

/* Split a decrypted "DATA" stream into the bag header and its class entries. */
static int ftl_keybag_parse(const u8 *p, u32 len, struct ftl_kb *kb)
{
	struct ftl_kb_class *c = NULL;
	bool first = true;
	u32 off = 0;

	memset(kb, 0, sizeof(*kb));
	while (off + 8 <= len) {
		u32 tag = get_unaligned_be32(p + off);
		u32 n = get_unaligned_be32(p + off + 4);
		const u8 *v = p + off + 8;

		if (n > len - off - 8)
			return -EBADMSG;
		if (tag == KB_UUID) {
			/* the first UUID names the bag, the rest start class entries */
			if (!first) {
				if (kb->nclasses == KB_MAX_CLASSES)
					break;
				c = &kb->c[kb->nclasses++];
			}
			first = false;
		} else if (c) {
			if (tag == KB_CLAS && n == 4)
				c->class = get_unaligned_be32(v);
			else if (tag == KB_WRAP && n == 4)
				c->wrap = get_unaligned_be32(v);
			else if (tag == KB_WPKY) {
				c->wpky = v;
				c->wpky_len = n;
			}
		} else if (n == 4 && tag == KB_VERS) {
			kb->vers = get_unaligned_be32(v);
		} else if (n == 4 && tag == KB_TYPE) {
			kb->type = get_unaligned_be32(v);
		} else if (n == 4 && tag == KB_ITER) {
			kb->iter = get_unaligned_be32(v);
		} else if (tag == KB_SALT) {
			kb->salt = v;
			kb->salt_len = n;
		}
		off += 8 + n;
	}
	return 0;
}

/*
 * Passcode key of a bag with an empty passcode: PBKDF2-HMAC-SHA1 over the
 * bag salt with one iteration, then tangled with the hardware device key.
 * Each round fills 4 KiB with the key, every 32-byte copy XORed word by word
 * with a running counter, encrypts it with AES-CBC under the device key (the
 * IV chaining across rounds) and XORs up to 128 output blocks (one per
 * remaining iteration) back into the key. Bag version 1 restarts the counter
 * at 1 every round.
 */
static int ftl_keybag_passcode_key(const struct ftl_kb *kb, u8 key[AES_KEYSIZE_256])
{
	u8 dummy = 0, iv[AES_BLOCK_SIZE] = {}, hmac[SHA1_DIGEST_SIZE], base[AES_KEYSIZE_256];
	const unsigned int per_round = KB_TANGLE_SIZE / AES_KEYSIZE_256;
	struct crypto_shash *tfm;
	u8 *in = NULL, *out = NULL, *msg = NULL;
	u32 counter = 1, left = kb->iter, i, j, n;
	int ret;

	if (kb->type & KB_TYPE_SEP_KEY)
		return -EOPNOTSUPP;
	if (!kb->salt || !kb->iter || kb->iter > KB_MAX_ITER || kb->salt_len > 64)
		return -EINVAL;

	tfm = crypto_alloc_shash("hmac(sha1)", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	msg = kmalloc(kb->salt_len + 4, GFP_KERNEL);
	in = kmalloc(KB_TANGLE_SIZE, GFP_KERNEL);
	out = kmalloc(KB_TANGLE_SIZE, GFP_KERNEL);
	ret = -ENOMEM;
	if (!msg || !in || !out)
		goto out;

	/* PBKDF2 with one iteration: T(i) = HMAC(passcode, salt || be32(i)) */
	ret = crypto_shash_setkey(tfm, &dummy, 0);
	if (ret)
		goto out;
	memcpy(msg, kb->salt, kb->salt_len);
	for (i = 0; i * SHA1_DIGEST_SIZE < AES_KEYSIZE_256; i++) {
		put_unaligned_be32(i + 1, msg + kb->salt_len);
		ret = crypto_shash_tfm_digest(tfm, msg, kb->salt_len + 4, hmac);
		if (ret)
			goto out;
		memcpy(key + i * SHA1_DIGEST_SIZE, hmac,
		       min_t(u32, SHA1_DIGEST_SIZE, AES_KEYSIZE_256 - i * SHA1_DIGEST_SIZE));
	}

	/* version 1 refills from the evolving key; later ones from the PBKDF2 output */
	memcpy(base, key, sizeof(base));
	while (left) {
		const u8 *src = kb->vers == 1 ? key : base;

		if (kb->vers == 1)
			counter = 1;
		for (i = 0; i < per_round; i++, counter++)
			for (j = 0; j < AES_KEYSIZE_256; j += 4)
				put_unaligned_le32(get_unaligned_le32(src + j) ^ counter,
						   in + i * AES_KEYSIZE_256 + j);
		ret = s5l8947x_aes_hwkey_crypt(S5L8947X_AES_KEY_DEVICE, true, iv,
					       in, out, KB_TANGLE_SIZE);
		if (ret)
			goto out;
		memcpy(iv, out + KB_TANGLE_SIZE - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
		n = min(left, per_round);
		for (i = 0; i < n; i++)
			crypto_xor(key, out + i * AES_KEYSIZE_256, AES_KEYSIZE_256);
		left -= n;
		cond_resched();
	}
	ret = 0;
out:
	if (ret)
		memzero_explicit(key, AES_KEYSIZE_256);
	memzero_explicit(hmac, sizeof(hmac));
	memzero_explicit(base, sizeof(base));
	kfree_sensitive(in);
	kfree_sensitive(out);
	kfree_sensitive(msg);
	crypto_free_shash(tfm);
	return ret;
}

/* Unwrap the class keys found in a decrypted "DATA" stream. */
static int ftl_keybag_classes(const u8 *p, u32 len, const u8 dev_key[16])
{
	struct crypto_skcipher *tfm = NULL;
	struct skcipher_request *req = NULL;
	u8 pkey[AES_KEYSIZE_256];
	bool have_pkey = false;
	struct ftl_kb *kb;
	int unlocked = 0, ret;
	u8 *key = NULL;
	u32 i;

	kb = kmalloc(sizeof(*kb), GFP_KERNEL);
	key = kmalloc(AES_KEYSIZE_256 + 8, GFP_KERNEL);
	if (!kb || !key) {
		unlocked = -ENOMEM;
		goto out;
	}
	ret = ftl_keybag_parse(p, len, kb);
	if (ret) {
		unlocked = ret;
		goto out;
	}
	pr_info("s5l8947x-ftl: keybag version %u, type %#x, %u iterations, %u classes\n",
		kb->vers, kb->type, kb->iter, kb->nclasses);

	tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		unlocked = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	if (!req || crypto_skcipher_setkey(tfm, dev_key, 16)) {
		unlocked = -ENOMEM;
		goto out;
	}

	for (i = 0; i < kb->nclasses; i++) {
		const struct ftl_kb_class *c = &kb->c[i];
		u8 iv[AES_BLOCK_SIZE] = {};
		u32 klen = c->wpky_len;
		const char *why = NULL;

		if (c->class < CP_CLASS_FIRST || c->class >= CP_CLASS_OPEN)
			continue;
		if (test_bit(c->class, &ftl_class_valid))
			continue;
		if (!c->wpky || !(c->wrap & (KB_WRAP_DEVICE | KB_WRAP_PASSCODE)) ||
		    c->wrap & ~(KB_WRAP_DEVICE | KB_WRAP_PASSCODE)) {
			why = "unsupported wrap";
			goto report;
		}
		if (c->wrap & KB_WRAP_PASSCODE) {
			if (klen != CP_WRAP_LEN) {
				why = "bad wrapped key";
				goto report;
			}
			if (!have_pkey) {
				ret = ftl_keybag_passcode_key(kb, pkey);
				if (ret) {
					why = "no passcode key";
					goto report;
				}
				have_pkey = true;
			}
			/* fails its integrity check unless the passcode is empty */
			if (ftl_rfc3394_unwrap(pkey, sizeof(pkey), c->wpky, key)) {
				why = "passcode set";
				goto report;
			}
			klen = AES_KEYSIZE_256;
		} else {
			if (klen != AES_KEYSIZE_256) {
				why = "bad wrapped key";
				goto report;
			}
			memcpy(key, c->wpky, klen);
		}
		if ((c->wrap & KB_WRAP_DEVICE) &&
		    ftl_decrypt(req, key, key, AES_KEYSIZE_256, iv)) {
			why = "device unwrap failed";
			goto report;
		}
		memcpy(ftl_class_key[c->class], key, AES_KEYSIZE_256);
		smp_wmb();
		set_bit(c->class, &ftl_class_valid);
		unlocked++;
report:
		pr_info("s5l8947x-ftl: keybag class %u wrap %u: %s%s%s\n", c->class, c->wrap,
			why ? "locked (" : "unlocked", why ?: "", why ? ")" : "");
		memzero_explicit(key, AES_KEYSIZE_256 + 8);
	}
out:
	memzero_explicit(pkey, sizeof(pkey));
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	kfree_sensitive(key);
	kfree(kb);
	return unlocked;
}

static int ftl_keybag_load(const u8 *file, size_t len)
{
	struct crypto_skcipher *tfm = NULL;
	struct skcipher_request *req = NULL;
	u8 iv[AES_BLOCK_SIZE], dev_key[16];
	const u8 *v, *keys;
	u64 vlen, plen, klen;
	u8 *plain = NULL;
	u32 off, n;
	int ret;

	if (!ftl_bag_valid)
		return -ENOKEY;
	v = ftl_bplist_data(file, len, "_MKBIV", &vlen);
	if (!v || vlen != AES_BLOCK_SIZE)
		return -EINVAL;
	memcpy(iv, v, sizeof(iv));
	v = ftl_bplist_data(file, len, "_MKBPAYLOAD", &plen);
	if (!v || !plen || plen % AES_BLOCK_SIZE)
		return -EINVAL;

	plain = kmalloc(plen, GFP_KERNEL);
	tfm = crypto_alloc_skcipher("cbc(aes)", 0, 0);
	if (IS_ERR(tfm)) {
		ret = PTR_ERR(tfm);
		tfm = NULL;
		goto out;
	}
	req = skcipher_request_alloc(tfm, GFP_KERNEL);
	ret = -ENOMEM;
	if (!plain || !req)
		goto out;
	ret = crypto_skcipher_setkey(tfm, ftl_bag_key, sizeof(ftl_bag_key)) ?:
	      ftl_decrypt(req, v, plain, plen, iv);
	if (ret)
		goto out;

	/* strip the padding, then find the class key stream */
	n = plain[plen - 1];
	ret = -EBADMSG;
	if (!n || n > AES_BLOCK_SIZE)
		goto out;
	keys = ftl_bplist_data(plain, plen - n, "KeyBagKeys", &klen);
	if (!keys || klen < 8 || get_unaligned_be32(keys) != KB_DATA)
		goto out;
	off = get_unaligned_be32(keys + 4);
	if (off > klen - 8)
		goto out;

	ret = s5l8947x_aes_hwkey_crypt(S5L8947X_AES_KEY_DEVICE, true, NULL,
				       ftl_fkey_seed, dev_key, sizeof(dev_key));
	if (!ret)
		ret = ftl_keybag_classes(keys + 8, off, dev_key);
	memzero_explicit(dev_key, sizeof(dev_key));
out:
	skcipher_request_free(req);
	crypto_free_skcipher(tfm);
	kfree_sensitive(plain);
	return ret;
}

static ssize_t keybag_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int class;
	int n = 0;

	for (class = CP_CLASS_FIRST; class <= CP_CLASS_OPEN; class++) {
		bool open = class == CP_CLASS_OPEN ? ftl_file_master_valid :
			    test_bit(class, &ftl_class_valid);

		n += sysfs_emit_at(buf, n, "class %u %s, %d files\n", class,
				   open ? "unlocked" : "locked",
				   atomic_read(&ftl_class_files[class]));
	}
	return n;
}

static ssize_t keybag_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int ret;

	if (!count || count > KB_MAX_SIZE)
		return -EINVAL;
	mutex_lock(&ftl_keybag_lock);
	ret = ftl_keybag_load(buf, count);
	mutex_unlock(&ftl_keybag_lock);
	return ret < 0 ? ret : count;
}
static DEVICE_ATTR_RW(keybag);

static struct attribute *ftl_attrs[] = {
	&dev_attr_serial.attr,
	&dev_attr_model.attr,
	&dev_attr_emac.attr,
	&dev_attr_wmac.attr,
	&dev_attr_bmac.attr,
	&dev_attr_keybag.attr,
	NULL,
};

static const struct attribute_group ftl_attr_group = {
	.attrs = ftl_attrs,
};

static const struct attribute_group *ftl_attr_groups[] = {
	&ftl_attr_group,
	NULL,
};

static int ftl_add_disk(struct ftl *f, struct ftl_disk *d, const char *name, bool ro)
{
	struct queue_limits lim = {
		.logical_block_size = FTL_LP_SIZE,
		.physical_block_size = FTL_LP_SIZE,
		.max_hw_sectors = 256,
		.features = ro ? 0 : (BLK_FEAT_WRITE_CACHE | BLK_FEAT_FUA),
	};
	int ret;

	d->ftl = f;
	d->ro = ro;
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
	set_disk_ro(d->disk, ro);

	ret = device_add_disk(&f->mtd->dev, d->disk,
			      d == &f->whole ? ftl_attr_groups : NULL);
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

/* --- system configuration, key store and volume map --- */

static void ftl_set_dt_mac(const char *compat, const char *propname,
			   const u8 *mac, bool reverse)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, compat);
	struct property *prop;
	u8 *dst;
	int i;

	if (!np)
		return;
	/*
	 * Unflattened properties point into the read-only flattened tree, so
	 * replace the property instead of writing its value in place, adding
	 * it when the tree has none. A replaced property moves to the node's
	 * dead list, so neither is freed.
	 */
	prop = kzalloc(sizeof(*prop) + ETH_ALEN, GFP_KERNEL);
	if (!prop)
		goto out;
	prop->name = kstrdup(propname, GFP_KERNEL);
	if (!prop->name) {
		kfree(prop);
		goto out;
	}
	dst = (u8 *)(prop + 1);
	if (reverse) {
		for (i = 0; i < ETH_ALEN; i++)
			dst[i] = mac[ETH_ALEN - 1 - i];
	} else {
		ether_addr_copy(dst, mac);
	}
	prop->value = dst;
	prop->length = ETH_ALEN;
	if (of_update_property(np, prop)) {
		kfree(prop->name);
		kfree(prop);
	}
out:
	of_node_put(np);
}

static void ftl_copy_ascii(char *dst, size_t dst_size, const u8 *src, u32 len)
{
	size_t i, max = min_t(size_t, len, dst_size - 1);

	for (i = 0; i < max && src[i] >= 0x20 && src[i] < 0x7f; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static void ftl_read_syscfg(struct ftl *f)
{
	u32 start = f->blocks > KS_RESERVED_BLOCKS ? f->blocks - KS_RESERVED_BLOCKS : 0;
	u32 scfg_max = f->mtd->writesize - SCFG_HDR_LEN;
	u8 *buf, *best = NULL;
	u32 cau, blk, best_ver = 0;
	u32 count, i;

	if (f->mtd->writesize <= SCFG_HDR_LEN + SCFG_ENTRIES)
		return;
	buf = kmalloc(f->mtd->writesize, GFP_KERNEL);
	best = kmalloc(scfg_max, GFP_KERNEL);
	if (!buf || !best)
		goto out;

	for (cau = 0; cau < f->caus; cau++) {
		for (blk = start; blk < f->blocks; blk++) {
			u32 ver;

			if (ftl_read_slc(f, ftl_page(f, cau, blk, 0), buf,
					 f->mtd->writesize) ||
			    get_unaligned_le32(buf + SCFG_HDR_VERSION) != 1 ||
			    get_unaligned_le32(buf + SCFG_HDR_LEN) != SCFG_MAGIC)
				continue;
			ver = get_unaligned_le32(buf + SCFG_HDR_COPY_VER);
			if (!best_ver || ver >= best_ver) {
				memcpy(best, buf + SCFG_HDR_LEN, scfg_max);
				best_ver = ver ? ver : 1;
			}
		}
	}
	if (!best_ver)
		goto out;

	count = get_unaligned_le32(best + SCFG_COUNT);
	count = min_t(u32, count, (scfg_max - SCFG_ENTRIES) / SCFG_ENTRY_SIZE);
	for (i = 0; i < count; i++) {
		const u8 *e = best + SCFG_ENTRIES + i * SCFG_ENTRY_SIZE;
		u32 tag = get_unaligned_le32(e);
		const u8 *val = e + 4;
		u32 vlen = 16;

		if (tag == SCFG_TAG_CNTB) {
			u32 voff = get_unaligned_le32(e + 12);

			tag = get_unaligned_le32(e + 4);
			vlen = get_unaligned_le32(e + 8);
			if (voff >= scfg_max || vlen > scfg_max - voff)
				continue;
			val = best + voff;
		}
		switch (tag) {
		case SCFG_TAG_SRNM:
			ftl_copy_ascii(f->serial, sizeof(f->serial), val, vlen);
			break;
		case SCFG_TAG_MODN:
			ftl_copy_ascii(f->model, sizeof(f->model), val, vlen);
			break;
		case SCFG_TAG_EMAC:
			if (vlen >= ETH_ALEN && is_valid_ether_addr(val))
				ether_addr_copy(f->emac, val);
			break;
		case SCFG_TAG_WMAC:
			if (vlen >= ETH_ALEN && is_valid_ether_addr(val))
				ether_addr_copy(f->wmac, val);
			break;
		case SCFG_TAG_BMAC:
			if (vlen >= ETH_ALEN && is_valid_ether_addr(val))
				ether_addr_copy(f->bmac, val);
			break;
		}
	}

	if (is_valid_ether_addr(f->emac))
		ftl_set_dt_mac("apple,s5l8947x-ethernet", "local-mac-address",
			       f->emac, false);
	if (is_valid_ether_addr(f->wmac))
		ftl_set_dt_mac("usba5c,bd1a", "local-mac-address",
			       f->wmac, false);
	if (is_valid_ether_addr(f->bmac))
		ftl_set_dt_mac("brcm,bcm4334-bt", "local-bd-address",
			       f->bmac, true);

	dev_info(&f->mtd->dev,
		 "system config: serial %s, model %s, emac %pM, wmac %pM, bmac %pM\n",
		 f->serial[0] ? f->serial : "-",
		 f->model[0] ? f->model : "-",
		 f->emac, f->wmac, f->bmac);
out:
	kfree(best);
	kfree(buf);
}

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
		} else if (tag == KS_L_BAG && len >= AES_KEYSIZE_256) {
			/* the system keybag's payload key ends the locker */
			memcpy(ftl_bag_key, store + off + 8 + len - AES_KEYSIZE_256,
			       AES_KEYSIZE_256);
			ftl_bag_valid = true;
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
	const u8 *p = attr, *kek;
	u32 class;
	int i, ret;

	if (len < CP_V4_HEADER_LEN + CP_WRAP_LEN)
		return -EINVAL;
	class = get_unaligned_le32(p + 8) & 0x1f;
	if (get_unaligned_le16(p) != CP_V4_VERSION ||
	    class < CP_CLASS_FIRST || class > CP_CLASS_OPEN ||
	    get_unaligned_le32(p + 12) != CP_WRAP_LEN)
		return -EOPNOTSUPP;
	if (class == CP_CLASS_OPEN) {
		if (!ftl_file_master_valid)
			return -ENOKEY;
		kek = ftl_file_master_key;
	} else {
		/* class keys are set once by the keybag load and never change */
		if (!test_bit(class, &ftl_class_valid))
			return -ENOKEY;
		smp_rmb();
		kek = ftl_class_key[class];
	}
	ret = ftl_rfc3394_unwrap(kek, AES_KEYSIZE_256, p + CP_V4_HEADER_LEN, fkey);
	if (ret)
		return ret;
	atomic_inc(&ftl_class_files[class]);

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

int s5l8947x_file_block_encrypt(struct block_device *bdev, u64 blocknr,
				const u8 fkey[32], const u8 iv_key[16],
				u64 file_off, const void *src)
{
	struct ftl_disk *d = bdev ? bdev->bd_disk->private_data : NULL;
	struct crypto_aes_ctx aes;
	u8 iv[AES_BLOCK_SIZE];
	__le32 lfsr[4];
	struct ftl *f;
	u64 pos, at;
	int ret;

	if (!d || !d->ftl || d->ro || (file_off & (FTL_LP_SIZE - 1)))
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

	ret = crypto_skcipher_setkey(ftl_file_tfm, fkey, AES_KEYSIZE_256);
	if (!ret) {
		ftl_block_iv((u32)file_off, lfsr);
		aes_encrypt(&aes, iv, (const u8 *)lfsr);
		ret = ftl_encrypt(ftl_file_req, src, ftl_file_buf,
				  FTL_LP_SIZE, iv);
	}
	if (!ret) {
		at = ftl_disk_offset(d, pos);
		mutex_lock(&f->lock);
		ret = ftl_write(f, NULL, at >> FTL_LP_SHIFT, 0, ftl_file_buf,
				FTL_LP_SIZE);
		mutex_unlock(&f->lock);
	}
out:
	mutex_unlock(&ftl_file_lock);
	memzero_explicit(&aes, sizeof(aes));
	memzero_explicit(iv, sizeof(iv));
	return ret;
}
EXPORT_SYMBOL_GPL(s5l8947x_file_block_encrypt);

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
		bool ro;

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
		ro = !(f->mtd->flags & MTD_WRITEABLE) ||
		     ((attr & LV_ATTR_ENCRYPTED) && !d->req);
		snprintf(name, sizeof(name), "%sp%u", base, i + 1);
		if (ftl_add_disk(f, d, name, ro)) {
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

	if (mtd->type != MTD_MLCNANDFLASH || !lps || lps > FTL_MAX_LPS ||
	    mtd->writesize % FTL_LP_SIZE || mtd->oobsize != lps * FTL_META ||
	    !mtd->numeraseregions || mtd->numeraseregions > FTL_MAX_CAUS)
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
	bool ro = !(f->mtd->flags & MTD_WRITEABLE);
	u8 *hdr;
	int ret;

	ftl_read_syscfg(f);
	ret = ftl_build(f);
	if (ret) {
		dev_err(&f->mtd->dev, "translation map failed: %d\n", ret);
		return;
	}
	snprintf(base, sizeof(base), "nand%d", f->mtd->index);
	hdr = ftl_volume_header(f);
	f->whole.size = hdr ? get_unaligned_le64(hdr + LV_MEDIA_SIZE) :
			      (u64)f->nlba << FTL_LP_SHIFT;
	if (!ftl_add_disk(f, &f->whole, base, ro) && hdr)
		ftl_volumes(f, hdr, base);
	kfree(hdr);
}

static void ftl_notify_add(struct mtd_info *mtd)
{
	struct ftl *f;
	u32 sb_lps;

	if (!ftl_usable(mtd))
		return;
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return;
	f->mtd = mtd;
	f->caus = mtd->numeraseregions;
	f->blocks = mtd->eraseregions[0].numblocks;
	f->data_blocks = f->blocks > KS_RESERVED_BLOCKS ?
			 f->blocks - KS_RESERVED_BLOCKS : f->blocks;
	f->pages = mtd->erasesize / mtd->writesize;
	f->lps = mtd->writesize >> FTL_LP_SHIFT;
	f->nlba = f->caus * f->blocks * f->pages * f->lps;
	sb_lps = f->caus * f->pages * f->lps;
	f->cached = FTL_UNMAPPED;
	mutex_init(&f->lock);
	f->l2p = vmalloc(array_size(f->nlba, sizeof(*f->l2p)));
	f->sbs = kcalloc(f->blocks, sizeof(*f->sbs), GFP_KERNEL);
	f->wsb_lbas = kmalloc_array(sb_lps, sizeof(*f->wsb_lbas), GFP_KERNEL);
	f->page = kmalloc(mtd->writesize, GFP_KERNEL);
	f->oob = kmalloc(mtd->oobsize, GFP_KERNEL);
	f->wbuf_data = kmalloc(mtd->writesize, GFP_KERNEL);
	f->wbuf_oob = kmalloc(mtd->oobsize, GFP_KERNEL);
	f->dec = kmalloc(FTL_LP_SIZE, GFP_KERNEL);
	if (!f->l2p || !f->sbs || !f->wsb_lbas || !f->page || !f->oob ||
	    !f->wbuf_data || !f->wbuf_oob || !f->dec) {
		vfree(f->l2p);
		kfree(f->sbs);
		kfree(f->wsb_lbas);
		kfree(f->page);
		kfree(f->oob);
		kfree(f->wbuf_data);
		kfree(f->wbuf_oob);
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
	mutex_lock(&f->lock);
	ftl_flush_wbuf(f);
	mutex_unlock(&f->lock);
	__put_mtd_device(f->mtd);
	vfree(f->l2p);
	kfree(f->sbs);
	kfree(f->wsb_lbas);
	kfree(f->page);
	kfree(f->oob);
	kfree_sensitive(f->wbuf_data);
	kfree(f->wbuf_oob);
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
	memzero_explicit(ftl_bag_key, sizeof(ftl_bag_key));
	memzero_explicit(ftl_class_key, sizeof(ftl_class_key));
}

module_init(ftl_init);
module_exit(ftl_exit);
MODULE_DESCRIPTION("S5L8947X managed NAND translation layer and volume map");
MODULE_LICENSE("GPL");
