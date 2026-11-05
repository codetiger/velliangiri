#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Fetch and extract mainline Linux 7.x source tree into build/linux

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-7.3.1}"
KERNEL_MAJOR="${KERNEL_VERSION%%.*}"
TARBALL="linux-${KERNEL_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_MAJOR}.x/${TARBALL}"

DL_DIR="${ROOT}/build/dl"
TREE_DIR="${KERNEL_TREE:-${ROOT}/build/linux}"

mkdir -p "${DL_DIR}" "$(dirname "${TREE_DIR}")"

if [ -f "${TREE_DIR}/Makefile" ]; then
	EXISTING_VER="$(awk '
		/^VERSION =/ { v=$3 }
		/^PATCHLEVEL =/ { p=$3 }
		/^SUBLEVEL =/ { s=$3 }
		END { if (s == "" || s == "0") printf "%s.%s\n", v, p; else printf "%s.%s.%s\n", v, p, s }
	' "${TREE_DIR}/Makefile")"
	if [ "${EXISTING_VER}" = "${KERNEL_VERSION}" ] || [ "${EXISTING_VER}.0" = "${KERNEL_VERSION}" ]; then
		echo "[fetch-kernel] Kernel tree ${EXISTING_VER} already present at ${TREE_DIR}"
		exit 0
	fi
	echo "[fetch-kernel] Replacing existing kernel tree (${EXISTING_VER} -> ${KERNEL_VERSION}) at ${TREE_DIR} ..."
	rm -rf "${TREE_DIR}"
fi

if [ ! -f "${DL_DIR}/${TARBALL}" ]; then
	echo "[fetch-kernel] Downloading ${URL} ..."
	curl -fL --retry 3 -o "${DL_DIR}/${TARBALL}.part" "${URL}"
	mv "${DL_DIR}/${TARBALL}.part" "${DL_DIR}/${TARBALL}"
fi

echo "[fetch-kernel] Extracting ${TARBALL} -> ${TREE_DIR} ..."
rm -rf "${TREE_DIR}.tmp"
mkdir -p "${TREE_DIR}.tmp"
tar --no-same-owner --no-same-permissions -xf "${DL_DIR}/${TARBALL}" --strip-components=1 -C "${TREE_DIR}.tmp"
mv "${TREE_DIR}.tmp" "${TREE_DIR}"
echo "[fetch-kernel] Linux ${KERNEL_VERSION} ready at ${TREE_DIR}"
