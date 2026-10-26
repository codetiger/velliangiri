/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X Display Pipe & HDMI Audio Bridge Interface
 */

#ifndef __LINUX_S5L8947X_DISPLAY_H
#define __LINUX_S5L8947X_DISPLAY_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_FB_S5L8947X_DISPLAY)
int s5l8947x_hdmi_audio_configure(unsigned int sample_rate, bool enable);
#else
static inline int s5l8947x_hdmi_audio_configure(unsigned int sample_rate,
						bool enable)
{
	return 0;
}
#endif

#endif /* __LINUX_S5L8947X_DISPLAY_H */
