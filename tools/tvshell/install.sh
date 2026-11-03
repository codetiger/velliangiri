#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Install /bin/tvshell, /bin/bt, and the Literata + Atkinson Hyperlegible fonts
# onto a running S5L8947X device over SSH.
#
#   ./tools/tvshell/install.sh [root@HOST]
#
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build/tvshell/tvshell"
BT_SH="$ROOT/initramfs/bt.sh"
FONT_DIR="$ROOT/build/dl/fonts"
DEST="${1:-root@192.168.68.112}"

[ -x "$BIN" ] || { echo "error: $BIN not found; run ./tools/tvshell/build.sh" >&2; exit 1; }

SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 $DEST"

echo "==> installing /bin/tvshell and /bin/bt on $DEST"
$SSH "cat > /bin/tvshell.tmp && chmod 0755 /bin/tvshell.tmp && mv /bin/tvshell.tmp /bin/tvshell" < "$BIN"
$SSH "cat > /bin/bt.tmp && chmod 0755 /bin/bt.tmp && mv /bin/bt.tmp /bin/bt" < "$BT_SH"

if [ -d "$FONT_DIR" ]; then
	echo "==> installing fonts in /usr/share/fonts/tvshell on $DEST"
	$SSH "mkdir -p /usr/share/fonts/tvshell"
	for f in "$FONT_DIR"/*.ttf; do
		[ -f "$f" ] || continue
		bn="$(basename "$f")"
		$SSH "cat > /usr/share/fonts/tvshell/$bn" < "$f"
	done
fi

echo "==> done: installed /bin/tvshell on $DEST"
