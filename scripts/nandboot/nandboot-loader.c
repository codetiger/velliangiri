// SPDX-License-Identifier: GPL-2.0-only
/*
 * S5L8947X Stage-1 NAND boot loader entry point (runs at 0x80000000).
 *
 * Responsibilities:
 *   1. Boost CPU to 850 MHz with peripheral divider 2 and assert PMGR clock
 *      gates for FMI0, CDMA, display pipe, HDMI, and UARTs.
 *   2. Initialize 1080p30 HDMI scanout with a zeroed black framebuffer at
 *      0x9e400000 (BOOT_SPLASH=0).
 *   3. Read the boot manifest from NAND, select slot A or B, load the
 *      uncompressed kernel Image to 0x80008000 and DTB to 0x83000000, and
 *      verify their CRC-32 checksums.
 *   4. Jump to Linux at 0x80008000 with r0=0, r1=0xffffffff, r2=0x83000000.
 */

#include <stdint.h>
#define BOOT_SPLASH 0
#include "../boot/pmgr-init.h"
#include "../boot/hdmi-boot.h"
#include "manifest.h"

#define IMAGE_PHYS	0x80008000U
#define DTB_PHYS	0x83000000U

extern void jump_to_linux(uint32_t image_phys, uint32_t dtb_phys);

/*
 * Patch the first occurrence of "ubiblock0_0" / "slot=a" in the loaded DTB's
 * bootargs string in-place when booting from slot B (slot == 1), so the
 * kernel mounts /dev/ubiblock0_1 (rootfs_b) without needing libfdt.
 */
static void patch_dtb_slot(uint8_t *dtb, uint32_t max_len, uint8_t slot)
{
	uint32_t i;

	if (slot != 1U || max_len < 16U)
		return;
	for (i = 0; i + 11U <= max_len; i++) {
		if (dtb[i + 0] == 'u' && dtb[i + 1] == 'b' &&
		    dtb[i + 2] == 'i' && dtb[i + 3] == 'b' &&
		    dtb[i + 4] == 'l' && dtb[i + 5] == 'o' &&
		    dtb[i + 6] == 'c' && dtb[i + 7] == 'k' &&
		    dtb[i + 8] == '0' && dtb[i + 9] == '_' &&
		    dtb[i + 10] == '0')
			dtb[i + 10] = '1';
		if (dtb[i + 0] == 's' && dtb[i + 1] == 'l' &&
		    dtb[i + 2] == 'o' && dtb[i + 3] == 't' &&
		    dtb[i + 4] == '=' && dtb[i + 5] == 'a')
			dtb[i + 5] = 'b';
	}
}

void linux_boot_main(void)
{
	uint8_t slot = 0;

	pmgr_init_apply_gates();
	hdmi_boot_init();

	if (boot_load_from_nand((void *)IMAGE_PHYS, (void *)DTB_PHYS, &slot) == 0) {
		patch_dtb_slot((uint8_t *)DTB_PHYS, 65536U, slot);
		jump_to_linux(IMAGE_PHYS, DTB_PHYS);
	}

	/* If both NAND slots fail CRC, spin until the hardware watchdog resets */
	for (;;)
		__asm__ volatile("wfi");
}
