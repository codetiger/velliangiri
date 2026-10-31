<p align="center">
  <img src="logo.svg" alt="Velliangiri logo" width="160">
</p>

<h1 align="center">Velliangiri</h1>

<p align="center">
  <strong>Mainline Linux 6.12 for the S5L8947X (<code>j33iap</code>) and related ARMv7 devices.</strong><br>
  Clean-room drivers · Upstream-style kernel overlay · <code>GPL-2.0-only</code>
</p>

---

Velliangiri brings a current, mainline **Linux 6.12** kernel and an **Alpine Linux** userspace to retired ARMv7 hardware built around the **S5L8940X / S5L8947X** SoC family.

Every driver in this repository is written from scratch based on observed hardware behaviour, uses standard Linux kernel subsystems, and is kept as a small overlay on top of a pristine kernel tree rather than a frozen vendor fork.

This project is built for two groups of people:
1. **Users and tinkerers** who want to boot Linux on the **S5L8947X (`j33iap`)** set-top box and use its hardware (HDMI video and audio, optical audio, Ethernet, Wi-Fi, Bluetooth, IR remote, USB networking, internal NAND storage, and hardware AES).
2. **Developers and porters** who want to bring up mainline Linux on other devices in the same SoC family by reusing the interrupt controller, clock, GPIO, I2C, PMU, USB, DMA, crypto, NAND, display, and audio drivers already working here.

---

## Hardware Support (`S5L8947X` / `j33iap`)

Today, the system boots over USB into RAM (an embedded Alpine Linux initramfs) in about 20 seconds and exposes the internal NAND volumes (`/dev/nand0p1` and `/dev/nand0p2`) with full read-write HFS+ and hardware AES support. Nothing is installed to the device, but the internal flash is not left untouched: at every boot `stage2` mounts `System` read-only to copy the wireless firmware and mounts `Data` read-write to keep the SSH host key and Wi-Fi settings under `/linux/etc` there.

### What works

| Hardware | Status & Details |
|---|---|
| **CPU & Core SoC** | Single-core Cortex-A9 up to 850 MHz, CPU frequency scaling (`212.5` / `425` / `850 MHz`), 512 KB PL310 L2 cache, `aic,1` interrupt controller, 24 MHz timer, 42-channel central DMA (`dmaengine`), hardware watchdog, clean reboot and standby power-off |
| **Clocks, GPIO, I2C, PMU** | PMGR peripheral clock gates, 144-pin GPIO and pinctrl controller (with IRQs), 3x I2C controllers, Dialog I2C PMU power switches |
| **HDMI Video** | 720p60 framebuffer console (`simple-framebuffer` + `s5l8947x-display`), a logo boot splash drawn by the pre-kernel loader, 10-bit hardware gamma LUT, and memory-to-memory hardware scaler |
| **Audio (HDMI & Optical)** | ALSA playback (`s5l8947x-audio`) on two PCM devices: HDMI over I2S (`pcmC0D0p`, verified clean with speech) and optical S/PDIF (`pcmC0D1p`), each fed by a cyclic DMA transfer with period interrupts, with HDMI hot-plug mute/unmute and a `wavplay` CLI player |
| **Ethernet** | 10/100 Fast Ethernet (`eth0`, `s5l_fe` MAC + SMSC PHY) with automatic DHCP at boot |
| **USB Networking & Console** | Micro-USB port runs in USB device mode (`dwc2`), providing a virtual Ethernet link (`usb0` at `10.55.0.2`, SSH enabled) and a USB serial console (`/dev/ttyGS0`) |
| **Wi-Fi (2.4 & 5 GHz)** | Broadcom BCM4334 over internal HSIC USB host (`ehci-platform` + custom PHY handshake); scans and joins WPA2 networks via `wpa_supplicant` and the built-in `wifi` helper; firmware, NVRAM, and the factory MAC address are read from the device's own NAND at boot; the regulatory country can be set with `iw reg set` or `country=` in `wpa_supplicant.conf` |
| **Bluetooth** | BCM4334 Bluetooth 4.1 (`hci0` over `uart2`); patch firmware and the factory device address are read from the device's own NAND at boot; LE scanning verified with `btscan` |
| **IR Remote & System MCU** | Infrared remote buttons reported as a standard Linux input device (`S5L8947X IR Remote`, `/dev/input/event*`) via the MSP430 system microcontroller on `uart1` |
| **Hardware AES Engine** | CDMA AES crypto engine (`s5l8947x-aes` at `0x37801000`): `ecb(aes)` and `cbc(aes)` `skcipher` algorithms (128 / 192 / 256-bit keys) over CDMA channels 1 and 2, plus hardware device/group key operations (`s5l8947x_aes_hwkey_crypt()`) |
| **Internal NAND & Volumes** | Managed NAND controller (`s5l8947x-fmi`, 8.9 GB, read/write/erase on `/dev/mtd0`, ~17 MiB/s reads over CDMA) and read-write translation layer (`s5l8947x-ftl`) exposing `/dev/nand0`, `/dev/nand0p1` (`System`, 1.4 GB), and `/dev/nand0p2` (`Data`, 6.5 GB, volume and per-file `com.apple.system.cprotect` class-4 keys unwrapped from the reserved SLC key store); both volumes mount read-write as `hfsplus` with journal verification/replay, transparent file encryption/decryption, and clean unmount handling |

### What doesn't work yet (honest limitations)

| Area | Current Limitation |
|---|---|
| **Passcode-bound keybag classes** | Unauthenticated volume and class-4 per-file keys (`com.apple.system.cprotect`, 7,117 of 7,297 files on `nand0p2`) are unwrapped for read and write, while passcode-bound keybag classes (1–3, 180 files) are not unwrapped. |
| **3D GPU acceleration** | On hold. The PowerVR SGX543 power domain and clocks come up for bus telemetry, but there is no DRM/3D rendering driver. Graphics use the 720p CPU framebuffer. |
| **USB host on the external port** | The external Micro-USB port is currently set to peripheral/device mode only (a CDC-ECM + CDC-ACM composite gadget). Plugging USB flash drives or keyboards into the Micro-USB port is not supported yet. |
| **Audio sample rates** | The I2S and S/PDIF blocks are clocked from the 24 MHz crystal, so HDMI playback runs at `46,875 Hz` or `31,250 Hz` and optical at `46,875 Hz` instead of `48,000 Hz`. Use `wavplay` (which resamples automatically) or ALSA resampling. The optical output has not been checked on a receiver yet. |
| **Bluetooth** | The controller runs at 115,200 baud, and `BlueZ` userspace is not in the small initramfs, so pairing and connections are untested. |
| **Wireless without NAND** | The Wi-Fi firmware and Bluetooth patch cannot be redistributed and are copied from the device's own `System` volume at boot. Without it, Wi-Fi does not come up and Bluetooth runs on its ROM firmware. |

For exact register addresses, driver file paths, and per-block notes, see [progress.md](progress.md).

---

## Running Linux on Your Device

### 1. Prerequisites

- **On your computer:** Docker (the build runs inside a reproducible Debian cross-compile container on Linux or macOS).
- **For the device:** A Micro-USB cable and a way to load and jump to a raw binary payload in RAM over USB.

### 2. Wi-Fi and Bluetooth firmware

Nothing to do: the Broadcom firmware cannot be redistributed, so it is never part of the build. At boot, `stage2` mounts the device's own `System` volume (`/dev/nand0p1`) read-only and copies the Wi-Fi firmware, NVRAM, and Bluetooth patch into `/lib/firmware/brcm/` (the Bluetooth patch is carved out of a system tool by `hcdcarve`, which picks the one patch naming this board). The Ethernet, Wi-Fi, and Bluetooth addresses come from the factory system configuration on the same NAND (`/sys/block/nand0/{emac,wmac,bmac}`). Without a readable `System` volume, Wi-Fi does not come up and Bluetooth runs on its ROM firmware.

*(Optional)* To make the device join your Wi-Fi network automatically at boot, place a standard `wpa_supplicant.conf` at `research/wpa_supplicant.conf` (or point `WIFI_CONF` at another file inside the repository) before building. On first boot it is saved to the `Data` volume, and from then on the saved copy is used. To install your SSH key, pass a public key file inside the repository, for example `SSH_PUBKEY=research/id_ed25519.pub ./scripts/velliangiri.sh all`.

### 3. Build the bootable bundle

```bash
./scripts/velliangiri.sh all
```

This builds the Docker container, assembles the Alpine 3.21 `armhf` initramfs, fetches and patches mainline Linux 6.12, compiles the kernel and Device Tree, and packs everything into a single self-relocating image:

```text
out/boot-bundle-s5l8947x-j33iap.bin
```

You can also run individual build stages with `./scripts/velliangiri.sh rootfs | kernel | bundle | clean`, or open an interactive cross-compile shell with `./scripts/velliangiri.sh shell`.

### 4. Boot and connect

Load `out/boot-bundle-s5l8947x-j33iap.bin` into the device's RAM over USB and jump to its entry point. Within ~20 seconds:

1. **HDMI:** The Velliangiri boot splash appears on screen, followed by the Linux framebuffer console (`tty0`).
2. **USB Network & SSH:** Your computer detects a USB Ethernet gadget. Assign your computer's side of the USB interface an IP in `10.55.0.0/24` (for example `10.55.0.1/24`) and SSH in as `root` (no password by default):
   ```bash
   ssh root@10.55.0.2
   ```
3. **USB Serial Console:** A virtual serial port (`/dev/ttyACM0` or `/dev/cu.usbmodem*`) also appears over the same USB cable with a root login prompt.
4. **Ethernet:** If an Ethernet cable is plugged in, `eth0` gets an IP via DHCP automatically.

### 5. Useful commands on the device

```bash
hwtest                       # Run the automated on-device hardware test suite
hwtest -i display audio      # Run interactive display and audio checks
mount -t hfsplus /dev/nand0p1 /mnt        # Mount the System volume read-write (-o ro for read-only)
mount -t hfsplus /dev/nand0p2 /mnt        # Mount the decrypted Data volume read-write (-o ro for read-only)
wifi scan                    # Scan for 2.4 GHz and 5 GHz Wi-Fi networks
wifi connect "MySSID" "PSK"  # Connect to WPA2 Wi-Fi and obtain a DHCP lease
wifi status                  # Check Wi-Fi link speed and IP address
wavplay /path/to/file.wav    # Play a WAV file over HDMI (-d /dev/snd/pcmC0D1p for optical)
btscan 10                    # Scan for Bluetooth LE advertisers for 10 seconds
```

---

## Porting to Similar Devices

The `S5L8947X` is one chip in the **S5L8940X** family (alongside related dual-core and variant SoCs from the same generation). Most of the on-chip peripherals use the same register layout across the family, which makes bringing up another board much easier than starting from zero.

### Reusable drivers in this repository

Drivers that apply across the SoC family are already named `s5l8940x` and parameterized via Device Tree:

| Subsystem | Driver Path | Notes for Porters |
|---|---|---|
| **Interrupts & Timer** | `drivers/irqchip/irq-apple-aic1.c` | `aic,1` controller and 24 MHz local timer. Already includes `CONFIG_SMP` IPI support and per-CPU register windows for multi-core chips. |
| **Watchdog & Reset** | `arch/arm/mach-apple/` | 24 MHz watchdog timer, clocksource, and system restart handler (`apple,s5l8940x-wdt-timer`). |
| **Clocks (PMGR)** | `drivers/clk/clk-s5l8940x-pmgr.c` | Linear table of 32-bit peripheral clock gate registers (`0xf` to enable, bits `7:4` status). |
| **CPU Frequency** | `drivers/cpufreq/s5l8947x-cpufreq.c` | PMGR performance state switching without disturbing peripheral baud clocks. |
| **GPIO & Pinctrl** | `drivers/pinctrl/pinctrl-s5l8940x-gpio.c` | Banked pin controller and GPIO IRQ chip (`apple,npins` set in DT). |
| **I2C** | `drivers/i2c/busses/i2c-s5l8940x.c` | Master controller for `i2c0`..`i2c2`. |
| **PMU & Regulators** | `drivers/mfd/apple-pmu-i2c.c`<br>`drivers/regulator/apple-pmu-pwrsw.c` | Dialog I2C PMU (`D1815` / `D1974` / `D2045`) regmap MFD and GPIO-backed power switches. |
| **USB 2.0 PHY** | `drivers/phy/phy-apple-s5l-usb.c` | OTG device PHY (paired with mainline `dwc2`) and EHCI HSIC host PHY with GPIO ready handshaking. |
| **UART** | `scripts/patches/0002-*.patch` | Hooks `ARCH_APPLE_S5L` into mainline `samsung_tty.c` (`apple,s5l-uart`). |
| **Display & HDMI** | `drivers/video/fbdev/s5l8947x-display.c` | `rgbout` display pipe, 10-bit gamma LUTs, M2M hardware scaler, and HDMI 1.4 transmitter. |
| **Central DMA** | `drivers/dma/s5l8947x-cdma.c` | 42-channel `dmaengine` provider: slave scatter-gather (both directions, audio `0x34000000` and NAND `0x31000000` windows) and cyclic transfers with per-channel interrupts. |
| **Audio** | `sound/arm/s5l8947x-audio.c` | I2S (HDMI) and S/PDIF (optical) transmitters, each fed by a cyclic DMA transfer. |
| **CDMA AES Engine** | `drivers/crypto/s5l8947x-aes.c`<br>`include/linux/s5l8947x-aes.h` | `ecb(aes)` and `cbc(aes)` `skcipher` algorithms over CDMA channels 1 and 2, plus `s5l8947x_aes_hwkey_crypt()` for the hardware device and group keys. |
| **Managed NAND & FTL** | `drivers/mtd/devices/s5l8947x-fmi.c`<br>`drivers/mtd/s5l8947x-ftl.c`<br>`scripts/patches/0008-*.patch` | Managed NAND controller (`FMI0`), read-write translation layer (`/dev/nand0`), key-store unwrapping, and encrypted volume + per-file `hfsplus` read/write encryption and journal handling (`/dev/nand0p1`, `/dev/nand0p2`). |

### How the boot bundle works (`scripts/boot/`)

Device bootloaders in this family do not boot standard Linux `zImage` + Device Tree pairs directly—they may enter with the MMU or caches active, leave peripheral clocks gated off, and pass custom boot arguments instead of a Flattened Device Tree.

To solve this cleanly for any board, `scripts/build-bundle.sh` wraps the kernel in a tiny, self-relocating pre-kernel loader:

1. **Position-independent entry stub (`scripts/boot/entry-stub.S`):** Runs from whatever RAM address the bootloader loads it at. It masks interrupts, cleanly flushes/invalidates L1 and PL310 L2 caches whether the MMU was on or off, disables the MMU, arms the hardware watchdog as a 120-second safety net, copies the payload to the start of RAM (`0x80000000`), and jumps there.
2. **Pre-kernel C loader (`scripts/boot/boot-loader.c`):** Runs in the first 32 KB of RAM (`0x80000000..0x80007fff`), turns on essential PMGR clock gates, draws the boot splash and starts 720p scanout for `simple-framebuffer`, and jumps to `zImage` at `0x80008000` with `r2` pointing to the appended `.dtb`. Its stack grows down from `0x80007000` in the same slot, so the build rejects a loader image larger than 24 KB.
3. **Boot splash (`scripts/boot/gen-splash.py`, `splash.c`):** At build time the generator flattens `logo.svg` and the wordmark into polygon tables with precomputed edge slopes, and the loader fills them into the framebuffer with no runtime division. To brand a port, replace `logo.svg`.

### Step-by-step guide to porting a new board

If you have another device with a serial console (or USB / framebuffer output) and a way to load code into RAM:

1. **Adapt the pre-kernel stub (`scripts/boot/`):**
   - Check the physical RAM base address (`0x80000000` on `S5L8947X`) and watchdog base in `entry-stub.S`.
   - If the bootloader already sets up a display framebuffer, point a `simple-framebuffer` node at that physical address to get an instant early console (`earlycon` / `tty0`) before writing any platform drivers.
2. **Create a SoC `.dtsi` and board `.dts` (`arch/arm/boot/dts/apple/`):**
   - Copy `s5l8947x.dtsi` and `s5l8947x-j33iap.dts` as a template.
   - Update the MMIO base addresses, IRQ numbers, and clock gate indices for your chip. Start with just `aic`, `wdt`, `uart0` (or `usb0` for USB gadget console/network), and `simple-framebuffer`.
3. **Use the built-in hardware exploration tools:**
   - Once the initramfs boots to a shell over serial or USB, the image includes static diagnostic utilities compiled from `scripts/tools/`:
     - `peek <phys-addr> [count]` / `peek -w <phys-addr> <value>` — read or write 32-bit MMIO registers from userspace.
     - `i2cpoke <bus> scan` / `i2cpoke <bus> <addr> <reg> [count]` — scan I2C buses and read or write device registers.
     - `l2bench` — measure L1, L2 and DRAM latency to confirm the caches are on.
4. **Enable peripherals incrementally:**
   - Turn on `gpio`, `i2c`, `pmu`, `usb_phy`, `cdma`, `aes`, `fmi0`, `display_pipe`, and `audio` nodes one by one in your board's `.dts`, adding compatible strings or small register-offset tables to the existing drivers where your chip differs.

If you are working on bringing up a related device, feel free to open an issue with your SoC and board details.

---

## Repository Layout

Instead of storing a full kernel source tree, this repository holds only the files added for the platform and a short patch series for existing kernel files. `scripts/apply-kernel-tree.py` overlays them onto a clean Linux 6.12 checkout (and can `--revert` back to a pristine tree at any time).

| Path | Contents |
|---|---|
| `arch/arm/boot/dts/apple/` | SoC (`.dtsi`) and board (`.dts`) Device Trees |
| `arch/arm/mach-apple/` | Machine init, CPU clock calibration, watchdog clocksource & reset |
| `drivers/`, `sound/`, `include/` | Clean-room kernel drivers in their standard mainline paths |
| `scripts/patches/` | 9 small, self-contained patches to mainline Linux 6.12 files |
| `scripts/boot/` | Position-independent entry stub, pre-kernel C loader, HDMI init, boot splash and its generator |
| `scripts/tests/` | On-device hardware test suite (`hwtest`) |
| `scripts/tools/` | Userspace tools built into the image: diagnostics (`peek`, `i2cpoke`, `l2bench`), `wavplay`, `btscan`, the Bluetooth patch carver `hcdcarve`, and the early entropy seeder `jitterseed` |
| `config/` | Kernel configuration fragments layered on `multi_v7_defconfig` |
| `initramfs/` | Init scripts, USB gadget / SSH setup, and the `wifi` helper |

---

## Contributing & Clean-Room Rules

Contributions of any kind are welcome—whether that is a new board Device Tree, a driver for an unsupported hardware block, a bug fix, or hardware test coverage.

High-impact open tasks on `S5L8947X` right now (see [progress.md](progress.md) for details):
- **USB OTG host mode** (role switching + OHCI companion controller at `0x36500000`) to allow plugging in external USB devices.
- **Audio at exact 48 kHz**: clock the I2S master from the PMGR clock synthesizer instead of the 24 MHz crystal.

### Guardrails

To keep the work upstreamable and legally clean, this repository enforces strict source-tree rules. Before making your first commit, install the git hooks:

```bash
./scripts/setup-guardrails.sh
```

And run the verification check before submitting changes:

```bash
./scripts/velliangiri.sh check
```

1. **Clean-room & open-source (`GPL-2.0-only`) only:** Write code only from observed hardware behaviour and public specifications.
2. **No binaries or blobs:** Never commit firmware, binary objects, archives, raw memory dumps, or cryptographic keys.
3. **No proprietary terminology:** Do not use internal vendor codenames, proprietary bootloader names, or key names in code, comments, documentation, or commit messages.
4. **Keep local research separate:** Store any local notes, firmware files, or non-distributable test material strictly inside the git-ignored `research/` directory.

---

## The Name

Velliangiri is a mountain range in the Western Ghats of South India; *velli* means white. The logo is that peak, with a white slope and a Linux penguin's beak for a summit.

## License

All kernel code, drivers, and build scripts are licensed under `GPL-2.0-only`. See [LICENSE](LICENSE).
