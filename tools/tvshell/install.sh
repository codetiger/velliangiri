#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Install /bin/tvshell, /bin/media-gallery (symlink), /bin/bt, Plus Jakarta Sans
# + JetBrains Mono fonts, and on-device JSON catalogs onto a running S5L8947X
# device over SSH.
#
# Sample media and placeholder artwork are NOT bundled in built OS images.
# Pass --media or --media-only once to transcode and transfer sample media
# to persistent storage (/mnt/data/linux/media and /mnt/data/linux/placeholders):
#
#   ./tools/tvshell/install.sh [root@HOST]
#   ./tools/tvshell/install.sh --media [root@HOST]
#   ./tools/tvshell/install.sh --media-only [root@HOST]
#
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build/tvshell/tvshell"
BT_SH="$ROOT/initramfs/bt.sh"
FONT_DIR="$ROOT/build/dl/fonts"
DESIGN_DIR="$HERE/design"

INSTALL_APP=1
INSTALL_MEDIA=0
DEST="root@192.168.68.112"

for arg in "$@"; do
	case "$arg" in
	--media)
		INSTALL_MEDIA=1
		;;
	--media-only)
		INSTALL_APP=0
		INSTALL_MEDIA=1
		;;
	-h|--help)
		echo "Usage: $0 [--media | --media-only] [root@HOST]"
		exit 0
		;;
	*)
		DEST="$arg"
		;;
	esac
done

SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8 $DEST"

if [ "$INSTALL_APP" = "1" ]; then
	[ -x "$BIN" ] || { echo "error: $BIN not found; run ./tools/tvshell/build.sh" >&2; exit 1; }

	echo "==> installing /bin/tvshell, /bin/media-gallery -> tvshell, and /bin/bt on $DEST"
	$SSH "cat > /bin/tvshell.tmp && chmod 0755 /bin/tvshell.tmp && cp -f /bin/tvshell.tmp /bin/tvshell.real && mv /bin/tvshell.tmp /bin/tvshell && ln -sf tvshell /bin/media-gallery" < "$BIN"
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
fi

if [ "$INSTALL_MEDIA" = "1" ]; then
	PLACEHOLDER_DIR="$ROOT/build/dl/placeholders"
	GALLERY_DIR="$ROOT/build/dl/gallery"

	if [ ! -f "$PLACEHOLDER_DIR/dawn_ridge.jpg" ] || [ ! -d "$GALLERY_DIR" ]; then
		echo "==> preparing sample media and placeholder artwork via prepare-media.sh"
		"$HERE/prepare-media.sh"
	fi

	if [ -d "$PLACEHOLDER_DIR" ]; then
		echo "==> transferring placeholder artwork to /mnt/data/linux/placeholders on $DEST (one-time)"
		$SSH "mkdir -p /mnt/data/linux/placeholders /usr/share/tvshell/placeholders"
		tar -C "$PLACEHOLDER_DIR" -cf - . | $SSH "tar -C /mnt/data/linux/placeholders -xf - && cp -a /mnt/data/linux/placeholders/. /usr/share/tvshell/placeholders/"
	fi

	if [ -d "$GALLERY_DIR" ]; then
		echo "==> transferring gallery photos & videos to /mnt/data/linux/media on $DEST (one-time)"
		$SSH "mkdir -p /mnt/data/linux/media && rm -rf /mnt/data/linux/media/.frames"
		tar -C "$GALLERY_DIR" -cf - . | $SSH "tar -C /mnt/data/linux/media -xf -"
	fi
fi

echo "==> done ($DEST)"
