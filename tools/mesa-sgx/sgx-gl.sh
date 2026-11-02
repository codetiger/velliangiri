#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# sgx-gl PROGRAM [ARGS] -- run PROGRAM with the Mesa that has the SGX's
# driver instead of the system's.
#
#   sgx-gl glclear 100 --fb
#   sgx-gl gltri
#   SGX_EVENT=/tmp/gl-event.pds sgx-gl glclear
#
# SGX_DEBUG=frame says what each render is, SGX_DEBUG=fps the frame rate.
P=${SGX_MESA:-/usr/local/lib/sgx-mesa}
export LD_LIBRARY_PATH="$P/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GBM_BACKENDS_PATH="$P/lib/gbm"
export PATH="$P/bin:$PATH"
if [ "$SGX_PRELOAD" = 1 ]; then
	export LD_PRELOAD="$(ls "$P"/lib/libgallium-*.so) $P/lib/libEGL.so.1 $P/lib/libGLESv2.so.2${LD_PRELOAD:+ $LD_PRELOAD}"
fi
exec "$@"
