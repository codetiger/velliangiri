# Driver Progress (`S5L8947X` / `j33iap`)

Status of every hardware block, checked against the drivers in the tree and the enabled nodes in `s5l8947x-j33iap.dts`. Last reviewed 2026-10-31 (`hwtest`: 87 passed, 0 failed, 2 skipped).

## Done

| Block | Address | Driver |
|---|---|---|
| Interrupt controller, 24 MHz timer | `0x3f200000` | `drivers/irqchip/irq-apple-aic1.c` |
| Watchdog, clocksource, restart | `0x3f103020` | `arch/arm/mach-apple/` |
| L2 cache (PL310, 512 KB) | `0x3e000000` | mainline `cache-l2x0.c`, started from `arch/arm/mach-apple/s5l8947x.c`; loader leaves it disabled |
| PMGR clock gates and audio oscillators | `0x3f101008`, `0x3f100110` | `drivers/clk/clk-s5l8940x-pmgr.c`; 28 peripheral gates on `j33iap` are claimed via Device Tree (`cdma`, `display_pipe`, `eth0`, `i2c0`/`i2c2`, `uart1`/`uart2`, `fmi0`, `audio0`, `usb_phy`/`usb_ehci`, `aes`, `sgx0`), while the pre-kernel loader enables core and early-boot gates. The seven audio oscillators (NCOs) are a second provider: the output is set to the hertz from a 342 MHz reference (measured, two thirds of the 513 MHz PLL), and channel 0 clocks the I2S master |
| CPU frequency (212.5 / 425 / 850 MHz) | `0x3f100000` | `drivers/cpufreq/s5l8947x-cpufreq.c`; every state keeps the peripheral divider, so the 51.3 MHz UART clock does not change with the CPU speed |
| I2C 0/1/2 | `0x33200000`..`0x33400000` | `drivers/i2c/busses/i2c-s5l8940x.c` |
| Power management unit, power switches and PMU interrupts | I2C0 `0x74`, interrupt on GPIO 109 | `drivers/mfd/apple-pmu-i2c.c`, `drivers/regulator/apple-pmu-pwrsw.c`. The PMU is an interrupt controller (`regmap-irq`): 48 events latched in six read-to-clear registers from `0x01` (buttons, charger, RTC, its GPIO inputs: GPIOs 0..7 are events 16..23, GPIOs 8..9 events 40..41), masks from `0x0c`, and an active-low line on SoC GPIO 109. Bluetooth host wake uses event 22 (PMU GPIO 6). |
| Real-time clock (in the PMU) | I2C0 `0x74` | `drivers/rtc/rtc-apple-pmu.c` (`rtc0`): wall time is the PMU seconds counter at `0x4c` (runs from PMU power-up, kept across reboots and DFU) plus a calendar offset in PMU scratch registers `0x84`..`0x87` (fraction at `0x95`..`0x96`, kept at zero), the same layout the stock OS uses. Setting the time rewrites the offset; while the offset is zero (after power loss) the clock reports itself invalid. The alarm compares the counter with `0x46`, is armed by bit 6 of `0x4a` and raises PMU event 32 (`wakealarm`). The kernel sets the system clock from it at boot; `init.sh` falls back to the image build date only when the clock is older than that (`hwtest rtc` 4/4, time kept across a reboot). |
| 10/100 Ethernet | `0x3b000000` | `drivers/net/ethernet/apple/s5l_fe.c` |
| USB 2.0 device mode and PHY | `0x36000000`, `0x36100000` | `drivers/phy/phy-apple-s5l-usb.c` + mainline `dwc2` (`scripts/patches/0005-*`) |
| USB 2.0 host (EHCI) on the HSIC port | `0x36400000` | mainline `ehci-platform`; host PHY in `drivers/phy/phy-apple-s5l-usb.c` (HSIC tuning, INCRx, port reset release, host-ready/device-ready handshake, device ready after 42 and 57 ms on two measured boots) |
| GPIO controller (144 pins, 18 ports) | `0x3fa00000` | `drivers/pinctrl/pinctrl-s5l8940x-gpio.c`, IRQ 119; drives the HSIC handshake lines (GPIO 113 out, 128 in) and Bluetooth control lines (GPIO 104, 105), and delivers pin interrupts (level and edge; the PMU interrupt on GPIO 109). The per-group enable and disable registers both read back the disabled pins. |
| Display pipe, HDMI 720p, scaler, gamma | `0x3a200000`, `0x39600000`, `0x39700000`, `0x39200000` | `drivers/video/fbdev/s5l8947x-display.c`; the pre-kernel loader (`scripts/boot/hdmi-720p.h`) brings up 720p60 scanout and draws the boot splash from `logo.svg` (polygon tables generated at build time by `scripts/boot/gen-splash.py`, rasterized by `splash.c`) |
| System / IR remote MCU (UART1) | `0x32600000` | `drivers/input/misc/s5l8947x-mcu.c`; button input, firmware query (`03.07`), break, raw commands (`hwtest mcu` 3/3), and standby power-off on halt |
| Wi-Fi (BCM4334, HSIC) | EHCI port 2 | mainline `brcmfmac` USB, patched by `scripts/patches/0007-*` (boot loader `bd1a`, NVRAM packed into the TRX, optional CLM blob). Firmware 6.53.3 and NVRAM copied from the on-device `System` volume (`nand0p1`) on first boot and cached on `nand0p2` (`/linux/lib/firmware/brcm`), so later boots skip `System`; factory MAC from the NAND system configuration, applied to the DT by `s5l8947x-ftl`. The DT sets `brcm,ccode-map-trivial`, so a country set through cfg80211 (`iw reg set`, `country=` in `wpa_supplicant.conf`) reaches the firmware's built-in table (GB, JP, US each change the channel list). `wlan0` scans 2.4 and 5 GHz and joins WPA2-PSK networks with `wpa_supplicant` and the `wifi` helper (5 GHz, 135 Mbit/s, DHCP lease, routed traffic; `hwtest wireless` Wi-Fi 5/5). `wireless-regdb` installed. An optional build-time `wpa_supplicant.conf` is joined at boot; `stage2` saves it to `nand0p2` (`/linux/etc`) on first boot and uses the saved copy from then on. |
| Bluetooth (BCM4334, UART2) | `0x32700000` | mainline `hci_uart_bcm` (`brcm,bcm4334-bt` over `serdev`); patch firmware carved from the on-device `System` volume on first boot (`hcdcarve` selects the one patch naming this board) and cached on `nand0p2` with the Wi-Fi firmware is loaded (HCI 4.1, build 2393, LE), repeated rebinds clean. Runs at 1,603,125 baud (`max-speed`; patch download about 1.2 s instead of 8 s at 115200). The factory address goes from the NAND system configuration into the DT (`local-bd-address`), and `scripts/patches/0009-*` lists the address the patched controller reports (`43:34:43:34:1f:ac`) as a default in `btbcm`, so the kernel applies it. Verified on the radio with `btscan` (raw HCI socket). BlueZ 5.79 is in the image: `stage2` starts the D-Bus system bus and `bluetoothd`, which powers the adapter, and `bluetoothctl` discovers nearby devices. Host wake comes through the PMU interrupt controller and `scripts/patches/0010-*` lets `hci_bcm` take that threaded interrupt, so the controller runs in UART sleep mode with runtime PM: it sleeps after 5 s idle and wakes the host when it has data (`hwtest wireless` Bluetooth 6/6, including the factory address and host wake). |
| Central DMA (42 channels) | `0x37000000`, `0x37800000` | `drivers/dma/s5l8947x-cdma.c`, `dmaengine` provider (`dmas = <&cdma channel request>`): slave scatter-gather (both directions) and cyclic transfers, per-channel interrupts (end of list, and per segment for cyclic periods), segment-granularity residue. Serves audio playback (channels 23, 31), managed NAND page/metadata transfers (channels 5, 6), and the hardware AES engine (channels 1, 2). |
| Audio: I2S -> HDMI, S/PDIF -> optical | `0x34190000`, `0x34194000`, `0x34195000` | `sound/arm/s5l8947x-audio.c`, two PCMs fed by cyclic `dmaengine` transfers (period interrupts, position from the DMA residue). PCM 0 (HDMI, `pcmC0D0p`): I2S0 as a slave of the I2S clock master, CDMA channel 23, 32 / 44.1 / 48 kHz exactly (the master runs from PMGR oscillator 0 at 256 fs and divides it by 8; the HDMI-measured CTS matches the announced one at every rate); the slave raises DMA requests with a FIFO threshold set in its TX control register. PCM 1 (optical, `pcmC0D1p`): S/PDIF at 46 875 Hz with one word per channel, CDMA channel 31. HDMI packet layer in `s5l8947x-display.c` (I2S receiver in basic I2S format, reset pin selection, ACR with the HDMI-recommended N per rate, valid AUI; `hdmi_audio` accepts `off`). Speech is clean over HDMI at 44.1 and 48 kHz (`hwtest audio` 8/8, including the measured rate check). Hot-plug loss mutes the packets and replug re-announces. `wavplay` plays WAV files at their own rate when the device takes it and resamples otherwise (`-r` forces a rate; `-d /dev/snd/pcmC0D1p` for optical). |
| Hardware crypto (CDMA AES engine) | `0x37800000`, `0x37801000` | `drivers/crypto/s5l8947x-aes.c`, `ecb(aes)` and `cbc(aes)` `skcipher` algorithms (128 / 192 / 256-bit keys, priority 300) fed by CDMA channels 1 (input) and 2 (output), plus `s5l8947x_aes_hwkey_crypt()` for the SoC's built-in hardware keys (`S5L8947X_AES_KEY_DEVICE`, `GROUP0`, `GROUP1`), with a FIPS-197 CBC self-test at probe. 16 KiB transfers, self-test passes at probe, and the hardware device key unwraps the NAND volume and class 4 keys from the key store and derives the system keybag's passcode key at boot (`hwtest probe` 8/8). |
| Managed NAND (FMI0, 8.9 GB) & FTL volumes | `0x31200000`, `0x31240000`, `0x31280000` | `drivers/mtd/devices/s5l8947x-fmi.c`: identifies the package and reads its geometry and read/write timings (`timing 0x1010`) from the parameter page; read/write/erase `/dev/mtd0` (`MTD_CAP_NANDFLASH`, 16 KiB pages, 64 B metadata per page as OOB, one 4 MiB erase region per CAU, `MTD_OPS_RAW` for SLC reserved blocks); page reads move by CDMA (data ch 5, metadata ch 6, ~17 MiB/s at 850 MHz), page/OOB writes stream the logical page layout over PIO (`MN_CMD_PROGRAM` + `MN_CMD_PROG_CONFIRM` + `MN_CMD_NEXT_STATUS`), and block erase uses `MN_CMD_ERASE`. `drivers/mtd/s5l8947x-ftl.c` + `scripts/patches/0008-*`: rebuilds the translation map from the per-superblock tables of contents (~4.8 s at boot), supports log-structured 4 KiB -> 16 KiB write buffering, superblock allocation in rotation, TOC closure with a generation per run, and garbage collection (`REQ_OP_WRITE`, `REQ_OP_FLUSH`, `REQ_FUA`). Each boot resumes the newest superblock a previous session left open instead of starting a fresh one; garbage collection reclaims closed and stale open superblocks, copies live blocks into the open superblock when free ones run low, keeps the last free superblock for itself, and retires superblocks that fail to erase. It unwraps the 32-byte volume key and class-4 master key from the reserved SLC key store via `s5l8947x_aes_hwkey_crypt()`, and unlocks protection classes 1–3 from the system keybag that `stage2` writes to `/sys/block/nand0/keybag` (payload key from the key store; class keys wrapped with the device key and a passcode key derived for the empty passcode by PBKDF2-HMAC-SHA1 and a 50,000-iteration mix through AES with the hardware device key, ~30 ms), and exposes the 8 GB logical disk as `/dev/nand0` and each volume as `/dev/nand0p1` (`System`, 1.4 GB) and `/dev/nand0p2` (`Data`, 6.5 GB, encrypted and decrypted per 4 KiB block with AES-256-CBC and a 4-word Galois LFSR IV). `hfsplus` verifies and replays journals on mount, marks volumes cleanly unmounted on unmount and read-only remount, and transparently decrypts and encrypts per-file `com.apple.system.cprotect` files of classes 1–4 (all files on `nand0p2` read), and inflates `decmpfs`-compressed files (`hwtest nand` 16/16; 64 + 48 MiB writes under garbage-collection pressure left all 7,296 files on `nand0p2` byte-identical across a reboot). |

### Wi-Fi known limits

- The first probe fails; `brcmfmac` attaches only after `stage2` has provisioned the firmware and re-probes the HSIC device (firmware up about 9 s after boot).
- The board's CLM blob is rejected by this firmware (`clmload` -52, which aborts the attach), so it is not provisioned and the firmware's built-in regulatory data is used (NVRAM default `ccode=XZ`, regrev 20); it still covers 2.4 and 5 GHz and accepts ISO 3166 countries. The factory system configuration has a region entry (`Regn`), which is not used: it names a sales region rather than where the device is used.
- HSIC runtime suspend stays off (USB default); enabling it needs the port's resume workaround.
- `stage2` selects the board's firmware files by name (one variable for both radios); another board in the family needs its own name there.

### Bluetooth known limits

- Not the stock 3 Mbaud: the UART's two clock sources measure about 51.3 MHz and 24 MHz, there is no fractional divider (the slot register does not latch), and 3,206,250 baud (51.3 MHz / 16) is not matched by the controller.
- This controller firmware sets extra high bits in the LE advertising report type, so the kernel logs "unknown advertising packet type" for some reports and drops them.
- Pairing and connections are untested (needs a device to pair with), and pairings are not yet kept across reboots (`/var/lib/bluetooth` is in RAM).

### RTC known limits

- Nothing sets the clock automatically: the image runs no NTP client, so the time has to be set once after power-up (`date -s`, then `hwclock -w`). With the clock set, the kernel keeps it in step from then on (`CONFIG_RTC_SYSTOHC`) only while the system clock is synchronised.
- The alarm fires and is reported, but waking the board from MCU standby with it is untested.

### Central DMA known limits

- The device-address register holds only 24 bits; the upper address window comes from the channel's hardware wiring rather than a register. Two peripheral FIFO windows are measured and accepted by `s5l8947x-cdma`: `0x34000000` (audio, memory to device) and `0x31000000` (NAND FIFOs on channels 5–8, device to memory), while channels 1 and 2 are driven directly by `s5l8947x-aes`. Other peripheral clients (UART1/2, SHA, display) are still refused until their FIFO windows are verified.

### Audio known limits

- Optical output is not verified on a receiver; its DMA runs at real time. It plays 46 875 Hz only (one word per channel needs MCLK / 512).
- Optical still runs from the 24 MHz reference; exact rates there need the S/PDIF transmitter on PMGR oscillator 4 and a receiver to verify it.
- HDMI rates above 48 kHz (88.2 / 96 kHz) are not offered.
- Only stereo on I2S0; I2S1..3 (the other HDMI data lines, for multichannel) are unused.

### NAND and FTL known limits

- The system keybag is unlocked with an empty passcode; on a device with a passcode set, classes 1–3 stay locked (`/sys/block/nand0/keybag` reports it). Keybags of a type with bit 30 set use another hardware key and are not supported.
- Seen once (2026-10-31, about 11 minutes after boot, first NAND read after roughly 10 minutes idle under the `ondemand` governor): every page read timed out waiting for the CDMA completion while the FMI transfer itself finished. Package resets did not clear it; switching the governor to `performance` did, and reads then worked at all three CPU speeds. A repeat of the same idle period on a fresh boot read cleanly, so the trigger is unknown.
- Wear levelling is by rotation only: free superblocks are taken in turn after the last one written, but no erase counts are kept. Garbage collection picks the superblock with the fewest live blocks.
- No discard (TRIM): Linux `hfsplus` never issues discards, so blocks of deleted files stay live in the FTL until overwritten. Garbage collection copies them like any other data.
- Superblocks that fail to erase are retired for the session (two did on 2026-10-31, status `0x45`, the status the package returns for its retired blocks); they are skipped again after the next map rebuild.
- Files created from Linux get no protection class: they are stored under the volume key only. `decmpfs`-compressed files are read-only (opening one for writing returns `EOPNOTSUPP`).
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
| Temperature sensors (PMU ADC) | I2C0 `0x74`, registers `0x30`..`0x33`, `0x43`..`0x44` | Four 10 kOhm thermistors (SoC, Wi-Fi, power supply, NAND) and the PMU junction temperature, read through the PMU's 12-bit ADC. |

## Suggested order

1. Temperature sensors on the PMU ADC (`hwmon`), with a thermal zone on the SoC thermistor.
2. NAND: reproduce the one-off CDMA completion timeout (stress reads across `ondemand` frequency changes) and make `s5l8947x-fmi` recover from it.
3. Add host mode to the OTG port.
4. Optical audio at exact rates: clock the S/PDIF transmitter from PMGR oscillator 4 and verify it on a receiver.
