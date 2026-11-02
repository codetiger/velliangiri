#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Put the Weston built by tools/weston/build.sh on the device.
#
#   tools/weston/install.sh              # root@192.168.68.112
#   tools/weston/install.sh HOST
#   tools/weston/install.sh --no-cache   # do not leave it on Data
#
# Installs /usr/local/bin/weston, libweston, the DRM backend and GL
# renderer, the kiosk shell, weston-simple-egl/shm, the xkb data and the
# runtime libraries.  A copy of the tarball is left on Data (unless
# --no-cache), and stage2 unpacks it at boot before anything else starts.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
T=$ROOT/build/weston-armv7.tar
DEST=root@192.168.68.112
CACHE=1

for a in "$@"; do
	case $a in
	--no-cache) CACHE=0 ;;
	-*) echo "usage: $0 [--no-cache] [root@]HOST" >&2; exit 2 ;;
	*) DEST=$a ;;
	esac
done

[ -f "$T" ] || { echo "error: $T is not built; run tools/weston/build.sh" >&2; exit 1; }

SSH="ssh -o BatchMode=yes -o ConnectTimeout=8 $DEST"

echo "==> installing Weston in /usr/local on $DEST"
$SSH "tar -xf - -C /" < "$T"

if [ "$CACHE" = 1 ]; then
	if $SSH 'test -d /mnt/data/linux' &&
	   $SSH 'cat > /mnt/data/linux/lib/weston-armv7.tar.tmp' < "$T"; then
		if $SSH 'mv /mnt/data/linux/lib/weston-armv7.tar.tmp /mnt/data/linux/lib/weston-armv7.tar'; then
			echo "==> cached on Data (stage2 unpacks it at the next boot)"
		else
			echo "warning: could not rename the cached tarball" >&2
		fi
	else
		echo "warning: Data is not mounted; Weston will not survive a reboot" >&2
	fi
fi

$SSH "LD_LIBRARY_PATH=/usr/local/lib:/usr/local/lib/weston:/usr/local/lib/libweston-14:/usr/local/lib/sgx-mesa/lib /usr/local/bin/weston --version | head -1"
echo "==> done: $DEST:/usr/local"
