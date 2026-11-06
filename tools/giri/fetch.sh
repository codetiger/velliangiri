#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Fetch third-party inputs for Giri (never committed):
#   - llama.cpp source (MIT) at a pinned release tag -> build/giri-src/llama.cpp
#   - SmolLM2 Instruct GGUF weights (Apache-2.0)     -> build/dl/giri/models
#
#   bash tools/giri/fetch.sh            # source + default bench models
#   GIRI_MODELS="135M-Instruct-Q8_0" bash tools/giri/fetch.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LLAMA_REF="${LLAMA_REF:-b12138}"
LLAMA_URL="https://github.com/ggml-org/llama.cpp/archive/refs/tags/${LLAMA_REF}.tar.gz"
LLAMA_TAR="${ROOT}/build/dl/llama.cpp-${LLAMA_REF}.tar.gz"
LLAMA_OUT="${ROOT}/build/giri-src/llama.cpp"
MODEL_DIR="${ROOT}/build/dl/giri/models"

GIRI_MODELS="${GIRI_MODELS:-135M-Instruct-Q8_0 135M-Instruct-Q4_0 360M-Instruct-Q8_0 360M-Instruct-Q4_0}"

mkdir -p "${ROOT}/build/dl" "${ROOT}/build/giri-src" "${MODEL_DIR}"

if [ ! -f "${LLAMA_OUT}/.ref-${LLAMA_REF}" ]; then
	if [ ! -f "${LLAMA_TAR}" ]; then
		echo "==> downloading llama.cpp ${LLAMA_REF}"
		curl -fL --retry 3 -o "${LLAMA_TAR}.part" "${LLAMA_URL}"
		mv "${LLAMA_TAR}.part" "${LLAMA_TAR}"
	fi
	rm -rf "${LLAMA_OUT}"
	mkdir -p "${LLAMA_OUT}"
	tar -xzf "${LLAMA_TAR}" -C "${LLAMA_OUT}" --strip-components=1
	for p in "${ROOT}/tools/giri/patches/"*.patch; do
		[ -f "${p}" ] || continue
		echo "==> applying $(basename "${p}")"
		patch -d "${LLAMA_OUT}" -p1 --forward < "${p}"
	done
	touch "${LLAMA_OUT}/.ref-${LLAMA_REF}"
	echo "==> extracted: ${LLAMA_OUT}"
else
	echo "==> llama.cpp ${LLAMA_REF} already present"
fi

# musl (MIT) sources matching the cross toolchain's libc.a, used only to
# rebuild the few libm routines that toolchain ships compiled for VFPv4.
MUSL_VER="${MUSL_VER:-1.2.4}"
MUSL_TAR="${ROOT}/build/dl/musl-${MUSL_VER}.tar.gz"
MUSL_OUT="${ROOT}/build/giri-src/musl"
if [ ! -f "${MUSL_OUT}/.ver-${MUSL_VER}" ]; then
	if [ ! -f "${MUSL_TAR}" ]; then
		echo "==> downloading musl ${MUSL_VER}"
		curl -fL --retry 3 -o "${MUSL_TAR}.part" \
			"https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"
		mv "${MUSL_TAR}.part" "${MUSL_TAR}"
	fi
	rm -rf "${MUSL_OUT}"
	mkdir -p "${MUSL_OUT}"
	tar -xzf "${MUSL_TAR}" -C "${MUSL_OUT}" --strip-components=1
	touch "${MUSL_OUT}/.ver-${MUSL_VER}"
	echo "==> extracted: ${MUSL_OUT}"
fi

for m in ${GIRI_MODELS}; do
	size="${m%%-*}"
	f="SmolLM2-${m}.gguf"
	if [ ! -s "${MODEL_DIR}/${f}" ]; then
		echo "==> downloading ${f}"
		curl -fL --retry 3 -o "${MODEL_DIR}/${f}.part" \
			"https://huggingface.co/bartowski/SmolLM2-${size}-Instruct-GGUF/resolve/main/${f}"
		mv "${MODEL_DIR}/${f}.part" "${MODEL_DIR}/${f}"
	fi
done
ls -lh "${MODEL_DIR}"
