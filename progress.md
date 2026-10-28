# Driver Progress (`S5L8947X` / `j33iap`)

Status of every hardware block, checked against the drivers in the tree and the enabled nodes in `s5l8947x-j33iap.dts`. Last reviewed 2026-10-28.

## Done

| Block | Address | Driver |
|---|---|---|
| Interrupt controller, 24 MHz timer | `0x3f200000` | `drivers/irqchip/irq-apple-aic1.c` |
| Watchdog, clocksource, restart | `0x3f103020` | `arch/arm/mach-apple/` |
| PMGR clock gates | `0x3f101008` | `drivers/clk/clk-s5l8940x-pmgr.c`; I2C and USB host gates are claimed by their drivers. The loader still pre-enables a few for the console and early boot. |
| CPU frequency (212.5 / 425 / 850 MHz) | `0x3f100000` | `drivers/cpufreq/s5l8947x-cpufreq.c` |
| I2C 0/1/2 | `0x33200000`.. | `drivers/i2c/busses/i2c-s5l8940x.c` |
| Power management unit and power switches | I2C0 `0x74` | `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c` |
| 10/100 Ethernet | `0x3b000000` | `drivers/net/ethernet/apple/s5l_fe.c` |
| USB 2.0 device mode and PHY | `0x36100000` | `drivers/phy/phy-apple-s5l-usb.c` + mainline `dwc2` |
| Display pipe, HDMI 720p, scaler, gamma | `0x3a200000` | `drivers/video/fbdev/s5l8947x-display.c` |
| System / IR remote MCU (UART1) | `0x32600000` | `drivers/input/misc/s5l8947x-mcu.c` |
| Bluetooth (UART2) | `0x32700000` | mainline `hci_uart_bcm` |

## Partly implemented

| Block | Works | Pending |
|---|---|---|
| Audio: I2S0 (HDMI) and S/PDIF | ALSA PCM and mixer, PIO feed from an hrtimer | HDMI audio is silent. The driver still programs the Samsung I2S layout, but the block uses the measured layout in `research/docs/audio-i2s-registers.md`. Needs a rewrite on that layout, then DMA. I2S0 is limited to 46.875 kHz from the 24 MHz clock. |
| SGX543 GPU — **on hold** | Gates 7/8/9, cores released from reset, telemetry at `/dev/sgx0` | No command submission, no DRM driver. Per-core registers hang the bus until a core is brought up. No further work and no on-device test until this is resumed. |
| Wi-Fi (BCM4334, HSIC) | `REG_ON` switch via PMU, HSIC PHY path in the USB PHY driver | No EHCI node in the device tree, `brcmfmac` not enabled, wake GPIO unused. |
| USB host mode | Device mode only | `dr_mode` is `peripheral`; host needs the host PHY tuning values and a role switch. |
| GPIO / pinctrl (224 pins) | Driver builds | No GPIO node in the device tree, so it never probes. Blocks MCU line GPIO 2 and the Wi-Fi wake GPIO. |
| System MCU | Button input, firmware query, break, raw commands | Reset-to-recovery trigger and wake source are not wired; standby is only sent on halt. |
| Bluetooth speed | 115200 baud via `hci_uart_bcm` | The board's higher operating speed is not set up. |
| L2 cache (PL310) | Node present | `status = "disabled"`; not enabled or tested. |

## Not started

| Block | Address | Notes |
|---|---|---|
| NAND flash controller (FMI0 / FMI1, BCH) | `0x31200000`, `0x31300000` | Highest priority: persistent storage. Needs register reconnaissance first. |
| Central DMA | `0x37000000` | Required by NAND, audio and crypto. |
| Hardware crypto (SHA, AES, public-key engine) | `0x30200000`, `0x33100000` | Low priority. |
| I2S1..3 | `0x341a0000`.. | Only I2S0 and S/PDIF are modelled. |
| SD/MMC, SPI, RTC, thermal, hardware RNG | — | Not investigated; `CONFIG_MMC` is off. |

## Suggested order

1. Add the GPIO node (driver already exists).
2. Fix the I2S register layout so HDMI audio works with PIO.
3. Enable the HSIC EHCI host and `brcmfmac`.
4. Add host mode to the USB controller.
5. NAND reconnaissance, then the CDMA and FMI drivers.
