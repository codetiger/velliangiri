# Hardware & Feature Support Progress (`S5L8947X` / `j33iap`)

Focused status of supported, in-progress, and pending hardware/software features on `S5L8947X` (`j33iap`). Last verified on hardware on 2026-11-02 (`hwtest` suite passing with 0 failures).

For GPU and OpenGL ES / Wayland specifics, see [SGX.md](SGX.md) and [sgx-architecture.md](sgx-architecture.md). For performance measurements, see [benchmarks.md](benchmarks.md). For driver mappings and hardware register specifications, see [drivers-and-porting.md](drivers-and-porting.md), [audio-i2s-spdif.md](audio-i2s-spdif.md), [cdma-registers.md](cdma-registers.md), [nand-fmi-and-crypto.md](nand-fmi-and-crypto.md), [media-scalers-and-jpeg.md](media-scalers-and-jpeg.md), and [pmu-mcu-and-usb.md](pmu-mcu-and-usb.md).

---

## 1. Supported

| Subsystem | Supported Capabilities |
|---|---|
| **CPU, Cache & Core SoC** | Single-core Cortex-A9 (`VFPv3-D32` + `ARM NEON`), CPU frequency scaling (`212.5` / `425` / `850 MHz`), 512 KB L2 cache, interrupt controller, 24 MHz clocksource and local timer, hardware watchdog (`60 s` timeout), clean reboot, and standby power-off |
| **Clocks, GPIO, I2C & PMU** | Peripheral clock gates (38 active gates), 7 audio oscillators (NCOs), 144-pin GPIO and pinctrl controller with edge/level interrupts, 3x I2C controllers, PMU power switches, and 48-event PMU interrupt controller |
| **Real-Time Clock (RTC)** | Persistent PMU seconds counter with calendar offset across reboots, automatic NTP synchronization on network lease, and wake alarm interrupt |
| **Thermal & Hardware Monitoring** | 5 temperature channels via the PMU 12-bit ADC (SoC, Wi-Fi, Power supply, and NAND board thermistors + PMU junction) and CPU thermal zone (`70 °C` passive cpufreq throttling, `95 °C` critical shutdown) |
| **Central DMA** | 42-channel DMA engine supporting scatter-gather and cyclic transfers with per-channel completion interrupts (serving audio playback, managed NAND, and hardware AES) |
| **HDMI Display** | DRM/KMS display pipeline with EDID mode detection up to `1920x1080@30` (the default, matching the pre-kernel splash), `XRGB8888`/`ARGB8888` scanout, `fbdev` console (`tty0`), 1080p pre-kernel boot splash, gamma brightness control, synthesized vblank pacing, and zero-copy `dma-buf` import |
| **SGX543 GPU & Graphics** | Microkernel bring-up, two-level GPU MMU, DRM render node (`/dev/dri/renderD128`) with GEM buffers, syncobjs and scheduler, 2D scaled blit engine, TA/3D render queue, **OpenGL ES 2.0** and opt-in **Desktop OpenGL 2.1** (Mesa 26.1.8), zero-copy HDMI presentation (`PRIME`, live-scanout blit, and `EGL/GBM` via `kmsro` with implicit fences), and **Weston 14** Wayland compositor |
| **Hardware Scalers (M2)** | Dual V4L2 memory-to-memory scalers (`/dev/video0`, `/dev/video1`): up to 8x horizontal and 7x vertical downscale with dynamic polyphase area-averaging filters, crop and compose letterboxing, `NV12`/`NV16`/`YUYV` ↔ `RGB565`/`XRGB1555`/`XRGB8888` conversion (`BT.601`/`BT.709`, limited/full range), and zero-copy `dma-buf` streaming |
| **Hardware JPEG Decoder** | Baseline JPEG V4L2 memory-to-memory decoder (`/dev/video2`): hardware header parsing (including quantisation and Huffman tables) and body decode into `NV12`, `YUYV`, or `UYVY` (`4:2:0`, `4:2:2`, and `4:4:4` inputs supported) |
| **Audio (HDMI & Optical)** | ALSA playback on two PCM devices: HDMI (`pcmC0D0p`, 2/4/6/8-channel LPCM at exact `32` / `44.1` / `48 kHz` with CEA-861 channel mapping and hot-plug mute/unmute) and S/PDIF optical (`pcmC0D1p` at `46,875 Hz`), plus `wavplay` with NEON resampling |
| **Ethernet & USB Gadget** | 10/100 Fast Ethernet (`eth0` with factory MAC and DHCP) and USB 2.0 peripheral gadget (`usb0` CDC-ECM network + `/dev/ttyGS0` CDC-ACM serial console) |
| **Wi-Fi (2.4 & 5 GHz)** | Broadcom BCM4334 over internal HSIC USB 2.0 host with GPIO ready handshaking, automatic on-device firmware/NVRAM provisioning and caching, factory MAC address, ISO 3166 regulatory domain configuration, and WPA2-PSK networking |
| **Bluetooth 4.1 + LE** | BCM4334 Bluetooth (`hci0` over UART at 1.6 Mbaud) with automatic patch carving and caching, factory BD_ADDR, BlueZ stack, LE advertising scan, Classic (`hidp`) and LE (`uhid`) HID keyboard support, persistent pairings on `Data`, and UART runtime sleep with PMU host-wake |
| **UARTs, IR Remote & System MCU** | All 5 on-chip UARTs enabled (MCU link, Bluetooth HCI, and 3 test-point ports); NEC IR remote button input (game and media keymaps), MCU probe handshake, firmware version query, ACK-waited raw transactions, unsolicited event trace, NMI wake source, and standby power-off |
| **Hardware & SIMD Cryptography** | Hardware DMA AES (`ecb(aes)`, `cbc(aes)` 128/192/256-bit at ~70 MB/s, plus hardware device/group keys for NAND key unwrapping), hardware SHA-2 (`sha224`, `sha256`, `sha384`, `sha512`), ARMv7/NEON kernel crypto (`sha1/256/512-neon`, `blake2b-neon`, `blake2s-arm`, `chacha20-neon`, `poly1305-neon`, `aes-arm`, `aes-neonbs`), and `AF_ALG` sockets |
| **Managed NAND, FTL & HFS+** | 8.9 GB managed NAND (`/dev/mtd0`, DMA page reads at ~47 MB/s, PIO writes, block erase), log-structured read-write FTL (`/dev/nand0`) with superblock rotation, bad-block retirement, and garbage collection, hardware key-store and system keybag unlocking (classes 1–4), and read-write `hfsplus` (`System` and `Data`) with journal replay, per-file encryption/decryption, and `decmpfs` transparent decompression |

---

## 2. In-Progress (Partial Support)

| Subsystem | Working | In-Progress / Remaining |
|---|---|---|
| **SGX543 GPU & Wayland Desktop** | Microkernel, MMU, render node, 2D scaled blit, TA/3D queue, OpenGL ES 2.0 & Desktop GL 2.1 (Mesa 26.1.8), PRIME / live-scanout / `EGL/GBM` presentation with implicit sync, Weston 14 kiosk compositor | External input devices (awaiting USB host mode), full desktop shell / Xwayland, 2D solid fill / rotation / multi-transfer queueing, hardware IRQ completion (currently polled by timer), and runtime PM |
| **H.264 / MPEG-4 Video Decoder (VXD)** | Power gate control, automatic firmware carving (`vxd390.fw`) and caching on `Data`, and 40 KiB DMA upload image assembly | Initializing the media clock source (reset and microkernel handshake currently time out), bitstream command submission, and V4L2 mem2mem decoder interface |
| **HDMI Display** | EDID modes up to `1080p30` (the `1080p30` default matches the boot splash), ARGB/XRGB scanout, boot splash, gamma brightness, timer-synthesized vblank | Pixel clocks above 108 MHz (`1080p60` produces link bit errors), hardware vblank interrupt, seamless takeover from the bootloader splash without blanking, and native gamma LUT curve encoding |
| **Audio** | HDMI 2/4/6/8-channel LPCM at exact `32` / `44.1` / `48 kHz`; S/PDIF optical DMA playback at `46,875 Hz` | Clocking S/PDIF optical from audio oscillator 4 for exact `32` / `44.1` / `48 kHz` rates, verifying optical output on an external receiver, and HDMI sample rates above 48 kHz (`88.2` / `96 kHz`) |
| **Hardware Scalers (M2) & JPEG** | Scaling, CSC, crop/compose letterboxing, zero-copy `dma-buf`, and baseline JPEG decoding | Scaler rotation/mirroring ioctls, multi-planar V4L2 API, >7x single-pass vertical reduction; JPEG V4L2 source-change events (progressive JPEG is unsupported by the hardware revision) |
| **Wi-Fi & Bluetooth** | 2.4/5 GHz WPA2 Wi-Fi, Bluetooth 4.1 + LE scanning, BlueZ daemon, UART sleep and host-wake | End-to-end Bluetooth device pairing and connection testing, HSIC USB runtime suspend, and controller-side filtering of non-standard LE advertising report bits |
| **IR Remote & System MCU** | IR button input, probe handshake, ACK-waited commands, event trace, NMI wake, standby power-off | Mapping PMU `menu`/`hold` event numbers, decoding the recovery reboot command, and UART0 MCU bootstrap |
| **RTC & Thermal** | Persistent timekeeping, NTP sync, wake alarm IRQ, 5 temperature sensors, CPU thermal throttling | Waking the system from MCU standby via RTC alarm, and refining board thermistor trip points against die temperature |
| **Managed NAND, FTL & HFS+** | Full read/write FTL, garbage collection, keybag unwrapping, encrypted read-write `hfsplus`, read-only `decmpfs` | TRIM/discard support, writable `decmpfs`-compressed files, passcode-protected keybag unlocking, and investigating a rare CDMA read timeout after extended idle under the `ondemand` governor |
| **IOP Coprocessor** | Clock gate, firmware carving (`iop.fw`), heap/page-table patching, and core start sequence | EmbeddedIOP control-message and endpoint protocol (core currently aborts in its scheduler; no Linux subsystem requires the IOP) |

---

## 3. Pending Support (Not Started)

| Hardware Block | Status & Notes |
|---|---|
| **USB OTG External Host Mode & OHCI Companion** | External Micro-USB port is currently fixed to peripheral mode (`dwc2`). Host mode for USB keyboards, mice, and storage requires OTG role switching and enabling the USB 1.1 OHCI companion controller. |
| **DART IOMMU** | Left in hardware bypass mode (`CONFIG = 0`) so the M2 scalers and JPEG decoder operate directly on physically contiguous buffers. Needed only for scatter-gather DMA and fault isolation. |
| **Public-Key Crypto Engine (PKE)** | Hardware RSA/ECC accelerator is unclaimed; software crypto handles all current workloads. |
| **Dynamic Voltage Scaling (DWI / SWI)** | CPU frequency scaling runs at the boot buck voltage because per-chip voltage calibration tables are not populated in the raw NAND device tree template. |
| **Hardware SHA-2 DMA Mode** | Central DMA FIFO feeding for the SHA-2 engine does not complete on hardware; the driver uses PIO (~15–17 MB/s) while ARM NEON (`sha256-neon` at ~49 MB/s, `sha512-neon` at ~32 MB/s) handles high-throughput hashing. |

---

## Priority Roadmap

1. **External USB Host Mode (OTG + OHCI):** Enable USB keyboards, mice, and storage on the Micro-USB port (unlocking interactive input for Weston and console use).
2. **H.264 / MPEG-4 Video Decoder (VXD):** Initialize the media clock domain to complete the VXD microkernel handshake and build a V4L2 mem2mem video decoder.
3. **SGX543 2D & Desktop Enhancements:** Add 2D fill/rotation operations, runtime power management, and optional desktop shell / Xwayland support.
4. **Optical Audio Clocking:** Switch S/PDIF to audio oscillator 4 for exact `44.1` / `48 kHz` optical output and verify on an S/PDIF receiver.
5. **NAND Idle CDMA Timeout Recovery:** Stress-test FMI DMA reads across `ondemand` frequency transitions and add automatic recovery if a transfer times out.
