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
| `mesa.patch` | the lines that register the driver with Mesa (a meson option, the subdirectories, the DRM descriptor, the pipe loader, dril) | MIT |
| `gl/` | the test programs: `glclear`, `gltri`, `glfs`, `gltex`, `glblend`, `gldepth`, `glsize` | GPL-2.0-only |
| `container.conf` | the image Mesa is built in: Alpine armv7, musl, the same libraries the device has | — |
| `fetch.sh` | Mesa at the pinned tag into `build/mesa/src` (on the host: git under emulation is far too slow) | GPL-2.0-only |
| `build.sh` | the driver in, configure, build, install, pack (inside the image) | GPL-2.0-only |
| `install.sh` | onto the device, over ssh | GPL-2.0-only |
| `sgx-gl.sh` | on the device (installed as `sgx-gl`): run a program with this Mesa instead of the system's | GPL-2.0-only |

`meson.build` files are kept as `meson.build.txt` here — the source tree
allows only the file types `scripts/check-no-proprietary.sh` lists — and
`build.sh` puts them in place under their real names.

## What this port changed

Three files, and nothing else:

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

## What it needs from the kernel

- UAPI 3: the kernel's parameter buffer and the TA's heap, so the driver
  builds its frame at addresses it asks for.
- The render node accepts buffers from the whole range it reports, and a
  render's buffers are listed in `bo_handles`.
- `APPLE_SGX_PARAM_FB_*` for the screen (the built frame draws the screen's
  background from it).  A render to a resource does not need it.
