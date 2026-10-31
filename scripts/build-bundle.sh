#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Assemble the self-relocating S5L8947X (j33iap) boot bundle:
#   [entry-stub] + [boot-loader @ +0x0000 (padded to 0x8000)] + [zImage @ +0x8000] + [DTB]

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

ZIMAGE_SIZE=$(wc -c < "${ZIMAGE}" | tr -d ' ')
DTB_PHYS=$(printf "0x%08xU" $((0x80008000 + ZIMAGE_SIZE)))

echo "[bundle] Compiling pre-kernel loader (0x80000000, PMGR + 720p HDMI, DTB_PHYS=${DTB_PHYS}) ..."
CFLAGS_BOOT=(-march=armv7-a -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -O2 -Wall -Wextra)
"${CC}" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/boot-start.S" -o "${BOOT_BUILD}/boot-start.o"
"${CC}" "${CFLAGS_BOOT[@]}" -DDTB_PHYS="${DTB_PHYS}" -c "${BOOT_SRC}/boot-loader.c" -o "${BOOT_BUILD}/boot-loader.o"
"${CC}" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/pmgr-init.c" -o "${BOOT_BUILD}/pmgr-init.o"
python3 "${BOOT_SRC}/gen-splash.py" "${ROOT}/logo.svg" > "${BOOT_BUILD}/splash-data.h"
"${CC}" "${CFLAGS_BOOT[@]}" -I"${BOOT_BUILD}" -c "${BOOT_SRC}/splash.c" -o "${BOOT_BUILD}/splash.o"

"${LD}" -z noexecstack -T "${BOOT_SRC}/boot-loader.ld" \
	"${BOOT_BUILD}/boot-start.o" \
	"${BOOT_BUILD}/boot-loader.o" \
	"${BOOT_BUILD}/pmgr-init.o" \
	"${BOOT_BUILD}/splash.o" \
	-o "${BOOT_BUILD}/boot-loader.elf"
"${OBJCOPY}" -O binary "${BOOT_BUILD}/boot-loader.elf" "${BOOT_BUILD}/boot-loader.bin"

# The loader stack grows down from 0x80007000 inside the same 32 KB slot;
# keep at least 4 KB of it clear of the image.
LOADER_SIZE=$(wc -c < "${BOOT_BUILD}/boot-loader.bin" | tr -d ' ')
if [ "${LOADER_SIZE}" -gt 24576 ]; then
	echo "[bundle] ERROR: boot-loader.bin (${LOADER_SIZE} bytes) runs into the stack at +0x6000" >&2
	exit 1
fi

echo "[bundle] Packing inner payload (boot-loader + zImage @ +0x8000 + appended DTB) ..."
INNER="${BOOT_BUILD}/inner-bundle.bin"
rm -f "${INNER}"
truncate -s 32768 "${INNER}"
dd if="${BOOT_BUILD}/boot-loader.bin" of="${INNER}" bs=4096 conv=notrunc 2>/dev/null
cat "${ZIMAGE}" "${DTB}" >> "${INNER}"

# Pad inner bundle to a 32-byte boundary for entry-stub ldmia/stmia copy loop
INNER_RAW=$(wc -c < "${INNER}" | tr -d ' ')
INNER_PAD=$(( (32 - (INNER_RAW % 32)) % 32 ))
if [ "${INNER_PAD}" -gt 0 ]; then
	dd if=/dev/zero bs=1 count="${INNER_PAD}" >> "${INNER}" 2>/dev/null
fi
INNER_SIZE=$(wc -c < "${INNER}" | tr -d ' ')

echo "[bundle] Compiling position-independent entry stub (BUNDLE_SIZE=${INNER_SIZE}) ..."
"${CC}" "${CFLAGS_BOOT[@]}" \
	-DPAYLOAD_PATH="\"${INNER}\"" \
	-DBUNDLE_SIZE="${INNER_SIZE}" \
	-c "${BOOT_SRC}/entry-stub.S" -o "${BOOT_BUILD}/entry-stub.o"
"${LD}" -z noexecstack -T "${BOOT_SRC}/entry-stub.ld" "${BOOT_BUILD}/entry-stub.o" -o "${BOOT_BUILD}/entry-stub.elf"
"${OBJCOPY}" -O binary "${BOOT_BUILD}/entry-stub.elf" "${BUNDLE_OUT}"
cp "${BOOT_BUILD}/boot-loader.bin" "${OUT}/boot-loader.bin"

echo "[bundle] Created self-relocating boot bundle:"
ls -lh "${BUNDLE_OUT}"
