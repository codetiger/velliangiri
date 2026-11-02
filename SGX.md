# SGX543 GPU: command submission and the render node

The S5L8947X's PowerVR SGX543 is no longer just a powered block that can be
identified.  `drivers/gpu/drm/apple-sgx/` now boots the device's own GPU
microkernel, speaks its command protocol, and exposes a DRM render node for
userspace.  This page describes what is implemented, how it fits together,
and what remains before a program can draw through OpenGL.

The driver is a clean-room port of the SGX543 work first published in
Project Cascadia (GPL-2.0-only), adapted to this board.  No vendor binary is
part of this repository: the microkernel is carved from the device's own
system image at first boot, as the Wi-Fi and Bluetooth firmware already are.

## What works

| Piece | Status |
|---|---|
| Power and clocks | GFX_SYS and GFX power states (PMGR gates 7 and 8), master reset, module clock gating |
| Register access | Six 16 KiB banks; bank 0 is write-only broadcast, bank 1 the master, bank 2 + n core n.  Reads of bank 0 and of absent cores are refused |
| GPU MMU | Two-level page table (4 KiB pages, one directory shared by every requestor) over cache-consistent, uncached buffers |
| Microkernel | Booted from `s5l8947x/sgx543.fw`, which `stage2` carves with `sgxcarve`; handshake checked |
| Kernel CCB | 256-entry command ring; power, performance-counter, transfer and TA commands |
| Transfer queue | One queue with a 64 KiB command buffer; a dummy transfer and a real 2D blit have both run through it |
| Render queue | One TA + 3D queue with a 64 KiB command buffer; used by the render node and debugfs.  Exercised on hardware: `sgxframe` clears 1280x720 and 640x360 through the TA and the 3D pass |
| Render node | `/dev/dri/renderD128`, driver `apple_sgx`: GEM buffers, syncobjs, a DRM scheduler, fences and timeouts |
| Screen | the display pipe's live scanout is mapped at `0x90000000` and reported by `APPLE_SGX_PARAM_FB_*`; `BLIT` with `dst_handle` 0 copies onto it with no CPU copy |
| Mesa / OpenGL ES 2.0 | the SGX Gallium driver in `tools/mesa-sgx/`, built into Mesa 26.1.8 for Alpine armv7 (musl) and installed as the system GL; clears, draws, the fragment compiler, textures, blending, depth and render targets of any size verified on the device |
| EGL/GBM on the display | kmsro pairs the render node with the `s5l8947x` display: GBM surfaces are contiguous SGX buffers the display imports and scans out, flipped with page-flip events (`gbmtest`, 300 frames on hardware) |
| Desktop GL (opt-in build) | `MESA_BUILD=desktop`: GLX, the X11/Wayland EGL platforms and desktop OpenGL; desktop GL 2.1 verified through EGL on the device (GLX needs an X server, which the image has not) |
| Mesa install across reboots | `install.sh` keeps the tarball on Data; `stage2` unpacks it at boot and makes it the system GL |
| Debug interface | `/sys/kernel/debug/apple-sgx/{regs,boot,cmd,mem}` |
| Test tools | `sgxinfo` prints the render node's parameters; `sgxgem` creates, maps and checks a GEM buffer (`-c` for a contiguous, dma-buf-exportable one, `-i` to import a display buffer); `sgx2d` exercises the 2D engine; `sgxpresent` puts a buffer on the HDMI output (`-s` draws on the live screen); `sgxframe` runs one clear through the TA/3D queue; `hwtest gpu` checks the handshake, a CCB command, the render node and the screen blit |

### Verified on hardware (2026-11-02, `j33iap`)

- **OpenGL ES 2.0 runs on the GPU through Mesa.**  The Gallium driver from
  the clean-room SGX543 work (MIT, `tools/mesa-sgx/`) builds into Mesa
  26.1.8 as a Gallium driver read from the render node, and the device's
  system GL is now that Mesa:

  | program | what it does | result |
  |---|---|---|
  | `glclear` | a clear on the GPU, read back and checked | 0 wrong, 55-69 ms a clear with its read-back |
  | `gltri` | three draws: a colour per vertex, a uniform, a mat2 | 11 of 11 checks |
  | `glfs` | the fragment compiler's 40 cases (arithmetic, branches, loops, arrays, uniforms) | 40 of 40 |
  | `glsize` | render targets 1x1 to 4096x4096 | 15 of 15 sizes |
  | `gltex` | textures: minification, NPOT, linear | 14 of 14 cases |
  | `glblend` | blending, the colour mask, repeated renders | 14 of 14 cases |
  | `gldepth` | the depth test within and across renders | 12 of 12 cases |

  The driver builds its frame with `sgx_template.c`/`sgx_rt.c` and takes
  only the event program from the stock driver.  `GL_RENDERER` is
  "PowerVR SGX543MP1" -- the kernel's own core count, not the reference's
  fixed MP2.
- **EGL/GBM works through kmsro.**  `gbmtest` created a 1280x720 GBM
  surface, got an EGL window surface and a GLES 2 context, cleared and drew
  into the scanout buffer (a blue pixel read back at the triangle's top),
  and flipped 300 frames with `DRM_MODE_PAGE_FLIP_EVENT`; the console's
  framebuffer came back on exit.  No CPU copy and no `/dev/fb0`.
- **The Mesa install survives a reboot.**  After a DFU boot the initramfs
  has no `/usr/local/lib/sgx-mesa`; `stage2` unpacked the tarball left on
  Data ("Mesa userspace unpacked from cache"), made its `lib` the system's
  GL, and `sgx-gl glclear`, `gbmtest` and `glclear-gl` all ran without
  reinstalling.
- **Desktop OpenGL is in the opt-in image.**  `MESA_BUILD=desktop` adds
  `-Dplatforms=x11,wayland -Dglx=dri -Dopengl=true` and the X11/Wayland
  libraries the libraries link against.  On the device `glclear-gl`
  created a desktop OpenGL 2.1 context through surfaceless EGL, cleared 0
  pixels wrong, and reported `GL_RENDERER PowerVR SGX543MP1`.  GLX itself
  cannot be exercised without an X server, but `libGL` and its xcb/X11
  dependencies load (no missing libraries), and its DRI name for this
  display (`s5l8947x_dri.so`) is installed.
- The TA/3D render queue runs: `sgxframe` builds the template frame and a
  render target's data, submits a full-screen clear through `SUBMIT`, and
  reads back every checked pixel right at 1280x720 and 640x360, twice in a
  row.  It uses the stock graphics driver's event PDS program (0xa8 bytes,
  five data rows and 22 instructions) carved from the device's own system
  image; its SHA-256 is `a1b855d41993968b40005e886f7ac11c8c3abb42aa0987e005eec4e6cb2a2285`,
  the same template the reference extracts.  The stock driver keeps two
  adjacent records in `IMGSGX543GLDriver`'s `__TEXT,__const`; only the
  second draws, so `stage2` tries each carved candidate with `sgxframe` and
  keeps the one that does.

The device reports an **SGX543MP1, one core, revision 1.2.2** (`CORE_ID
0x01194101`, `CORE_REVISION 0x00010202`).  With the driver in the boot
bundle:

- The microkernel boots from the carved firmware: `host[0] = 0x00000001`
  about 5 ms after the kick, repeatedly.
- The kernel CCB takes a `HWPERF` command in 6–10 µs; core 0 reports
  `EVENT_STATUS 0x20002a00` (timer, TA finished, TPC and DPM clear).
- The transfer queue runs a dummy transfer (`tq 0x20 0xcafe0001`): the
  completion value lands in the scratch word, the queue read offset
  reaches the write offset, the context word clears, and there is no BIF
  fault or lockup.
- A real 2D transfer, built from scratch with the reference's `tqgen.py`
  and loaded through debugfs `map`/`mem`, scales a 64x64 four-quadrant
  source 2x into a 128x128 linear BGRA buffer.  The four quadrant centre
  pixels come back red, green, blue and white exactly; the transfer
  completes, core 0 raises `PIXELBE_END_RENDER`, and there is no BIF
  fault.  This exercises the transfer queue, the USSE/PDS programs, the
  pixel back end and the GPU MMU end to end.
- The render node is `/dev/dri/renderD128`, UAPI version 3.  `sgxgem`
  creates a 4 KiB GEM buffer, maps it write-combined, writes and reads a
  pattern, reads back its GPU address (`0xdfffe000` in the test), and
  waits on it successfully.
- The 2D engine draws on the screen: the display driver's `plane_info`
  gave the current scanout buffer (`0x9a100000`, stride 5120, 1280x720),
  `map_phys` mapped it at GPU `0x9e000000`, and a `tqgen.py`-generated
  1280x720 transfer wrote the four-quadrant pattern into it.  `peek` at
  the display's physical framebuffer read the quadrant words back
  (`ffff0000`, `ff00ff00`, `ffffffff`, `ff0000ff`), so the GPU wrote the
  buffer the display is scanning out.  `map_phys` here had no CPU view
  (`ioremap_wc` refused the large RAM range), which is fine for drawing.
- The clean presentation path works end to end: `sgxpresent` makes a
  contiguous 1280x720 SGX buffer at GPU `0x8d000000`, exports it as a
  dma-buf, the display driver imports it (physical `0x9a500000`) and scans
  it out, and a 2D transfer into the same GPU address painted the quadrant
  pattern; `peek` at the display's framebuffer read the four quadrant
  colours back.
- The 2D engine is a real ioctl: `sgx2d` scales its 64x64 four-quadrant
  source over 1280x720 and 640x360 destination buffers with
  `DRM_APPLE_SGX_BLIT` and every quadrant word comes back right, twice in a
  row (the second blit runs the same code with different destination
  words).  `sgxpresent -b` shows the same blit on HDMI.
- `hwtest gpu` passes 11/11, and the full `hwtest` suite passes 125/125 with
  2 skips on the same boot.

The first probe happens at ~0.6 s, before `stage2` has copied the firmware
from NAND (~7.2 s).  The driver now retries the microkernel start when a
render-node client opens, so the first `sgxinfo`/`sgxgem` after boot starts
it; a failed attempt is not sticky.  The TA/3D render queue itself still
needs a userspace command (the reference's frame packs or Mesa) to be
exercised.

### Presenting on the display (PRIME)

A buffer created with `APPLE_SGX_BO_CONTIGUOUS` is one `dma_alloc_wc()`
block from the platform's CMA pool (64 MiB total, about 60 MiB free on this
board), so its dma-buf has a one-entry scatter/gather table — which is what
the display driver's DMA-GEM import needs; it rejects a scattered table with
`-EINVAL`.  `sgxpresent` runs the whole path:

```sh
sgxpresent 10			# seconds; the console comes back afterwards
```

It creates a 1280x720 contiguous buffer at GPU `0x8d000000`, fills it with
colour bars, exports it with `PRIME_HANDLE_TO_FD`, imports the dma-buf into
the display driver (`/dev/dri/card1` here: the render node takes DRM minor
0), adds it as a framebuffer and sets it as the CRTC.  The render node is
closed once the display holds the buffer, so the GPU mapping stays (the
dma-buf owns the buffer object) while the microkernel's debugfs controls
stop being refused as "render node busy".  A transfer into `0x8d000000` then
draws on the live screen.

On hardware this scanned out at physical `0x9a500000`, and after a
`tqgen.py` transfer `peek` at the display's framebuffer read the four
quadrant colours back — the GPU wrote the buffer the display is showing
without a CPU copy.

The other direction works too: the render node's `PRIME_FD_TO_HANDLE` takes
a foreign dma-buf (a display-owned buffer), maps it into the GPU address
space at import time and `GEM_INFO` reports where, so a display-allocated
scanout buffer can be a render target.  `sgxgem -i /dev/dri/card1` checks
this: a 64x64 display dumb buffer came back as GPU `0xdfffb000`.

### The 2D engine (BLIT)

`DRM_APPLE_SGX_BLIT` is a scaled copy through the transfer queue:

```c
struct drm_apple_sgx_blit {
	__u32 src_handle, dst_handle;		/* the render node's buffers */
	__u32 src_width, src_height, src_stride;	/* BGRA8888, linear */
	__u32 dst_width, dst_height, dst_stride;
	__u32 flags, pad;
};
```

The source rectangle is scaled into the destination's whole
`dst_width x dst_height` area.  Both buffers must be the render node's (GEM
buffers or imported dma-bufs); the kernel holds them for the duration and
waits for the microkernel's completion, so the ioctl returns when the copy
is over.

The kernel builds the transfer itself — the USSE programs, the PDS block
and the parameter page (the reference's `tqgen.py`, ported to C) — and owns
three small buffers for it:

| piece | GPU address | why there |
|---|---|---|
| USSE code | `0x1000` | a DOUTU names a program by a 20-bit index from zero |
| PDS block | `0x980f3000` | fixed, reserved in the render node's address space |
| parameter page | `0x8c006000` | fixed, reserved too |

No address comes from userspace except the two buffer objects' own, so a
blit cannot name memory the caller does not own.  The programs carry the
destination's address, stride and size as immediates, so the kernel writes
the code page on every blit; the engine reads it again per program.

`sgx2d` checks it: a 64x64 source into 1280x720 and 640x360 destinations,
four quadrant centres each.  `sgxpresent -b` puts the result on HDMI.

### Drawing on the screen the display is already showing

The display pipe publishes the buffer it is scanning out (`plane_info`, and
now `include/linux/s5l8947x-display.h`): its physical address, stride, size
and a generation that changes when the pipe switches buffers.  The SGX
driver maps that range at `0x90000000` (the reserved framebuffer window)
with PTEs that are not cache-consistent, because the pipe reads memory and
not the CPU's caches.  `APPLE_SGX_PARAM_FB_VA/_WIDTH/_HEIGHT/_STRIDE` report
where it is, and `DRM_APPLE_SGX_BLIT` with `dst_handle` 0 copies a buffer
onto it:

```sh
sgxpresent -s 10		# 2D engine -> the live screen, no modeset
```

No dma-buf and no PRIME are involved: the kernel holds the display's buffer
for as long as the pipe is showing it, and a modeset that changes it is
noticed by the generation, which remaps before the next copy.  The blit
lock is held across the check and the transfer, so a modeset cannot pull
the buffer out from under a running copy.  This is the path a Mesa flush or
a full-screen renderer uses when it does not want a compositor.

The screen mapping and `sgxframe -s` are verified on the device: the
kernel maps what the pipe is showing and the 2D engine draws on it.

### EGL and GBM (kmsro)

Mesa's kmsro pairs the display's GBM device (`/dev/dri/card1`, kernel driver
`s5l8947x`) with the `apple_sgx` render node.  GBM allocates a surface
buffer on the render node with `PIPE_BIND_SCANOUT`; the SGX driver makes it
contiguous (`APPLE_SGX_BO_CONTIGUOUS`, one CMA block) so its dma-buf has a
one-entry scatter/gather table, exports it, and the display imports it as a
scanout buffer.  `WINSYS_HANDLE_TYPE_KMS` then returns the display's handle,
which is what `drmModeAddFB` needs, and `resource_get_param` reports the
stride, offset and linear modifier kmsro asks for.  A page flip puts the
rendered buffer on HDMI.

```sh
sgx-gl gbmtest 300		# 300 frames on HDMI, then the console returns
```

`gbmtest` keeps the console's mode, clears and draws into each surface
buffer, checks one pixel through `glReadPixels`, and flips with
`DRM_MODE_PAGE_FLIP_EVENT`; the display's simulated vblank completes the
event.  It ran 300 frames on the device, and the console's framebuffer came
back on exit.  Rendering and scanning out are not implicitly ordered: the
test calls `glFinish()` before the flip, and a compositor has to order its
own commits (the render node's syncobjs export sync files, but the display
driver's atomic commit does not wait on them yet).

### Drawing into the display's own buffer (bring-up path)

The display driver exposes the buffer it is scanning out now as
`/sys/bus/platform/devices/3a200000.display-controller/plane_info`:

```
0x<physical> <stride> <width> <height>
```

The SGX debugfs `cmd` accepts `map_phys VA PA SIZE`, which maps an existing
physical range into the GPU address space (the range is not owned or freed
by the SGX driver).  Mapping the display's current scanout buffer at a free
GPU address (`0x9e000000` here) and pointing a transfer at it lets the 2D
engine draw directly on the screen:

```sh
D=/sys/kernel/debug/apple-sgx
info=$(cat /sys/bus/platform/devices/3a200000.display-controller/plane_info)
set -- $info
echo 1 > $D/boot                       # allocates the microkernel buffers
sleep 0.2
echo "map_phys 0x9e000000 $1 $(($2 * $4))" > $D/cmd
# load a tqgen.py-generated transfer with destination 0x9e000000,
# stride $2, size $3x$4, then:
echo tqkick > $D/cmd
```

The first boot is needed because the buffer set (and therefore `map` and
`map_phys`) exists only after the microkernel has been loaded.  A kick after
the page tables changed carries `SGX_CC_INVAL_BIF_*`, so the microkernel
invalidates its MMU caches itself; the extra boot is only needed for
mappings made *before* the boot.  `0x9e000000` is outside the render node's
reserved framebuffer window at `0x90000000`.

A boot zeroes the microkernel's queues, so a transfer command has to be
written (`apple-sgx/mem`, at the transfer CCB's GPU address) after the last
boot.

`map_phys` may not get a CPU view: `ioremap_wc` can refuse a large RAM
range, in which case the mapping is GPU-only and `apple-sgx/mem` cannot
read it.  The display's framebuffer can still be checked with `peek` at the
physical address from `plane_info`.

The console may repaint the screen; `echo 0 > /sys/class/vtconsole/vtcon1/bind`
stops it for the test, and `echo 1 > ...` brings it back.

PRIME export from the render node works, and a buffer created with
`APPLE_SGX_BO_CONTIGUOUS` has a one-entry scatter/gather table that the
display's DMA-GEM import accepts.  Mesa allocates scanout resources that
way for kmsro (above); the `map_phys` debugfs path below is the older
bring-up route and needs no PRIME at all.

What is not there yet:

- **No KMS plane on the render node.**  The `s5l8947x` driver owns the HDMI
  output and the render node has no connector; kmsro is how EGL/GBM pairs
  them.  The overlay ioctl still returns `-EOPNOTSUPP`, and the UAPI stays
  version 3.
- **No compositor.**  The EGL/GBM path is there (`gbmtest`), but nothing
  uses it as a display server yet.  A compositor also needs explicit
  ordering: the display's atomic commit does not wait on the render node's
  fence through the dma-buf yet.
- **No implicit fencing on scanout buffers.**  The render node signals
  syncobjs; the display driver waits on none of them, so a client finishes
  its renders (`glFinish`/a fence wait) before the flip.

## The route to OpenGL ES 2.0

The kernel interface a Gallium driver uses is complete (UAPI 3): GEM with
fixed, code-zone and TA-heap placement, `SUBMIT` with a TA command and a
parameter buffer, syncobjs, `GEM_INFO`, dma-buf import and the screen blit.
What is unproven is the TA/3D execution path on this one-core microkernel;
everything else is userspace.

1. **Prove the render queue.**  Done on hardware: `sgxframe` builds the
   template frame (the PDS block, the state area, the TA command and the 3D
   register block), one render target's data, and submits a full-screen
   clear, then checks the pixels.  It needs 0xa8 bytes the stock graphics driver
   keeps -- its event PDS program -- which the device's own system image
   carries.  `glcarve` finds that template by its shape in the system-wide
   code cache, stage2 runs it on the first boot, and the result is cached on
   `Data` as `/lib/firmware/s5l8947x/gl-event.pds`; nothing is committed.
   Several windows can match, so `glcarve -a CACHE DIR` writes them all and
   `sgxframe -e` says which one draws.  If the clear is wrong or hangs,
   compare the words with the stock driver's decompiled render-target code
   (`research/ghidra/out/kc_sgx_r.c`).
2. **Port the Gallium driver.**  Done on hardware: the clean-room driver
   published with Project Cascadia (about 15.5k lines, MIT) is in
   `tools/mesa-sgx/`, builds into Mesa 26.1.8 for Alpine `armv7` (musl) in
   an armv7 container, and is installed as the system GL.  It builds its own
   frame from `sgx_template.c`; the only input it takes from outside is the
   same event program, read from `/lib/firmware/s5l8947x/gl-event.pds`
   (`SGX_EVENT` names another copy).  Three files differ from the reference:
   that path, the same default directory for the frame, and the renderer
   string.  `glclear`, `gltri`, `glfs`, `glsize`, `gltex`, `glblend` and
   `gldepth` all pass on the device.
3. **Put it on the screen.**  Done for an EGL/GBM client: kmsro pairs the
   render node with the `s5l8947x` display driver, GBM allocates the
   scanout buffer on the render node (contiguous, CMA), the display imports
   its dma-buf, and page flips put it on HDMI (`gbmtest`, 300 frames on the
   device).  For full-screen programs the screen blit above is the shorter
   path.  A compositor is the next step; it needs explicit fence ordering
   until the display waits on the render node's dma-buf.
4. **Then the 2D engine's leftovers** (fill and non-scaled copy), the
   interrupt and runtime PM.

## Next steps

1. **A compositor.**  The EGL/GBM half works; a Wayland compositor with a
   DRM backend (or an SDL client) would be the first real consumer.  The
   open piece is ordering: the display's atomic commit does not wait on the
   render node's fence, so a compositor has to wait explicitly (or the
   kernel gains dma-fence waiting in the display's commit).
2. **Implicit fencing between the render node and the display.**  The
   render node signals syncobjs, and its dma-bufs could carry a fence the
   display's atomic commit waits on.  Until then `gbmtest` calls
   `glFinish()` before each flip.
3. **The desktop-GL half of the driver** (GLX and the x11/wayland
   platforms) is an opt-in build (`MESA_BUILD=desktop`) with the
   X11/Wayland libraries it needs; desktop GL 2.1 is verified through EGL
   (`glclear-gl`), while GLX waits for an X server.  Build it into the
   default image if a compositor or X client needs it.
4. **More of the 2D engine.**  BLIT covers a scaled copy; a fill and a
   non-scaled copy are the same transfer with other programs.
5. **Power management.**  The microkernel's power and idle commands are
   implemented in debugfs; wiring them to runtime PM is a later step.

## Hardware map

| Block | Address | Notes |
|---|---|---|
| SGX registers | `0x35100000` | 96 KiB, six 16 KiB banks |
| GFX_SYS power state | PMGR `0x3f101024` | gate 7 |
| GFX power state | PMGR `0x3f101028` | gate 8 |
| Interrupt | AIC 49 | not enabled by default |
| Microkernel firmware | `/lib/firmware/s5l8947x/sgx543.fw` | carved by `sgxcarve` |

The device tree node is `sgx0: gpu@5100000` (the `soc@30000000` ranges map it
to `0x35100000`).  It uses the compatible strings `apple,s5l8947x-sgx` and
`apple,s5l8940x-sgx`, two clocks (`gfx_sys`, `gfx`) and the
`apple,brn-31195` erratum flag.

## Firmware

`sgxcarve` finds the GPU module inside the stock kernel image on the
device's `System` volume, checks its module UUID, and writes an `SGXF`
container:

```
u32 magic 'SGXF'; u32 version (1); u32 region count; u32 header bytes
u8  module UUID[16]
count x { u32 id; u32 offset; u32 size; u32 reserved }
region bytes, each 64-byte aligned
```

Region ids are fixed:

| id | contents |
|---|---|
| 1 | start-up program, placed at offset 0 of the code buffer |
| 2 | microkernel program, placed at offset `0x1000` |
| 3..7 | PDS templates (`0x18`, `0x38`, `0x18`, `0x350`, `0x9c`) |
| 8 | second code buffer (`0x3b58`) |
| 9 | "other" PDS template (`0x258`) |
| 10 | code table (`0x140`) |
| 11 | PDS table (`0xf0`) |
| 12 | descriptor table (`0x28`) |

The driver refuses a container whose module UUID or region sizes it does not
know.  The accepted build is the one `sgxcarve` already supports for this
board.  `stage2` caches the result on `Data`, so the stock volume is read
only on the first boot.

## GPU address space

The GPU has its own 32-bit address space, mapped by its MMU.  The driver
uses one layout for everything:

```
0x80000000  microkernel buffers and kernel queues (8 MiB)
0x80800000  render node buffers, debugfs mappings
0x89000000  kernel parameter buffer (~8.3 MiB)
0x8a000000  TA heap (render target data), up to 0x97800000
0x90000000  reserved framebuffer window (16 MiB)
0x9a000000  USSE code zone (8 MiB)
0xa8000000  kernel-picked buffer addresses, up to the tiled window
0xe0000000  BIF tiled window 1 (stride 4096), used only on request
0xf0000000  BIF tiled window 2 (stride 8192), unused
```

PDS data pointers carry bit 31 implied, so everything a PDS program reads
must be at `0x80000000` or above.  A DOUTU or PHAS names a program by its
index from a code base, so USSE code must stay within 8 MiB of
`APPLE_SGX_PARAM_CODE_BASE`.

## Command flow

1. **Kernel CCB.**  The host writes a 32-byte command to `B_750`, advances
   the write offset in `B_758`, bumps the kicker count and writes 1 to core
   0's `EVENT_KICK`.  The microkernel advances the read offset when it has
   taken the command.
2. **Transfer queue.**  A transfer command is a 0x140-byte record in
   `B_TQ_CCB`.  The kernel sends `TRANSFER` through the CCB with the queue's
   hardware context.  A transfer is the GPU's blit/fill path: the 3D pipe
   draws one quad and the pixel back end writes the result.  `BLIT` builds
   one from two buffers, and `tqkick` replays one written through debugfs.
3. **Render queue.**  A render is a TA command (0x11c..0x200 bytes) in
   `B_R_CCB`, sent as `TA` with the render context.  The TA pass builds
   tiles; the 3D pass shades them.  The job is done when the microkernel
   writes the TA completion value and clears the render details' word at
   `+0x24`.
4. **Render node.**  `SUBMIT` copies the TA command, sets the parameter
   buffer descriptor in the render context, writes the render details and
   queues the job.  A DRM scheduler serialises jobs across clients, syncobjs
   provide ordering, and an hrtimer polls the two completion words.

The microkernel keeps a parameter buffer's state between renders.  A render
that uses a different parameter buffer object than the previous one restarts
the microkernel first (`pb_restart`).  A render the microkernel leaves
unread in its queue gets TA again after `rekick_ms`.  A render that does not
finish in `render_timeout_ms` restarts the microkernel and fails with
`ETIMEDOUT`.

## Building and enabling

The driver is enabled by `CONFIG_DRM_APPLE_SGX=y` in
`config/s5l8947x.config`.  The kernel patch series adds it to
`drivers/gpu/drm/Kconfig` and `drivers/gpu/drm/Makefile`; the overlay copies
`drivers/gpu/drm/apple-sgx/` and the UAPI header into the kernel tree.

`./scripts/velliangiri.sh kernel` builds it.  The device tree node is already
enabled on `j33iap`.

## Testing on the device

With the device booted and the firmware provisioned:

```sh
hwtest gpu                    # handshake, CCB command, render node, contiguous export
sgxinfo                       # interface parameters and address windows
sgxgem                        # GEM create/map/info/wait on the render node
sgxgem -c                     # contiguous buffer, exported as a dma-buf
sgxgem -i /dev/dri/card1      # import a display dumb buffer into the GPU
sgx2d                         # 2D engine: scale a source over two destinations
sgxpresent 10                 # put an SGX buffer on HDMI through PRIME
sgxpresent -b 10              # ... with the 2D engine drawing it
sgxpresent -s 10              # 2D engine straight onto the live scanout
sgxframe -s 5                 # one clear through the TA/3D queue, shown
glcarve -a /mnt/system/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7 /tmp/glc
cat /sys/kernel/debug/apple-sgx/regs
echo 1 > /sys/kernel/debug/apple-sgx/boot      # restart the microkernel
echo "hwperf 0" > /sys/kernel/debug/apple-sgx/cmd
echo "tq 0x20 0xcafe0001" > /sys/kernel/debug/apple-sgx/cmd
```

`regs` shows the master and core registers, the CCB offsets, the host
control words and the microkernel state.  `mem` exposes GPU memory at file
offsets equal to GPU addresses, for replay experiments.  `cmd` also accepts
`power 1|2|3`, `map VA SIZE`, `map_phys VA PA SIZE`, `tqkick` and
`rkick PB DET CMD [CC]`.

The render node is `/dev/dri/renderD128`.  `sgxinfo` is the quickest check
that a userspace process can open it and read the parameters.  `sgxframe`
reads `/lib/firmware/s5l8947x/gl-event.pds`, which `stage2` provisions with
`glcarve`; with no display the last two commands skip.

On this board the direct boot route takes the **whole self-relocating
bundle** as the image DATA, not a separate direct stub.  The bundle's entry
stub is position-independent and copies its payload to `0x80000000`; wrapping
the bundle directly avoids the double copy that a separate stub would make,
where the entry stub overwrites itself while copying.  The wrapper needs a
stock recovery-image template and is built with the sibling workspace's
`img3pack.py`; the template is not part of this repository.

## Porting notes and differences from the reference

- **Power:** the reference board uses two PMGR power states at
  `0x3f101024`/`0x3f101028`; this board exposes the same two as PMGR gate
  indices 7 and 8.  Gate 9 (HPERF-NRT) is not needed.
- **Firmware:** the reference extracts two sections from a host image and
  uses a `SGX543FW` container.  This port keeps the on-device `SGXF`
  container that `sgxcarve` already produced, so no re-provisioning is
  needed on existing installations.
- **Display:** the reference's KMS-on-simplefb and fbcon hooks were removed.
  The render node has no modeset feature, so presentation is PRIME: a
  contiguous SGX buffer is exported and the `s5l8947x` driver imports it.
  The reference's 2D engine blit is back as the `BLIT` ioctl, with the
  kernel building the transfer.  `APPLE_SGX_PARAM_FB_*` return 0 and the
  overlay ioctl returns `-EOPNOTSUPP`.
- **Core count and clock:** both are read from the hardware.  The microkernel
  timer clock is 256.5 MHz (the 513 MHz PLL divided by two), the value the
  clean-room work measured on the same GPU family.
- **Mesa:** the Gallium driver in `tools/mesa-sgx/` is the reference's,
  unchanged except for three files (the event program's path and the frame
  directory default, both this port's layout, and the renderer string,
  which now names the kernel's core count).  It reads the kernel's own
  `apple_sgx_drm.h` at build time instead of carrying a copy.  The image
  builds with `-Dplatforms=` (surfaceless EGL, GBM) because the device runs
  no window system; see the Next steps above for the desktop-GL half.

## Licence and attribution

The kernel driver and the UAPI header are `GPL-2.0-only` and `MIT`
respectively, matching the clean-room SGX543 work in Project Cascadia from
which they are derived.  No firmware, key material or vendor binary is
included.
