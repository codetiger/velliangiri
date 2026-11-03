#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fetch doomgeneric (GPL-2.0) source into build/doom-src/doomgeneric and
# Freedoom Phase 1 IWAD (BSD-3-Clause) into build/dl/doom/freedoom1.wad.
#
#   bash tools/doom/fetch.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DOOM_REF="master"
DOOM_URL="https://github.com/ozkl/doomgeneric/archive/refs/heads/${DOOM_REF}.tar.gz"
DOOM_TAR="${ROOT}/build/dl/doomgeneric-${DOOM_REF}.tar.gz"
DOOM_OUT="${ROOT}/build/doom-src/doomgeneric"

FREEDOOM_VER="0.13.0"
FREEDOOM_URL="https://github.com/freedoom/freedoom/releases/download/v${FREEDOOM_VER}/freedoom-${FREEDOOM_VER}.zip"
FREEDOOM_ZIP="${ROOT}/build/dl/freedoom-${FREEDOOM_VER}.zip"
WAD_DIR="${ROOT}/build/dl/doom"

mkdir -p "${ROOT}/build/dl" "${ROOT}/build/doom-src" "${WAD_DIR}"

if [ ! -d "${DOOM_OUT}/doomgeneric" ]; then
	if [ ! -f "${DOOM_TAR}" ]; then
		echo "==> downloading doomgeneric (${DOOM_REF})"
		curl -fL --retry 3 -o "${DOOM_TAR}.part" "${DOOM_URL}"
		mv "${DOOM_TAR}.part" "${DOOM_TAR}"
	fi
	rm -rf "${DOOM_OUT}"
	mkdir -p "${DOOM_OUT}"
	tar -xzf "${DOOM_TAR}" -C "${DOOM_OUT}" --strip-components=1
	echo "==> extracted: ${DOOM_OUT}"
else
	echo "==> build/doom-src/doomgeneric already present"
fi

if [ ! -s "${WAD_DIR}/freedoom1.wad" ]; then
	if [ ! -f "${FREEDOOM_ZIP}" ]; then
		echo "==> downloading Freedoom v${FREEDOOM_VER} IWAD"
		curl -fL --retry 3 -o "${FREEDOOM_ZIP}.part" "${FREEDOOM_URL}"
		mv "${FREEDOOM_ZIP}.part" "${FREEDOOM_ZIP}"
	fi
	python3 -S - "${FREEDOOM_ZIP}" "${WAD_DIR}/freedoom1.wad" <<'PYEOF'
import sys, zipfile
zip_path, out_path = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(zip_path, "r") as zf:
    for name in zf.namelist():
        if name.endswith("freedoom1.wad"):
            with zf.open(name) as src, open(out_path, "wb") as dst:
                dst.write(src.read())
            break
PYEOF
	echo "==> extracted: ${WAD_DIR}/freedoom1.wad"
else
	echo "==> build/dl/doom/freedoom1.wad already present"
fi

if [ ! -s "${WAD_DIR}/doom1.wad" ]; then
	echo "==> downloading Doom v1.9 Shareware IWAD (doom1.wad)"
	for url in \
		"https://distro.ibiblio.org/pub/linux/distributions/slitaz/sources/packages/d/doom1.wad" \
		"https://raw.githubusercontent.com/Akbar30Bill/DOOM_wads/master/doom1.wad" \
		"https://raw.githubusercontent.com/Daivuk/PureDOOM/master/doom1.wad"; do
		if curl -fL --retry 2 -o "${WAD_DIR}/doom1.wad.part" "${url}"; then
			mv "${WAD_DIR}/doom1.wad.part" "${WAD_DIR}/doom1.wad"
			break
		fi
	done
	ls -lh "${WAD_DIR}/doom1.wad"
else
	echo "==> build/dl/doom/doom1.wad already present"
fi
