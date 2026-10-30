# Driver Progress (`S5L8947X` / `j33iap`)

Status of every hardware block, checked against the drivers in the tree and the enabled nodes in `s5l8947x-j33iap.dts`. Last reviewed 2026-10-30.

## Done

| Block | Address | Driver |
|---|---|---|
| Interrupt controller, 24 MHz timer | `0x3f200000` | `drivers/irqchip/irq-apple-aic1.c` |
| Watchdog, clocksource, restart | `0x3f103020` | `arch/arm/mach-apple/` |
| L2 cache (PL310, 512 KB) | `0x3e000000` | mainline `cache-l2x0.c`, started from `arch/arm/mach-apple/s5l8947x.c`; loader leaves it disabled |
| PMGR clock gates | `0x3f101008` | `drivers/clk/clk-s5l8940x-pmgr.c`; I2C and USB host gates are claimed by their drivers. The loader still pre-enables a few for the console and early boot. |
| CPU frequency (212.5 / 425 / 850 MHz) | `0x3f100000` | `drivers/cpufreq/s5l8947x-cpufreq.c` |
| I2C 0/1/2 | `0x33200000`.. | `drivers/i2c/busses/i2c-s5l8940x.c` |
| Power management unit and power switches | I2C0 `0x74` | `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c` |
| 10/100 Ethernet | `0x3b000000` | `drivers/net/ethernet/apple/s5l_fe.c` |
| USB 2.0 device mode and PHY | `0x36100000` | `drivers/phy/phy-apple-s5l-usb.c` + mainline `dwc2` |
| Display pipe, HDMI 720p, scaler, gamma | `0x3a200000` | `drivers/video/fbdev/s5l8947x-display.c` |
| System / IR remote MCU (UART1) | `0x32600000` | `drivers/input/misc/s5l8947x-mcu.c` |
| Audio: S/PDIF -> HDMI and optical | `0x34194000` | `sound/arm/s5l8947x-audio.c`; ALSA PCM at 46 875 / 31 250 Hz, measured register layout, fed by CDMA channel 31 (frame-clock position, MAR liveness check). HDMI packet layer in `s5l8947x-display.c` (ACR from the produced rate, valid AUI, S/PDIF receiver enabled, `I2S_CLK_CON=0` on this path). Audible on hardware; hot-plug loss mutes the packets and replug re-announces. `wavplay` plays WAV files (resampled to 46 875 Hz). |

## Partly implemented

| Block | Works | Pending |
|---|---|---|
| SGX543 GPU — **on hold** | Gates 7/8/9, cores released from reset, telemetry at `/dev/sgx0` | No command submission, no DRM driver. Per-core registers hang the bus until a core is brought up. No further work and no on-device test until this is resumed. |
| Wi-Fi (BCM4334, HSIC) | Verified end to end 2026-10-30 (`hwtest wireless` 5/5): HSIC handshake, firmware 6.53.3 from plain `.bin` + `.txt` with the MAC from the DT, `wlan0` scans 2.4 and 5 GHz and joins a WPA2-PSK network on 5 GHz with `wifi connect` (135 Mbit/s, DHCP lease, routed traffic); `wireless-regdb` loaded. `brcmfmac` USB patched for `bd1a` / chip 0x4334, NVRAM in the TRX, CLM blob support. | The board's CLM blob is rejected by this firmware (`clmload` -52), so the built-in regulatory data is used. MAC is locally administered (the real one is in OTP or system config). HSIC runtime suspend is left off (USB default); enabling it would need the port's resume workaround. |
| USB host mode | Device mode only | `dr_mode` is `peripheral`; host needs the host PHY tuning values and a role switch. |
| GPIO / pinctrl (144 pins, 18 ports) | Node at `0x3fa00000`, IRQ 119; used for the HSIC handshake lines | Not yet probed on hardware. MCU line GPIO 2 is not wired. |
| System MCU | Button input, firmware query, break, raw commands | Reset-to-recovery trigger and wake source are not wired; standby is only sent on halt. |
| Bluetooth (UART2) | `hci_uart_bcm` binds and registers `hci0` | The controller never answers HCI Reset (`-110`), in every recorded run since 2026-10-28, with or without the GPIO node and Wi-Fi power. UART2 takes almost no interrupts. The boot loader leaves RTS (pin 20) as a GPIO output held high; the stock tree drives RTS as a GPIO, BT power on GPIO 104 and BT wake on GPIO 105. Higher baud not set up. |

## Not started

| Block | Address | Notes |
|---|---|---|
| NAND flash controller (FMI0 / FMI1, BCH) | `0x31200000`, `0x31300000` | Highest priority: persistent storage. Needs register reconnaissance first. |
| Central DMA | `0x37000000` | Required by NAND, audio and crypto. |
| Hardware crypto (SHA, AES, public-key engine) | `0x30200000`, `0x33100000` | Low priority. |
| I2S1..3 | `0x341a0000`.. | Only I2S0 and S/PDIF are modelled. |
| SD/MMC, SPI, RTC, thermal, hardware RNG | — | Not investigated; `CONFIG_MMC` is off. |

## Suggested order

1. Wi-Fi: read the real MAC address instead of the DT placeholder.
2. Add host mode to the USB controller.
3. NAND reconnaissance, then the CDMA and FMI drivers.
