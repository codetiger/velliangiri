#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fetch SIL Open Font License (OFL) static TrueType fonts for Giri on
# Velliangiri OS (v1.2 TV Design System) into build/dl/fonts/:
#   - Plus Jakarta Sans 500 (Medium), 600 (SemiBold), 700 (Bold)
#   - JetBrains Mono 500 (Medium), 600 (SemiBold)
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
		curl -fsSL --retry 3 -o "${dst}.part" "${url}"
		mv "${dst}.part" "${dst}"
	fi
}

fetch_font "PlusJakartaSans-Medium.ttf" \
	"https://raw.githubusercontent.com/tokotype/PlusJakartaSans/master/fonts/ttf/PlusJakartaSans-Medium.ttf"
fetch_font "PlusJakartaSans-SemiBold.ttf" \
	"https://raw.githubusercontent.com/tokotype/PlusJakartaSans/master/fonts/ttf/PlusJakartaSans-SemiBold.ttf"
fetch_font "PlusJakartaSans-Bold.ttf" \
	"https://raw.githubusercontent.com/tokotype/PlusJakartaSans/master/fonts/ttf/PlusJakartaSans-Bold.ttf"
fetch_font "JetBrainsMono-Medium.ttf" \
	"https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/fonts/ttf/JetBrainsMono-Medium.ttf"
fetch_font "JetBrainsMono-SemiBold.ttf" \
	"https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/fonts/ttf/JetBrainsMono-SemiBold.ttf"

echo "[fonts] Ready in ${FONT_DIR}"
