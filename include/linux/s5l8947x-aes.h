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

#endif
