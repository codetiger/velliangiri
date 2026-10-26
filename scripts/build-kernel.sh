#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Cross-compile mainline Linux 6.12 zImage + s5l8947x-j33iap.dtb

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="${KERNEL_TREE:-${ROOT}/build/linux}"
OUT="${ROOT}/out"
CONFIG_BASE="${ROOT}/config/s5l8947x.config"
CONFIG_TRIM="${ROOT}/config/s5l8947x-trim.config"
INITRAMFS_CPIO="${ROOT}/build/initramfs-s5l8947x.cpio"

export ARCH=arm
export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
CLEAN=0

while [ $# -gt 0 ]; do
	case "$1" in
		--clean) CLEAN=1; shift ;;
		-j) JOBS="$2"; shift 2 ;;
		*) echo "Unknown argument: $1" >&2; exit 2 ;;
	esac
done

bash "${ROOT}/scripts/fetch-kernel.sh"
mkdir -p "${OUT}"

echo "[kernel] Overlaying S5L8947X platform & drivers onto ${TREE} ..."
python3 "${ROOT}/scripts/apply-kernel-tree.py" --tree "${TREE}"

if [ "${CLEAN}" = 1 ]; then
	echo "[kernel] Running make mrproper ..."
	make -C "${TREE}" mrproper
fi

echo "[kernel] Generating .config (multi_v7_defconfig + s5l8947x.config + s5l8947x-trim.config) ..."
make -C "${TREE}" multi_v7_defconfig >/dev/null
"${TREE}/scripts/kconfig/merge_config.sh" -m -O "${TREE}" \
	"${TREE}/.config" "${CONFIG_BASE}" "${CONFIG_TRIM}" >/dev/null

if [ -f "${INITRAMFS_CPIO}" ]; then
	echo "[kernel] Embedding uncompressed initramfs (${INITRAMFS_CPIO}) for single-stream XZ ..."
	FRAG="$(mktemp)"
	{
		echo "CONFIG_INITRAMFS_SOURCE=\"${INITRAMFS_CPIO}\""
		echo "CONFIG_INITRAMFS_COMPRESSION_NONE=y"
	} > "${FRAG}"
	"${TREE}/scripts/kconfig/merge_config.sh" -m -O "${TREE}" \
		"${TREE}/.config" "${FRAG}" >/dev/null
	rm -f "${FRAG}"
fi

make -C "${TREE}" olddefconfig >/dev/null

echo "[kernel] Verifying required S5L8947X config symbols ..."
awk -f "${ROOT}/scripts/check-config-fragment.awk" "${CONFIG_BASE}" "${TREE}/.config"

echo "[kernel] Compiling zImage and s5l8947x-j33iap.dtb (-j${JOBS}) ..."
rm -f "${TREE}/arch/arm/boot/dts/apple/s5l8947x-j33iap.dtb"
make -C "${TREE}" -j"${JOBS}" zImage dtbs

cp "${TREE}/arch/arm/boot/zImage" "${OUT}/zImage"
cp "${TREE}/arch/arm/boot/dts/apple/s5l8947x-j33iap.dtb" "${OUT}/s5l8947x-j33iap.dtb"
cp "${TREE}/.config" "${OUT}/kernel.config"

echo "[kernel] Build complete:"
ls -lh "${OUT}/zImage" "${OUT}/s5l8947x-j33iap.dtb"
