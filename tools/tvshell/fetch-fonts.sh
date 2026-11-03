#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fetch SIL Open Font License (OFL) static TrueType fonts for the TV Shell
# into build/dl/fonts/:
#   - Literata 400 (Regular) & 600 (SemiBold) for the Paper theme
#   - Atkinson Hyperlegible 400 (Regular) & 700 (Bold) for the Signal theme
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FONT_DIR="${ROOT}/build/dl/fonts"
mkdir -p "${FONT_DIR}"

fetch_font() {
	local name="$1"
	local url="$2"
	local dst="${FONT_DIR}/${name}"
	if [ ! -s "${dst}" ]; then
		echo "[fonts] Downloading ${name} ..."
		curl -fL --retry 3 -o "${dst}.part" "${url}"
		mv "${dst}.part" "${dst}"
	fi
}

fetch_font "Literata-Regular.ttf" \
	"https://raw.githubusercontent.com/googlefonts/literata/main/fonts/ttf/Literata-Regular.ttf"
fetch_font "Literata-SemiBold.ttf" \
	"https://raw.githubusercontent.com/googlefonts/literata/main/fonts/ttf/Literata-SemiBold.ttf"
fetch_font "AtkinsonHyperlegible-Regular.ttf" \
	"https://raw.githubusercontent.com/google/fonts/main/ofl/atkinsonhyperlegible/AtkinsonHyperlegible-Regular.ttf"
fetch_font "AtkinsonHyperlegible-Bold.ttf" \
	"https://raw.githubusercontent.com/google/fonts/main/ofl/atkinsonhyperlegible/AtkinsonHyperlegible-Bold.ttf"

echo "[fonts] Ready in ${FONT_DIR}"
