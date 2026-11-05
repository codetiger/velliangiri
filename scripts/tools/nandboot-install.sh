#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Verified A/B kernel slot + SquashFS rootfs volume installer for S5L8947X

set -eu

usage() {
	cat >&2 <<'EOF'
Usage: nandboot-install --slot <a|b> --image <Image> --dtb-prod <prod.dtb> \
                        --dtb-debug <debug.dtb> --rootfs <rootfs.sqsh> \
                        --manifest <boot-manifest.bin>
EOF
	exit 1
}

SLOT=""
IMAGE=""
DTB_PROD=""
DTB_DEBUG=""
ROOTFS=""
MANIFEST=""

while [ $# -gt 0 ]; do
	case "$1" in
		--slot)      SLOT="$2"; shift 2 ;;
		--image)     IMAGE="$2"; shift 2 ;;
		--dtb-prod)  DTB_PROD="$2"; shift 2 ;;
		--dtb-debug) DTB_DEBUG="$2"; shift 2 ;;
		--rootfs)    ROOTFS="$2"; shift 2 ;;
		--manifest)  MANIFEST="$2"; shift 2 ;;
		*)           usage ;;
	esac
done

[ -n "$SLOT" ] && [ -f "$IMAGE" ] && [ -f "$DTB_PROD" ] && [ -f "$DTB_DEBUG" ] &&
	[ -f "$ROOTFS" ] && [ -f "$MANIFEST" ] || usage

# Require SLC data-area mode so EB offsets map 1:1 to interleaved SLC blocks
if [ "$(cat /sys/module/s5l8947x_fmi/parameters/slc_mode 2>/dev/null)" != "Y" ]; then
	echo "nandboot-install: ERROR: s5l8947x_fmi.slc_mode must be enabled (Y)" >&2
	exit 1
fi

BOOT_MTD=""
for sysm in /sys/class/mtd/mtd*; do
	[ -f "$sysm/name" ] || continue
	if [ "$(cat "$sysm/name")" = "boot" ]; then
		BOOT_MTD="/dev/${sysm##*/}"
		break
	fi
done

if [ -z "$BOOT_MTD" ] || [ ! -c "$BOOT_MTD" ]; then
	echo "nandboot-install: ERROR: 'boot' MTD partition not found" >&2
	exit 1
fi

case "$SLOT" in
	a)
		SLOT_EB_START=2
		UBI_VOL="/dev/ubi0_0"
		;;
	b)
		SLOT_EB_START=16
		UBI_VOL="/dev/ubi0_1"
		;;
	*)
		echo "nandboot-install: ERROR: --slot must be 'a' or 'b'" >&2
		exit 1
		;;
esac

# 1. Update and verify the inactive UBI rootfs volume first
ROOTFS_SIZE=$(wc -c < "$ROOTFS" | tr -d ' ')
ROOTFS_SHA=$(sha256sum "$ROOTFS" | awk '{print $1}')
echo "[nandboot-install] Writing $ROOTFS ($ROOTFS_SIZE bytes) -> $UBI_VOL ..."
ubiupdatevol "$UBI_VOL" "$ROOTFS"
VERIFY_SHA=$(dd if="$UBI_VOL" bs=131072 count=$(( (ROOTFS_SIZE + 131071) / 131072 )) 2>/dev/null |
	head -c "$ROOTFS_SIZE" | sha256sum | awk '{print $1}')
if [ "$VERIFY_SHA" != "$ROOTFS_SHA" ]; then
	echo "nandboot-install: ERROR: SHA-256 mismatch on $UBI_VOL ($VERIFY_SHA != $ROOTFS_SHA)" >&2
	exit 1
fi
echo "[nandboot-install] Verified $UBI_VOL SHA-256: $VERIFY_SHA"

# 2. Write kernel Image + prod/debug DTBs into the slot's 2 MiB SLC eraseblocks
# Slot layout (matches mkmanifest.py):
#   Image:     SLOT_EB_START + 0 .. +12
#   DTB prod:  SLOT_EB_START + 13 page 0
#   DTB debug: SLOT_EB_START + 13 page 4
EB_BYTES=2097152
PAGE_BYTES=16384
SLOT_OFFSET=$(( SLOT_EB_START * EB_BYTES ))
DTB_PROD_OFFSET=$(( (SLOT_EB_START + 13) * EB_BYTES ))
DTB_DEBUG_OFFSET=$(( DTB_PROD_OFFSET + 4 * PAGE_BYTES ))

echo "[nandboot-install] Erasing and writing $IMAGE -> $BOOT_MTD @ 0x$(printf '%x' $SLOT_OFFSET) ..."
flash_erase "$BOOT_MTD" "$SLOT_OFFSET" 14
nandwrite -p -s "$SLOT_OFFSET" "$BOOT_MTD" "$IMAGE"
nandwrite -p -s "$DTB_PROD_OFFSET" "$BOOT_MTD" "$DTB_PROD"
nandwrite -p -s "$DTB_DEBUG_OFFSET" "$BOOT_MTD" "$DTB_DEBUG"

IMG_SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
IMG_SHA=$(sha256sum "$IMAGE" | awk '{print $1}')
READ_IMG_SHA=$(dd if="$BOOT_MTD" bs=16384 skip=$(( SLOT_OFFSET / 16384 )) \
	count=$(( (IMG_SIZE + 16383) / 16384 )) 2>/dev/null |
	head -c "$IMG_SIZE" | sha256sum | awk '{print $1}')
if [ "$READ_IMG_SHA" != "$IMG_SHA" ]; then
	echo "nandboot-install: ERROR: kernel Image readback SHA-256 mismatch!" >&2
	exit 1
fi
echo "[nandboot-install] Verified kernel Image SHA-256: $READ_IMG_SHA"

# 3. Write manifest copies to EB 0 (offset 0) and EB 1 (offset 2 MiB)
echo "[nandboot-install] Erasing and committing manifest to EB 0 and EB 1 ..."
flash_erase "$BOOT_MTD" 0 2
nandwrite -p -s 0 "$BOOT_MTD" "$MANIFEST"
nandwrite -p -s "$EB_BYTES" "$BOOT_MTD" "$MANIFEST"
echo "[nandboot-install] Slot '$SLOT' installation complete and verified."
