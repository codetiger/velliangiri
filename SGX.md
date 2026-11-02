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
| Render queue | One TA + 3D queue with a 64 KiB command buffer; used by the render node and debugfs.  The command path is implemented; it still needs a userspace TA command to be exercised |
| Render node | `/dev/dri/renderD128`, driver `apple_sgx`: GEM buffers, syncobjs, a DRM scheduler, fences and timeouts |
| Debug interface | `/sys/kernel/debug/apple-sgx/{regs,boot,cmd,mem}` |
| Test tools | `sgxinfo` prints the render node's parameters; `sgxgem` creates, maps and checks a GEM buffer (`-c` for a contiguous, dma-buf-exportable one, `-i` to import a display buffer); `sgx2d` exercises the 2D engine; `sgxpresent` puts a buffer on the HDMI output; `hwtest gpu` checks the handshake, a CCB command and the render node |

### Verified on hardware (2026-11-02, `j33iap`)

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

PRIME export from the render node works (the GEM object's `get_sg_table` is
the shmem helper's), but the display driver's DMA-GEM import requires a
physically contiguous scatter/gather table.  The SGX render node's buffers
are shmem and may be scattered, so importing one into the display fails with
`-EINVAL`.  A clean future path is a CMA-backed SGX buffer object; until
then the `map_phys` debugfs path is how the 2D engine reaches the screen.

What is not there yet:

- **No KMS plane on the render node.**  The `s5l8947x` driver owns the HDMI
  output and the render node has no connector; presentation goes through
  PRIME as above.  `APPLE_SGX_PARAM_FB_*` still return 0 and the overlay
  ioctl `-EOPNOTSUPP`, and the UAPI stays version 3.
- **No Mesa/OpenGL ES userspace in the image.**  The render node is the
  kernel half; the Gallium driver and its USSE/PDS program packs are a
  separate body of work.

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
that a userspace process can open it and read the parameters.

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

## Next steps

1. **Userspace.**  Port the USSE/PDS program packs and the Mesa Gallium
   driver from the clean-room SGX543 work, then build them for Alpine
   `armv7`.  The TA/3D render queue (`SUBMIT`) still needs a userspace TA
   command to be exercised; the reference's frame packs come from stock GL
   captures that are not in either tree.
2. **More of the 2D engine.**  BLIT covers a scaled copy.  A fill and a
   non-scaled copy are the same transfer with other programs; the reference
   driver's simple-framebuffer hooks are not needed now that PRIME works.
3. **Power management.**  The microkernel's power and idle commands are
   implemented in debugfs; wiring them to runtime PM is a later step.

## Licence and attribution

The kernel driver and the UAPI header are `GPL-2.0-only` and `MIT`
respectively, matching the clean-room SGX543 work in Project Cascadia from
which they are derived.  No firmware, key material or vendor binary is
included.
