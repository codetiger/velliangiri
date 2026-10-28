# Project Velliangiri

Mainline **Linux 6.12** kernel port and open-source (`GPL-2.0-only`) hardware drivers for the **S5L8947X** (32nm single-core ARMv7 Cortex-A9, `j33iap`) SoC platform.

## Verified Hardware Subsystems (`S5L8947X` / `j33iap`)

- **Core SoC (`arch/arm/mach-apple/`, `drivers/irqchip/irq-apple-aic1.c`)**:
  - `aic,1` interrupt controller (`0x3f200000`) with per-CPU event handling (`0x5004`) and built-in **24 MHz local timer** (`clock_event_device`, rating 400).
  - Continuous **24 MHz hardware watchdog clocksource** (`apple-wdt-24m`, rating 350) and `sched_clock` at `0x3f103020` (active across `WFI`), Cortex-A9 `PMCCNTR` delay calibration, SoC restart handler, and `/dev/watchdog` character device.
- **Clocks & Pinctrl (`drivers/clk/clk-s5l8940x-pmgr.c`, `drivers/pinctrl/pinctrl-s5l8940x-gpio.c`)**:
  - PMGR peripheral clock gate provider (`0x3f101008`, one specifier cell = gate index, request/status handshake). Drivers switch gates only through the common clock framework (`clocks = <&pmgr_gates N>`), so `hwtest clocks` can account for them. The I2C gates are still left running by the pre-kernel loader rather than claimed by the I2C driver, and the not-yet-used USB host/HSIC PHY path switches its PMGR power domains directly.
  - 224-pin GPIO / pinctrl and GPIO interrupt controller driver (`0x3f000000`). It is built but not instantiated yet: the device tree has no GPIO node.
- **I2C & Power Management (`drivers/i2c/busses/i2c-s5l8940x.c`, `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c`)**:
  - Hardware I2C master (`i2c0@33200000`, `i2c1@33300000`, `i2c2@33400000`) with combined repeated-START FIFO transactions.
  - Dialog D1815 PMU MFD (`0x74` on `i2c0`, 8-bit regmap) and GPIO power-switch regulators (`eth-phy-reset` at `0x58` and `wifi-reg-on` at `0x57`).
- **Networking (`drivers/net/ethernet/apple/s5l_fe.c`)**:
  - 10/100 Fast Ethernet MAC (`0x3b000000`, IRQ 27) with NAPI descriptor rings, MDIO bus, SMSC LAN8742 PHY, DHCP (`udhcpc`), Dropbear SSH, and UDP `netconsole`.
- **USB 2.0 OTG & Host PHY (`drivers/phy/phy-apple-s5l-usb.c`)**:
  - Synopsys `dwc2` USB 2.0 OTG peripheral (`0x36100000`, IRQ 11) with CDC-ECM (`usb0` at `10.55.0.2/24`) and CDC-ACM (`ttyGS0`) composite gadget, plus EHCI/HSIC host PHY support (`0x36400000`, IRQ 12).
- **System / IR Remote MCU (`drivers/input/misc/s5l8947x-mcu.c`)**:
  - serdev client of `uart1` (`0x32600000`, IRQ 22, 115200 baud 8N1, mainline Samsung UART driver) speaking CRC-16/CCITT-FALSE `"St"` frames. Reports remote buttons as the `S5L8947X IR Remote` input device; firmware query, line break and raw commands on demand through sysfs; standby is sent only on halt/power-off.
- **BCM4334 Bluetooth & Wi-Fi power**:
  - Bluetooth via the mainline `hci_uart_bcm` serdev driver on `uart2` (`0x32700000`, IRQ 23, 115200 baud). Wi-Fi `REG_ON` (PMU `0x57`) is a `regulator-output` switched from userspace until the EHCI HSIC host and `brcmfmac` are enabled.
- **Display Pipe, HDMI 1.4 & M2M Scaler (`scripts/boot/hdmi-720p.h`, `drivers/video/fbdev/s5l8947x-display.c`)**:
  - Pre-kernel 720p@60Hz HDMI transmitter/PHY (`0x39700000`), `rgbout` display pipe (`0x3a200000`) scanning out reserved DRAM at `0x9ec00000` (`simple-framebuffer` / `fbcon`), runtime Plane 0 DMA page-flipping, 10-bit 3x256 hardware Gamma LUT scaling (`0x39600000`), HDMI Audio InfoFrame/ACR bridge, and M2M hardware scaler (`0x39200000`, PMGR gate 26, switched at runtime).
- **Audio (`sound/arm/s5l8947x-audio.c`)** — *bring-up, HDMI audio not yet confirmed on hardware*:
  - ALSA PCM and mixer driver (`S5L8947X HDMI/SPDIF`, 48 kHz S16_LE stereo) for `i2s0` HDMI audio (`0x34190000`) and the `spdif` optical TOSLINK transmitter (`0x34194000`).
  - No DMA engine driver yet: the ring buffer is fed to the I2S0 TX FIFO by PIO from a 250 µs hrtimer, paced by the FIFO full flag; S/PDIF gets one 16-bit sample per write. The I2S block turned out not to use the Samsung register layout the driver programs; the measured layout is documented at the top of `scripts/tools/audiotest.c`, whose `rate` and `pio` commands already use it. I2S0 runs from 24 MHz, so the closest rate to 48 kHz is 46 875 Hz; HDMI audio is still silent.
- **PowerVR SGX543 GPU (`drivers/soc/apple/s5l8947x-sgx.c`)**:
  - Clock/power domain switch (PMGR gates 7/8/9) and core telemetry driver (`/dev/sgx0`, `core_id=0x0c3fd6a7`) for the `sgx` GPU at `0x38000000` (IRQ 49).

## Repository Layout

- `arch/arm/boot/dts/apple/` — `s5l8947x.dtsi` SoC include and `s5l8947x-j33iap.dts` board device tree
- `arch/arm/mach-apple/` — `S5L8947X` machine descriptor, `PMCCNTR` calibration, 24 MHz WDT clocksource, and `/dev/watchdog`
- `drivers/` — Clean-room `S5L8947X` kernel drivers (`irqchip`, `clk`, `cpufreq`, `pinctrl`, `i2c`, `mfd`, `regulator`, `phy`, `net/ethernet/apple`, `input/misc`, `video/fbdev`, `soc/apple`)
- `sound/arm/` — `S5L8947X` I2S0 / S/PDIF ALSA driver
- `config/` — Linux 6.12 configuration fragments (`s5l8947x.config`, `s5l8947x-trim.config`) and initramfs prune list
- `initramfs/` — Early boot (`/init`), Stage 2 service bring-up, and `udhcpc` scripts
- `scripts/` — Dockerized cross-build environment, kernel fetch/patch/build scripts, pre-kernel loader (`scripts/boot/`), userspace diagnostic tools (`scripts/tools/`), and the on-device test suite (`scripts/tests/`)
- `scripts/patches/` — Changes to mainline files, applied in order on top of Linux 6.12. `0001` only wires the platform and drivers into Kconfig/Makefiles; the rest are small, individually described changes to core code (Samsung UART, AIC re-init in `init/main.c`, ARM alignment fixup, dwc2, scheduler)

## Kernel Tree Overlay

`scripts/apply-kernel-tree.py` copies `arch/`, `drivers/`, `include/` and `sound/` into the kernel tree and applies `scripts/patches/`. It is idempotent and keeps the tree in step with the repository:

- files moved or deleted here are removed from the kernel tree (tracked in `.velliangiri-overlay-files`);
- if any patch was edited, renamed or removed, every previously applied patch is reverted and the current series is applied again;
- `--revert` restores the pristine tree.

Inside the build container the tree lives in the `velliangiri-kbuild` Docker volume at `/kbuild/linux`; `./scripts/velliangiri.sh shell` opens a shell with the same mounts.

## Building

```bash
# Build everything (Alpine 3.21 armhf initramfs + Linux 6.12 zImage/DTB + boot bundle)
./scripts/velliangiri.sh all

# Individual stages
./scripts/velliangiri.sh rootfs
./scripts/velliangiri.sh kernel
./scripts/velliangiri.sh bundle

# Verify source tree hygiene and guardrail compliance
./scripts/velliangiri.sh check
```

Build outputs are written to `out/`:
- `out/initramfs-s5l8947x.cpio` — Alpine `armhf` initramfs archive
- `out/zImage` — Single-stream XZ compressed Linux 6.12 kernel with embedded initramfs
- `out/s5l8947x-j33iap.dtb` — Compiled Flattened Device Tree
- `out/boot-bundle-s5l8947x-j33iap.bin` — Self-relocating entry stub + pre-kernel loader + `zImage` + DTB

## Hardware Tests

Nothing is tested at boot. The initramfs ships an on-demand test suite (`scripts/tests/`, installed as `/bin/hwtest`) and the `audiotest` helper (`scripts/tools/audiotest.c`):

```bash
ssh root@10.55.0.2 hwtest                 # probe, clocks, display, audio, gpu, mcu, wireless
ssh -t root@10.55.0.2 hwtest -i audio     # interactive: confirm what the TV played
ssh root@10.55.0.2 audiotest rate         # measure the I2S0 frame clock (FIFO-paced, 1 s)

# Run updated test scripts without rebuilding the initramfs
scp -r scripts/tests root@10.55.0.2:/tmp/tests
ssh -t root@10.55.0.2 HWTEST_DIR=/tmp/tests sh /tmp/tests/hwtest.sh -i audio
```

Each check prints `PASS`, `FAIL`, or `SKIP`; the exit status is non-zero if anything failed. `hwtest clocks` checks every PMGR gate a driver holds against its status bits and lists the gates the boot loader left running. No test sends the MCU standby state, which puts the whole box into standby.

## License

All kernel code, drivers, and build scripts in this repository are licensed under `GPL-2.0-only`. See [LICENSE](LICENSE) for details.
