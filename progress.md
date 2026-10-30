# Driver Progress (`S5L8947X` / `j33iap`)

Status of every hardware block, checked against the drivers in the tree and the enabled nodes in `s5l8947x-j33iap.dts`. Last reviewed 2026-10-30.

## Done

| Block | Address | Driver |
|---|---|---|
| Interrupt controller, 24 MHz timer | `0x3f200000` | `drivers/irqchip/irq-apple-aic1.c` |
| Watchdog, clocksource, restart | `0x3f103020` | `arch/arm/mach-apple/` |
| L2 cache (PL310, 512 KB) | `0x3e000000` | mainline `cache-l2x0.c`, started from `arch/arm/mach-apple/s5l8947x.c`; loader leaves it disabled |
| PMGR clock gates | `0x3f101008` | `drivers/clk/clk-s5l8940x-pmgr.c`; I2C and USB host gates are claimed by their drivers. The loader still pre-enables a few for the console and early boot. |
| CPU frequency (212.5 / 425 / 850 MHz) | `0x3f100000` | `drivers/cpufreq/s5l8947x-cpufreq.c`; every state keeps the peripheral divider, so the 51.3 MHz UART clock does not change with the CPU speed |
| I2C 0/1/2 | `0x33200000`.. | `drivers/i2c/busses/i2c-s5l8940x.c` |
| Power management unit and power switches | I2C0 `0x74` | `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c` |
| 10/100 Ethernet | `0x3b000000` | `drivers/net/ethernet/apple/s5l_fe.c` |
| USB 2.0 device mode and PHY | `0x36100000` | `drivers/phy/phy-apple-s5l-usb.c` + mainline `dwc2` |
| USB 2.0 host (EHCI) on the HSIC port | `0x36400000` | mainline `ehci-platform`; host PHY in `drivers/phy/phy-apple-s5l-usb.c` (HSIC tuning, INCRx, port reset release, host-ready/device-ready handshake, device ready in about 42 ms) |
| GPIO controller (144 pins, 18 ports) | `0x3fa00000` | `drivers/pinctrl/pinctrl-s5l8940x-gpio.c`, IRQ 119; probes on hardware and drives the HSIC handshake lines (GPIO 113 out, 128 in) |
| Display pipe, HDMI 720p, scaler, gamma | `0x3a200000` | `drivers/video/fbdev/s5l8947x-display.c`; the pre-kernel loader (`scripts/boot/hdmi-720p.h`) brings up 720p60 scanout and draws a boot splash of `logo.svg` (polygon tables generated at build time by `scripts/boot/gen-splash.py`, rasterized by `splash.c`) in place of the old colour-bar pattern |
| System / IR remote MCU (UART1) | `0x32600000` | `drivers/input/misc/s5l8947x-mcu.c` |
| Wi-Fi (BCM4334, HSIC) | EHCI port 2 | mainline `brcmfmac` USB, patched by `scripts/patches/0007-*` (boot loader `bd1a`, NVRAM packed into the TRX, optional CLM blob). Firmware 6.53.3 from user-supplied `.bin` + `.txt`, MAC from the DT. `wlan0` scans 2.4 and 5 GHz and joins WPA2-PSK networks with `wpa_supplicant` and the `wifi` helper (verified 2026-10-30: 5 GHz, 135 Mbit/s, DHCP lease, routed traffic; `hwtest wireless` 5/5). `wireless-regdb` installed; optional build-time `wpa_supplicant.conf` is joined at boot. |
| Bluetooth (BCM4334, UART2) | `0x32700000` | mainline `hci_uart_bcm`; patch firmware from the user's restore image is loaded at boot (HCI 4.1, revision 2393, LE), repeated rebinds clean. Verified 2026-10-30 on the radio: a passive LE scan with `btscan` (raw HCI socket, no Bluetooth userland needed) heard 19 advertisers and 546 reports in 10 s; `hwtest wireless` 8/8 runs the same scan. |
| Central DMA (42 channels) | `0x37000000` | `drivers/dma/s5l8947x-cdma.c`, `dmaengine` provider (`dmas = <&cdma channel request>`): slave scatter-gather (both directions) and cyclic transfers, per-channel interrupts (end of list, and per segment for cyclic periods), segment-granularity residue. Verified 2026-10-30 with audio: 3.00 s plays in 3.02 s at both rates, about 46 period interrupts a second, repeated start/stop clean, `hwtest` 55/55. |
| Audio: I2S -> HDMI, S/PDIF -> optical | `0x34190000`, `0x34194000`, `0x34195000` | `sound/arm/s5l8947x-audio.c`, two PCMs fed by cyclic `dmaengine` transfers (period interrupts, position from the DMA residue). PCM 0 (HDMI, `pcmC0D0p`): I2S0 as a slave of the I2S clock master, CDMA channel 23, 46 875 / 31 250 Hz; the slave only raises DMA requests with a FIFO threshold set in its TX control register. PCM 1 (optical, `pcmC0D1p`): S/PDIF at 46 875 Hz with one word per channel, CDMA channel 31. HDMI packet layer in `s5l8947x-display.c` (I2S receiver in basic I2S format, reset pin selection, ACR from the produced rate, valid AUI; `hdmi_audio` accepts `off`). Verified 2026-10-30: speech is clean over HDMI at both rates, 4.28 s plays in 4.31-4.33 s with about 46 period interrupts a second, measured CTS matches 46 875 and 31 250 Hz, optical DMA runs at real time, `hwtest audio` 5/5. Hot-plug loss mutes the packets and replug re-announces. `wavplay` plays WAV files (resampled to 46 875 Hz; `-d /dev/snd/pcmC0D1p` for optical). |

### Wi-Fi known limits

- The MAC is a locally administered placeholder from the DT; the real one is in OTP or system configuration.
- The board's CLM blob is rejected by this firmware (`clmload` -52), so the firmware's built-in regulatory data is used; it still covers 2.4 and 5 GHz.
- HSIC runtime suspend stays off (USB default); enabling it needs the port's resume workaround.
- The firmware is not distributable; `./scripts/velliangiri.sh firmware` extracts it (and the Bluetooth patch) from the user's own restore image into `research/firmware/`.

### Bluetooth known limits

- The address is the controller default (the stock system sets it from system config).
- Stays at 115200 baud: with integer divisors the 51.3 MHz clock reaches 460800 accurately but not the stock 3 Mbaud.
- No host-wake interrupt, so no UART sleep.
- No `bluetoothd`/BlueZ userland in the image; pairing and connections are untested.

### Central DMA known limits

- The device-address register holds only 24 bits; the window comes from the channel's wiring, not from a register. Two windows are measured and accepted: `0x34000000` (audio, memory to device) and `0x31000000` (NAND FIFOs on channels 5-8, device to memory, verified 2026-10-30). Other clients (UART1/2, SHA, display) are still refused.
- A channel restarted after a finished non-cyclic list stopped after its first segment, and a transfer left armed resumed when its channel was enabled again; `start_next` now always aborts first and channel allocation aborts stale state (verified 2026-10-30 with back-to-back NAND reads).

### Audio known limits

- Fixed 2026-10-30: the speech static came from feeding HDMI through its internal S/PDIF receiver (samples arrived intact, but the output followed each sample's sign). HDMI now takes I2S, as the stock system does, and plays speech cleanly.
- Optical output is not verified on a receiver; its DMA runs at real time. It plays 46 875 Hz only (one word per channel needs MCLK / 512).
- Only 46 875 and 31 250 Hz are advertised (24 MHz reference); `wavplay` and ALSA resample. Exact 48 kHz needs the I2S master clocked from the PMGR NCO (channel 0).
- Only stereo on I2S0; I2S1..3 (the other HDMI data lines, for multichannel) are unused.

## Partly implemented

| Block | Works | Pending |
|---|---|---|
| NAND (FMI0, managed NAND, 8.9 GB) | `drivers/mtd/devices/s5l8947x-fmi.c`: identifies the package and reads its geometry and timings from it; read-only `/dev/mtd0` (16 KiB pages, 64 B metadata per page as OOB, one erase region per CAU); pages move by CDMA (data ch 5, metadata ch 6); interface timing computed from the package (about 14 MB/s). `drivers/mtd/s5l8947x-ftl.c`: rebuilds the stock translation layer's map from the per-superblock tables of contents (about 4 s at boot) and exposes the 8 GB logical disk as `/dev/nand0` and each volume of its volume map as `/dev/nand0p1` (System, 1.4 GB) and `/dev/nand0p2` (Data, 6.5 GB), read-only. Verified 2026-10-30: the map equals a full metadata scan of a raw backup, the whole of `nand0p1` matches an image checked offline, `mount -t hfsplus -o ro /dev/nand0p1` lists the system volume (10,631 files), a 348 MB file reads at 14 MB/s; `hwtest nand` 8/8. | Data volume is block-encrypted (needs the hardware AES path and its wrapped key), so it does not mount. The system volume's journal is not replayed (Linux mounts the on-disk state read-only). No writes or erase; polled, no interrupts. FMI1 is unpopulated on j33iap. |
| SGX543 GPU — **on hold** | Gates 7/8/9, cores released from reset, telemetry at `/dev/sgx0` | No command submission, no DRM driver. Per-core registers hang the bus until a core is brought up. No further work and no on-device test until this is resumed. |
| USB OTG port host mode | Device mode on `dwc2`; EHCI host serves only the internal HSIC port | `dr_mode` is `peripheral`; host on the OTG port needs a role switch. The OHCI companion (`0x36500000`, IRQ 13) is not described, so full/low-speed devices are unsupported. |
| System MCU | Button input, firmware query, break, raw commands | Reset-to-recovery trigger and wake source are not wired (the MCU line, GPIO 2, is unused); standby is only sent on halt. |

## Not started

| Block | Address | Notes |
|---|---|---|
| Hardware crypto (SHA, AES, public-key engine) | `0x30200000`, `0x33100000` | Low priority. |
| SD/MMC, SPI, RTC, thermal, hardware RNG | — | Not investigated; `CONFIG_MMC` is off. |

## Suggested order

1. NAND: decrypt the Data volume (hardware AES and its wrapped volume key), then the write and erase path (needs a decision on the stock data, which is backed up).
2. Wi-Fi and Bluetooth: read the real MAC and Bluetooth addresses instead of placeholders.
3. Add host mode to the OTG port.
4. Audio at exact 48 / 44.1 kHz: clock the I2S master from the PMGR NCO (channel 0) and verify the optical output on a receiver.
