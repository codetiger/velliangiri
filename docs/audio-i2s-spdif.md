# S5L8947X Audio Subsystem: I2S, S/PDIF, NCO & Multichannel HDMI

Clean-room hardware and register specification for the `S5L8947X` audio subsystem (`sound/arm/s5l8947x-audio.c`, `drivers/gpu/drm/s5l8947x/s5l8947x-display.c`).

---

## 1. Block Topology, Clocks & CDMA Channels

The `S5L8947X` audio subsystem consists of four slave I2S transmitters, one I2S clock master, one S/PDIF transmitter, and five PMGR numerically controlled oscillators (NCOs):

| Block | MMIO Base | PMGR Gate | IRQ | CDMA TX Ch | CDMA FIFO Addr | Request Line | Role |
|---|---|---|---|---|---|---|---|
| `i2s0` | `0x34190000` | `38` | `99` | `23` (`0x17`) | `0x34190010` | `1` | HDMI LPCM pair 0 (`FL` / `FR`) |
| `i2s1` | `0x34191000` | `39` | `100` | `25` (`0x19`) | `0x34191010` | `3` | HDMI LPCM pair 1 (`FC` / `LFE` or `RL` / `RR`) |
| `i2s2` | `0x34192000` | `40` | `101` | `27` (`0x1b`) | `0x34192010` | `5` | HDMI LPCM pair 2 (`RL` / `RR`) |
| `i2s3` | `0x34193000` | `41` | `102` | `29` (`0x1d`) | `0x34193010` | `7` | HDMI LPCM pair 3 (`SL` / `SR`) |
| `spdif` | `0x34194000` | `42` | `103` | `31` (`0x1f`) | `0x34194010` | `8` | Optical TOSLINK TX & HDMI S/PDIF receiver input |
| `i2s-master` | `0x34195000` | `37` | — | — | `0x34195010` | — | Master `BCLK` (`32 × fs`) & `LRCK` (`fs`) generator |

- **PMGR NCOs (`0x3f100110 + i * 0x10`, `i = 0..4`):**
  - Driven by PLL3 (`513 MHz`).
  - NCO 0 drives the I2S master `MCLK` (`256 × fs`).
  - NCO 4 drives the S/PDIF external `MCLK` (`256 × fs`).
  - Enable bit: `0x80000000` at `+0x00`; increment and modulo registers at `+0x04` and `+0x08` produce exact `8.192 MHz` (`32 kHz`), `11.2896 MHz` (`44.1 kHz`), and `12.288 MHz` (`48 kHz`) master clocks.

---

## 2. I2S Register Map (`0x80` Byte Stride)

`I2S0..3` (`0x34190000 + n * 0x1000`) and `I2S_MASTER` (`0x34195000`) share the same `0x80`-byte register file:

| Offset | Name | Reset | Writable Mask | Description |
|---|---|---|---|---|
| `0x00` | `CTL` | `0x00000022` | `0x000003fd` | `bit 0`: enable (`0x15` = internal 24 MHz clock, `0x01` = NCO MCLK). `bit 1`: read-only disabled status. `bit 6`: self-clearing soft reset. `bits [8:7]`: MCLK ratio (`×1`, `×2`, `×4`). Stop sequence: `TXCOM = 0`, `CTL = 0x34`, poll `CTL bit 1`. |
| `0x04` | `TXCON` | `0x00000000` | `0x1fffffff` | TX clock & frame format. Base `0x03000001`. `bits [6:5]`: `(width - 16) / 4`. `bits [4:3]`: frame length (`0` = 32 BCLK, `1` = 48/64 BCLK, `3` = slave/custom). `bits [10:8]`: `BCLK` divider `24 MHz / (2 × (v + 1))` (`bit 7` = bypass). `bit 20`: external/slave clock (`0x03100019` on `I2S0..3` slaves; `0x03000301` on NCO-clocked master). |
| `0x08` | `TXCOM` | `0x01f80000` | `0x01ffffff` | TX FIFO & channel control. `bit 0`: TX pause. `bit 2`: TX enable. Start value: `0x01f80006` (with threshold in `bits [18:14]`). |
| `0x10` | `TXDATA` | — | Write-only | TX FIFO data port (one 32-bit word per sample slot; 2 words per stereo frame). |
| `0x30` | `RXCON` | `0x00000000` | `0x00003fff` | RX clock/format mirror. |
| `0x34` | `RXCOM` | `0x01f80000` | `0x01ffffff` | RX FIFO/channel mirror. |
| `0x3c` | `STATUS` | `0x00000024` | Read-only | `bit 0`: `LRCK` phase. `bit 2`: TX disabled. `bits [12:7]`: 6-bit TX FIFO level (`0..63`). `bit 19`: TX enabled. |
| `0x40` | `EXTFRAME` | `0x00000000` | `0x0000ffff` | Extended frame length when `TXCON[4:3] == 3`. |
| `0x44` | `VERSION` | `0x00020006` | Read-only | Hardware revision constant. |

---

## 3. S/PDIF Register Map (`0x34194000`)

The S/PDIF transmitter follows the Samsung S/PDIF layout (`sound/soc/samsung/spdif.c`) with a 16-entry FIFO:

| Offset | Name | Description |
|---|---|---|
| `0x00` | `CLKCON` | `bit 0`: power on (`1`). `bit 2`: external NCO MCLK (`0x5` = NCO4 external MCLK, `0x1` = internal 24 MHz). |
| `0x04` | `CON` | `bit 0`: PCM (`1`) vs compressed bitstream (`0`). `bits [2:1]`: 16/20/24-bit sample width. `bits [4:3]`: `MCLK` divider (`/256`, `/384`, `/512`). `bit 5`: soft reset. `bits [21:19]`: FIFO DMA threshold. `bits [26:22]`: 5-bit FIFO level (`0..15`). Standard 16-bit `/256` value: `0x00381001`. |
| `0x08` | `BSTAS` | Burst status for IEC 61937 compressed audio passthrough. |
| `0x0c` | `CSTAS` | IEC 60958 channel status (`bits [27:24]` sample rate code, `bits [15:8]` category, `bit 2` non-copyright, `bit 1` non-audio/compressed flag). |
| `0x10` | `DATA` | 16-bit sample FIFO (`bits [15:0]`; two writes per stereo frame: `L`, then `R`). |

---

## 4. Multichannel HDMI LPCM (`2 / 4 / 6 / 8` Channels)

- `I2S_MASTER` (`0x34195000`) generates `BCLK = 32 × fs` and `LRCK = fs` from NCO 0 (`256 × fs / 8`).
- `I2S0..3` (`0x34190000..0x34193000`) run in slave mode (`TXCON = 0x03100019`) and share the master's bit and frame clocks in lockstep, each driving one stereo data line into the HDMI transmitter (`0x39740000`).
- Each active I2S slave uses its own coherent DMA ring (`frames * 8` bytes) and cyclic CDMA transfer (`DCR = 0x0008008a | (req << 16)`). Only `I2S0` enables period interrupts; ALSA pointer residue is read from `I2S0` (`CAR` at `+0x14`).
- The driver's `copy` callback deinterleaves incoming ALSA frames into per-line `(L, R)` pairs and remaps standard ALSA channel order into CEA-861 HDMI LPCM order:

| Channels | ALSA Input Order | HDMI I2S Data Lines (CEA-861 Order) | `HDMI_I2S_MUX_CH` | AUI `CA` |
|---|---|---|---|---|
| `2` | `FL FR` | Line 0: `FL FR` | `0x03` | `0x00` |
| `4` | `FL FR RL RR` | Line 0: `FL FR`, Line 1: `RL RR` | `0x0f` | `0x08` |
| `6` | `FL FR RL RR FC LFE` | Line 0: `FL FR`, Line 1: `FC LFE`, Line 2: `RL RR` | `0x3f` | `0x0b` |
| `8` | `FL FR RL RR FC LFE SL SR` | Line 0: `FL FR`, Line 1: `FC LFE`, Line 2: `RL RR`, Line 3: `SL SR` | `0xff` | `0x13` |

- **HDMI Transmitter Audio Setup (`0x39700000`):**
  - `HDMI_I2S_CON_1 = 0`, `HDMI_I2S_CON_2 = 0x04` (basic 16-bit I2S; `0x06` is left-justified and shifts the MSB).
  - `HDMI_I2S_PIN_SEL_0..3 = 0x77, 0x77, 0x77, 0x07`, `HDMI_I2S_CLK_CON = 1`, `HDMI_I2S_MUX_CUV = 3`, `HDMI_I2S_MUX_CON = 0x77`.
  - `HDMI_ACR_CON = 4` (hardware-measured CTS in `0x39710410..0x39710418`), `N = 4096 / 6272 / 6144` for `32 / 44.1 / 48 kHz`.
  - Audio InfoFrame (`AUI`) byte 1 encodes `(channels - 1)` and byte 4 encodes the CEA-861 speaker allocation (`CA`).
