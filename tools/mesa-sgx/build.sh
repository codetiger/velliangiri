#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Mesa with the SGX543's driver, for the J33.  Runs inside the
# velliangiri-mesa image (tools/mesa-sgx/container.conf).
#
#   tools/mesa-sgx/build.sh            # assertions on (default)
#   MESA_BUILD=release tools/mesa-sgx/build.sh
#
# The result is build/mesa/install/usr/local/lib/sgx-mesa: libEGL,
# libGLESv2, libgbm and libgallium with only the sgx driver in it, plus the
# test programs.  Mesa itself must be fetched on the host first
# (tools/mesa-sgx/fetch.sh): git under emulation is far too slow.
set -eu
ROOT=/workspace
# Mesa's tree and the build itself live in a named volume (a bind mount of
# 12 000 files is slow on a Mac); MESA_DIR names it
B=${MESA_DIR:-/workspace/build/mesa}
COMMIT=$(sed -n 's/^COMMIT=\([0-9a-f]*\).*/\1/p' "$ROOT/tools/mesa-sgx/fetch.sh")
PREFIX=/usr/local/lib/sgx-mesa

say() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
# the tree was checked out by the host's git: its index has the host's inode
# numbers, so compare files by size and time only, or git would read every
# file again (slow, under emulation)
g() { git -c safe.directory='*' -c core.checkStat=minimal -c core.trustctime=false -C "$B/src" "$@"; }

[ "$(g rev-parse -q --verify HEAD 2>/dev/null)" = "$COMMIT" ] \
	|| fail "build/mesa/src is not Mesa at $COMMIT -- run tools/mesa-sgx/fetch.sh on the host"

say "the render node's interface into the tree"
# one source of truth: the kernel's own header, with the ioctls this port
# added (BLIT, the screen's parameters)
mkdir -p "$B/src/include/drm-uapi"
if ! cmp -s "$ROOT/include/uapi/drm/apple_sgx_drm.h" \
            "$B/src/include/drm-uapi/apple_sgx_drm.h"; then
	cp "$ROOT/include/uapi/drm/apple_sgx_drm.h" \
	   "$B/src/include/drm-uapi/apple_sgx_drm.h"
	printf '    include/drm-uapi/apple_sgx_drm.h\n'
fi

say "the sgx driver into the tree"
# copy only what changed, so ninja rebuilds only that; the build files are
# kept as *.build.txt in this tree (a source tree holds no other names)
( cd "$ROOT/tools/mesa-sgx/files" && find . -type f ) | while read -r f; do
	dst=${f%.build.txt}
	[ "$dst" = "$f" ] || dst=$dst.build
	if ! cmp -s "$ROOT/tools/mesa-sgx/files/$f" "$B/src/$dst"; then
		mkdir -p "$(dirname "$B/src/$dst")"
		cp "$ROOT/tools/mesa-sgx/files/$f" "$B/src/$dst"
		printf '    %s\n' "${dst#./}"
	fi
done

# the glue: applied once; an older version of it is taken out first
if ! g apply --reverse --check "$ROOT/tools/mesa-sgx/mesa.patch" 2>/dev/null; then
	changed=$(g diff --name-only)
	[ -z "$changed" ] || g checkout -q -- $changed
	g apply "$ROOT/tools/mesa-sgx/mesa.patch"
	printf '    applied tools/mesa-sgx/mesa.patch\n'
fi

# -mtls-dialect=gnu: left to itself, Mesa's meson.build finds that the
# compiler takes -mtls-dialect=gnu2 (TLS descriptors) and builds with it,
# and on 32-bit ARM the current GL context then comes back wrong: garbage
# (glGetString NULL, "Inside glBegin/glEnd", a bus error), a crash in
# _mesa_make_current.  The classic dialect is right.
if [ "${MESA_BUILD:-}" = release ]; then
	BT="--buildtype=release -Db_ndebug=true"; BD=$B/build-release; ID=$B/install-release
else
	BT=--buildtype=debugoptimized; BD=$B/build; ID=$B/install
fi
OPTS="--prefix=$PREFIX --libdir=lib $BT
    -Dc_args=-mtls-dialect=gnu -Dcpp_args=-mtls-dialect=gnu
    -Dgallium-drivers=sgx,softpipe -Dvulkan-drivers= -Dplatforms=
    -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles1=enabled -Dgles2=enabled
    -Dopengl=false -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled
    -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=enabled -Dexpat=enabled
    -Dshader-cache=disabled -Dtools= -Dbuild-tests=false -Dgallium-va=disabled
    -Dgallium-rusticl=false -Dvideo-codecs= -Dteflon=false -Dlmsensors=disabled
    -Dmicrosoft-clc=disabled -Dspirv-tools=disabled"
if [ ! -f "$BD/build.ninja" ]; then
	say "configuring"
	# shellcheck disable=SC2086
	meson setup "$BD" "$B/src" $OPTS >/dev/null
	printf '%s\n' "$OPTS" > "$BD/.velliangiri-options"
elif [ "$(cat "$BD/.velliangiri-options" 2>/dev/null)" != "$OPTS" ]; then
	say "configuring again (the options changed: everything is built again)"
	# shellcheck disable=SC2086
	meson setup --reconfigure "$BD" "$B/src" $OPTS >/dev/null
	printf '%s\n' "$OPTS" > "$BD/.velliangiri-options"
fi

say "building (the first time is slow under emulation)"
ninja -C "$BD"
rm -rf "$ID"
DESTDIR="$ID" ninja -C "$BD" install >/dev/null

I=$ID$PREFIX
say "stripping the libraries' debug information ($(du -sh "$I/lib" | cut -f1) before)"
find "$I/lib" -type f -name '*.so*' -exec strip --strip-debug {} + 2>/dev/null || true
# the device's root is minimal: libdrm and the C++ runtime are not in it, so
# they go beside Mesa's own libraries, where LD_LIBRARY_PATH finds them
for l in libdrm.so.2 libexpat.so.1 libgcc_s.so.1 libstdc++.so.6 libz.so.1; do
	cp -L "/usr/lib/$l" "$I/lib/" 2>/dev/null || true
done
mkdir -p "$I/bin"
say "the test programs"
for t in glclear gltri glfs gltex glblend gldepth glsize; do
	cc -O2 -Wall -I"$I/include" "$ROOT/tools/mesa-sgx/gl/$t.c" \
	   -L"$I/lib" -lEGL -lGLESv2 -lm \
	   -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/$t"
done
cp "$ROOT/tools/mesa-sgx/sgx-gl.sh" "$I/bin/sgx-gl"
# out of the volume and onto the host, for tools/mesa-sgx/install.sh
tar -C "$ID" -cf "${TARBALL:-/workspace/build/mesa-sgx-install.tar}" usr
say "built: $I"
say "packed: ${TARBALL:-/workspace/build/mesa-sgx-install.tar}"
