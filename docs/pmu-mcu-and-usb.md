# S5L8947X PMU, System MCU, USB HSIC, SHA & IOP Reference

Clean-room hardware and protocol specification for the Dialog `D1815` PMU (`drivers/mfd/apple-pmu-i2c.c`, `drivers/rtc/rtc-apple-pmu.c`, `drivers/hwmon/apple-pmu-hwmon.c`), UART1 System/IR Microcontroller (`drivers/input/misc/s5l8947x-mcu.c`), USB 2.0 OTG/HSIC PHY (`drivers/phy/phy-apple-s5l-usb.c`), SHA-2 accelerator (`drivers/crypto/s5l8947x-sha.c`), and IOP coprocessor (`drivers/soc/apple/s5l8947x-iop.c`).

---

## 1. Dialog `D1815` PMU (`I2C0` @ `0x74`, IRQ 106)

The `D1815` PMU on `I2C0` (`0x33200000`, slave address `0x74`) provides event interrupts, a 32-bit uptime counter with battery-less scratch calendar offset, a 12-bit multiplexing ADC, and power-rail control.

### 1.1 RTC Counter, Scratch Calendar Offset & Alarm

| PMU Registers | Width | Description |
|---|---|---|
| `0x4c..0x4f` | 32-bit LE | Read-only seconds counter from PMU power-up (survives warm reboots and USB DFU; resets on AC power removal). Read twice until two consecutive reads match. |
| `0x84..0x87` | 32-bit LE | Scratch calendar offset whole seconds (`offset >> 15`). |
| `0x95..0x96` | 15-bit LE | Scratch calendar sub-second fraction in `1 / 32768 s` units (`0x95` = `bits [7:0]`, `0x96` = `bits [14:8]`). |
| `0x46..0x49` | 32-bit LE | Alarm compare value in counter seconds. |
| `0x4a` | 8-bit | Alarm control: `0x41` arms alarm (`bit 6` = alarm enable), `0x01` disarms. |
| `0x05` `bit 0` | 1-bit | Alarm interrupt status (regmap-irq event `32`). |

- **Wall Clock Calculation:** `wall_time_sec = counter_u32 + scratch_offset_u32`. While `scratch_offset_u32 == 0` (after AC power loss before NTP sync), `rtc-apple-d1815` reports `-EINVAL` on `read_time`, and `set_time` writes `wall_time_sec - counter_u32` into `0x84..0x87`.

### 1.2 12-Bit ADC & Thermistor Conversion (`hwmon`)

| PMU Register | Description |
|---|---|
| `0x30` | `ADC_CONTROL`: `bits [3:0]` = channel select, `bit 4` = start conversion, `bit 5` = extra settling time, `bit 6` = thermistor bias enable (for channels `4, 10..14`). Clear `bits [6:4]` after conversion. |
| `0x31..0x32` | `ADC_RESULT`: 12-bit unsigned value `(r[0x31] & 0x0f) \| (r[0x32] << 4)`. |
| `0x33` | `ADC_CONFIG`: write `0x10` before conversion. |
| `0x43..0x44` | `TJUNC_CAL`: 12-bit factory junction calibration `(r[0x43] << 4) \| (r[0x44] & 0x0f)` (fallback `0x425`). |

- **Thermistor Channels (`11..14`, `10 kΩ` NTC at `25 °C` with `50 µA` bias):**
  - `channel 11` (`temp1`): SoC proximity (`tdev1`, bound to `soc-thermal` zone).
  - `channel 12` (`temp2`): Wi-Fi proximity (`tdev2`).
  - `channel 13` (`temp3`): Power supply (`tdev3`).
  - `channel 14` (`temp4`): NAND proximity (`tdev4`).
  - Resistance formula: `V = raw * 2.5 V / 4096`, `R_ohm = V / 50 µA = raw * 12207 / 1000`, interpolated against the standard `10 kΩ` NTC curve (`-40 °C .. 125 °C` in `5 °C` steps).
- **PMU Die Junction (`channel 8`, `temp5`):**
  - `T_mC = 26000 + (cal - raw) * 320`.

---

## 2. System & IR Microcontroller (`UART1` @ `0x32600000`, 250,000 Baud)

An onboard microcontroller connected to `UART1` (`0x32600000`, IRQ `22`, PMGR gate `89`, `250000 8N1`) handles the front-panel IR receiver, status LED, NMI wake line (`GPIO 2`, falling edge), and system standby power-off.

### 2.1 Frame Format

```text
+-----+-----+-----+---------+--------+--------+------------+--------+--------+
| 'S' | 't' | cmd | ver=0x1 | len_hi | len_lo | payload... | crc_hi | crc_lo |
+-----+-----+-----+---------+--------+--------+------------+--------+--------+
```

- **Checksum:** `CRC-16/CCITT-FALSE` (`poly = 0x1021`, `init = 0xffff`, no final XOR) computed over the 6-byte header (`'S' 't' cmd ver len_hi len_lo`) plus `payload[0..len-1]`.

### 2.2 Commands & Unsolicited Events

| Direction | `cmd` | Payload | Meaning |
|---|---|---|---|
| Host → MCU | `0x02` | Empty (`0 B`) | Query MCU firmware version string (`NUL`-terminated ASCII returned in `0x80` ACK payload). |
| Host → MCU | `0x04` | `1 B`: state | Set system power state: `0x02` = active (sent at probe), **`0x03` = system standby / power-off**. |
| MCU → Host | `0x80` | Variable | `ACK` (command succeeded; optional response payload). |
| MCU → Host | `0x84` | Empty | `BUSY` (retry after 20 ms guard). |
| MCU → Host | `0x10` | `4 B` NEC IR | IR button press: `[0x87, 0xee, remote_id, key_code]`. |
| MCU → Host | `0x11` | Empty | IR button repeat (~110 ms interval while held). |

### 2.3 NEC IR Key Codes (`0x87 0xee <id> <cmd>`)

| `cmd` Byte (Aluminium / White Remote) | Linux Input Key |
|---|---|
| `0x0a` / `0x0b` | `KEY_UP` |
| `0x0c` / `0x0d` | `KEY_DOWN` |
| `0x09` / `0x08` | `KEY_LEFT` |
| `0x06` / `0x07` | `KEY_RIGHT` |
| `0x5c` / `0x5d` (Select) | `KEY_ENTER` |
| `0x02` / `0x03` (Menu) | `KEY_MENU` |
| `0x5f` / `0x5e` (Play/Pause) | `KEY_PLAYPAUSE` |

---

## 3. USB 2.0 Complex, OTG PHY & HSIC (`0x36000000`..`0x36800000`)

### 3.1 Controller Map

| Block | MMIO Base | IRQ | PMGR Gate | Role |
|---|---|---|---|---|
| `otgphyctrl` | `0x36000000` | — | `60..64` | Shared USB 2.0 OTG & HSIC PHY (`drivers/usb/phy/phy-s5l8947x-usb.c`) |
| `usb-otg` (`dwc2`) | `0x36100000` | `15` | `61` | Micro-USB port (`role-switch`: peripheral CDC-ACM/ECM or host) |
| `usb-ehci` | `0x36400000` | `16` | `62` | EHCI host controller (port 2 HSIC -> `BCM4334` Wi-Fi `0a5c:bd1d`) |
| `usb-ohci0` | `0x36600000` | `17` | `63` | OHCI companion controller 0 |
| `usb-ohci1` | `0x36800000` | `18` | `64` | OHCI companion controller 1 |
| `usb-complex` | `0x3f108000` | — | `60` | USB arbitrator & HSIC control (`USB_CTL` at `+0x00`) |

### 3.2 PHY Power Ladder (`0x36000000`) & HSIC Port 2 Bring-Up

1. **Level `0 -> 1`:**
   - Assert resets: `ORSTCON (+0x08) |= 0x3f`.
   - Configure host burst & tuning: `reg(+0x60) = 0x2e0`, `OPHYPWR (+0x00) = 0x0f`, `OPHYCLK (+0x04) = 0x11` (24 MHz reference), `UOTGTUNE1 (+0x40) = 0x349` (`0x34f` in host mode), `UOTGTUNE2 (+0x44) = 0x6ff3` (`0x6e73` in host mode), `HSIC_TUNE (+0x5c) = 0x53`.
   - Wait 20 µs, deassert PHY clock reset `ORSTCON &= ~0x02`, wait 1,000 µs.
2. **Level `1 -> 2`:**
   - Set `USB_CTL (0x3f108000) |= 0x80` (`0xe4` with HSIC `0x64` enabled).
   - Clear `OPHYPWR &= ~0x01`, wait 20 µs, deassert `ORSTCON &= ~0x01`, wait 1,000 µs.
3. **Level `2 -> 3`:**
   - Clear `OPHYPWR &= ~0x08`.
4. **Host Mode & HSIC Port 2 Release:**
   - Set `reg(+0x64) |= 0x02` (host mode) or `reg(+0x1c) |= 0x02` (device mode).
   - Clear per-port reset in `ORSTCON (+0x08)`: port 2 (HSIC) is `bit 3` (`0x08`); ports 0/1/5 share `bit 4` (`0x10`).
   - Toggle `BCM4334` WLAN power/reset and handshake GPIOs (`HOST_READY` output / `DEVICE_READY` input) so `BCM4334` enumerates on EHCI port 2 (`usb 1-1`).

---

## 4. Hardware SHA-1 / SHA-2 Engine (`0x30200000`)

- **Clock Gate:** PMGR gate `46` (`0x3f1010c0`), IRQ `38`.
- **Registers:**
  - `+0x08` (`MODE`): `0x10` = SHA-1, `0x20` = SHA-224, `0x30` = SHA-256, `0x40` = SHA-384, `0x50` = SHA-512 (`bit 0` = FIFO mode when writing to `+0x20`).
  - `+0x0c` (`CTRL`): `0x03` = init IV + hash first block, `0x01` = continue hashing block; `bit 0` = busy.
  - `+0x10` (`STATUS`): `bit 0` = done.
  - `+0x40..0x7f` (`STATE`): 20/28/32/48/64-byte digest/state in memory word order (supports state import/export).
  - `+0x80..0xff` (`BLOCK`): 64-byte (`16 words`) or 128-byte (`32 words` for SHA-384/512) input block in memory word order.
- **Note:** On the single Cortex-A9 core at 850 MHz, uncached MMIO writes limit PIO throughput to `~14 MB/s`, which beats software `sha512sum` (`10.1 MB/s`); `sha384-s5l8947x` and `sha512-s5l8947x` are registered at `priority 300`, while `sha224-s5l8947x` and `sha256-s5l8947x` are registered at `priority 90` below the ARM/NEON software implementations (`19.5 MB/s`).

---

## 5. IOP Coprocessor (`0x3f300000`)

- **Hardware:** `apple,s5l8947x-iop` at `0x3f300000`, IRQ `2`, PMGR gate `29` (`0x3f10107c`; companion gate `30` at `0x3f101080` is enabled at boot).
- **Registers:**
  - `+0x00` (`CTRL`): `0` = halt/reset, `1` = run (reads back `1` while running).
  - `+0x04` (`IMAGE_PA`): physical address of the `0x24000`-byte (`147,456 B`) IOP firmware image.
  - `+0x34` (`CONFIG`): `0x01ff01ff` (non-coherent) or `0x03ff01ff` (coherent).
- **Firmware Header (`+0x13000`, magic `"gfnc"`):**
  - `+0x13004`: config flags (`bit 0 = 1` for non-coherent).
  - `+0x1318c` / `+0x13190`: 40 KiB (`0xa000`) heap physical address and byte size.
  - `+0x13360` / `+0x13364`: 16 KiB (`0x4000`) L1 page-table byte size and physical address.
  - `+0x1b3c0`: ASCII panic log buffer (`up to 0x1a2c0 bytes`).
- **Status:** Linux drives `FMI0` NAND directly from the Cortex-A9 (`s5l8947x-fmi`), so the IOP is not needed for storage; `s5l8947x-iop` provides sysfs firmware loading, MMU/heap patching, start/stop control, and panic-log inspection.
