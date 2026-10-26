#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Fetch and extract mainline Linux 6.12 LTS source tree into build/linux

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KERNEL_VERSION="${KERNEL_VERSION:-6.12.79}"
KERNEL_MAJOR="${KERNEL_VERSION%%.*}"
TARBALL="linux-${KERNEL_VERSION}.tar.xz"
URL="https://cdn.kernel.org/pub/linux/kernel/v${KERNEL_MAJOR}.x/${TARBALL}"

DL_DIR="${ROOT}/build/dl"
TREE_DIR="${KERNEL_TREE:-${ROOT}/build/linux}"

mkdir -p "${DL_DIR}" "$(dirname "${TREE_DIR}")"

if [ -f "${TREE_DIR}/Makefile" ]; then
	echo "[fetch-kernel] Kernel tree already present at ${TREE_DIR}"
	exit 0
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
