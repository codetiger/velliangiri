// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X pre-kernel C loader (runs at 0x80000000)
 *
 * Layout inside the boot bundle relocated to 0x80000000:
 *   0x80000000 .. 0x80007fff  pre-kernel loader (this binary, padded to 32 KB)
 *   0x80008000 .. +zImage     Linux zImage (with embedded XZ initramfs)
 *   DTB_PHYS   .. +dtb        Flattened Device Tree (appended immediately after zImage)
 *
 * Responsibilities:
 *   1. Assert all required PMGR clock gates so peripheral blocks are clocked
 *      before kernel driver probe.
 *   2. Draw the boot splash and initialize 720p60 HDMI scanout at 0x9ec00000
 *      for simple-framebuffer.
 *   3. Jump to zImage at 0x80008000 with r0=0, r1=0xffffffff, r2=DTB_PHYS.
 */

#include <stdint.h>
#include "pmgr-init.h"
#include "hdmi-720p.h"

#define ZIMAGE_PHYS	0x80008000U
#ifndef DTB_PHYS
#define DTB_PHYS	0x82000000U
#endif

extern void jump_to_linux(uint32_t zimage_phys, uint32_t dtb_phys);

void linux_boot_main(void)
{
	pmgr_init_apply_gates();
	hdmi_720p_init();
	jump_to_linux(ZIMAGE_PHYS, DTB_PHYS);
}
