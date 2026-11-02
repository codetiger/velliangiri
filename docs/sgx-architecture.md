# SGX543 GPU Driver, Mesa & Compositor Implementation Notes (`S5L8947X` / `j33iap`)

Detailed architecture, firmware container format, GPU virtual address space, command submission flow, and userspace integration notes moved from the top-level `SGX.md`.

## 1. Overview & Hardware Map

The `S5L8947X` integrates a single-core **PowerVR SGX543MP1** revision `1.2.2` (`CORE_ID 0x01194101`, `CORE_REVISION 0x00010202`). `drivers/gpu/drm/apple-sgx/` is a clean-room port of the SGX543 driver from Project Cascadia (`GPL-2.0-only`), adapted to the `S5L8947X` SoC.

| Resource | Address / Index | Notes |
|---|---|---|
| SGX registers | `0x35100000` (`0x18000`) | 96 KiB across six 16 KiB banks: bank 0 is write-only broadcast (reading hangs the bus), bank 1 is master, bank `2 + n` is core `n` |
| `GFX_SYS` power state | PMGR `0x3f101024` | Clock gate 7 |
| `GFX` power state | PMGR `0x3f101028` | Clock gate 8 (gate 9 `HPERF-NRT` is not needed) |
| Interrupt | AIC IRQ 49 | Disabled by default (`apple_sgx.irq=1` opt-in); completions are polled via `hrtimer` |
| Microkernel clock | `256.5 MHz` | Half of the 513 MHz PLL; used by the microkernel to scale its internal timers |
| Microkernel firmware | `/lib/firmware/s5l8947x/sgx543.fw` | Carved from the on-device system kernel image by `sgxcarve` at first boot and cached on `nand0p2` |
| 3D Event PDS program | `/lib/firmware/s5l8947x/gl-event.pds` | `0xa8`-byte event program (SHA-256 `a1b855d41993968b40005e886f7ac11c8c3abb42aa0987e005eec4e6cb2a2285`) carved from the shared cache by `glcarve` and verified with `sgxframe` |

The Device Tree node is `sgx0: gpu@5100000` (`compatible = "apple,s5l8947x-sgx", "apple,s5l8940x-sgx"`, `apple,brn-31195`).

---

## 2. Firmware Container (`SGXF`)

`sgxcarve` locates the GPU module inside the stock kernel image on `/dev/nand0p1`, verifies its module UUID, and writes an `SGXF` container:

```text
u32 magic 'SGXF'; u32 version (1); u32 region count; u32 header bytes
u8  module UUID[16]
count x { u32 id; u32 offset; u32 size; u32 reserved }
region bytes, each 64-byte aligned
```

| Region ID | Contents |
|---|---|
| `1` | Start-up program, placed at offset `0x0` of the code buffer |
| `2` | Microkernel program, placed at offset `0x1000` |
| `3..7` | PDS templates (`0x18`, `0x38`, `0x18`, `0x350`, `0x9c`) |
| `8` | Second code buffer (`0x3b58`) |
| `9` | Additional PDS template (`0x258`) |
| `10` | Code table (`0x140`) |
| `11` | PDS table (`0xf0`) |
| `12` | Descriptor table (`0x28`) |

Note on boot order: the initial driver probe happens at ~0.6 s before `stage2` has mounted NAND (~7.2 s). The driver retries microkernel boot whenever a render-node client opens `/dev/dri/renderD128`.

---

## 3. GPU Virtual Address Space

The GPU MMU uses a two-level page table (4 KiB pages, single shared page directory) over cache-consistent uncached buffers:

```text
0x80000000  Microkernel buffers and kernel queues (8 MiB)
0x80800000  Render node buffers, debugfs mappings
0x89000000  Kernel parameter buffer (~8.3 MiB)
0x8a000000  TA heap (render target data), up to 0x97800000
0x90000000  Reserved live-framebuffer window (16 MiB)
0x9a000000  USSE code zone (8 MiB)
0xa8000000  Kernel-selected buffer addresses, up to the tiled window
0xe0000000  BIF tiled window 1 (stride 4096), used only on request
0xf0000000  BIF tiled window 2 (stride 8192), unused
```

PDS data pointers imply bit 31 (`>= 0x80000000`), and `DOUTU` / `PHAS` instructions index USSE code within 8 MiB of `APPLE_SGX_PARAM_CODE_BASE`.

---

## 4. Command Submission Flow

1. **Kernel CCB**: The host writes a 32-byte command to `B_750`, advances the write offset in `B_758`, increments the kicker count, and writes `1` to core 0's `EVENT_KICK`. The microkernel advances the read offset when consumed.
2. **Transfer Queue (`B_TQ_CCB`)**: A transfer command is a `0x140`-byte record submitted via `TRANSFER` on the kernel CCB. Used for 2D blits.
3. **Render Queue (`B_R_CCB`)**: A render is a TA command (`0x11c..0x200` bytes) submitted as `TA` with the render context. The TA pass builds tiles; the 3D pass shades them. Completion is signalled when the microkernel writes the TA completion word and clears word `+0x24` of the render details.
4. **DRM Render Node (`/dev/dri/renderD128`, UAPI v3)**: `SUBMIT` copies the TA command, binds the parameter buffer descriptor, writes render details, and queues the job on the DRM scheduler. Jobs are serialised across clients, ordered via syncobjs, and polled by an `hrtimer`. Switching parameter buffer objects triggers a microkernel restart (`pb_restart`).

---

## 5. 2D Blit Engine (`DRM_APPLE_SGX_BLIT`)

```c
struct drm_apple_sgx_blit {
	__u32 src_handle, dst_handle;		/* render node GEM handles (dst_handle 0 = live screen) */
	__u32 src_width, src_height, src_stride;	/* BGRA8888, linear */
	__u32 dst_width, dst_height, dst_stride;
	__u32 flags, pad;
};
```

The kernel constructs the USSE programs, PDS block, and parameter page internally (ported from `tqgen.py`) using three fixed internal buffers:
- USSE code at `0x1000`
- PDS block at `0x980f3000`
- Parameter page at `0x8c006000`

---

## 6. Display Presentation Paths

1. **PRIME (`APPLE_SGX_BO_CONTIGUOUS`)**: Allocates a single CMA block (`dma_alloc_wc()`) so the exported `dma-buf` has a 1-entry scatter/gather table accepted by `s5l8947x_drm`. Bidirectional PRIME (`PRIME_HANDLE_TO_FD` and `PRIME_FD_TO_HANDLE`) is supported.
2. **Live Screen Blit (`dst_handle = 0`)**: `s5l8947x_drm` publishes its active scanout physical address, stride, size, and generation counter via `include/linux/s5l8947x-display.h`. `apple_sgx` maps it at GPU VA `0x90000000` (`APPLE_SGX_PARAM_FB_*`) and `DRM_APPLE_SGX_BLIT` with `dst_handle = 0` copies directly onto the active framebuffer without a modeset or CPU copy.
3. **EGL / GBM via Mesa `kmsro`**: `kmsro` pairs `/dev/dri/card1` (`s5l8947x`) with `/dev/dri/renderD128` (`apple_sgx`). Scanout surfaces are allocated contiguous on the SGX render node, imported into the display driver via `dma-buf`, and page-flipped with implicit synchronization (`drm_gem_plane_helper_prepare_fb()` waits on the SGX job fence in `dma_resv`). Note: `sgx_resource_get_param()` intentionally omits `PIPE_RESOURCE_PARAM_HANDLE_TYPE_*` so Mesa's DRI image query falls back to `resource_get_handle()` and exports a real `dma-buf` file descriptor for Wayland clients.

---

## 7. Mesa Gallium Driver (`tools/mesa-sgx/`) & Weston (`tools/weston/`)

- **Mesa 26.1.8**: Builds the clean-room SGX Gallium driver (`MIT`) for Alpine `armv7` (`musl`). Uses `sgx_template.c` and `sgx_rt.c` to construct frames dynamically and loads `/lib/firmware/s5l8947x/gl-event.pds`. Reports `GL_RENDERER` from the kernel's detected core count (`PowerVR SGX543MP1`).
- **Desktop OpenGL (`MESA_BUILD=desktop`)**: Adds `-Dplatforms=x11,wayland -Dglx=dri -Dopengl=true` and bundles X11/Wayland client libraries, providing OpenGL 2.1 (`glclear-gl`).
- **Weston 14 (`tools/weston/`)**: Built from source with `drm-backend.so`, `gl-renderer.so`, `kiosk-shell.so`, and simple Wayland clients (`weston-simple-egl`, `weston-simple-shm`), launched via `seatd` and `weston-run`.
- **Persistence**: `install.sh` caches `sgx-mesa-install.tar` and `weston-armv7.tar` under `/mnt/data/linux/lib/`, which `stage2` unpacks into RAM at every boot.
