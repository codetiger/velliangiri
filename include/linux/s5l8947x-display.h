/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X Display Pipe & HDMI Audio Bridge Interface
 */

#ifndef __LINUX_S5L8947X_DISPLAY_H
#define __LINUX_S5L8947X_DISPLAY_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_DRM_S5L8947X)
int s5l8947x_hdmi_audio_configure(unsigned int sample_rate,
				  unsigned int channels, bool enable);
void s5l8947x_hdmi_audio_arm(unsigned int sample_rate);
#else
static inline int s5l8947x_hdmi_audio_configure(unsigned int sample_rate,
						unsigned int channels,
						bool enable)
{
	return 0;
}

static inline void s5l8947x_hdmi_audio_arm(unsigned int sample_rate)
{
}
#endif

#endif /* __LINUX_S5L8947X_DISPLAY_H */
