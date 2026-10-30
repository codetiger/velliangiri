# Project Velliangiri

Mainline **Linux 6.12** kernel port and open-source (`GPL-2.0-only`) hardware drivers for the **S5L8947X** (32nm single-core ARMv7 Cortex-A9, `j33iap`) SoC platform.

## Verified Hardware Subsystems (`S5L8947X` / `j33iap`)

- **Core SoC (`arch/arm/mach-apple/`, `drivers/irqchip/irq-apple-aic1.c`)**:
  - `aic,1` interrupt controller (`0x3f200000`) with per-CPU event handling (`0x5004`) and built-in **24 MHz local timer** (`clock_event_device`, rating 400).
  - Continuous **24 MHz hardware watchdog clocksource** (`apple-wdt-24m`, rating 350) and `sched_clock` at `0x3f103020` (active across `WFI`), Cortex-A9 `PMCCNTR` delay calibration, SoC restart handler, and `/dev/watchdog` character device.
- **L2 cache (mainline `arch/arm/mm/cache-l2x0.c`, `0x3e000000`)**:
  - PL310 r3p1-50rel0, 16 ways x 32 KB = 512 KB, brought up by `l2x0_of_init()` from `s5l8947x_init_irq()` (a machine without auxiliary overrides is otherwise never initialised). The pre-kernel loader cleans and invalidates L2 and leaves it **disabled**, so Linux always configures it from a known state: erratum 769419 workaround, early BRESP, full line of zeros, clock gating. Latencies stay at the hardware defaults and prefetch is off, because prefetch measured slower. `l2bench` shows the effect: 64-512 KB working sets read in 45-69 ns against 136-160 ns from DRAM.
- **Clocks & Pinctrl (`drivers/clk/clk-s5l8940x-pmgr.c`, `drivers/pinctrl/pinctrl-s5l8940x-gpio.c`)**:
  - PMGR peripheral clock gate provider (`0x3f101008`, one specifier cell = gate index, request/status handshake). Drivers switch gates only through the common clock framework (`clocks = <&pmgr_gates N>`), so `hwtest clocks` can account for them. The I2C controllers claim their own gates, and the USB PHY holds the OTG gates while bound and the host/HSIC gates while the host PHY is powered. The pre-kernel loader still pre-enables a few gates for the console and early boot.
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
- **BCM4334 Wi-Fi & Bluetooth**:
  - Wi-Fi on EHCI port 2 over HSIC (`0x36400000`, IRQ 12), driven by `brcmfmac` over USB (`scripts/patches/0007-*`): the host PHY raises `REG_ON` (PMU `0x57`), releases the HSIC port and does the host/device-ready handshake (GPIO 113/128). Scans 2.4 and 5 GHz; `wpa_supplicant`, `iw` and the `wifi` helper (`wifi scan`, `wifi connect SSID [PSK]`, `wifi status`, `wifi down`) are in the initramfs, with the `wireless-regdb` database.
  - Firmware and NVRAM are not distributable: place `brcmfmac4334.bin` and `brcmfmac4334.txt` (the board's TRX firmware and NVRAM text from `/usr/share/firmware/wifi/4334b1/` in your own tvOS root filesystem) in `research/firmware/wifi/`. The firmware needs a MAC in its NVRAM; the DT supplies a locally administered one (`local-mac-address` on `wifi@2`). To join a network at boot, put a `wpa_supplicant.conf` at `research/wpa_supplicant.conf` (or set `WIFI_CONF`); it is copied into the image, so keep the built image private.
  - Bluetooth via the mainline `hci_uart_bcm` serdev driver on `uart2` (`0x32700000`, IRQ 23); the controller does not answer HCI Reset yet.
- **Display Pipe, HDMI 1.4 & M2M Scaler (`scripts/boot/hdmi-720p.h`, `drivers/video/fbdev/s5l8947x-display.c`)**:
  - Pre-kernel 720p@60Hz HDMI transmitter/PHY (`0x39700000`), `rgbout` display pipe (`0x3a200000`) scanning out reserved DRAM at `0x9ec00000` (`simple-framebuffer` / `fbcon`), runtime Plane 0 DMA page-flipping, 10-bit 3x256 hardware Gamma LUT scaling (`0x39600000`), HDMI Audio InfoFrame/ACR bridge, and M2M hardware scaler (`0x39200000`, PMGR gate 26, switched at runtime).
- **Audio (`sound/arm/s5l8947x-audio.c`)**:
  - ALSA PCM (`S5L8947X HDMI/SPDIF`, 16-bit stereo at 46 875 or 31 250 Hz) driving the S/PDIF transmitter (`0x34194000`), which feeds both the HDMI transmitter and the optical TOSLINK output. The only rates the 24 MHz reference can produce are 46 875 and 31 250 Hz, and those are exactly the rates advertised; applications resample.
  - Fed by central DMA channel 31 from a coherent ring of 4 KB descriptors (single-word bursts, one word per FIFO request). The playback position follows the 24 MHz frame clock, and the controller's address register is watched as a liveness signal that raises an XRUN if the clock stops.
  - The S/PDIF frame is four FIFO words (`0, L, 0, R`: the 16-bit sample left-justified in the 32-bit subframe), the ACR is programmed from the produced rate (`CTS = 3 564 000 000 / fs`), the Audio InfoFrame uses the three-byte header, and the S/PDIF receiver is enabled with `I2S_CLK_CON` left at 0 — setting it to 1 selects the (dead) I2S clock and silences the path.
  - `wavplay` (`scripts/tools/wavplay.c`) plays 8/16-bit PCM WAV files of any rate and channel count, converting to 46 875 Hz stereo.
- **PowerVR SGX543 GPU (`drivers/soc/apple/s5l8947x-sgx.c`)** — *on hold*:
  - Clock/power domain switch (PMGR gates 7/8/9 (GFX_SYS, GFX, HPERF-NRT)) and core telemetry driver (`/dev/sgx0`) for the `sgx` GPU at `0x35100000` (IRQ 49). Only the always-clocked master block (`cfg0` at `0x4010`) is read; the per-core registers at the bottom of the window hang the bus until a core is brought up.

## Pending Work

See [progress.md](progress.md) for the full list. In short:

- **Not started:** NAND flash controller (FMI), hardware crypto, I2S1..3.
- **On hold:** SGX GPU (no command submission, no on-device test).
- **Partly done:** HDMI audio (register layout rework, no DMA), Wi-Fi over HSIC (no EHCI node or `brcmfmac`), USB host mode, GPIO/pinctrl (no DT node).

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

Nothing is tested at boot. The initramfs ships an on-demand test suite (`scripts/tests/`, installed as `/bin/hwtest`) and small helpers in `scripts/tools/`:

```bash
ssh root@10.55.0.2 hwtest                 # probe, clocks, cache, display, audio, mcu, wireless
ssh -t root@10.55.0.2 hwtest -i audio     # interactive: confirm what the TV played
ssh root@10.55.0.2 l2bench              # memory latency by working-set size
scp sample.wav root@10.55.0.2:/tmp && ssh root@10.55.0.2 wavplay /tmp/sample.wav

# Run updated test scripts without rebuilding the initramfs
scp -r scripts/tests root@10.55.0.2:/tmp/tests
ssh -t root@10.55.0.2 HWTEST_DIR=/tmp/tests sh /tmp/tests/hwtest.sh -i audio
```

Each check prints `PASS`, `FAIL`, or `SKIP`; the exit status is non-zero if anything failed. `hwtest clocks` checks every PMGR gate a driver holds against its status bits and lists the gates the boot loader left running. No test sends the MCU standby state, which puts the whole box into standby.

## License

All kernel code, drivers, and build scripts in this repository are licensed under `GPL-2.0-only`. See [LICENSE](LICENSE) for details.
