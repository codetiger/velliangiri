// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X AES engine.
 *
 * The engine sits in the central DMA block and works memory to memory:
 * CDMA channel 1 feeds it and channel 2 drains it. Its registers were
 * measured on hardware:
 *
 *   0x37800000         key disable bits: bit n set = hardware key n unusable
 *   0x37801000 +0x00   control
 *                         bit 8      always set
 *                         bit 16     encrypt
 *                         bit 17     CBC (clear: ECB)
 *                         19:18      user key length: 0 = 128, 1 = 192, 2 = 256
 *                         bit 20     user key (clear: hardware key)
 *                         22:21      hardware key: 0 device, 1 and 2 group
 *              +0x10   IV, four little-endian words
 *              +0x20   user key, eight little-endian words
 *
 * The two channels take one segment each and are started with values the
 * dmaengine API cannot express (the segment control and CSR bits route the
 * data through the engine), so they are reserved through the DMA binding and
 * programmed here directly. Transfers are polled.
 *
 * ecb(aes) and cbc(aes) are registered with the crypto API. The hardware
 * keys are only reachable from the kernel, through s5l8947x_aes_hwkey_crypt().
 */

#include <crypto/aes.h>
#include <crypto/algapi.h>
#include <crypto/internal/skcipher.h>
#include <crypto/scatterwalk.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/s5l8947x-aes.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define AES_CTRL		0x00
#define AES_IV			0x10
#define AES_KEY			0x20

#define AES_CTRL_ON		BIT(8)
#define AES_CTRL_ENCRYPT	BIT(16)
#define AES_CTRL_CBC		BIT(17)
#define AES_CTRL_KEYLEN		GENMASK(19, 18)
#define AES_CTRL_USER_KEY	BIT(20)
#define AES_CTRL_HWKEY		GENMASK(22, 21)

/* CDMA channel registers and the engine route */
#define CH_CSR			0x00
#define CH_LEN			0x0c
#define CH_SEG			0x14
#define CH_CSR_RUN		BIT(0)
#define CH_CSR_ABORT		BIT(1)
#define CH_CSR_STATE		GENMASK(17, 16)
#define CH_CSR_STATE_BUSY	1
#define CH_CSR_ENGINE		0x21c0080	/* type 2, status cleared, engine route */
#define CH_CSR_ENGINE_IN	BIT(8)
#define SEG_CTL_ENGINE		0x2000103
#define SEG_CTL_ENGINE_IN	0x30000

#define AES_CHUNK		SZ_16K
#define AES_POLL_US		200000

enum { CH_IN, CH_OUT };

struct s5l_aes_seg {
	u32 next;
	u32 ctl;
	u32 addr;
	u32 len;
	u32 reserved[4];
};

struct s5l_aes {
	struct device *dev;
	void __iomem *regs;
	void __iomem *keydis;
	void __iomem *ch[2];
	struct dma_chan *chan[2];
	struct mutex lock;	/* the engine and the buffers */
	struct workqueue_struct *wq;

	size_t chunk;
	u8 *in, *out;
	struct s5l_aes_seg *segs;
	dma_addr_t in_dma, out_dma, segs_dma;
};

struct s5l_aes_ctx {
	u8 key[AES_MAX_KEY_SIZE];
	unsigned int keylen;
};

struct s5l_aes_reqctx {
	struct work_struct work;
	struct skcipher_request *req;
	bool encrypt;
	bool cbc;
};

static struct s5l_aes *s5l_aes;	/* there is one engine */

/* --- the engine --- */

static void s5l_aes_chan_start(struct s5l_aes *a, int ch, dma_addr_t addr, u32 len)
{
	struct s5l_aes_seg *s = &a->segs[ch * 2];
	dma_addr_t s_dma = a->segs_dma + ch * 2 * sizeof(*s);
	void __iomem *c = a->ch[ch];

	writel(CH_CSR_ABORT, c + CH_CSR);
	memset(s, 0, 2 * sizeof(*s));
	s[0].next = s_dma + sizeof(*s);		/* an all-zero segment ends the list */
	s[0].ctl = SEG_CTL_ENGINE | (ch == CH_IN ? SEG_CTL_ENGINE_IN : 0);
	s[0].addr = addr;
	s[0].len = len;
	dma_wmb();
	writel(len, c + CH_LEN);
	writel(s_dma, c + CH_SEG);
	writel(CH_CSR_ENGINE | (ch == CH_IN ? CH_CSR_ENGINE_IN : 0), c + CH_CSR);
}

/*
 * Run a->in -> a->out through the engine. hwkey < 0 selects the user key.
 * Caller holds a->lock.
 */
static int s5l_aes_run(struct s5l_aes *a, int hwkey, const u8 *key, unsigned int keylen,
		       bool encrypt, const u8 *iv, size_t len)
{
	u32 ctrl = AES_CTRL_ON, csr;
	int i, ret = 0;

	if (hwkey >= 0 && (readl(a->keydis) & BIT(hwkey)))
		return -EPERM;
	if (encrypt)
		ctrl |= AES_CTRL_ENCRYPT;
	if (iv)
		ctrl |= AES_CTRL_CBC;
	if (hwkey < 0) {
		ctrl |= AES_CTRL_USER_KEY |
			FIELD_PREP(AES_CTRL_KEYLEN, (keylen - AES_KEYSIZE_128) / 8);
		for (i = 0; i < 8; i++)
			writel(i * 4 < keylen ? get_unaligned_le32(key + i * 4) : 0,
			       a->regs + AES_KEY + i * 4);
	} else {
		ctrl |= FIELD_PREP(AES_CTRL_HWKEY, hwkey);
	}
	for (i = 0; i < 4; i++)
		writel(iv ? get_unaligned_le32(iv + i * 4) : 0, a->regs + AES_IV + i * 4);
	writel(ctrl, a->regs + AES_CTRL);

	s5l_aes_chan_start(a, CH_IN, a->in_dma, len);
	s5l_aes_chan_start(a, CH_OUT, a->out_dma, len);
	for (i = CH_IN; i <= CH_OUT; i++)
		writel(readl(a->ch[i] + CH_CSR) | CH_CSR_RUN, a->ch[i] + CH_CSR);
	for (i = CH_IN; i <= CH_OUT; i++) {
		if (readl_poll_timeout(a->ch[i] + CH_CSR, csr,
				       FIELD_GET(CH_CSR_STATE, csr) != CH_CSR_STATE_BUSY,
				       2, AES_POLL_US)) {
			dev_err(a->dev, "channel %d timeout, csr %08x\n", i, csr);
			writel(CH_CSR_ABORT, a->ch[i] + CH_CSR);
			ret = -ETIMEDOUT;
		}
	}

	if (hwkey < 0)
		for (i = 0; i < 8; i++)
			writel(0, a->regs + AES_KEY + i * 4);
	return ret;
}

/*
 * Crypt len bytes from in to out (either may be NULL to use a scatterlist
 * walk done by the caller), chunk by chunk, carrying the CBC IV. iv is updated
 * to the chaining value after the last block.
 */
static int s5l_aes_crypt_linear(struct s5l_aes *a, int hwkey, const u8 *key,
				unsigned int keylen, bool encrypt, u8 *iv,
				const u8 *in, u8 *out, size_t len)
{
	u8 next[AES_BLOCK_SIZE];
	int ret = 0;

	mutex_lock(&a->lock);
	while (len && !ret) {
		size_t n = min(len, a->chunk);

		memcpy(a->in, in, n);
		if (iv && !encrypt)
			memcpy(next, a->in + n - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
		ret = s5l_aes_run(a, hwkey, key, keylen, encrypt, iv, n);
		memcpy(out, a->out, n);
		if (iv)
			memcpy(iv, encrypt ? a->out + n - AES_BLOCK_SIZE : next, AES_BLOCK_SIZE);
		in += n;
		out += n;
		len -= n;
	}
	mutex_unlock(&a->lock);
	return ret;
}

int s5l8947x_aes_hwkey_crypt(enum s5l8947x_aes_hwkey key, bool encrypt,
			     const u8 *iv, const void *in, void *out, size_t len)
{
	u8 chain[AES_BLOCK_SIZE];

	if (!s5l_aes)
		return -ENODEV;
	if (key > S5L8947X_AES_KEY_GROUP1 || len % AES_BLOCK_SIZE)
		return -EINVAL;
	if (iv)
		memcpy(chain, iv, sizeof(chain));
	return s5l_aes_crypt_linear(s5l_aes, key, NULL, 0, encrypt, iv ? chain : NULL,
				    in, out, len);
}
EXPORT_SYMBOL_GPL(s5l8947x_aes_hwkey_crypt);

/* --- crypto API --- */

static void s5l_aes_work(struct work_struct *work)
{
	struct s5l_aes_reqctx *rctx = container_of(work, struct s5l_aes_reqctx, work);
	struct skcipher_request *req = rctx->req;
	struct s5l_aes_ctx *ctx = crypto_skcipher_ctx(crypto_skcipher_reqtfm(req));
	struct s5l_aes *a = s5l_aes;
	unsigned int off = 0, len = req->cryptlen;
	u8 next[AES_BLOCK_SIZE];
	u8 *iv = rctx->cbc ? req->iv : NULL;
	int ret = 0;

	mutex_lock(&a->lock);
	while (len && !ret) {
		size_t n = min_t(size_t, len, a->chunk);

		scatterwalk_map_and_copy(a->in, req->src, off, n, 0);
		if (iv && !rctx->encrypt)
			memcpy(next, a->in + n - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
		ret = s5l_aes_run(a, -1, ctx->key, ctx->keylen, rctx->encrypt, iv, n);
		scatterwalk_map_and_copy(a->out, req->dst, off, n, 1);
		if (iv)
			memcpy(iv, rctx->encrypt ? a->out + n - AES_BLOCK_SIZE : next,
			       AES_BLOCK_SIZE);
		off += n;
		len -= n;
	}
	mutex_unlock(&a->lock);
	skcipher_request_complete(req, ret);
}

static int s5l_aes_queue(struct skcipher_request *req, bool encrypt, bool cbc)
{
	struct s5l_aes_reqctx *rctx = skcipher_request_ctx(req);

	if (req->cryptlen % AES_BLOCK_SIZE)
		return -EINVAL;
	if (!req->cryptlen)
		return 0;
	rctx->req = req;
	rctx->encrypt = encrypt;
	rctx->cbc = cbc;
	INIT_WORK(&rctx->work, s5l_aes_work);
	queue_work(s5l_aes->wq, &rctx->work);
	return -EINPROGRESS;
}

static int s5l_aes_ecb_encrypt(struct skcipher_request *req)
{
	return s5l_aes_queue(req, true, false);
}

static int s5l_aes_ecb_decrypt(struct skcipher_request *req)
{
	return s5l_aes_queue(req, false, false);
}

static int s5l_aes_cbc_encrypt(struct skcipher_request *req)
{
	return s5l_aes_queue(req, true, true);
}

static int s5l_aes_cbc_decrypt(struct skcipher_request *req)
{
	return s5l_aes_queue(req, false, true);
}

static int s5l_aes_setkey(struct crypto_skcipher *tfm, const u8 *key, unsigned int len)
{
	struct s5l_aes_ctx *ctx = crypto_skcipher_ctx(tfm);

	if (len != AES_KEYSIZE_128 && len != AES_KEYSIZE_192 && len != AES_KEYSIZE_256)
		return -EINVAL;
	memcpy(ctx->key, key, len);
	ctx->keylen = len;
	return 0;
}

static int s5l_aes_init_tfm(struct crypto_skcipher *tfm)
{
	crypto_skcipher_set_reqsize(tfm, sizeof(struct s5l_aes_reqctx));
	return 0;
}

static void s5l_aes_exit_tfm(struct crypto_skcipher *tfm)
{
	memzero_explicit(crypto_skcipher_ctx(tfm), sizeof(struct s5l_aes_ctx));
}

#define S5L_AES_ALG(mode, ivlen)						\
{									\
	.base = {							\
		.cra_name		= #mode "(aes)",			\
		.cra_driver_name	= "s5l8947x-" #mode "-aes",		\
		.cra_priority		= 300,					\
		.cra_flags		= CRYPTO_ALG_ASYNC |			\
					  CRYPTO_ALG_KERN_DRIVER_ONLY,		\
		.cra_blocksize		= AES_BLOCK_SIZE,			\
		.cra_ctxsize		= sizeof(struct s5l_aes_ctx),		\
		.cra_module		= THIS_MODULE,				\
	},								\
	.min_keysize	= AES_MIN_KEY_SIZE,					\
	.max_keysize	= AES_MAX_KEY_SIZE,					\
	.ivsize		= ivlen,						\
	.setkey		= s5l_aes_setkey,					\
	.encrypt	= s5l_aes_##mode##_encrypt,				\
	.decrypt	= s5l_aes_##mode##_decrypt,				\
	.init		= s5l_aes_init_tfm,					\
	.exit		= s5l_aes_exit_tfm,					\
}

static struct skcipher_alg s5l_aes_algs[] = {
	S5L_AES_ALG(ecb, 0),
	S5L_AES_ALG(cbc, AES_BLOCK_SIZE),
};

/* --- probe --- */

/* CBC-encrypt a random buffer in hardware and in software and compare. */
static int s5l_aes_selftest(struct s5l_aes *a)
{
	struct crypto_aes_ctx sw;
	u8 key[AES_KEYSIZE_256], iv[AES_BLOCK_SIZE], chain[AES_BLOCK_SIZE];
	size_t len = a->chunk, i;
	u8 *src, *hw, *ref;
	int ret = -ENOMEM;

	src = kmalloc(len, GFP_KERNEL);
	hw = kmalloc(len, GFP_KERNEL);
	ref = kmalloc(len, GFP_KERNEL);
	if (!src || !hw || !ref)
		goto out;
	get_random_bytes(key, sizeof(key));
	get_random_bytes(iv, sizeof(iv));
	get_random_bytes(src, len);

	ret = aes_expandkey(&sw, key, sizeof(key));
	if (ret)
		goto out;
	memcpy(chain, iv, sizeof(chain));
	for (i = 0; i < len; i += AES_BLOCK_SIZE) {
		crypto_xor_cpy(ref + i, src + i, chain, AES_BLOCK_SIZE);
		aes_encrypt(&sw, ref + i, ref + i);
		memcpy(chain, ref + i, AES_BLOCK_SIZE);
	}

	memcpy(chain, iv, sizeof(chain));
	ret = s5l_aes_crypt_linear(a, -1, key, sizeof(key), true, chain, src, hw, len);
	if (!ret && memcmp(hw, ref, len))
		ret = -EIO;
out:
	memzero_explicit(&sw, sizeof(sw));
	memzero_explicit(key, sizeof(key));
	kfree(ref);
	kfree(hw);
	kfree(src);
	return ret;
}

/* The device key works when it is enabled and gives a stable, non-trivial result. */
static bool s5l_aes_device_key_usable(struct s5l_aes *a)
{
	static const u8 zero[AES_BLOCK_SIZE];
	u8 r1[AES_BLOCK_SIZE], r2[AES_BLOCK_SIZE];
	bool ok;

	if (s5l_aes_crypt_linear(a, S5L8947X_AES_KEY_DEVICE, NULL, 0, true, NULL,
				 zero, r1, sizeof(r1)) ||
	    s5l_aes_crypt_linear(a, S5L8947X_AES_KEY_DEVICE, NULL, 0, true, NULL,
				 zero, r2, sizeof(r2)))
		return false;
	ok = !memcmp(r1, r2, sizeof(r1)) && memcmp(r1, zero, sizeof(r1));
	memzero_explicit(r1, sizeof(r1));
	memzero_explicit(r2, sizeof(r2));
	return ok;
}

static void s5l_aes_release(void *data)
{
	struct s5l_aes *a = data;

	s5l_aes = NULL;
	if (a->wq)
		destroy_workqueue(a->wq);
	if (a->segs)
		dma_free_coherent(a->chan[CH_IN]->device->dev, 4 * sizeof(*a->segs),
				  a->segs, a->segs_dma);
	if (a->in)
		dma_free_coherent(a->chan[CH_IN]->device->dev, 2 * SZ_16K, a->in, a->in_dma);
}

static void s5l_aes_release_chan(void *chan)
{
	dma_release_channel(chan);
}

static int s5l_aes_probe(struct platform_device *pdev)
{
	static const char *const names[] = { "in", "out" };
	struct device *dev = &pdev->dev;
	struct device *dma_dev;
	struct resource *res;
	struct s5l_aes *a;
	struct clk *clk;
	int i, ret;

	if (s5l_aes)
		return -EBUSY;
	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	mutex_init(&a->lock);

	a->regs = devm_platform_ioremap_resource_byname(pdev, "engine");
	if (IS_ERR(a->regs))
		return PTR_ERR(a->regs);
	a->keydis = devm_platform_ioremap_resource_byname(pdev, "keys");
	if (IS_ERR(a->keydis))
		return PTR_ERR(a->keydis);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to enable clock\n");

	for (i = CH_IN; i <= CH_OUT; i++) {
		a->chan[i] = dma_request_chan(dev, names[i]);
		if (IS_ERR(a->chan[i]))
			return dev_err_probe(dev, PTR_ERR(a->chan[i]), "%s channel\n", names[i]);
		ret = devm_add_action_or_reset(dev, s5l_aes_release_chan, a->chan[i]);
		if (ret)
			return ret;
		/* The channel's registers belong to the CDMA block's window. */
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   i == CH_IN ? "ch-in" : "ch-out");
		if (!res)
			return -EINVAL;
		a->ch[i] = devm_ioremap(dev, res->start, resource_size(res));
		if (!a->ch[i])
			return -ENOMEM;
	}

	dma_dev = a->chan[CH_IN]->device->dev;
	a->chunk = AES_CHUNK;
	a->in = dma_alloc_coherent(dma_dev, 2 * SZ_16K, &a->in_dma, GFP_KERNEL);
	a->segs = dma_alloc_coherent(dma_dev, 4 * sizeof(*a->segs), &a->segs_dma, GFP_KERNEL);
	a->wq = alloc_ordered_workqueue("s5l8947x-aes", WQ_MEM_RECLAIM);
	ret = devm_add_action_or_reset(dev, s5l_aes_release, a);
	if (ret)
		return ret;
	if (!a->in || !a->segs || !a->wq)
		return -ENOMEM;
	a->out = a->in + SZ_16K;
	a->out_dma = a->in_dma + SZ_16K;

	ret = s5l_aes_selftest(a);
	if (ret) {
		a->chunk = SZ_4K;
		ret = s5l_aes_selftest(a);
	}
	if (ret)
		return dev_err_probe(dev, ret, "self-test against software AES failed\n");

	s5l_aes = a;
	ret = crypto_register_skciphers(s5l_aes_algs, ARRAY_SIZE(s5l_aes_algs));
	if (ret) {
		s5l_aes = NULL;
		return ret;
	}
	platform_set_drvdata(pdev, a);
	dev_info(dev, "%zu-byte transfers, self-test passed, device key %s\n", a->chunk,
		 s5l_aes_device_key_usable(a) ? "usable" : "unavailable");
	return 0;
}

static void s5l_aes_remove(struct platform_device *pdev)
{
	struct s5l_aes *a = platform_get_drvdata(pdev);

	crypto_unregister_skciphers(s5l_aes_algs, ARRAY_SIZE(s5l_aes_algs));
	flush_workqueue(a->wq);
}

static const struct of_device_id s5l_aes_of_match[] = {
	{ .compatible = "apple,s5l8947x-aes" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_aes_of_match);

static struct platform_driver s5l_aes_driver = {
	.probe	= s5l_aes_probe,
	.remove	= s5l_aes_remove,
	.driver	= {
		.name		= "s5l8947x-aes",
		.of_match_table	= s5l_aes_of_match,
	},
};
module_platform_driver(s5l_aes_driver);

MODULE_DESCRIPTION("S5L8947X AES engine");
MODULE_LICENSE("GPL");
