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
| Display pipe, HDMI 720p, scaler, gamma | `0x3a200000` | `drivers/video/fbdev/s5l8947x-display.c` |
| System / IR remote MCU (UART1) | `0x32600000` | `drivers/input/misc/s5l8947x-mcu.c` |
| Wi-Fi (BCM4334, HSIC) | EHCI port 2 | mainline `brcmfmac` USB, patched by `scripts/patches/0007-*` (boot loader `bd1a`, NVRAM packed into the TRX, optional CLM blob). Firmware 6.53.3 from user-supplied `.bin` + `.txt`, MAC from the DT. `wlan0` scans 2.4 and 5 GHz and joins WPA2-PSK networks with `wpa_supplicant` and the `wifi` helper (verified 2026-10-30: 5 GHz, 135 Mbit/s, DHCP lease, routed traffic; `hwtest wireless` 5/5). `wireless-regdb` installed; optional build-time `wpa_supplicant.conf` is joined at boot. |
| Audio: S/PDIF -> HDMI and optical | `0x34194000` | `sound/arm/s5l8947x-audio.c`; ALSA PCM at 46 875 / 31 250 Hz, measured register layout, fed by CDMA channel 31 (frame-clock position, MAR liveness check). HDMI packet layer in `s5l8947x-display.c` (ACR from the produced rate, valid AUI, S/PDIF receiver enabled, `I2S_CLK_CON=0` on this path). Audible on hardware; hot-plug loss mutes the packets and replug re-announces. `wavplay` plays WAV files (resampled to 46 875 Hz). |

### Wi-Fi known limits

- The MAC is a locally administered placeholder from the DT; the real one is in OTP or system configuration.
- The board's CLM blob is rejected by this firmware (`clmload` -52), so the firmware's built-in regulatory data is used; it still covers 2.4 and 5 GHz.
- HSIC runtime suspend stays off (USB default); enabling it needs the port's resume workaround.
- The firmware is not distributable; `./scripts/velliangiri.sh firmware` extracts it (and the Bluetooth patch) from the user's own restore image into `research/firmware/`.

## Partly implemented

| Block | Works | Pending |
|---|---|---|
| SGX543 GPU — **on hold** | Gates 7/8/9, cores released from reset, telemetry at `/dev/sgx0` | No command submission, no DRM driver. Per-core registers hang the bus until a core is brought up. No further work and no on-device test until this is resumed. |
| USB OTG port host mode | Device mode on `dwc2`; EHCI host serves only the internal HSIC port | `dr_mode` is `peripheral`; host on the OTG port needs a role switch. The OHCI companion (`0x36500000`, IRQ 13) is not described, so full/low-speed devices are unsupported. |
| Central DMA | `0x37000000`: audio programs channel 31 directly | No `dmaengine` driver yet; NAND and crypto will need one. |
| System MCU | Button input, firmware query, break, raw commands | Reset-to-recovery trigger and wake source are not wired (the MCU line, GPIO 2, is unused); standby is only sent on halt. |
| Bluetooth (UART2) | `hci_uart_bcm` brings up `hci0` at boot (verified 2026-10-30): patch firmware loaded (HCI 4.1, revision 2393, LE), repeated rebinds clean, `hwtest wireless` passes | The address is the controller default (the stock system sets it from system config). Stays at 115200: with integer divisors the 51.3 MHz clock reaches 460800 accurately but not the stock 3 Mbaud. No host-wake interrupt, so no UART sleep. No `bluetoothd`/BlueZ userland in the image. |

## Not started

| Block | Address | Notes |
|---|---|---|
| NAND flash controller (FMI0 / FMI1, BCH) | `0x31200000`, `0x31300000` | Highest priority: persistent storage. Needs register reconnaissance first. |
| Hardware crypto (SHA, AES, public-key engine) | `0x30200000`, `0x33100000` | Low priority. |
| I2S ports | `0x341a0000`.. | The I2S bring-up code was removed; audio uses S/PDIF only. |
| SD/MMC, SPI, RTC, thermal, hardware RNG | — | Not investigated; `CONFIG_MMC` is off. |

## Suggested order

1. NAND reconnaissance, then a CDMA `dmaengine` driver and the FMI driver.
2. Wi-Fi and Bluetooth: read the real MAC and Bluetooth addresses instead of placeholders.
3. Add host mode to the OTG port.
4. Recheck the audio speech-noise bug: the old 850 MHz state also doubled peripheral clocks.
