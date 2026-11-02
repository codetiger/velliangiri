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
# a buffer the display can scan out on the SGX is contiguous (CMA), and
# kmsro pairs the render node with the display device for EGL/GBM; the
# desktop build adds X11/Wayland platforms, GLX and desktop OpenGL
if [ "${MESA_BUILD:-}" = release ]; then
	BT="--buildtype=release -Db_ndebug=true"; BD=$B/build-release; ID=$B/install-release
	TAR=${TARBALL:-/workspace/build/mesa-sgx-install-release.tar}
elif [ "${MESA_BUILD:-}" = desktop ]; then
	BT="--buildtype=release -Db_ndebug=true"; BD=$B/build-desktop; ID=$B/install-desktop
	TAR=${TARBALL:-/workspace/build/mesa-sgx-install-desktop.tar}
else
	BT=--buildtype=debugoptimized; BD=$B/build; ID=$B/install
	TAR=${TARBALL:-/workspace/build/mesa-sgx-install.tar}
fi
if [ "${MESA_BUILD:-}" = desktop ]; then
	PLATFORM="-Dplatforms=x11,wayland -Dglx=dri -Dopengl=true"
else
	PLATFORM="-Dplatforms= -Dglx=disabled -Dopengl=false"
fi
OPTS="--prefix=$PREFIX --libdir=lib $BT
    -Dc_args=-mtls-dialect=gnu -Dcpp_args=-mtls-dialect=gnu
    -Dgallium-drivers=sgx,softpipe -Dvulkan-drivers= $PLATFORM
    -Degl=enabled -Dgbm=enabled -Dgles1=enabled -Dgles2=enabled
    -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled
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
if [ "${MESA_BUILD:-}" = desktop ]; then
	# the X11 and Wayland libraries libGL/libEGL link against: the device
	# has neither (GLX needs an X server in any case, but libGL has to
	# load, and EGL's X11/Wayland platforms have to find their clients)
	for l in libX11.so.6 libX11-xcb.so.1 libXau.so.6 libXdmcp.so.6 \
	         libXext.so.6 libXfixes.so.3 libXxf86vm.so.1 \
	         libxcb.so.1 libxcb-glx.so.0 libxcb-randr.so.0 libxcb-shm.so.0 \
	         libxcb-dri3.so.0 libxcb-present.so.0 libxcb-sync.so.1 \
	         libxcb-xfixes.so.0 libxshmfence.so.1 libbsd.so.0 libmd.so.0 \
	         libxkbcommon.so.0 libwayland-client.so.0 \
	         libwayland-server.so.0 libffi.so.8; do
		cp -L "/usr/lib/$l" "$I/lib/" 2>/dev/null || true
	done
fi
mkdir -p "$I/bin"
say "the test programs"
for t in glclear gltri glfs gltex glblend gldepth glsize glcubes; do
	cc -O2 -Wall -I"$I/include" "$ROOT/tools/mesa-sgx/gl/$t.c" \
	   -L"$I/lib" -lEGL -lGLESv2 -lm \
	   -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/$t"
done
# the EGL/GBM surface test (kmsro): libgbm and libdrm are in every build
cc -O2 -Wall -I"$I/include" -I/usr/include/libdrm \
   "$ROOT/tools/mesa-sgx/gl/gbmtest.c" \
   -L"$I/lib" -lEGL -lGLESv2 -lgbm -ldrm -lm \
   -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/gbmtest"
if [ "${MESA_BUILD:-}" = desktop ]; then
	# desktop GL through EGL: GLX itself needs an X server, which the
	# device does not have (the libraries are installed all the same)
	cc -O2 -Wall -I"$I/include" "$ROOT/tools/mesa-sgx/gl/glclear-gl.c" \
	   -L"$I/lib" -lEGL -lGL -lm \
	   -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/glclear-gl"
fi
cp "$ROOT/tools/mesa-sgx/sgx-gl.sh" "$I/bin/sgx-gl"
# out of the volume and onto the host, for tools/mesa-sgx/install.sh
tar -C "$ID" -cf "$TAR" usr
say "built: $I"
say "packed: $TAR"
