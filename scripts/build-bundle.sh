#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Assemble the self-relocating S5L8947X (j33iap) boot bundle:
#   [entry-stub] + [boot-loader @ +0x0000] + [zImage @ +0x8000] + [DTB @ +0x2000000]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT_SRC="${ROOT}/scripts/boot"
BOOT_BUILD="${ROOT}/build/boot"
OUT="${ROOT}/out"

ZIMAGE="${OUT}/zImage"
DTB="${OUT}/s5l8947x-j33iap.dtb"
BUNDLE_OUT="${OUT}/boot-bundle-s5l8947x-j33iap.bin"

CROSS="${CROSS_COMPILE:-arm-linux-gnueabihf-}"
CC="${CROSS}gcc"
LD="${CROSS}ld"
OBJCOPY="${CROSS}objcopy"

[ -f "${ZIMAGE}" ] || { echo "[bundle] Missing ${ZIMAGE} (run build-kernel.sh first)" >&2; exit 1; }
[ -f "${DTB}" ] || { echo "[bundle] Missing ${DTB} (run build-kernel.sh first)" >&2; exit 1; }

mkdir -p "${BOOT_BUILD}" "${OUT}"

echo "[bundle] Compiling pre-kernel loader (0x80000000, PMGR + 720p HDMI) ..."
CFLAGS_BOOT=(-march=armv7-a -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -O2 -Wall -Wextra)
"${CC}" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/boot-start.S" -o "${BOOT_BUILD}/boot-start.o"
"${CC}" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/boot-loader.c" -o "${BOOT_BUILD}/boot-loader.o"
"${CC}" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/pmgr-init.c" -o "${BOOT_BUILD}/pmgr-init.o"

"${LD}" -T "${BOOT_SRC}/boot-loader.ld" \
	"${BOOT_BUILD}/boot-start.o" \
	"${BOOT_BUILD}/boot-loader.o" \
	"${BOOT_BUILD}/pmgr-init.o" \
	-o "${BOOT_BUILD}/boot-loader.elf"
"${OBJCOPY}" -O binary "${BOOT_BUILD}/boot-loader.elf" "${BOOT_BUILD}/boot-loader.bin"

LOADER_SIZE=$(wc -c < "${BOOT_BUILD}/boot-loader.bin" | tr -d ' ')
if [ "${LOADER_SIZE}" -gt 32768 ]; then
	echo "[bundle] ERROR: boot-loader.bin (${LOADER_SIZE} bytes) exceeds 32 KB slot" >&2
	exit 1
fi

ZIMAGE_SIZE=$(wc -c < "${ZIMAGE}" | tr -d ' ')
DTB_OFFSET=$((0x2000000))
if [ $((0x8000 + ZIMAGE_SIZE)) -gt "${DTB_OFFSET}" ]; then
	echo "[bundle] ERROR: zImage (${ZIMAGE_SIZE} bytes) overlaps DTB slot at +0x2000000" >&2
	exit 1
fi

echo "[bundle] Packing inner payload (boot-loader + zImage @ +0x8000 + DTB @ +0x2000000) ..."
INNER="${BOOT_BUILD}/inner-bundle.bin"
rm -f "${INNER}"
truncate -s "${DTB_OFFSET}" "${INNER}"
dd if="${BOOT_BUILD}/boot-loader.bin" of="${INNER}" bs=4096 conv=notrunc 2>/dev/null
dd if="${ZIMAGE}" of="${INNER}" bs=32768 seek=1 conv=notrunc 2>/dev/null
cat "${DTB}" >> "${INNER}"

# Pad inner bundle to a 32-byte boundary for entry-stub ldmia/stmia copy loop
INNER_RAW=$(wc -c < "${INNER}" | tr -d ' ')
INNER_PAD=$(( (32 - (INNER_RAW % 32)) % 32 ))
if [ "${INNER_PAD}" -gt 0 ]; then
	dd if=/dev/zero bs=1 count="${INNER_PAD}" >> "${INNER}" 2>/dev/null
fi
INNER_SIZE=$(wc -c < "${INNER}" | tr -d ' ')

echo "[bundle] Compiling position-independent entry stub (BUNDLE_SIZE=${INNER_SIZE}) ..."
"${CC}" "${CFLAGS_BOOT[@]}" \
	-DBUNDLE_SIZE="${INNER_SIZE}" \
	-c "${BOOT_SRC}/entry-stub.S" -o "${BOOT_BUILD}/entry-stub.o"
"${LD}" -T "${BOOT_SRC}/entry-stub.ld" "${BOOT_BUILD}/entry-stub.o" -o "${BOOT_BUILD}/entry-stub.elf"
"${OBJCOPY}" -O binary "${BOOT_BUILD}/entry-stub.elf" "${BOOT_BUILD}/entry-stub.bin"

cat "${BOOT_BUILD}/entry-stub.bin" "${INNER}" > "${BUNDLE_OUT}"
cp "${BOOT_BUILD}/boot-loader.bin" "${OUT}/boot-loader.bin"

echo "[bundle] Created self-relocating boot bundle:"
ls -lh "${BUNDLE_OUT}"
