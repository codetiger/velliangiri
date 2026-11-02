#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Build the standalone ~8-12 KiB Stage-1 NAND boot loader (out/nandboot.bin)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${ROOT}/out"
BOOT_SRC="${ROOT}/scripts/boot"
NAND_SRC="${ROOT}/scripts/nandboot"
CROSS="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

mkdir -p "${OUT}"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

CFLAGS_BOOT=(-march=armv7-a -marm -mfloat-abi=soft -ffreestanding -fno-builtin -fno-stack-protector -Os -Wall -Wextra)

"${CROSS}gcc" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/boot-start.S" -o "${TMP}/boot-start.o"
"${CROSS}gcc" "${CFLAGS_BOOT[@]}" -c "${BOOT_SRC}/pmgr-init.c" -o "${TMP}/pmgr-init.o"
"${CROSS}gcc" "${CFLAGS_BOOT[@]}" -c "${NAND_SRC}/crc32.c" -o "${TMP}/crc32.o"
"${CROSS}gcc" "${CFLAGS_BOOT[@]}" -c "${NAND_SRC}/fmi-read.c" -o "${TMP}/fmi-read.o"
"${CROSS}gcc" "${CFLAGS_BOOT[@]}" -c "${NAND_SRC}/manifest.c" -o "${TMP}/manifest.o"
"${CROSS}gcc" "${CFLAGS_BOOT[@]}" -c "${NAND_SRC}/nandboot-loader.c" -o "${TMP}/nandboot-loader.o"

"${CROSS}ld" -z noexecstack -T "${BOOT_SRC}/boot-loader.ld" \
	"${TMP}/boot-start.o" \
	"${TMP}/pmgr-init.o" \
	"${TMP}/crc32.o" \
	"${TMP}/fmi-read.o" \
	"${TMP}/manifest.o" \
	"${TMP}/nandboot-loader.o" \
	-o "${TMP}/nandboot-loader.elf"

"${CROSS}objcopy" -O binary "${TMP}/nandboot-loader.elf" "${TMP}/payload.bin"

PAYLOAD_SIZE="$(wc -c < "${TMP}/payload.bin" | tr -d ' ')"
if [ "${PAYLOAD_SIZE}" -gt 24576 ]; then
	echo "[nandboot] ERROR: payload.bin (${PAYLOAD_SIZE} bytes) exceeds 24 KB stack limit" >&2
	exit 1
fi
PAD=$(( (32 - (PAYLOAD_SIZE % 32)) % 32 ))
if [ "${PAD}" -gt 0 ]; then
	dd if=/dev/zero bs=1 count="${PAD}" >> "${TMP}/payload.bin" 2>/dev/null
fi
PAYLOAD_SIZE="$(wc -c < "${TMP}/payload.bin" | tr -d ' ')"

"${CROSS}gcc" "${CFLAGS_BOOT[@]}" \
	-DPAYLOAD_PATH="\"${TMP}/payload.bin\"" \
	-DBUNDLE_SIZE="${PAYLOAD_SIZE}" \
	-c "${BOOT_SRC}/entry-stub.S" -o "${TMP}/entry-stub.o"
"${CROSS}ld" -z noexecstack -T "${BOOT_SRC}/entry-stub.ld" \
	"${TMP}/entry-stub.o" -o "${TMP}/entry-stub.elf"
"${CROSS}objcopy" -O binary "${TMP}/entry-stub.elf" "${OUT}/nandboot.bin"

echo "[nandboot] Built ${OUT}/nandboot.bin ($(wc -c < "${OUT}/nandboot.bin" | tr -d ' ') bytes)"
