#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Phase 0: cross-compile llama.cpp benchmark tools for the S5L8947X
# (single Cortex-A9, ARMv7-A NEON, VFPv3, no FMA) as static musl binaries.
#
#   bash tools/giri/fetch.sh
#   bash tools/giri/build-bench.sh      # -> build/giri/bench/bin/
#
# A/B baseline without the Giri ARMv7 kernels:
#   OUT=build/giri/bench-base EXTRA_CFLAGS=-DGGML_ARMV7_DISABLE \
#	TARGETS=llama-completion bash tools/giri/build-bench.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${ROOT}/build/giri-src/llama.cpp"
OUT="${OUT:-${ROOT}/build/giri/bench}"
CROSS="${CROSS:-armv7-linux-musleabihf}"
ARCH_FLAGS="-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -marm"
TARGETS="${TARGETS:-llama-bench llama-completion llama-simple}"

[ -f "${SRC}/CMakeLists.txt" ] || { echo "error: run tools/giri/fetch.sh first" >&2; exit 1; }
command -v "${CROSS}-gcc" >/dev/null || { echo "error: ${CROSS}-gcc not found" >&2; exit 1; }

# Overlay the Giri ARMv7 NEON kernels used by patches 0003/0006 (copied only
# when changed so make does not rebuild needlessly).
overlay() {
	cmp -s "${ROOT}/tools/giri/ggml/$1" "${SRC}/ggml/src/ggml-cpu/$2" ||
		cp "${ROOT}/tools/giri/ggml/$1" "${SRC}/ggml/src/ggml-cpu/$2"
}
overlay armv7-kernels.h arch/arm/armv7-kernels.h
overlay armv7-f32.h armv7-f32.h

mkdir -p "${OUT}/compat"
# Toolchain compatibility objects, linked ahead of libc.a/libgcc.a:
#  - futex_shim: std::future futex helpers missing from baseline-ARMv7 libstdc++
#  - vfp3_shim + musl libm rebuilds: the toolchain's copies use VFPv4 FMA,
#    which the Cortex-A9 (VFPv3) does not implement (SIGILL).
MUSL="${ROOT}/build/giri-src/musl"
[ -d "${MUSL}/src/math" ] || { echo "error: musl source missing, run tools/giri/fetch.sh" >&2; exit 1; }
COMPAT=""
# shellcheck disable=SC2086
"${CROSS}-gcc" ${ARCH_FLAGS} -O2 -Wall -Wextra -Werror \
	-c "${ROOT}/tools/giri/futex_shim.c" -o "${OUT}/compat/futex_shim.o"
# shellcheck disable=SC2086
"${CROSS}-gcc" ${ARCH_FLAGS} -O2 -Wall -Wextra -Werror \
	-c "${ROOT}/tools/giri/vfp3_shim.c" -o "${OUT}/compat/vfp3_shim.o"
for f in log log2 pow fma fmaf; do
	# shellcheck disable=SC2086
	"${CROSS}-gcc" ${ARCH_FLAGS} -std=c99 -O2 -ffreestanding -fno-builtin \
		-D_XOPEN_SOURCE=700 -I"${MUSL}/arch/arm" -I"${MUSL}/arch/generic" \
		-I"${MUSL}/src/include" -I"${MUSL}/src/internal" -I"${MUSL}/include" \
		-c "${MUSL}/src/math/${f}.c" -o "${OUT}/compat/musl_${f}.o"
done
for o in "${OUT}"/compat/*.o; do
	COMPAT="${COMPAT} ${o}"
done

cmake -S "${SRC}" -B "${OUT}" -G "Unix Makefiles" \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_SYSTEM_PROCESSOR=armv7-a \
	-DCMAKE_C_COMPILER="${CROSS}-gcc" \
	-DCMAKE_CXX_COMPILER="${CROSS}-g++" \
	-DCMAKE_C_FLAGS="${ARCH_FLAGS} ${EXTRA_CFLAGS:-}" \
	-DCMAKE_CXX_FLAGS="${ARCH_FLAGS} ${EXTRA_CFLAGS:-}" \
	-DCMAKE_EXE_LINKER_FLAGS="-static${COMPAT}" \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_OPENMP=OFF \
	-DGGML_LLAMAFILE=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_OPENSSL=OFF \
	-DLLAMA_BUILD_TESTS="${LLAMA_BUILD_TESTS:-OFF}" \
	-DLLAMA_BUILD_SERVER=OFF

# shellcheck disable=SC2086
cmake --build "${OUT}" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" --target ${TARGETS}

# Refuse binaries that still contain VFPv4-only instructions (FMA, fp16 cvt).
for t in ${TARGETS}; do
	bad="$("${CROSS}-objdump" -d "${OUT}/bin/${t}" |
		awk '/^[0-9a-f]+ <.*>:$/ {f=$2} /\tv(fn?m[as]|cvt[bt])\./ {print f}' | sort -u)"
	if [ -n "${bad}" ]; then
		echo "error: ${t} contains VFPv4 instructions in:" ${bad} >&2
		exit 1
	fi
	"${CROSS}-strip" "${OUT}/bin/${t}"
done
ls -lh "${OUT}/bin/"
