#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Serve a GGUF model from the development host (e.g. a Mac) to tvshell on the
# LAN, using llama.cpp's OpenAI-compatible llama-server built natively from the
# same pinned source as the device tools (Metal on Apple silicon).
#
#   bash tools/giri/fetch.sh                       # once: source (+ models)
#   bash tools/giri/serve-host.sh MODEL.gguf [PORT] [ALIAS]
#   ./tools/tvshell/llm-config.sh remote http://<host-ip>:PORT ALIAS
#
# Listens on all interfaces so the device can reach it; stop it with Ctrl-C.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${ROOT}/build/giri-src/llama.cpp"
OUT="${ROOT}/build/giri/host"
MODEL="${1:?usage: $0 MODEL.gguf [PORT] [ALIAS]}"
PORT="${2:-8080}"
ALIAS="${3:-$(basename "${MODEL}" .gguf)}"

[ -f "${SRC}/CMakeLists.txt" ] || { echo "error: run tools/giri/fetch.sh first" >&2; exit 1; }
[ -f "${MODEL}" ] || { echo "error: ${MODEL} not found" >&2; exit 1; }

if [ ! -x "${OUT}/bin/llama-server" ]; then
	echo "==> building native llama-server in ${OUT}"
	cmake -S "${SRC}" -B "${OUT}" -DCMAKE_BUILD_TYPE=Release \
		-DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF -DLLAMA_BUILD_TESTS=OFF >/dev/null
	cmake --build "${OUT}" -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" --target llama-server
fi

for ifc in en0 en1; do
	ip="$(ipconfig getifaddr "${ifc}" 2>/dev/null || true)"
	[ -n "${ip}" ] && echo "==> device URL: http://${ip}:${PORT}  (model alias: ${ALIAS})"
done
exec "${OUT}/bin/llama-server" -m "${MODEL}" --host 0.0.0.0 --port "${PORT}" \
	-c 4096 --alias "${ALIAS}"
