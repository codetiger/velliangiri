#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Put Weston's source at build/weston-src/weston-14.0.0.  Run on the host.
#
#   bash tools/weston/fetch.sh
#
# The tarball is checked by SHA-256, so a changed release is refused.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
VER=14.0.0
SHA=47fd0325b0b948e9b003a38fdf4eb3a8581f3fdc740b8932b35ae8793bf4e4a5
URL="https://gitlab.freedesktop.org/wayland/weston/-/releases/${VER}/downloads/weston-${VER}.tar.xz"
OUT="$ROOT/build/weston-src/weston-$VER"

if [ -d "$OUT" ]; then
	echo "==> build/weston-src/weston-$VER already present"
	exit 0
fi

mkdir -p "$ROOT/build/weston-src"
TMP="$ROOT/build/weston-$VER.tar.xz"
if [ ! -f "$TMP" ] || ! echo "$SHA  $TMP" | sha256sum -c - >/dev/null 2>&1; then
	echo "==> downloading Weston $VER"
	curl -fL --retry 3 -o "$TMP.part" "$URL"
	echo "$SHA  $TMP.part" | sha256sum -c - >/dev/null
	mv "$TMP.part" "$TMP"
fi
tar -xf "$TMP" -C "$ROOT/build/weston-src"
echo "==> done: $OUT"
