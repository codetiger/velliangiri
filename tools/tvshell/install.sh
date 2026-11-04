#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Install /bin/tvshell, /bin/bt, the Plus Jakarta Sans + JetBrains Mono fonts,
# and on-device JSON catalogs (screen.json, supported_models.json, apps.json)
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
DESIGN_DIR="$HERE/design"
DEST="${1:-root@192.168.68.112}"

[ -x "$BIN" ] || { echo "error: $BIN not found; run ./tools/tvshell/build.sh" >&2; exit 1; }

SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 $DEST"

echo "==> installing /bin/tvshell and /bin/bt on $DEST"
$SSH "cat > /bin/tvshell.tmp && chmod 0755 /bin/tvshell.tmp && mv /bin/tvshell.tmp /bin/tvshell" < "$BIN"
if [ -f "$BT_SH" ]; then
	$SSH "cat > /bin/bt.tmp && chmod 0755 /bin/bt.tmp && mv /bin/bt.tmp /bin/bt" < "$BT_SH"
fi

if [ -d "$FONT_DIR" ]; then
	echo "==> installing Plus Jakarta Sans & JetBrains Mono fonts in /usr/share/fonts/tvshell on $DEST"
	$SSH "mkdir -p /usr/share/fonts/tvshell"
	for bn in \
		PlusJakartaSans-Medium.ttf \
		PlusJakartaSans-SemiBold.ttf \
		PlusJakartaSans-Bold.ttf \
		JetBrainsMono-Medium.ttf \
		JetBrainsMono-SemiBold.ttf; do
		f="$FONT_DIR/$bn"
		[ -f "$f" ] || continue
		$SSH "cat > /usr/share/fonts/tvshell/$bn" < "$f"
	done
fi

if [ -d "$DESIGN_DIR" ]; then
	echo "==> installing on-device JSON catalogs in /usr/share/tvshell on $DEST"
	$SSH "mkdir -p /usr/share/tvshell /etc/tvshell"
	for j in screen.json supported_models.json apps.json tokens.json; do
		f="$DESIGN_DIR/$j"
		[ -f "$f" ] || continue
		$SSH "cat > /usr/share/tvshell/$j" < "$f"
	done
fi

PLACEHOLDER_DIR="$ROOT/build/dl/placeholders"
GALLERY_DIR="$ROOT/build/dl/gallery"

if [ -d "$PLACEHOLDER_DIR" ]; then
	echo "==> installing high-res internet placeholders in /usr/share/tvshell/placeholders and /mnt/data/linux/placeholders on $DEST"
	$SSH "mkdir -p /usr/share/tvshell/placeholders /mnt/data/linux/placeholders"
	tar -C "$PLACEHOLDER_DIR" -cf - . | $SSH "tar -C /usr/share/tvshell/placeholders -xf - && cp -a /usr/share/tvshell/placeholders/. /mnt/data/linux/placeholders/"
fi

if [ -d "$GALLERY_DIR" ]; then
	echo "==> installing gallery photos & videos in /mnt/data/linux/media on $DEST"
	$SSH "mkdir -p /mnt/data/linux/media && rm -rf /mnt/data/linux/media/.frames"
	tar -C "$GALLERY_DIR" -cf - . | $SSH "tar -C /mnt/data/linux/media -xf -"
fi

echo "==> done: installed /bin/tvshell on $DEST"
