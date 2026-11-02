<p align="center">
  <img src="logo.svg" alt="Velliangiri logo" width="160">
</p>

<h1 align="center">Velliangiri</h1>

<p align="center">
  <strong>Mainline Linux 6.12 for the S5L8947X (<code>j33iap</code>) and related ARMv7 devices.</strong><br>
  Clean-room drivers · Upstream-style kernel overlay · <code>GPL-2.0-only</code>
</p>

---

Velliangiri brings a mainline **Linux 6.12** kernel and an **Alpine Linux (`armv7`)** userspace to retired ARMv7 hardware built around the **S5L8940X / S5L8947X** SoC family.

The system boots over USB into RAM in ~20 seconds while using the internal NAND flash for persistent state: on first boot, required firmware is automatically extracted from the device's `System` volume and cached on the encrypted `Data` volume alongside SSH host keys, Wi-Fi configuration, Bluetooth pairings, and cached graphics packages.

---

## Hardware Support (`S5L8947X` / `j33iap`)

### Supported

| Subsystem | Supported Capabilities |
|---|---|
| **CPU & Core SoC** | Single-core Cortex-A9 up to 850 MHz (`VFPv3-D32` + `ARM NEON`), CPU frequency scaling (`212.5` / `425` / `850 MHz`), 512 KB L2 cache, interrupt controller, 24 MHz timer, 42-channel central DMA, hardware watchdog, clean reboot and standby power-off |
| **Clocks, GPIO, I2C & PMU** | Peripheral clock gates and audio oscillators, 144-pin GPIO and pinctrl controller with interrupts, 3x I2C controllers, PMU power switches and event interrupts, persistent real-time clock (RTC) with wake alarm, 5 temperature sensors (4 board thermistors + PMU junction), and CPU thermal throttling |
| **HDMI Display** | DRM/KMS display output with EDID mode detection (up to `1920x1080@30`, the default, matching the boot splash) with synthesized vblank frame pacing), framebuffer console (`tty0`), 1080p pre-kernel boot splash, hardware gamma brightness control, and zero-copy `dma-buf` scanout |
| **2D / 3D GPU (SGX543)** | PowerVR SGX543MP1 microkernel bring-up, GPU MMU, DRM render node (`/dev/dri/renderD128`), hardware 2D scaled blit engine, TA/3D rendering queue, **OpenGL ES 2.0** and opt-in **Desktop OpenGL 2.1** via Mesa 26.1.8, zero-copy HDMI presentation (`PRIME`, live-scanout blit, and `EGL/GBM` via `kmsro` with implicit fence sync), and **Weston 14** Wayland compositor |
| **Hardware Scalers (M2)** | Dual V4L2 memory-to-memory scalers (`/dev/video0`, `/dev/video1`): up to 8x horizontal and 7x vertical downscaling with polyphase area-averaging filters, crop and compose letterboxing, `NV12`/`NV16`/`YUYV` ↔ `RGB565`/`XRGB1555`/`XRGB8888` colour-space conversion (`BT.601`/`BT.709`, limited/full range), and zero-copy `dma-buf` display output |
| **Hardware JPEG Decoder** | Baseline JPEG V4L2 memory-to-memory decoder (`/dev/video2`): hardware header parsing and body decoding into `NV12`, `YUYV`, or `UYVY` (~22 ms for a 1080p image, ~32 ms scaled to HDMI at 1% CPU) |
| **Audio (HDMI & Optical)** | ALSA playback over HDMI I2S (`pcmC0D0p`, 2/4/6/8-channel LPCM at exact `32` / `44.1` / `48 kHz` with hot-plug mute/unmute) and optical S/PDIF (`pcmC0D1p` at `46,875 Hz`), plus the `wavplay` CLI player with automatic NEON resampling |
| **Ethernet & USB Gadget** | 10/100 Fast Ethernet (`eth0`, automatic DHCP) and Micro-USB peripheral gadget (`usb0` virtual Ethernet at `10.55.0.2` with Dropbear SSH + `/dev/ttyGS0` serial console) |
| **Wi-Fi (2.4 & 5 GHz)** | Broadcom BCM4334 802.11a/b/g/n over internal HSIC USB host; WPA2 scanning and connection via `wpa_supplicant` and the `wifi` helper, with regulatory domain support and factory MAC provisioning |
| **Bluetooth 4.1 + LE** | BCM4334 Bluetooth (`hci0` at 1.6 Mbaud) with factory address provisioning, BlueZ (`bluetoothd`, `bluetoothctl`), LE scanning (`btscan`), Classic (`hidp`) and LE (`uhid`) HID keyboard support, persistent pairings, and UART runtime sleep with PMU host-wake |
| **IR Remote & System MCU** | Infrared remote input (`/dev/input/event*`) with switchable game/media keymaps, system MCU handshake, ACK-waited commands, NMI wake source, and standby power-off |
| **Hardware & NEON Crypto** | Hardware DMA AES (`ecb(aes)`, `cbc(aes)` 128/192/256-bit at ~70 MB/s, plus hardware device/group keys), hardware SHA-2 (`sha224`, `sha256`, `sha384`, `sha512`), ARMv7/NEON kernel crypto (`sha1/256/512-neon`, `blake2b-neon`, `chacha20-neon`, `poly1305-neon`, `aes-neonbs`), and `AF_ALG` userspace sockets |
| **Internal NAND & Volumes** | 8.9 GB managed NAND (`/dev/mtd0`, ~47 MB/s DMA reads), read-write Flash Translation Layer (`/dev/nand0`) with superblock rotation and garbage collection, hardware key-store and system keybag unwrapping (protection classes 1–4), and read-write `hfsplus` mounts for `System` (`/dev/nand0p1`) and `Data` (`/dev/nand0p2`) with journal replay, transparent per-file encryption, and `decmpfs` transparent decompression |

### In-Progress & Pending Support

| Area | Status |
|---|---|
| **External USB Host Mode** | **Pending:** The external Micro-USB port currently runs in peripheral/gadget mode only. Supporting external USB keyboards, mice, and storage requires OTG role switching and the USB 1.1 OHCI companion controller. |
| **H.264 / MPEG-4 Video Decoder (VXD)** | **In-Progress:** Power gate control, on-device firmware extraction, and DMA upload image assembly are in place; blocked on initializing the media clock source before reset/handshake and implementing the V4L2 decode interface. |
| **GPU & Wayland Desktop Polish** | **In-Progress:** OpenGL ES 2.0, EGL/GBM, and Weston kiosk compositing work on HDMI. Remaining items: external input (awaiting USB host mode), full desktop shell / Xwayland, 2D solid fill and rotation, hardware IRQ completion (currently polled), and runtime PM. |
| **Display >108 MHz & Hardware Vblank** | **In-Progress:** Pixel clocks are currently capped at 108 MHz (`1080p30` / `720p60`), and frame pacing uses a timer-synthesized vblank as no hardware vblank IRQ is wired. |
| **Optical Audio Exact Sample Rates** | **In-Progress:** HDMI plays exact `32` / `44.1` / `48 kHz`. Optical S/PDIF currently runs at `46,875 Hz` from the 24 MHz crystal (`wavplay` resamples automatically) and awaits switching to the audio oscillator and receiver verification. |
| **Bluetooth Pairing Verification** | **In-Progress:** Controller runs at 1.6 Mbaud (due to integer UART clock dividers); BlueZ and HID support are enabled, with end-to-end device pairing verification pending. |
| **IOP Coprocessor** | **In-Progress:** Firmware extraction and core start work, but the EmbeddedIOP message channel is not implemented (not required on Linux since NAND is driven directly from the main CPU). |
| **DART IOMMU, PKE & Dynamic Voltage Scaling** | **Pending:** DART stays in bypass mode (media blocks use contiguous buffers), the PKE public-key accelerator is unused, and CPU frequency scaling runs at fixed boot voltage. |

See [docs/progress.md](docs/progress.md) for the detailed support matrix, [docs/SGX.md](docs/SGX.md) for GPU and OpenGL details, and [docs/benchmarks.md](docs/benchmarks.md) for on-device cache, NEON, crypto, and storage benchmarks.

### Documentation (`docs/`)

| Document | Contents |
|---|---|
| [docs/progress.md](docs/progress.md) | Hardware and software support matrix (Supported, In-Progress, Pending, and Priority Roadmap) |
| [docs/SGX.md](docs/SGX.md) | PowerVR SGX543MP1 GPU, Mesa 26.1.8 OpenGL ES 2.0 / GL 2.1, and Weston 14 Wayland status & usage |
| [docs/benchmarks.md](docs/benchmarks.md) | On-device L1/L2/DRAM latency, NEON bandwidth, hardware AES/SHA, and NAND/HFS+ benchmarks |
| [docs/drivers-and-porting.md](docs/drivers-and-porting.md) | Driver map, MMIO addresses, clock/IRQ bindings, boot bundle architecture, and SoC porting guide |
| [docs/sgx-architecture.md](docs/sgx-architecture.md) | SGX543 register banks, `SGXF` firmware container, GPU MMU layout, TA/3D/2D queues, and `kmsro`/PRIME display integration |
| [docs/audio-i2s-spdif.md](docs/audio-i2s-spdif.md) | I2S0–I2S3, I2S clock master, S/PDIF, PMGR NCOs, and 2/4/6/8-channel HDMI LPCM register specification |
| [docs/cdma-registers.md](docs/cdma-registers.md) | 42-channel Central DMA (CDMA) register file, 32-byte segment descriptor ring, and peripheral request map |
| [docs/nand-fmi-and-crypto.md](docs/nand-fmi-and-crypto.md) | FMI0/1 controller, PPN 1.5 command set, FTL superblocks/TOC, `LwVM` volumes, CDMA AES, and `Face`/`cprotect` key hierarchy |
| [docs/media-scalers-and-jpeg.md](docs/media-scalers-and-jpeg.md) | Dual M2 Scaler/CSC engines, baseline JPEG decoder, DART IOMMU, and VXD390 video decoder register specification |
| [docs/pmu-mcu-and-usb.md](docs/pmu-mcu-and-usb.md) | Dialog D1815 PMU (RTC & 12-bit ADC thermistors), UART1 System/IR MCU protocol, USB OTG/HSIC PHY ladder, SHA-2, and IOP |

---

## Running Linux on Your Device

### 1. Prerequisites

- **Host computer:** Docker (builds run inside a reproducible cross-compile container on Linux or macOS).
- **Target device:** A Micro-USB cable and a way to load and jump to a raw binary payload in RAM over USB.

### 2. Build

```bash
./scripts/velliangiri.sh all
```

This builds the Alpine `armv7` initramfs, compiles mainline Linux 6.12 and the Device Tree, and packs the self-relocating boot image:

```text
out/boot-bundle-s5l8947x-j33iap.bin
```

Other build targets:
```bash
./scripts/velliangiri.sh rootfs | kernel | bundle | mesa | weston | clean | check | shell
```

*(Optional)* Place a `wpa_supplicant.conf` at `research/wpa_supplicant.conf` (or set `WIFI_CONF=...`) before building to auto-join Wi-Fi on boot, or pass `SSH_PUBKEY=path/to/key.pub` to embed an SSH authorized key.

### 3. Boot and Connect

Load `out/boot-bundle-s5l8947x-j33iap.bin` into RAM over USB and jump to its entry point. Within ~20 seconds:

1. **HDMI:** The boot splash appears, followed by the Linux framebuffer console (`tty0`).
2. **USB Network & SSH:** Assign your host USB interface an IP in `10.55.0.0/24` (e.g. `10.55.0.1/24`) and connect:
   ```bash
   ssh root@10.55.0.2
   ```
3. **USB Serial Console:** Available on `/dev/ttyACM0` or `/dev/cu.usbmodem*`.
4. **Ethernet:** `eth0` acquires a DHCP lease automatically when plugged in.

### 4. Useful Commands on the Device

```bash
hwtest                                # Run the automated on-device hardware test suite
hwtest -i display audio               # Run interactive display and audio checks
wifi scan                             # Scan for 2.4 GHz and 5 GHz Wi-Fi networks
wifi connect "MySSID" "PSK"           # Connect to WPA2 Wi-Fi and obtain a DHCP lease
wifi status                           # Check Wi-Fi link status and IP address
btscan 10                             # Scan for Bluetooth LE advertisers for 10 seconds
wavplay /path/to/file.wav             # Play WAV audio over HDMI (-d /dev/snd/pcmC0D1p for optical)
jpegtest -s 5 photo.jpg               # Hardware-decode a JPEG and display it via the hardware scaler
m2mshow                               # Stream zero-copy hardware-scaled test frames to HDMI
sgxpresent -b 10                      # Present a 2D GPU-rendered buffer on HDMI for 10 seconds
sgx-gl glcubes --screen               # Render 1,000 textured 3D cubes on the SGX543 and flip to HDMI
weston-run 20                         # Run the Weston Wayland compositor on HDMI for 20 seconds
l2bench                               # Measure L1, L2, and DRAM latency and NEON bandwidth
```

---

## Contributing & Clean-Room Rules

Before making your first commit, install the git hooks and verify the tree:

```bash
./scripts/setup-guardrails.sh
./scripts/velliangiri.sh check
```

1. **Clean-room & open-source (`GPL-2.0-only`) only:** Write code only from observed hardware behaviour and public specifications.
2. **No binaries or blobs:** Never commit firmware, binary objects, archives, raw memory dumps, or cryptographic keys.
3. **No proprietary terminology:** Do not use internal vendor codenames, proprietary bootloader names, or key names in code, comments, documentation, or commit messages.
4. **Keep local research separate:** Store any local notes, verbose hardware documentation, firmware files, or non-distributable test material strictly inside the git-ignored `research/` directory.

## License

All kernel code, drivers, and build scripts are licensed under `GPL-2.0-only`. See [LICENSE](LICENSE).
