# tools/weston — a compositor on the SGX's EGL/GBM path

Weston 14 built for the S5L8947X, with the DRM backend, the GL renderer, the
kiosk shell and the simple clients, running against the Mesa in
`tools/mesa-sgx` (EGL/GLES/GBM through kmsro).  It puts a Wayland desktop on
HDMI and composites its clients with the SGX.

| path | what | licence |
|---|---|---|
| `fetch.sh` | Weston 14.0.0 source into `build/weston-src`, checked by SHA-256 | GPL-2.0-only |
| `build.sh` | the build: Alpine armv7 container, minimal options, `/usr/local` tree with its runtime libraries, packed as `build/weston-armv7.tar` | GPL-2.0-only |
| `install.sh` | onto the device, `/usr/local`, and a copy on `Data` for `stage2` | GPL-2.0-only |
| `run.sh` | starts `seatd`, Weston on `card1`, a client, and returns the console | GPL-2.0-only |

## Why these options

Weston's stock package in Alpine has no `drm-backend.so` (the backend is not
in the package), so it cannot drive a KMS device.  This build turns on only
what this board needs:

- DRM backend, GL renderer, kiosk shell, `weston-simple-shm`/`-egl`;
- no desktop shell (cairo/pango), no Xwayland, no pipewire/rdp/vnc/remoting,
  no lcms, no tests.

The compositor keeps the console's 1280x720 mode.  `seatd` is started by
`run.sh` because the image's libseat has no built-in backend; it is the
small daemon that grants the DRM/input seat to Weston.

## Building and installing

```sh
bash tools/weston/fetch.sh                    # once, on the host
./scripts/velliangiri.sh weston               # or the docker line in build.sh
tools/weston/install.sh                       # /usr/local + a copy on Data
```

`build/mesa-sgx-install.tar` must exist (build it with
`./scripts/velliangiri.sh mesa` first): Weston is compiled and run against
the SGX Mesa, not Alpine's.

`install.sh` leaves the tarball at
`/mnt/data/linux/lib/weston-armv7.tar`; `stage2` unpacks it at boot after
the Mesa install, so the compositor survives a reboot in the same way.

## Running

```sh
weston-run 20                       # 20 s of weston-simple-shm on HDMI
weston-run 20 weston-simple-shm
weston-run 20 weston-terminal       # needs a font; not installed here
```

The screen is `card1`; the compositor's buffers are SGX render targets the
display imports and scans out (kmsro), and its GL renderer is the SGX.  The
console comes back when the compositor exits (a blank/unblank makes fbcon
repaint).

## What is not there yet

- **EGL clients.**  `weston-simple-egl` starts, chooses a config
  (`r8g8b8a0`), and its dmabufs reach Weston with a file descriptor that is
  not the buffer's (the compositor sees a duplicate of the client's stdout),
  so Weston rejects the import with `invalid stride` and the client exits.
  Shm clients (`weston-simple-shm`) work: the compositor uploads their
  buffers and renders them on the GPU, and the scanout buffer shows the
  pattern.  This needs the Wayland platform's image export path sorted out
  (a client-side Mesa issue, not the compositor).
- **Input.**  The board has no USB host mode yet, so Weston starts with
  `--continue-without-input`; there is no keyboard or mouse.
- **Fonts.**  `weston-terminal` runs but has no font package installed, so
  it has nothing to draw text with.
- **Xwayland/desktop shell.**  Not built; the desktop would need an X server
  and the cairo/pango pieces.
