#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Generate design token & SVG bitmap headers and cross-compile /bin/tvshell
# into build/tvshell/tvshell.
#
#   bash tools/tvshell/build.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${ROOT}/build/tvshell"

mkdir -p "${OUT_DIR}"
python3 -S "${ROOT}/scripts/gen-shell-assets.py" "${ROOT}/tools/tvshell/design" "${OUT_DIR}"

CC_BIN=""
for cand in \
	"${CROSS_CC:-}" \
	"/opt/homebrew/bin/armv7-linux-musleabihf-gcc" \
	"armv7-linux-musleabihf-gcc" \
	"arm-linux-musleabihf-gcc" \
	"arm-linux-gnueabihf-gcc"; do
	if [ -n "${cand}" ] && command -v "${cand}" >/dev/null 2>&1; then
		CC_BIN="${cand}"
		break
	fi
done

if [ -z "${CC_BIN}" ]; then
	echo "error: no ARMv7 Linux cross-compiler found" >&2
	exit 1
fi

echo "==> compiling tvshell with ${CC_BIN}"
"${CC_BIN}" \
	-O2 -mcpu=cortex-a9 -mfpu=neon \
	-Wall -Wextra -Werror \
	-static -D_GNU_SOURCE \
	-I"${ROOT}/tools/tvshell" \
	-I"${OUT_DIR}" \
	"${ROOT}/tools/tvshell/tvshell.c" \
	"${ROOT}/tools/tvshell/shell_display.c" \
	"${ROOT}/tools/tvshell/shell_font.c" \
	"${ROOT}/tools/tvshell/shell_input.c" \
	"${ROOT}/tools/tvshell/shell_media.c" \
	-o "${OUT_DIR}/tvshell" \
	-lm

"${CC_BIN%-gcc}-strip" "${OUT_DIR}/tvshell" 2>/dev/null || true
ls -lh "${OUT_DIR}/tvshell"
