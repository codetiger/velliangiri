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
#include <crypto/sha256_base.h>
#include <crypto/sha512_base.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
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
	unsigned long flags;
	unsigned int i;
	u32 v;

	while (blocks > 0) {
		int n = min(blocks, SHA_BLOCKS_PER_LOCK);

		spin_lock_irqsave(&s->lock, flags);
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
		spin_unlock_irqrestore(&s->lock, flags);
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

static int s5l8947x_sha256_update(struct shash_desc *desc, const u8 *data,
				  unsigned int len)
{
	return sha256_base_do_update(desc, data, len, s5l8947x_sha256_blocks);
}

static int s5l8947x_sha256_finup(struct shash_desc *desc, const u8 *data,
				 unsigned int len, u8 *out)
{
	if (len)
		sha256_base_do_update(desc, data, len, s5l8947x_sha256_blocks);
	sha256_base_do_finalize(desc, s5l8947x_sha256_blocks);
	return sha256_base_finish(desc, out);
}

static int s5l8947x_sha256_final(struct shash_desc *desc, u8 *out)
{
	return s5l8947x_sha256_finup(desc, NULL, 0, out);
}

static int s5l8947x_sha512_update(struct shash_desc *desc, const u8 *data,
				  unsigned int len)
{
	return sha512_base_do_update(desc, data, len, s5l8947x_sha512_blocks);
}

static int s5l8947x_sha512_finup(struct shash_desc *desc, const u8 *data,
				 unsigned int len, u8 *out)
{
	if (len)
		sha512_base_do_update(desc, data, len, s5l8947x_sha512_blocks);
	sha512_base_do_finalize(desc, s5l8947x_sha512_blocks);
	return sha512_base_finish(desc, out);
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
	S5L_SHA_ALG("sha224", SHA224, sha256, sha224_base_init, 90),
	S5L_SHA_ALG("sha256", SHA256, sha256, sha256_base_init, 90),
	S5L_SHA_ALG("sha384", SHA384, sha512, sha384_base_init, 300),
	S5L_SHA_ALG("sha512", SHA512, sha512, sha512_base_init, 300),
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
