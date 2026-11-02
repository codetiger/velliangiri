/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S5L8947X Display Pipe & HDMI Audio Bridge Interface
 *
 * Besides the audio bridge, the pipe publishes what it is scanning out so
 * that a driver which draws into that memory (the SGX render node's 2D
 * engine) can map the live buffer.  A generation counter changes whenever
 * the pipe switches buffers or stops, so a client can tell a stale range
 * from a live one without holding the display's locks.
 */
#ifndef __LINUX_S5L8947X_DISPLAY_H
#define __LINUX_S5L8947X_DISPLAY_H

#include <linux/types.h>

struct device;

struct s5l8947x_scanout {
	phys_addr_t phys;	/* the buffer's physical address */
	u32 stride;		/* bytes a line */
	u32 width, height;	/* pixels */
	u32 format;		/* a DRM_FORMAT_* */
	u32 generation;		/* changes when the buffer does */
};

#if IS_ENABLED(CONFIG_DRM_S5L8947X)
int s5l8947x_hdmi_audio_configure(unsigned int sample_rate,
				  unsigned int channels, bool enable);
void s5l8947x_hdmi_audio_arm(unsigned int sample_rate);
int s5l8947x_display_scanout(struct device *dev, struct s5l8947x_scanout *out);
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

static inline int s5l8947x_display_scanout(struct device *dev,
					   struct s5l8947x_scanout *out)
{
	return -ENODEV;
}
#endif

#endif /* __LINUX_S5L8947X_DISPLAY_H */
