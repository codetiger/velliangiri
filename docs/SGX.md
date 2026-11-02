# SGX543 GPU & Graphics Support (`S5L8947X` / `j33iap`)

Status of the clean-room **PowerVR SGX543MP1** (rev `1.2.2`) kernel driver, **Mesa 26.1.8** Gallium driver, and **Weston 14** Wayland compositor on `S5L8947X` (`j33iap`). For low-level register banks, `SGXF` firmware container layout, GPU MMU address space, and command submission internals, see [sgx-architecture.md](sgx-architecture.md).

---

## 1. Supported Features

| Area | Status & Capabilities |
|---|---|
| **Kernel Driver & Render Node** | Power/clock state management (`GFX_SYS`, `GFX`), guarded register bank access, two-level GPU MMU (4 KiB pages), microkernel boot and handshake from on-device firmware (`sgx543.fw`), kernel CCB ring, transfer queue, TA/3D render queue, and DRM render node (`/dev/dri/renderD128`, UAPI v3) with GEM buffers, syncobjs, and DRM scheduler |
| **2D Blit Engine** | Hardware-accelerated scaled `BGRA8888` blits through the transfer queue (`DRM_APPLE_SGX_BLIT`), supporting both offscreen GEM buffers and direct blits onto the live HDMI scanout buffer |
| **OpenGL ES 2.0 (Mesa 26.1.8)** | Clean-room SGX Gallium driver installed as the system GL provider: hardware clears, vertex and fragment shaders (arithmetic, control flow, loops, arrays, uniforms), NPOT/linear/minified textures, alpha blending, color masks, depth testing, and render targets from `1x1` up to `4096x4096` |
| **Desktop OpenGL 2.1 (Opt-In)** | Optional `MESA_BUILD=desktop` build providing `libGL`, GLX, desktop OpenGL 2.1 (verified via surfaceless EGL), and X11/Wayland EGL platforms |
| **Display Presentation (`PRIME` & `EGL/GBM`)** | Three zero-copy HDMI presentation paths:<br>1. **EGL/GBM (`kmsro`)**: Pairs the SGX render node with the HDMI display driver for hardware-rendered page flips with implicit `dma-buf` fence synchronization.<br>2. **PRIME (`dma-buf`)**: Bidirectional buffer sharing between the SGX render node and the HDMI KMS driver.<br>3. **Live Screen Blit**: Direct 2D engine copy onto the active HDMI scanout buffer with no modeset or CPU copy. |
| **Wayland Compositor (Weston 14)** | Weston 14 with the DRM backend, GL renderer on the SGX543, and kiosk shell (`weston-run`), compositing Wayland `EGL` and `shm` clients onto HDMI at `1280x720@60` |
| **Automatic Provisioning & Persistence** | On first boot, `stage2` extracts the GPU microkernel (`sgx543.fw`) and 3D event program (`gl-event.pds`), validates the event program with a hardware render check (`sgxframe`), and caches both on `Data`. Installed Mesa and Weston tarballs are also cached on `Data` and unpacked into RAM on every boot |

### Verified Hardware Test Suite

All kernel and OpenGL ES 2.0 / 2.1 test suites pass on `j33iap` hardware:
- **Kernel & DRM (`hwtest gpu`)**: Microkernel boot/restart, GEM create/map/wait, contiguous `dma-buf` export (`sgxgem -c`), display `dma-buf` import (`sgxgem -i`), 2D scaled blit (`sgx2d`), TA/3D hardware clear (`sgxframe`), and live-screen blit (`sgxpresent -s`).
- **Mesa OpenGL ES 2.0 & Desktop GL 2.1**: `glclear` (0 pixel errors), `gltri` (11/11), `glfs` (40/40 fragment compiler tests), `glsize` (15/15 sizes up to `4096x4096`), `gltex` (14/14), `glblend` (14/14), `gldepth` (12/12), `glclear-gl` (Desktop GL 2.1), `glcubes` (1,000 textured cubes benchmark), `gbmtest` (300 page-flipped frames on HDMI with implicit fence sync), and `weston-run` (`weston-simple-egl` and `weston-simple-shm` on HDMI).

---

## 2. In-Progress & Pending Support

| Item | Status |
|---|---|
| **External Input for Weston** | **Pending:** Weston currently starts with `--continue-without-input` because external USB host mode (for USB keyboards and mice) is not implemented yet. |
| **Desktop Shell & Xwayland** | **Pending:** Weston currently builds with the lightweight `kiosk-shell`; the full `desktop-shell` (cairo/pango), fonts for `weston-terminal`, and `Xwayland` / Xorg server are not bundled yet. |
| **Additional 2D Engine Operations** | **In-Progress:** `BLIT` currently implements scaled copies; solid fills, unscaled fast copies, rotation, and multiple in-flight transfers are pending. |
| **Hardware Interrupt Completion** | **In-Progress:** Render and transfer completions are currently polled via `hrtimer` (`apple_sgx.irq=1` is opt-in; the hardware IRQ line has not been observed to fire yet). |
| **Runtime Power Management** | **Pending:** Microkernel idle and power commands are available in debugfs but not yet wired to Linux runtime PM, and jobs are serialized one at a time across clients. |

---

## 3. Building, Installing & Testing

### Build and Install Mesa & Weston

```bash
# Build Mesa (OpenGL ES 2.0 + EGL/GBM; prefix with MESA_BUILD=desktop for Desktop GL 2.1)
./scripts/velliangiri.sh mesa
tools/mesa-sgx/install.sh --system

# Build and install Weston 14 Wayland compositor
./scripts/velliangiri.sh weston
tools/weston/install.sh
```

### Run on the Device

```bash
hwtest gpu                            # Run automated kernel GPU & render-node checks
sgxinfo                               # Print SGX render node parameters
sgx2d                                 # Test 2D scaled blits
sgxpresent -b 10                      # Present 2D-blitted frames on HDMI via PRIME
sgxpresent -s 10                      # Blit directly onto the live HDMI scanout
sgxframe -s 5                         # Run a TA/3D clear and display on HDMI
sgx-gl glclear                        # Verify OpenGL ES 2.0 GPU clear
sgx-gl gltri                          # Verify OpenGL ES 2.0 vertex/fragment draws
sgx-gl glfs                           # Run all 40 fragment shader compiler tests
sgx-gl gbmtest 300                    # Flip 300 OpenGL ES 2.0 frames on HDMI via EGL/GBM
sgx-gl glcubes --screen --frames 120  # Benchmark 1,000 textured 3D cubes on HDMI
weston-run 20                         # Run Weston 14 + weston-simple-egl on HDMI for 20s
```
