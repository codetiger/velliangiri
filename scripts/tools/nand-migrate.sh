#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# One-time migration utility for S5L8947X SLC UBI layout (ubi_sys + ubi_data).
# Requires s5l8947x_fmi.slc_mode=1 and an explicit --confirm-destructive-erase flag.

set -eu

CONFIRM=0
ROOTFS_SQSH=""
FW_SQSH=""
STATE_TAR=""

while [ $# -gt 0 ]; do
	case "$1" in
		--confirm-destructive-erase) CONFIRM=1; shift ;;
		--rootfs)  ROOTFS_SQSH="$2"; shift 2 ;;
		--firmware) FW_SQSH="$2"; shift 2 ;;
		--state-tar) STATE_TAR="$2"; shift 2 ;;
		*)
			echo "Usage: nand-migrate --confirm-destructive-erase --rootfs <rootfs.sqsh> --firmware <firmware.sqsh> [--state-tar <state.tar>]" >&2
			exit 1
			;;
	esac
done

if [ "$CONFIRM" != 1 ]; then
	echo "nand-migrate: ERROR: --confirm-destructive-erase is required." >&2
	exit 1
fi

if [ ! -s "$ROOTFS_SQSH" ] || [ ! -s "$FW_SQSH" ]; then
	echo "nand-migrate: ERROR: --rootfs and --firmware images must exist and be non-empty." >&2
	exit 1
fi

if [ "$(cat /sys/module/s5l8947x_fmi/parameters/slc_mode 2>/dev/null)" != "Y" ]; then
	echo "nand-migrate: ERROR: s5l8947x_fmi.slc_mode must be Y (boot with apple,slc-data-mode)." >&2
	exit 1
fi

find_mtd_num() {
	local want="$1"
	for sysm in /sys/class/mtd/mtd*; do
		[ -f "$sysm/name" ] || continue
		if [ "$(cat "$sysm/name")" = "$want" ]; then
			echo "${sysm##*mtd}"
			return 0
		fi
	done
	return 1
}

SYS_MTD_NUM=$(find_mtd_num "ubi_sys")
DATA_MTD_NUM=$(find_mtd_num "ubi_data")

echo "[nand-migrate] Formatting ubi_sys (/dev/mtd${SYS_MTD_NUM}) and attaching as ubi0 ..."
ubiformat -y "/dev/mtd${SYS_MTD_NUM}"
ubiattach -m "$SYS_MTD_NUM" -d 0

# Pinned volume IDs on ubi0 (ubi_sys):
#   vol 0: rootfs_a (static, 256 MiB) -> /dev/ubiblock0_0
#   vol 1: rootfs_b (static, 256 MiB) -> /dev/ubiblock0_1
#   vol 2: firmware (static,  64 MiB) -> /dev/ubiblock0_2
#   vol 3: state    (dynamic, 512 MiB) -> ubi0:state
ubimkvol /dev/ubi0 -n 0 -N rootfs_a -t static -s 256MiB
ubimkvol /dev/ubi0 -n 1 -N rootfs_b -t static -s 256MiB
ubimkvol /dev/ubi0 -n 2 -N firmware -t static -s 64MiB
ubimkvol /dev/ubi0 -n 3 -N state    -t dynamic -s 512MiB

echo "[nand-migrate] Populating rootfs_a, rootfs_b, and firmware volumes ..."
ubiupdatevol /dev/ubi0_0 "$ROOTFS_SQSH"
ubiupdatevol /dev/ubi0_1 "$ROOTFS_SQSH"
ubiupdatevol /dev/ubi0_2 "$FW_SQSH"

mkdir -p /mnt/state
mount -t ubifs ubi0:state /mnt/state
mkdir -p /mnt/state/etc/upper /mnt/state/etc/work /mnt/state/var/upper /mnt/state/var/work
if [ -n "$STATE_TAR" ] && [ -s "$STATE_TAR" ]; then
	tar -xf "$STATE_TAR" -C /mnt/state
fi
sync
umount /mnt/state

echo "[nand-migrate] Formatting ubi_data (/dev/mtd${DATA_MTD_NUM}) and attaching as ubi1 ..."
ubiformat -y "/dev/mtd${DATA_MTD_NUM}"
ubiattach -m "$DATA_MTD_NUM" -d 1
ubimkvol /dev/ubi1 -n 0 -N data -t dynamic -m
sync

echo "[nand-migrate] Migration of ubi_sys (ubi0) and ubi_data (ubi1) complete."
