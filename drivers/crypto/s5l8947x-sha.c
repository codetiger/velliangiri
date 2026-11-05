// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X SHA-2 engine
 *
 * A hash engine at 0x30200000 that computes SHA-1 and the SHA-2 family one
 * block at a time:
 *
 *   +0x08  mode: 0x10 SHA-1, 0x20 SHA-224, 0x30 SHA-256, 0x40 SHA-384,
 *          0x50 SHA-512
 *   +0x0c  control: bit 0 runs one block and reads back as busy, bit 1
 *          first loads the initial hash value of the mode
 *   +0x10  status: bit 0 busy
 *   +0x40  chaining state, 32 bytes (SHA-224/256) or 64 bytes (SHA-384/512),
 *          stored as the big-endian bytes of the hash words; it can be read
 *          and written, so a hash can be suspended and resumed
 *   +0x80  one message block, 64 or 128 bytes, in message byte order
 *
 * Padding is left to software. The CPU writes every block word, which
 * limits the engine to about 14 MB/s; the engine itself does about 47 MB/s
 * for SHA-256 and 76 MB/s for SHA-512. Software SHA-512 on the Cortex-A9
 * manages about 10 MB/s, so SHA-384/512 are registered with a priority
 * above the generic code; SHA-224/256 are registered below it, as software
 * is faster than this engine fed by the CPU. SHA-1 is faster in software
 * and is not offered.
 *
 * The engine is shared: each update takes a lock, loads the request's state,
 * hashes at most SHA_BLOCKS_PER_LOCK blocks and reads the state back.
 */

#include <crypto/internal/hash.h>
#include <crypto/sha2.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define SHA_MODE		0x08
#define  SHA_MODE_224		0x20
#define  SHA_MODE_256		0x30
#define  SHA_MODE_384		0x40
#define  SHA_MODE_512		0x50
#define SHA_CTRL		0x0c
#define  SHA_CTRL_RUN		BIT(0)
#define SHA_STATUS		0x10
#define  SHA_STATUS_BUSY	BIT(0)
#define SHA_STATE		0x40
#define SHA_DATA		0x80

#define SHA_TIMEOUT_US		100
#define SHA_BLOCKS_PER_LOCK	8	/* bounds the time interrupts are off */

struct s5l8947x_sha {
	struct device *dev;
	void __iomem *base;
	spinlock_t lock;	/* one request on the engine at a time */
};

static struct s5l8947x_sha *sha_dev;

/*
 * Hash blocks of len bytes each into state, given as nwords big-endian
 * 32-bit words (the hash words, high half first for SHA-384/512).
 */
static void s5l8947x_sha_blocks(u32 mode, u32 *state, unsigned int nwords,
				const u8 *src, int blocks, unsigned int len)
{
	struct s5l8947x_sha *s = sha_dev;
	unsigned int i;
	u32 v;

	while (blocks > 0) {
		int n = min(blocks, SHA_BLOCKS_PER_LOCK);

		spin_lock_bh(&s->lock);
		writel_relaxed(mode, s->base + SHA_MODE);
		for (i = 0; i < nwords; i++)
			writel_relaxed(swab32(state[i]), s->base + SHA_STATE + 4 * i);
		blocks -= n;
		while (n--) {
			for (i = 0; i < len; i += 4)
				writel_relaxed(get_unaligned_le32(src + i),
					       s->base + SHA_DATA + i);
			writel(SHA_CTRL_RUN, s->base + SHA_CTRL);
			if (readl_poll_timeout_atomic(s->base + SHA_CTRL, v,
						      !(v & SHA_CTRL_RUN), 0,
						      SHA_TIMEOUT_US))
				dev_err_ratelimited(s->dev, "block timed out\n");
			src += len;
		}
		if (readl_poll_timeout_atomic(s->base + SHA_STATUS, v,
					      !(v & SHA_STATUS_BUSY), 0,
					      SHA_TIMEOUT_US))
			dev_err_ratelimited(s->dev, "engine stayed busy\n");
		for (i = 0; i < nwords; i++)
			state[i] = swab32(readl_relaxed(s->base + SHA_STATE + 4 * i));
		spin_unlock_bh(&s->lock);
	}
}

static void s5l8947x_sha256_blocks(struct sha256_state *sst, const u8 *src,
				   int blocks)
{
	s5l8947x_sha_blocks(SHA_MODE_256, sst->state, 8, src, blocks,
			    SHA256_BLOCK_SIZE);
}

static void s5l8947x_sha512_blocks(struct sha512_state *sst, const u8 *src,
				   int blocks)
{
	u32 w[16];
	int i;

	for (i = 0; i < 8; i++) {
		w[2 * i] = upper_32_bits(sst->state[i]);
		w[2 * i + 1] = lower_32_bits(sst->state[i]);
	}
	s5l8947x_sha_blocks(SHA_MODE_512, w, 16, src, blocks, SHA512_BLOCK_SIZE);
	for (i = 0; i < 8; i++)
		sst->state[i] = (u64)w[2 * i] << 32 | w[2 * i + 1];
}

static int s5l8947x_sha224_init(struct shash_desc *desc)
{
	struct sha256_state *sctx = shash_desc_ctx(desc);

	sha224_block_init(&sctx->ctx);
	return 0;
}

static int s5l8947x_sha256_init(struct shash_desc *desc)
{
	struct sha256_state *sctx = shash_desc_ctx(desc);

	sha256_block_init(&sctx->ctx);
	return 0;
}

static void s5l8947x_sha256_do_update(struct sha256_state *sctx,
				      const u8 *data, unsigned int len)
{
	unsigned int partial = sctx->count % SHA256_BLOCK_SIZE;

	sctx->count += len;
	if (unlikely((partial + len) >= SHA256_BLOCK_SIZE)) {
		int blocks;

		if (partial) {
			int p = SHA256_BLOCK_SIZE - partial;

			memcpy(sctx->buf + partial, data, p);
			data += p;
			len -= p;
			s5l8947x_sha256_blocks(sctx, sctx->buf, 1);
		}
		blocks = len / SHA256_BLOCK_SIZE;
		len %= SHA256_BLOCK_SIZE;
		if (blocks) {
			s5l8947x_sha256_blocks(sctx, data, blocks);
			data += blocks * SHA256_BLOCK_SIZE;
		}
		partial = 0;
	}
	if (len)
		memcpy(sctx->buf + partial, data, len);
}

static int s5l8947x_sha256_update(struct shash_desc *desc, const u8 *data,
				  unsigned int len)
{
	s5l8947x_sha256_do_update(shash_desc_ctx(desc), data, len);
	return 0;
}

static int s5l8947x_sha256_finup(struct shash_desc *desc, const u8 *data,
				 unsigned int len, u8 *out)
{
	struct sha256_state *sctx = shash_desc_ctx(desc);
	unsigned int digest_size = crypto_shash_digestsize(desc->tfm);
	unsigned int bit_offset = SHA256_BLOCK_SIZE - sizeof(__be64);
	unsigned int partial;
	__be32 *digest = (__be32 *)out;
	int i;

	if (len)
		s5l8947x_sha256_do_update(sctx, data, len);

	partial = sctx->count % SHA256_BLOCK_SIZE;
	sctx->buf[partial++] = 0x80;
	if (partial > bit_offset) {
		memset(sctx->buf + partial, 0, SHA256_BLOCK_SIZE - partial);
		partial = 0;
		s5l8947x_sha256_blocks(sctx, sctx->buf, 1);
	}
	memset(sctx->buf + partial, 0, bit_offset - partial);
	put_unaligned_be64(sctx->count << 3, sctx->buf + bit_offset);
	s5l8947x_sha256_blocks(sctx, sctx->buf, 1);

	for (i = 0; i < digest_size / 4; i++)
		put_unaligned_be32(sctx->state[i], digest++);

	*sctx = (struct sha256_state){};
	return 0;
}

static int s5l8947x_sha256_final(struct shash_desc *desc, u8 *out)
{
	return s5l8947x_sha256_finup(desc, NULL, 0, out);
}

static int s5l8947x_sha384_init(struct shash_desc *desc)
{
	struct sha512_state *sctx = shash_desc_ctx(desc);

	sctx->state[0] = SHA384_H0;
	sctx->state[1] = SHA384_H1;
	sctx->state[2] = SHA384_H2;
	sctx->state[3] = SHA384_H3;
	sctx->state[4] = SHA384_H4;
	sctx->state[5] = SHA384_H5;
	sctx->state[6] = SHA384_H6;
	sctx->state[7] = SHA384_H7;
	sctx->count[0] = sctx->count[1] = 0;
	return 0;
}

static int s5l8947x_sha512_init(struct shash_desc *desc)
{
	struct sha512_state *sctx = shash_desc_ctx(desc);

	sctx->state[0] = SHA512_H0;
	sctx->state[1] = SHA512_H1;
	sctx->state[2] = SHA512_H2;
	sctx->state[3] = SHA512_H3;
	sctx->state[4] = SHA512_H4;
	sctx->state[5] = SHA512_H5;
	sctx->state[6] = SHA512_H6;
	sctx->state[7] = SHA512_H7;
	sctx->count[0] = sctx->count[1] = 0;
	return 0;
}

static void s5l8947x_sha512_do_update(struct sha512_state *sctx,
				      const u8 *data, unsigned int len)
{
	unsigned int partial = sctx->count[0] % SHA512_BLOCK_SIZE;

	sctx->count[0] += len;
	if (sctx->count[0] < len)
		sctx->count[1]++;

	if (unlikely((partial + len) >= SHA512_BLOCK_SIZE)) {
		int blocks;

		if (partial) {
			int p = SHA512_BLOCK_SIZE - partial;

			memcpy(sctx->buf + partial, data, p);
			data += p;
			len -= p;
			s5l8947x_sha512_blocks(sctx, sctx->buf, 1);
		}
		blocks = len / SHA512_BLOCK_SIZE;
		len %= SHA512_BLOCK_SIZE;
		if (blocks) {
			s5l8947x_sha512_blocks(sctx, data, blocks);
			data += blocks * SHA512_BLOCK_SIZE;
		}
		partial = 0;
	}
	if (len)
		memcpy(sctx->buf + partial, data, len);
}

static int s5l8947x_sha512_update(struct shash_desc *desc, const u8 *data,
				  unsigned int len)
{
	s5l8947x_sha512_do_update(shash_desc_ctx(desc), data, len);
	return 0;
}

static int s5l8947x_sha512_finup(struct shash_desc *desc, const u8 *data,
				 unsigned int len, u8 *out)
{
	struct sha512_state *sctx = shash_desc_ctx(desc);
	unsigned int digest_size = crypto_shash_digestsize(desc->tfm);
	unsigned int bit_offset = SHA512_BLOCK_SIZE - 2 * sizeof(__be64);
	unsigned int partial;
	__be64 *digest = (__be64 *)out;
	int i;

	if (len)
		s5l8947x_sha512_do_update(sctx, data, len);

	partial = sctx->count[0] % SHA512_BLOCK_SIZE;
	sctx->buf[partial++] = 0x80;
	if (partial > bit_offset) {
		memset(sctx->buf + partial, 0, SHA512_BLOCK_SIZE - partial);
		partial = 0;
		s5l8947x_sha512_blocks(sctx, sctx->buf, 1);
	}
	memset(sctx->buf + partial, 0, bit_offset - partial);
	put_unaligned_be64(sctx->count[1] << 3 | sctx->count[0] >> 61,
			   sctx->buf + bit_offset);
	put_unaligned_be64(sctx->count[0] << 3,
			   sctx->buf + bit_offset + sizeof(__be64));
	s5l8947x_sha512_blocks(sctx, sctx->buf, 1);

	for (i = 0; i < digest_size / 8; i++)
		put_unaligned_be64(sctx->state[i], digest++);

	*sctx = (struct sha512_state){};
	return 0;
}

static int s5l8947x_sha512_final(struct shash_desc *desc, u8 *out)
{
	return s5l8947x_sha512_finup(desc, NULL, 0, out);
}

#define S5L_SHA_ALG(_name, _bits, _family, _init, _prio)			\
{									\
	.digestsize	= _bits##_DIGEST_SIZE,				\
	.init		= _init,					\
	.update		= s5l8947x_##_family##_update,			\
	.final		= s5l8947x_##_family##_final,			\
	.finup		= s5l8947x_##_family##_finup,			\
	.descsize	= sizeof(struct _family##_state),		\
	.base = {							\
		.cra_name		= _name,			\
		.cra_driver_name	= _name "-s5l8947x",		\
		.cra_priority		= _prio,			\
		.cra_blocksize		= _bits##_BLOCK_SIZE,		\
		.cra_module		= THIS_MODULE,			\
	},								\
}

/* SHA-224 and SHA-384 run SHA-256 and SHA-512 from their own initial values. */
static struct shash_alg s5l8947x_sha_algs[] = {
	S5L_SHA_ALG("sha224", SHA224, sha256, s5l8947x_sha224_init, 90),
	S5L_SHA_ALG("sha256", SHA256, sha256, s5l8947x_sha256_init, 90),
	S5L_SHA_ALG("sha384", SHA384, sha512, s5l8947x_sha384_init, 350),
	S5L_SHA_ALG("sha512", SHA512, sha512, s5l8947x_sha512_init, 350),
};

static int s5l8947x_sha_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s5l8947x_sha *s;
	struct clk *clk;
	int ret;

	if (sha_dev)
		return -EBUSY;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;
	spin_lock_init(&s->lock);

	s->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot enable clock\n");

	platform_set_drvdata(pdev, s);
	sha_dev = s;
	ret = crypto_register_shashes(s5l8947x_sha_algs,
				      ARRAY_SIZE(s5l8947x_sha_algs));
	if (ret) {
		sha_dev = NULL;
		return dev_err_probe(dev, ret, "cannot register hashes\n");
	}
	dev_info(dev, "SHA-224/256/384/512 engine\n");
	return 0;
}

static void s5l8947x_sha_remove(struct platform_device *pdev)
{
	crypto_unregister_shashes(s5l8947x_sha_algs,
				  ARRAY_SIZE(s5l8947x_sha_algs));
	sha_dev = NULL;
}

static const struct of_device_id s5l8947x_sha_of_match[] = {
	{ .compatible = "apple,s5l8947x-sha" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l8947x_sha_of_match);

static struct platform_driver s5l8947x_sha_driver = {
	.driver = {
		.name		= "s5l8947x-sha",
		.of_match_table	= s5l8947x_sha_of_match,
	},
	.probe	= s5l8947x_sha_probe,
	.remove	= s5l8947x_sha_remove,
};
module_platform_driver(s5l8947x_sha_driver);

MODULE_DESCRIPTION("S5L8947X SHA-2 engine");
MODULE_LICENSE("GPL");
