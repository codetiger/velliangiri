# Project Velliangiri

Mainline **Linux 6.12** kernel port and open-source (`GPL-2.0-only`) hardware drivers for the **S5L8947X** (32nm single-core ARMv7 Cortex-A9, `j33iap`) SoC platform.

## Verified Hardware Subsystems (`S5L8947X` / `j33iap`)

- **Core SoC (`arch/arm/mach-apple/`, `drivers/irqchip/irq-apple-aic1.c`)**:
  - `aic,1` interrupt controller (`0x3f200000`) with per-CPU event handling (`0x5004`) and built-in **24 MHz local timer** (`clock_event_device`, rating 400).
  - Continuous **24 MHz hardware watchdog clocksource** (`apple-wdt-24m`, rating 350) and `sched_clock` at `0x3f103020` (active across `WFI`), Cortex-A9 `PMCCNTR` delay calibration, SoC restart handler, and `/dev/watchdog` character device.
- **Clocks & Pinctrl (`drivers/clk/clk-s5l8940x-pmgr.c`, `drivers/pinctrl/pinctrl-s5l8940x-gpio.c`)**:
  - Power and clock gate controller (`PMGR` at `0x3f100000`) and 224-pin GPIO / hierarchical IRQ controller (`0x3f000000`).
- **I2C & Power Management (`drivers/i2c/busses/i2c-s5l8940x.c`, `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c`)**:
  - Hardware I2C master (`i2c0@33200000`, `i2c1@33300000`, `i2c2@33400000`) with combined repeated-START FIFO transactions.
  - Dialog D1815 PMU MFD (`0x74` on `i2c0`, 8-bit regmap) and GPIO power-switch regulators (`eth-phy-reset` at `0x58` and `wifi-reg-on` at `0x57`).
- **Networking (`drivers/net/ethernet/apple/s5l_fe.c`)**:
  - 10/100 Fast Ethernet MAC (`0x3b000000`, IRQ 27) with NAPI descriptor rings, MDIO bus, SMSC LAN8742 PHY, DHCP (`udhcpc`), Dropbear SSH, and UDP `netconsole`.
- **USB 2.0 OTG & Host PHY (`drivers/phy/phy-apple-s5l-usb.c`)**:
  - Synopsys `dwc2` USB 2.0 OTG peripheral (`0x36100000`, IRQ 11) with CDC-ECM (`usb0` at `10.55.0.2/24`) and CDC-ACM (`ttyGS0`) composite gadget, plus EHCI/HSIC host PHY support.
- **Display (`scripts/boot/hdmi-720p.h`, `arch/arm/boot/dts/apple/s5l8947x-j33iap.dts`)**:
  - Pre-kernel 720p@60Hz HDMI transmitter/PHY (`0x39700000`) and `rgbout` display pipe (`0x3a200000`) scanning out reserved DRAM at `0x9ec00000` (`simple-framebuffer` / `fbcon`).

## Repository Layout

- `arch/arm/boot/dts/apple/` — `s5l8947x.dtsi` SoC include and `s5l8947x-j33iap.dts` board device tree
- `arch/arm/mach-apple/` — `S5L8947X` machine descriptor, `PMCCNTR` calibration, 24 MHz WDT clocksource, and `/dev/watchdog`
- `drivers/` — Clean-room `S5L8947X` kernel drivers (`irqchip`, `clk`, `pinctrl`, `i2c`, `mfd`, `regulator`, `phy`, `net/ethernet/apple`)
- `config/` — Linux 6.12 configuration fragments (`s5l8947x.config`, `s5l8947x-trim.config`) and initramfs prune list
- `initramfs/` — Early boot (`/init`), Stage 2 service bring-up, and `udhcpc` scripts
- `scripts/` — Dockerized cross-build environment, kernel fetch/patch/build scripts, pre-kernel loader (`scripts/boot/`), and userspace diagnostic tools (`scripts/tools/`)

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

## License

All kernel code, drivers, and build scripts in this repository are licensed under `GPL-2.0-only`. See [LICENSE](LICENSE) for details.
