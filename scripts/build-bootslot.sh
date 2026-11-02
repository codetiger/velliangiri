#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Generate per-slot (A/B) prod/debug DTBs with apple,slc-data-mode and partitions,
# and pack the 16 KiB boot manifest page (out/boot-manifest.bin).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/out"
BASE_DTB="${OUT}/s5l8947x-j33iap.dtb"
IMAGE="${OUT}/Image"

if [ ! -f "${BASE_DTB}" ] || [ ! -f "${IMAGE}" ]; then
	echo "ERROR: ${BASE_DTB} or ${IMAGE} missing; run './scripts/velliangiri.sh kernel' first." >&2
	exit 1
fi

make_slot_dtb() {
	local slot="$1"
	local profile="$2"
	local vol="$3"
	local ubiblk="$4"
	local out_dtb="${OUT}/s5l8947x-j33iap-slot-${slot}-${profile}.dtb"
	local bootargs

	if [ "${profile}" = "prod" ]; then
		bootargs="quiet loglevel=4 console=ttyGS0 ubi.mtd=ubi_sys ubi.block=0,${vol} ubi.block=0,firmware root=${ubiblk} rootfstype=squashfs ro rootwait lpj=96000"
	else
		bootargs="earlycon=s3c6400,0x32500000 console=ttySAC3,115200n8 console=ttyGS0 ignore_loglevel initcall_debug ubi.mtd=ubi_sys ubi.block=0,${vol} ubi.block=0,firmware root=${ubiblk} rootfstype=squashfs ro rootwait lpj=96000"
	fi

	FMI_PATH="/soc@30000000/nand-controller@1200000"
	cp "${BASE_DTB}" "${out_dtb}"
	fdtput -t s "${out_dtb}" /chosen bootargs "${bootargs}"
	fdtput -c "${out_dtb}" "${FMI_PATH}/partitions"
	fdtput -t s "${out_dtb}" "${FMI_PATH}" apple,slc-data-mode ""
	fdtput -t s "${out_dtb}" "${FMI_PATH}/partitions" compatible "fixed-partitions"
	fdtput -t i "${out_dtb}" "${FMI_PATH}/partitions" "#address-cells" 2
	fdtput -t i "${out_dtb}" "${FMI_PATH}/partitions" "#size-cells" 2

	# boot: EBs 0..31 (0x00000000 .. 0x04000000, 64 MiB)
	fdtput -c "${out_dtb}" "${FMI_PATH}/partitions/partition@0"
	fdtput -t s "${out_dtb}" "${FMI_PATH}/partitions/partition@0" label "boot"
	fdtput -t i "${out_dtb}" "${FMI_PATH}/partitions/partition@0" reg 0 0x00000000 0 0x04000000

	# ubi_sys: EBs 32..639 (0x04000000 size 0x4c000000, 1216 MiB)
	fdtput -c "${out_dtb}" "${FMI_PATH}/partitions/partition@4000000"
	fdtput -t s "${out_dtb}" "${FMI_PATH}/partitions/partition@4000000" label "ubi_sys"
	fdtput -t i "${out_dtb}" "${FMI_PATH}/partitions/partition@4000000" reg 0 0x04000000 0 0x4c000000

	# ubi_data: EBs 640..2093 (0x50000000 size 0xb5c00000, 2908 MiB)
	fdtput -c "${out_dtb}" "${FMI_PATH}/partitions/partition@50000000"
	fdtput -t s "${out_dtb}" "${FMI_PATH}/partitions/partition@50000000" label "ubi_data"
	fdtput -t i "${out_dtb}" "${FMI_PATH}/partitions/partition@50000000" reg 0 0x50000000 0 0xb5c00000

	if [ "${profile}" = "prod" ]; then
		fdtput -t s "${out_dtb}" /framebuffer@9e400000 status "disabled" 2>/dev/null || true
	fi
}

make_slot_dtb a prod  rootfs_a /dev/ubiblock0_0
make_slot_dtb a debug rootfs_a /dev/ubiblock0_0
make_slot_dtb b prod  rootfs_b /dev/ubiblock0_1
make_slot_dtb b debug rootfs_b /dev/ubiblock0_1

python3 "${ROOT}/scripts/nandboot/mkmanifest.py" \
	--seq 1 \
	--active-slot 0 \
	--image "${IMAGE}" \
	--dtb-a "${OUT}/s5l8947x-j33iap-slot-a-prod.dtb" \
	--dtb-b "${OUT}/s5l8947x-j33iap-slot-b-prod.dtb" \
	-o "${OUT}/boot-manifest.bin"

echo "[bootslot] Generated Slot A/B prod+debug DTBs and ${OUT}/boot-manifest.bin"
