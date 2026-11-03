#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Install the cross-compiled S5L8947X Doom binary (/bin/doom) and Freedoom
# Phase 1 IWAD (/mnt/data/linux/games/doom/freedoom1.wad) onto a running device.
#
#   ./tools/doom/install.sh [root@HOST]
#
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BIN="$ROOT/build/doom/doom"
DOOM1_WAD="$ROOT/build/dl/doom/doom1.wad"
WAD="$ROOT/build/dl/doom/freedoom1.wad"
DEST="${1:-root@192.168.68.112}"

[ -x "$BIN" ] || { echo "error: $BIN not found; run ./tools/doom/build.sh" >&2; exit 1; }

SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 $DEST"

echo "==> installing /bin/doom on $DEST"
$SSH "cat > /bin/doom.tmp && chmod 0755 /bin/doom.tmp && mv /bin/doom.tmp /bin/doom" < "$BIN"

if [ -f "$DOOM1_WAD" ]; then
	echo "==> installing doom1.wad on $DEST"
	$SSH "mkdir -p /mnt/data/linux/games/doom/saves /usr/share/games/doom"
	if $SSH "test -d /mnt/data/linux" ; then
		$SSH "cat > /mnt/data/linux/games/doom/doom1.wad.tmp && mv /mnt/data/linux/games/doom/doom1.wad.tmp /mnt/data/linux/games/doom/doom1.wad" < "$DOOM1_WAD"
	else
		$SSH "cat > /usr/share/games/doom/doom1.wad" < "$DOOM1_WAD"
	fi
elif [ -f "$WAD" ]; then
	echo "==> installing freedoom1.wad on $DEST"
	$SSH "mkdir -p /mnt/data/linux/games/doom/saves /usr/share/games/doom"
	if $SSH "test -d /mnt/data/linux" ; then
		$SSH "cat > /mnt/data/linux/games/doom/freedoom1.wad.tmp && mv /mnt/data/linux/games/doom/freedoom1.wad.tmp /mnt/data/linux/games/doom/freedoom1.wad" < "$WAD"
	else
		$SSH "cat > /usr/share/games/doom/freedoom1.wad" < "$WAD"
	fi
fi

echo "==> done: launch from tvshell or run '/bin/doom' on $DEST"
