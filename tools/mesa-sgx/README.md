# tools/mesa-sgx — the GPU's Mesa driver, built for this port

A Gallium driver for the S5L8947X's SGX543MP1, built into Mesa 26.1.8 and
loaded through the render node that `drivers/gpu/drm/apple-sgx` publishes.
It is the clean-room SGX543 userspace work published with Project Cascadia,
brought over unchanged except for the two things this port does differently
(the event program's path; the kernel header is the tree's own).

| path | what | licence |
|---|---|---|
| `files/src/gallium/drivers/sgx/` | the Gallium driver: screen, context, VDM streams, the template frame, render target data, the USSE compiler, textures | MIT |
| `files/src/gallium/winsys/sgx/drm/` | the winsys: one screen for the render node's fd | MIT |
| `files/src/gallium/drivers/sgx/drm-shim/` | a pretend render node, for running the driver on a PC (optional) | MIT |
| `mesa.patch` | the lines that register the driver with Mesa (a meson option, the subdirectories, the DRM descriptor, the pipe loader, dril, and kmsro) | MIT |
| `gl/` | the test programs: `glclear`, `gltri`, `glfs`, `gltex`, `glblend`, `gldepth`, `glsize`, `gbmtest`, `glclear-gl` | GPL-2.0-only |
| `container.conf` | the image Mesa is built in: Alpine armv7, musl, the same libraries the device has | — |
| `fetch.sh` | Mesa at the pinned tag into `build/mesa/src` (on the host: git under emulation is far too slow) | GPL-2.0-only |
| `build.sh` | the driver in, configure, build, install, pack (inside the image) | GPL-2.0-only |
| `install.sh` | onto the device, over ssh | GPL-2.0-only |
| `sgx-gl.sh` | on the device (installed as `sgx-gl`): run a program with this Mesa instead of the system's | GPL-2.0-only |

`meson.build` files are kept as `meson.build.txt` here — the source tree
allows only the file types `scripts/check-no-proprietary.sh` lists — and
`build.sh` puts them in place under their real names.

## What this port changed

The driver came over unchanged.  What this port adds around it:

- **`sgx_frame.c`** — `event_template()` reads the 3D pass's event program
  from `/lib/firmware/s5l8947x/gl-event.pds` (`SGX_EVENT` names another
  copy) before it looks for a template pack, and `SGX_EVENT_PATH` is that
  path.  The reference reads it out of the pack.  This port has no pack: the
  event program is the only piece of the stock driver a render needs, and
  the boot carves it out of the device's own system image (`stage2` runs
  `glcarve`, then `sgxframe` checks that it draws).  Everything else in the
  frame -- the PDS block, the state area, the VDM stream's tail, the TA
  command, the 3D register block, render target data of any size -- is
  built by `sgx_template.c` and `sgx_rt.c`, as it is in `sgxframe`.
- **`sgx_screen.c`** — the template frame's directory defaults to
  `/lib/firmware/s5l8947x` instead of `/usr/local/lib/sgx2d`, since that is
  where the event program is; and `GL_RENDERER` names the part the kernel
  found ("PowerVR SGX543MP1" here) instead of the reference's fixed
  "SGX543MP2".
- **kmsro** — `sgx_screen.c`, `sgx_resource.c` and the winsys take the
  `struct renderonly` a display's GBM device hands them: a resource bound for
  scanout is allocated contiguous (`APPLE_SGX_BO_CONTIGUOUS`, so its dma-buf
  has a one-entry scatter/gather table), imported into the display at
  creation, and `WINSYS_HANDLE_TYPE_KMS` returns the display's handle.  The
  winsys also exports `sgx_drm_screen_create_renderonly()`, and
  `mesa.patch` teaches kmsro that the `apple_sgx` render node pairs with
  this port's display (`s5l8947x`), including the DRI name the GLX loader
  asks for.

The kernel's interface header is **not** copied from the reference: the
build puts `include/uapi/drm/apple_sgx_drm.h` from this tree into Mesa's
`include/drm-uapi/`, so there is one definition of the UAPI (version 3, with
`BLIT` and the screen's parameters that this port added).

## Building

On the host (git is native here):

```sh
bash tools/mesa-sgx/fetch.sh          # Mesa 26.1.8 into build/mesa/src, once
docker build --platform linux/arm/v7 -t velliangiri-mesa \
    -f tools/mesa-sgx/container.conf tools/mesa-sgx/
```

The build runs in a named volume (`velliangiri-mesa`) because a bind mount
of 12 000 files is slow on a Mac; the source is copied in once:

```sh
docker volume create velliangiri-mesa
docker run --rm --platform linux/arm/v7 \
    -v "$PWD/build/mesa/src":/from:ro -v velliangiri-mesa:/to \
    alpine:3.21 sh -c 'rm -rf /to/src && cp -a /from /to/src'
```

Then, from the repository root:

```sh
docker run --rm --platform linux/arm/v7 -v "$PWD":/workspace \
    -v velliangiri-mesa:/workspace/build/mesa -w /workspace \
    velliangiri-mesa sh tools/mesa-sgx/build.sh
```

`MESA_BUILD=release` for the optimised build (no assertions).  The first
build takes the better part of an hour under ARM emulation; after that only
what changed is compiled again, unless the options changed.

The output is `build/mesa-sgx-install.tar` (the install tree) and, in the
volume, `build/mesa/install/usr/local/lib/sgx-mesa`.

## Installing and running

```sh
tools/mesa-sgx/install.sh             # root@192.168.68.112, /usr/local/lib/sgx-mesa
tools/mesa-sgx/install.sh --system    # ... and /etc/ld-musl-armhf.path makes it the system's GL
```

On the device, with the event program provisioned (it is, on any boot after
the first: `ls -l /lib/firmware/s5l8947x/gl-event.pds`):

```sh
sgx-gl glclear                        # one clear, read back and checked
sgx-gl glclear 100                    # timing
sgx-gl gltri                          # three draws, read back and checked
sgx-gl glfs                           # the fragment compiler's cases
sgx-gl glsize                         # render targets 1x1 to 4096x4096
SGX_DEBUG=frame sgx-gl glclear        # every word a render sets
SGX_DEBUG_DRAW=1 sgx-gl gltri         # the triangles each draw hands the GPU
```

The driver says which way it started:

```
sgx: SGX543MP1 rev 1.2.2, clears on the GPU     <- the template frame is built
sgx: SGX543MP1 rev 1.2.2, clears on the CPU     <- no event program (see dmesg)
```

`SGX_EVENT=FILE` names another event program; `SGX_PACK=DIR` another
template-frame directory.  The driver's own knobs (`SGX_DEBUG`,
`SGX_FRAME`, `SGX_NOCC`, `SGX_DEBUG_DRAW`, ...) are documented in the
reference's `mesa/README.md` and in the headers.

## On the display: EGL/GBM and kmsro

The display device (`/dev/dri/card1`, kernel driver `s5l8947x`) has no 3D
driver of its own.  Mesa's kmsro pairs it with the `apple_sgx` render node:
GBM opens the display, EGL gets a screen that allocates on the render node
and imports the scanout buffers into the display, and a page flip shows
them.  The pairing is in `mesa.patch` (the pipe loader's platform render
devices, kmsro's `apple_sgx` branch, and the `s5l8947x` DRI name for GLX).

`gbmtest` exercises it end to end:

```sh
sgx-gl gbmtest 120                    # 120 frames on HDMI, then the console
sgx-gl gbmtest 120 /dev/dri/card1
```

It keeps the console's mode (it sets none), clears and draws into each
GBM surface buffer, checks one pixel through `glReadPixels`, and flips with
`DRM_MODE_PAGE_FLIP_EVENT`; on exit it puts the console's framebuffer back.
The buffers are SGX render targets the display scans out -- no CPU copy and
no `/dev/fb0`.

The display waits on the render node's fence through the buffer's dma
reservation, so the test does not call `glFinish`; `--finish` is the
comparison and `--quads N` makes a frame heavy enough to show the wait:

```
no glFinish, 100 quads: swap 1.6 ms, flip ioctl 0.1 ms, flip done 390 ms
--finish,   100 quads: glFinish 390.9 ms, flip done 9.0 ms
```

A Wayland compositor on this path is in `tools/weston/` (Weston 14 with the
DRM backend and GL renderer; `weston-run`).

Scanout resources are allocated with `APPLE_SGX_BO_CONTIGUOUS`, because the
display's DMA-GEM import accepts only a one-entry scatter/gather table;
the SGX side needs no change to render into one.  `sgx_resource_get_param()`
reports the stride, offset, layer stride, plane count and the linear
modifier kmsro asks for.  It deliberately does not answer
`PIPE_RESOURCE_PARAM_HANDLE_TYPE_*`: Mesa's DRI image query prefers that
path and would take the boolean for a handle, giving a Wayland client fd 1
instead of its dma-buf.  Without it the query falls through to
`resource_get_handle`, which exports a real fd.

## The desktop build

The default image is OpenGL ES only (`-Dplatforms= -Dglx=disabled
-Dopengl=false`), because the device has neither X nor Wayland.  The
desktop variant adds them:

```sh
MESA_BUILD=desktop bash tools/mesa-sgx/build.sh      # in the container
# or, on the host:
MESA_BUILD=desktop ./scripts/velliangiri.sh mesa
```

It packs `build/mesa-sgx-install-desktop.tar` with `libGL`, GLX, the X11 and
Wayland EGL platforms, desktop OpenGL, and the X11/Wayland libraries they
link against.  `MESA_BUILD=desktop tools/mesa-sgx/install.sh --system`
installs it; `sgx-gl glclear-gl` checks desktop OpenGL through EGL (GLX
itself needs an X server, which this image does not have).  For GLX the
loader needs the megadriver directory:

```sh
LIBGL_DRIVERS_PATH=/usr/local/lib/sgx-mesa/lib/dri
```

`sgx-gl` sets it, along with `GBM_BACKENDS_PATH`.  Verified on the device:
`glclear-gl` reports `GL_VERSION 2.1 Mesa 26.1.8`, `GL_RENDERER PowerVR
SGX543MP1` and clears 0 pixels wrong; `libGL` loads with every dependency
bundled (`libxcb-*`, `libX11`, `libXext`, `libXxf86vm`, `libxshmfence`,
`libbsd`, `libwayland-client`), and `lib/dri/s5l8947x_dri.so` points at the
megadriver.  The desktop tarball is a superset: its `libEGL`/`libGLESv2`
still run the GLES and GBM tests.

## Surviving a reboot

The root filesystem is a ramfs, so `/usr/local/lib/sgx-mesa` is gone after
a reboot.  `install.sh` leaves the same tarball on Data
(`/mnt/data/linux/lib/sgx-mesa-install.tar`, with `--no-cache` to skip it),
and `stage2` unpacks it before any GL program runs and makes its `lib` the
system's GL.  The unpack is skipped when the install is already there, which
is the case only if a boot somehow kept the ramfs.  Verified on hardware
after a DFU boot: stage2 logged "Mesa userspace unpacked from cache" and
`sgx-gl glclear`, `gbmtest` and `glclear-gl` ran with no reinstall.

## What it needs from the kernel

- UAPI 3: the kernel's parameter buffer and the TA's heap, so the driver
  builds its frame at addresses it asks for.
- The render node accepts buffers from the whole range it reports, and a
  render's buffers are listed in `bo_handles`.
- `APPLE_SGX_PARAM_FB_*` for the screen (the built frame draws the screen's
  background from it).  A render to a resource does not need it.
