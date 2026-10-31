# Driver Progress (`S5L8947X` / `j33iap`)

Status of every hardware block, checked against the drivers in the tree and the enabled nodes in `s5l8947x-j33iap.dts`. Last reviewed 2026-10-31 (`hwtest`: 75 passed, 0 failed, 2 skipped).

## Done

| Block | Address | Driver |
|---|---|---|
| Interrupt controller, 24 MHz timer | `0x3f200000` | `drivers/irqchip/irq-apple-aic1.c` |
| Watchdog, clocksource, restart | `0x3f103020` | `arch/arm/mach-apple/` |
| L2 cache (PL310, 512 KB) | `0x3e000000` | mainline `cache-l2x0.c`, started from `arch/arm/mach-apple/s5l8947x.c`; loader leaves it disabled |
| PMGR clock gates | `0x3f101008` | `drivers/clk/clk-s5l8940x-pmgr.c`; 28 peripheral gates on `j33iap` are claimed via Device Tree (`cdma`, `display_pipe`, `eth0`, `i2c0`/`i2c2`, `uart1`/`uart2`, `fmi0`, `audio0`, `usb_phy`/`usb_ehci`, `aes`, `sgx0`), while the pre-kernel loader enables core and early-boot gates |
| CPU frequency (212.5 / 425 / 850 MHz) | `0x3f100000` | `drivers/cpufreq/s5l8947x-cpufreq.c`; every state keeps the peripheral divider, so the 51.3 MHz UART clock does not change with the CPU speed |
| I2C 0/1/2 | `0x33200000`..`0x33400000` | `drivers/i2c/busses/i2c-s5l8940x.c` |
| Power management unit and power switches | I2C0 `0x74` | `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c` |
| 10/100 Ethernet | `0x3b000000` | `drivers/net/ethernet/apple/s5l_fe.c` |
| USB 2.0 device mode and PHY | `0x36000000`, `0x36100000` | `drivers/phy/phy-apple-s5l-usb.c` + mainline `dwc2` (`scripts/patches/0005-*`) |
| USB 2.0 host (EHCI) on the HSIC port | `0x36400000` | mainline `ehci-platform`; host PHY in `drivers/phy/phy-apple-s5l-usb.c` (HSIC tuning, INCRx, port reset release, host-ready/device-ready handshake, device ready in about 42 ms) |
| GPIO controller (144 pins, 18 ports) | `0x3fa00000` | `drivers/pinctrl/pinctrl-s5l8940x-gpio.c`, IRQ 119; drives the HSIC handshake lines (GPIO 113 out, 128 in) and Bluetooth control lines (GPIO 104, 105) |
| Display pipe, HDMI 720p, scaler, gamma | `0x3a200000`, `0x39600000`, `0x39700000`, `0x39200000` | `drivers/video/fbdev/s5l8947x-display.c`; the pre-kernel loader (`scripts/boot/hdmi-720p.h`) brings up 720p60 scanout and draws the boot splash from `logo.svg` (polygon tables generated at build time by `scripts/boot/gen-splash.py`, rasterized by `splash.c`) |
| System / IR remote MCU (UART1) | `0x32600000` | `drivers/input/misc/s5l8947x-mcu.c`; button input, firmware query (`03.07`), break, raw commands (`hwtest mcu` 3/3), and standby power-off on halt |
| Wi-Fi (BCM4334, HSIC) | EHCI port 2 | mainline `brcmfmac` USB, patched by `scripts/patches/0007-*` (boot loader `bd1a`, NVRAM packed into the TRX, optional CLM blob). Firmware 6.53.3 and NVRAM copied at boot from the on-device `System` volume (`nand0p1`); factory MAC from the NAND system configuration, applied to the DT by `s5l8947x-ftl`. `wlan0` scans 2.4 and 5 GHz and joins WPA2-PSK networks with `wpa_supplicant` and the `wifi` helper (5 GHz, 135 Mbit/s, DHCP lease, routed traffic; `hwtest wireless` 5/5). `wireless-regdb` installed; optional build-time `wpa_supplicant.conf` is joined at boot. |
| Bluetooth (BCM4334, UART2) | `0x32700000` | mainline `hci_uart_bcm` (`brcm,bcm4334-bt` over `serdev`); patch firmware carved at boot from the on-device `System` volume (SHA-256 checked) is loaded (HCI 4.1, revision 2393, LE), repeated rebinds clean. Verified on the radio with `btscan` (raw HCI socket, no Bluetooth userland needed; `hwtest wireless` 3/3). |
| Central DMA (42 channels) | `0x37000000`, `0x37800000` | `drivers/dma/s5l8947x-cdma.c`, `dmaengine` provider (`dmas = <&cdma channel request>`): slave scatter-gather (both directions) and cyclic transfers, per-channel interrupts (end of list, and per segment for cyclic periods), segment-granularity residue. Serves audio playback (channels 23, 31), managed NAND page/metadata transfers (channels 5, 6), and the hardware AES engine (channels 1, 2). |
| Audio: I2S -> HDMI, S/PDIF -> optical | `0x34190000`, `0x34194000`, `0x34195000` | `sound/arm/s5l8947x-audio.c`, two PCMs fed by cyclic `dmaengine` transfers (period interrupts, position from the DMA residue). PCM 0 (HDMI, `pcmC0D0p`): I2S0 as a slave of the I2S clock master, CDMA channel 23, 46 875 / 31 250 Hz; the slave raises DMA requests with a FIFO threshold set in its TX control register. PCM 1 (optical, `pcmC0D1p`): S/PDIF at 46 875 Hz with one word per channel, CDMA channel 31. HDMI packet layer in `s5l8947x-display.c` (I2S receiver in basic I2S format, reset pin selection, ACR from the produced rate, valid AUI; `hdmi_audio` accepts `off`). Speech is clean over HDMI at both rates (`hwtest audio` 5/5). Hot-plug loss mutes the packets and replug re-announces. `wavplay` plays WAV files (resampled to 46 875 Hz; `-d /dev/snd/pcmC0D1p` for optical). |
| Hardware crypto (CDMA AES engine) | `0x37800000`, `0x37801000` | `drivers/crypto/s5l8947x-aes.c`, `ecb(aes)` and `cbc(aes)` `skcipher` algorithms (128 / 192 / 256-bit keys, priority 300) fed by CDMA channels 1 (input) and 2 (output), plus `s5l8947x_aes_hwkey_crypt()` for the SoC's built-in hardware keys (`S5L8947X_AES_KEY_DEVICE`, `GROUP0`, `GROUP1`), with a FIPS-197 CBC self-test at probe. 16 KiB transfers, self-test passes at probe, and the hardware device key unwraps the NAND volume and per-file key store in ~8 ms (`hwtest probe` 8/8). |
| Managed NAND (FMI0, 8.9 GB) & FTL volumes | `0x31200000`, `0x31240000`, `0x31280000` | `drivers/mtd/devices/s5l8947x-fmi.c`: identifies the package and reads its geometry and read/write timings (`timing 0x1010`) from the parameter page; read/write/erase `/dev/mtd0` (`MTD_CAP_NANDFLASH`, 16 KiB pages, 64 B metadata per page as OOB, one 4 MiB erase region per CAU, `MTD_OPS_RAW` for SLC reserved blocks); page reads move by CDMA (data ch 5, metadata ch 6, ~14 MB/s), page/OOB writes stream the logical page layout over PIO (`MN_CMD_PROGRAM` + `MN_CMD_PROG_CONFIRM` + `MN_CMD_NEXT_STATUS`), and block erase uses `MN_CMD_ERASE`. `drivers/mtd/s5l8947x-ftl.c` + `scripts/patches/0008-*`: rebuilds the translation map from the per-superblock tables of contents (~4.6 s at boot), supports log-structured 4 KiB -> 16 KiB write buffering, superblock allocation, TOC closure, and garbage collection (`REQ_OP_WRITE`, `REQ_OP_FLUSH`, `REQ_FUA`), unwraps the 32-byte volume key and class-4 master key from the reserved SLC key store via `s5l8947x_aes_hwkey_crypt()`, and exposes the 8 GB logical disk as `/dev/nand0` and each volume as `/dev/nand0p1` (`System`, 1.4 GB) and `/dev/nand0p2` (`Data`, 6.5 GB, encrypted and decrypted per 4 KiB block with AES-256-CBC and a 4-word Galois LFSR IV). `hfsplus` verifies and replays journals on mount, marks volumes cleanly unmounted on unmount and read-only remount, and transparently decrypts and encrypts per-file `com.apple.system.cprotect` class-4 files (`hwtest nand` 12/12). |

### Wi-Fi known limits

- The factory MAC is applied after the NAND system configuration is read, so `brcmfmac` attaches only after `stage2` provisions the firmware and re-probes the HSIC device.
- The board's CLM blob is rejected by this firmware (`clmload` -52, which aborts the attach), so it is not provisioned and the firmware's built-in regulatory data is used; it still covers 2.4 and 5 GHz.
- HSIC runtime suspend stays off (USB default); enabling it needs the port's resume workaround.
- The firmware is not distributable and is never in the image; it is copied from `nand0p1` at boot, so Wi-Fi needs a readable `System` volume.

### Bluetooth known limits

- The factory address is in the DT (`local-bd-address`, from the NAND system configuration), but `hci_uart_bcm` applies it only when the controller reports a known default address, and `43:34:43:34:1f:ac` is not in `btbcm`'s list; `btscan` sets it with a vendor command instead.
- The Bluetooth patch is carved from a fixed offset in a `System` file, valid only for the 12H1006 system software (rejected by its SHA-256 otherwise).
- Stays at 115200 baud: with integer divisors the 51.3 MHz clock reaches 460800 accurately but not the stock 3 Mbaud.
- No host-wake interrupt, so no UART sleep.
- No `bluetoothd`/BlueZ userland in the image; pairing and connections are untested.

### Central DMA known limits

- The device-address register holds only 24 bits; the upper address window comes from the channel's hardware wiring rather than a register. Two peripheral FIFO windows are measured and accepted by `s5l8947x-cdma`: `0x34000000` (audio, memory to device) and `0x31000000` (NAND FIFOs on channels 5–8, device to memory), while channels 1 and 2 are driven directly by `s5l8947x-aes`. Other peripheral clients (UART1/2, SHA, display) are still refused until their FIFO windows are verified.
- Non-cyclic channels retain their completion state until explicitly aborted, so `s5l8947x-cdma` aborts stale state on channel allocation and before starting each transfer.

### Audio known limits

- Optical output is not verified on a receiver; its DMA runs at real time. It plays 46 875 Hz only (one word per channel needs MCLK / 512).
- Only 46 875 and 31 250 Hz are advertised (24 MHz reference); `wavplay` and ALSA resample. Exact 48 kHz needs the I2S master clocked from the PMGR NCO (channel 0).
- Only stereo on I2S0; I2S1..3 (the other HDMI data lines, for multichannel) are unused.

### NAND and FTL known limits

- Passcode-bound keybag classes (1–3, 180 of 7,297 files on `nand0p2`) are not unwrapped; unauthenticated class-4 `com.apple.system.cprotect` files (7,117 of 7,297) and all non-`cprotect` files support full read and write.
- `s5l8947x-fmi` polls the controller status registers for command completion rather than using FMI interrupts; `FMI1` (`0x31300000`) is unpopulated on `j33iap`.

## Partly implemented

| Block | Works | Pending |
|---|---|---|
| SGX543 GPU (`0x35100000`, `0x3f10b000`) — **on hold** | `drivers/soc/apple/s5l8947x-sgx.c`: gates 7/8/9, cores released from reset, telemetry at `/dev/sgx0` | No command submission, no DRM driver. Per-core registers hang the bus until a core is brought up. No further work and no on-device test until this is resumed. |
| USB OTG port host mode (`0x36100000`) | Device mode on `dwc2`; EHCI host serves only the internal HSIC port | `dr_mode` is `peripheral`; host on the OTG port needs a role switch. The OHCI companion (`0x36500000`, IRQ 13) is not described, so full/low-speed devices are unsupported. |
| System MCU (`0x32600000`) | Button input, firmware query, break, raw commands (`hwtest mcu` 3/3), standby on halt | Reset-to-recovery trigger and wake source are not wired (the MCU line, GPIO 2, is unused). |

## Not started

| Block | Address | Notes |
|---|---|---|
| SD/MMC, SPI, RTC, thermal, hardware RNG | — | Not investigated; `CONFIG_MMC` is off. |

## Suggested order

1. Bluetooth: have the kernel apply the factory address (add this controller's default to `btbcm`'s list).
2. Add host mode to the OTG port.
3. Audio at exact 48 / 44.1 kHz: clock the I2S master from the PMGR NCO (channel 0) and verify the optical output on a receiver.
