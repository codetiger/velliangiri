/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X AES engine: in-kernel use of its hardware keys.
 */
#ifndef _LINUX_S5L8947X_AES_H
#define _LINUX_S5L8947X_AES_H

#include <linux/types.h>

/* Keys held by the engine itself; software never sees them. */
enum s5l8947x_aes_hwkey {
	S5L8947X_AES_KEY_DEVICE = 0,	/* unique to this SoC */
	S5L8947X_AES_KEY_GROUP0 = 1,	/* shared by a family of SoCs */
	S5L8947X_AES_KEY_GROUP1 = 2,
};

#if IS_ENABLED(CONFIG_CRYPTO_DEV_S5L8947X_AES)
/*
 * AES with a hardware key, ECB when iv is NULL, otherwise CBC (iv is not
 * updated). len must be a multiple of 16. May sleep.
 */
int s5l8947x_aes_hwkey_crypt(enum s5l8947x_aes_hwkey key, bool encrypt,
			     const u8 *iv, const void *in, void *out, size_t len);
#else
static inline int s5l8947x_aes_hwkey_crypt(enum s5l8947x_aes_hwkey key, bool encrypt,
					   const u8 *iv, const void *in, void *out,
					   size_t len)
{
	return -ENODEV;
}
#endif

struct block_device;

#if IS_ENABLED(CONFIG_MTD_S5L8947X_FTL)
int s5l8947x_file_key_unwrap(const void *attr, size_t len, u8 fkey[32], u8 iv_key[16]);
int s5l8947x_file_block_decrypt(struct block_device *bdev, u64 blocknr,
				const u8 fkey[32], const u8 iv_key[16],
				u64 file_off, void *dst);
int s5l8947x_file_block_encrypt(struct block_device *bdev, u64 blocknr,
				const u8 fkey[32], const u8 iv_key[16],
				u64 file_off, const void *src);
#else
static inline int s5l8947x_file_key_unwrap(const void *attr, size_t len,
					   u8 fkey[32], u8 iv_key[16])
{
	return -ENODEV;
}
static inline int s5l8947x_file_block_decrypt(struct block_device *bdev, u64 blocknr,
					      const u8 fkey[32], const u8 iv_key[16],
					      u64 file_off, void *dst)
{
	return -ENODEV;
}
static inline int s5l8947x_file_block_encrypt(struct block_device *bdev, u64 blocknr,
					      const u8 fkey[32], const u8 iv_key[16],
					      u64 file_off, const void *src)
{
	return -ENODEV;
}
#endif

#endif
