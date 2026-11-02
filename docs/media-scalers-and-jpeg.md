# S5L8947X Media Engines: M2 Scalers, JPEG Decoder, DART & VXD

Clean-room hardware and register specification for the `S5L8947X` non-realtime media domain (`HPERF-NRT`, PMGR parent gate `9`): dual M2 Scaler/CSC engines (`drivers/media/platform/apple/s5l8947x-m2sc.c`), baseline JPEG decoder (`drivers/media/platform/apple/s5l8947x-jpeg.c`), DART IOMMU (`0x38b00000`), and VXD390 video decoder (`drivers/soc/apple/s5l8947x-vxd.c`).

---

## 1. Media Domain Overview & Clock Gates

All four blocks sit under the `HPERF-NRT` parent domain (PMGR gate `9`, `0x3f10102c`) and share the `dart-nrt` IOMMU at `0x38b00000` (which boots in bypass mode, `CONFIG = 0`, allowing all masters to use full 32-bit physical DDR addresses at `0x80000000..0x9fffffff`):

| Block | MMIO Base | PMGR Gate (`0x3f101008 + 4*i`) | IRQ | Hardware Version | Linux Device |
|---|---|---|---|---|---|
| `vxd` | `0x38100000` | Gate `10` (`0x3f101030`) | `50` | `vxd-version = 2` | `/sys/bus/platform/devices/38100000.video-decoder` |
| `jpeg` | `0x38200000` | Gate `13` (`0x3f10103c`) | `118` | `+0x8c = 0x0001000e` (`o5d02`) | `/dev/video2` (`s5l8947x-jpeg`) |
| `scaler0` | `0x38300000` | Gate `11` (`0x3f101034`) | `9` | `+0x260 = 0x00070000` (Layout E) | `/dev/video0` (`s5l8947x-m2sc`) |
| `scaler1` | `0x38400000` | Gate `12` (`0x3f101038`) | `10` | `+0x260 = 0x00070000` (Layout E) | `/dev/video1` (`s5l8947x-m2sc`) |
| `dart-nrt` | `0x38b00000` | Always on with `HPERF-NRT` | `116` | `dart-version = 1` | Bypass (`+0x0c = 0`) |

---

## 2. Dual M2 Scaler / CSC (`0x38300000`, `0x38400000`)

The `S5L8947X` M2 scalers report hardware version `0x00070000` at `+0x260` (**Layout E**: 4-bit pixel format fields, 15-bit source dimensions up to `16384×16384`, 13-bit destination dimensions up to `4096×4096`, explicit CSC enable bit).

### 2.1 Register Map

| Offset | Name | Writable Mask | Description |
|---|---|---|---|
| `0x000` | `STATUS` | Read-only | Reads `1` when idle; `bit 1` = busy. |
| `0x004` | `CTRL` | `0x00000703` | `bit 0`: `START` (`RMW \|= 1`). `bit 1`: `SOFT_RESET` (self-clearing). `bit 8`: `CSC_EN` (`1`). `bit 9`: output YCbCr limited-range offset (`+16/+128`). `bit 10`: input YCbCr limited-range offset (`-16/-128`). |
| `0x008` | `IRQ_EN` | `0x00000001` | `bit 0`: completion interrupt enable. |
| `0x00c` | `IRQ_STATUS` | `0x00000001` | `bit 0`: job done (`W1C`). |
| `0x010` | `SRC_CFG` | `0x0000078f` | `bits [3:0]`: source format code (§2.2). `bit 6`: R/B swap (`ABGR8888`). `bit 10`: tiled surface. |
| `0x014` | `SRC_ADDR0` | `0xffffffff` | Source plane 0 (`Y` or `RGB`) physical address. |
| `0x018` | `SRC_ADDR1` | `0xffffffff` | Source plane 1 (`CbCr`) physical address. |
| `0x01c` | `SRC_STRIDE` | `0xffffffff` | `bits [15:0]`: plane 0 stride **in pixels** (`bytes / bpp`). `bits [31:16]`: plane 1 stride **in bytes**. |
| `0x020` | `SRC_OFFSET_Y` | `0xffffffff` | `bits [15:0]`: Y offset (`14.2` fixed-point). `bits [31:16]`: X offset (`14.2`). Set to `0` when crop is folded into `SRC_ADDR0/1`. |
| `0x024` | `SRC_SIZE` | `0x7fff7fff` | `bits [14:0]`: crop height `H`. `bits [30:16]`: crop width `W`. |
| `0x028` | `SRC_OFFSET_C` | `0xffffffff` | Chroma X/Y offset (`14.2` fixed-point; `0` when folded into `SRC_ADDR1`). |
| `0x030` | `DST_CFG` | `0x0000078f` | `bits [3:0]`: destination format code (§2.2). `bit 6`: R/B swap. `bit 10`: tiled. |
| `0x034` | `DST_ADDR0` | `0xffffffff` | Destination plane 0 physical address. |
| `0x038` | `DST_ADDR1` | `0xffffffff` | Destination plane 1 physical address. |
| `0x03c` | `DST_STRIDE` | `0xffffffff` | Layout E: `strideInPixels \| (strideInPixels << 16)` (both halves carry plane 0 stride in pixels). |
| `0x040` | `DST_SIZE` | `0x1fff1fff` | `bits [12:0]`: output height `dstH`. `bits [28:16]`: output width `dstW`. |
| `0x044` | `DST_OFFSET` | `0x0fff0fff` | `bits [11:0]`: Y offset, `bits [27:16]`: X offset (`0` when compose is folded into `DST_ADDR0/1`). |
| `0x050` | `H_STEP` | `0x0007ffff` | Horizontal luma DDA step `(srcW << 16) / dstW` in `3.16` fixed-point (max `< 8.0`). |
| `0x054` | `V_STEP` | `0x0007ffff` | Vertical luma DDA step `(srcH << 16) / dstH` in `3.16` fixed-point (**hard hardware limit: `≤ 7.0`**, i.e. `≤ 0x70000`). |
| `0x058` | `ROT_FLIP` | `0x0000000f` | `bits [1:0]`: `0` = 0°, `1` = 270° (`90° CW`), `2` = 180°, `3` = 90° (`270° CW`). `bit 2`: V-flip. `bit 3`: H-flip. (90°/270° swaps `dstW`/`dstH` in `H_STEP`/`V_STEP`). |
| `0x060..0x0ef` | `COEF_H_LUMA` | 36 words | 9 phases × 4 words (8 signed 9-bit taps in `2.7` format, sum = `128`). |
| `0x0f0..0x137` | `COEF_V_LUMA` | 18 words | 9 phases × 2 words (4 signed 9-bit taps in `2.7` format, sum = `128`). |
| `0x140..0x1cf` | `COEF_H_CHROMA`| 36 words | 9 phases × 4 words (8 taps). |
| `0x1d0..0x217` | `COEF_V_CHROMA`| 18 words | 9 phases × 2 words (4 taps). |
| `0x220..0x240` | `CSC_M[0..8]` | 9 words | 3×3 colour-space conversion matrix (`12-bit` two's complement, `0x200 = 1.0`). |
| `0x250` | `DITHER` | `0x000001ff` | Per-channel target bit-depth dither codes (`0x0db` for `RGB565`, `0x124` for `ARGB1555`, `0` for 8-bit). |
| `0x260` | `HW_VERSION` | Read-only | Reads `0x00070000`. |

### 2.2 Pixel Format Codes (`SRC_CFG[3:0]` / `DST_CFG[3:0]`)

| V4L2 FourCC | Format | HW Code (`[3:0]`) | `bit 6` | Planes | Bytes/Pixel (P0 / P1) | Chroma Subsampling |
|---|---|---|---|---|---|---|
| `NV12` / `NM12` | Y + interleaved CbCr 4:2:0 | `0x0` | `0` | 2 | `1` / `2` | `2 × 2` |
| `NV16` | Y + interleaved CbCr 4:2:2 | `0x2` | `0` | 2 | `1` / `2` | `2 × 1` |
| `NV24` | Y + interleaved CbCr 4:4:4 | `0x3` | `0` | 2 | `1` / `2` | `1 × 1` |
| `RGBP` (`RGB565`) | 16-bit RGB 5:6:5 | `0x4` | `0` | 1 | `2` | — |
| `AR15` (`ARGB1555`) | 16-bit ARGB 1:5:5:5 | `0x5` | `0` | 1 | `2` | — |
| `XR24` / `AR24` | 32-bit `BGRX` / `BGRA` (`0xAARRGGBB` LE) | `0x6` | `0` | 1 | `4` | — |
| `XB24` / `AB24` | 32-bit `RGBX` / `RGBA` | `0x0` | `1` | 1 | `4` | — |
| `YUYV` | Packed 4:2:2 (`Y0 Cb Y1 Cr`) | `0xa` | `0` | 1 | `2` | `2 × 1` |

### 2.3 Polyphase Filter Packing & Clean-Room Box/Bilinear Generation

For phase `p` (`0..8`, phases `9..15` are mirrored in hardware from `16 - p`), tap `k` is centred at `(k - centre) + p / 16` source pixels (`centre = 4` for 8-tap H, `2` for 4-tap V). The clean-room driver computes each tap weight as the overlap of the tap's unit interval with a box footprint of width `max(step, 1.0)`, normalized to sum to `128`, and packs 9-bit signed values (`& 0x1ff`):
- **8-tap Horizontal (`4 words/phase`):** `w0 = c6 | (c7 << 16)`, `w1 = c4 | (c5 << 16)`, `w2 = c2 | (c3 << 16)`, `w3 = c0 | (c1 << 16)`.
- **4-tap Vertical (`2 words/phase`):** `w0 = c2 | (c3 << 16)`, `w1 = c0 | (c1 << 16)`.

---

## 3. Baseline JPEG Decoder (`0x38200000`)

The `S5L8947X` JPEG block (`+0x8c = 0x0001000e`) is a hardware-autonomous baseline `SOF0` decoder (`1/1` scale; downscaling is handled by chaining into the M2 scaler via zero-copy `DMABUF`).

### 3.1 Register Map

| Offset | Name | Description |
|---|---|---|
| `0x000` | `MODE` | `bit 3`: `1` = decode (`0x8`), `0` = encode. `bits [2:0]`: filled by hardware from `SOF0` after header parse (`0` = 4:4:4, `1` = 4:2:2, `2` = 4:2:0, `3` = grayscale, `6` = 4:1:1). |
| `0x00c` | `ALIVE` | Reset handshake register: write `1`, poll readback `== 1`, then write `0`. |
| `0x010` | `HEIGHT` | Decoded image height (filled by hardware after header phase). |
| `0x014` | `WIDTH` | Decoded image width (filled by hardware after header phase). |
| `0x01c` | `IRQ_CTRL` | Interrupt enable / re-arm register (`0x78` on decode prepare and after header IRQ). |
| `0x020` | `IRQ_STATUS` | `bit 3` (`0x08`): `HDR_DONE`. `bit 4` (`0x10`): `STREAM_ERR`. `bit 6` (`0x40`): `FRAME_DONE`. |
| `0x028` | `DEC_ARM` | Write `1` immediately before `START`. |
| `0x050` | `PIX_ADDR0` | Output plane 0 (`Y` for `NV12`, or packed `YUYV`/`UYVY`) physical address (4-byte aligned). |
| `0x054` | `PIX_STRIDE0` | Output plane 0 byte stride (multiple of 4). |
| `0x058` | `BS_ADDR` | Input JPEG bitstream physical address (**must be 32-byte aligned**). |
| `0x068` | `CLK_CFG` | Write `0x20` on version `0x0001000e`. |
| `0x06c` | `ENABLE` | `bit 0`: block enable (`1` when active). |
| `0x070` | `START` | Write `1` to start phase 1 (hardware `SOI`/`DQT`/`DHT`/`SOF`/`SOS` header parse). |
| `0x074` | `DEC_CONTINUE`| Write `1` after `HDR_DONE` to start phase 2 (entropy & IDCT body decode). |
| `0x078` | `SOFT_RESET` | `bit 0`: self-clearing soft reset (`\|= 1`, poll until `0`). |
| `0x07c` | `TIMEOUT` | `bit 31` enable \| `bits [30:0]` watchdog cycle count (`0xffffffff`). |
| `0x080` | `TIMER` | `bit 31` timeout expired; `bits [30:0]` remaining cycles. |
| `0x088` | `DEC_OUT_CFG` | `bits [1:0]`: output pixel format (`0` = `NV12` bi-planar, `1` = `YUYV`, `3` = `UYVY`). `bit 10`: tiled (`0` for linear). |
| `0x08c` | `HW_VERSION` | Read-only; reads `0x0001000e`. |
| `0x094` | `BS_LEN` | Input bitstream buffer size in bytes (must be `> 256`; restore to `0x1ffffffe` after decode). |
| `0x0a0` | `PIX_OFFSET0` | Plane 0 X/Y offset (`0`). |
| `0x0a4` | `PIX_ADDR1` | Output plane 1 (`CbCr` for `NV12`, `0` for packed `YUYV`/`UYVY`) physical address. |
| `0x0a8` | `PIX_STRIDE1` | Output plane 1 byte stride (`0` for packed). |
| `0x0ac` | `PIX_OFFSET1` | Plane 1 X/Y offset (`0`). |

### 3.2 Two-Phase Decode Sequence

1. **Reset & Enable:** `SOFT_RESET |= 1` (poll `0`), `ALIVE = 1` (poll `1`), `ALIVE = 0`, `MODE = 8`, `ENABLE = 1`.
2. **Phase 1 (Header Parse):** Write `MODE = 8`, `IRQ_CTRL = 0x78`, `TIMEOUT = 0xffffffff`, `CLK_CFG = 0x20`, `BS_ADDR = src_pa`, `BS_LEN = max(len, 512)`, `DEC_ARM = 1`, `DEC_OUT_CFG = 0`, `START = 1`.
3. **Header IRQ (`IRQ_STATUS & 0x08`):** Re-arm `IRQ_CTRL = 0x78`, read `WIDTH` (`+0x14`), `HEIGHT` (`+0x10`), and chroma mode (`MODE & 7`).
4. **Phase 2 (Body Decode):** Write `DEC_OUT_CFG` (`0` = `NV12`, `1` = `YUYV`, `3` = `UYVY`), `PIX_STRIDE0/1`, `PIX_ADDR0/1`, `PIX_OFFSET0/1 = 0`, and kick `DEC_CONTINUE = 1`.
5. **Frame Done IRQ (`IRQ_STATUS & 0x40`):** Restore `BS_LEN = 0x1ffffffe` and run the soft-reset sequence.

---

## 4. DART IOMMU (`0x38b00000`)

The media DART (`dart-nrt`, IRQ `116`, `0x20`-byte register set aliased across `0x000..0xfff`) serves four stream IDs (`SID 0..1` = JPEG, `SID 2..3` = M2 scalers):

| Offset | Name | Description |
|---|---|---|
| `0x00` | `TLB_OP` | `bit 3`: busy. `bits [2:0]`: `5` = write STE from `+0x08`, `4` = read STE into `+0x08`, `2` = flush (`0x0fffff02` flushes all). `bits [9:8]`: SID (`0..3`). `bits [27:22]`: STE index (`0..63`, 4 MiB each = 256 MiB device VA). |
| `0x08` | `STE_DATA` | L2 page-table physical address \| `1` (valid). Each 4 KiB L2 table holds 1,024 32-bit PTEs (`PA \| 1`). |
| `0x0c` | `CONFIG` | One byte per SID (`SID 0..3`): `bit 7` = translation enable, `0x3b` = suppress faults. **Boots with `0x00000000` (all SIDs in physical bypass).** |
| `0x10` | `ERR_STATUS` | `bit 31`: error pending (`W1C` with `0x3f`); `SID = status >> 24`. |
| `0x14` | `ERR_ADDR` | Faulting 32-bit device virtual address. |
| `0x18` | `DIAG_CONFIG`| Invalid-translation fallback physical page (`0x30000000`). |

---

## 5. VXD390 Video Decoder (`0x38100000`)

- **Hardware:** `apple,s5l8947x-vxd` at `0x38100000` (size `0x100000`), IRQ `50`, PMGR gate `10` (`0x3f101030`).
- **Firmware Container (`VXDF`):** `scripts/tools/vxdcarve.c` extracts the two microcontroller segments (`21,612 B` + `6,296 B`) into `/lib/firmware/s5l8947x/vxd390.fw` (`VXDF` magic `0x46445856`), which `drivers/soc/apple/s5l8947x-vxd.c` formats into a 40 KiB (`0xa000`) DMA buffer (`0xcd` fill + `0x10101010` end marker at `+0x9ffc`).
- **Current Blocker:** Enabling PMGR gate `10` transitions the gate register from `0x300` to `0x1ff` (power state active, but `bit 9` "clock alive" remains clear because the `VDEC` root clock divider is left unconfigured by the pre-kernel boot chain). Consequently, the reset poll at `+0x600` (`0x11110100`, waiting for `bit 8`) times out cleanly (`-ETIMEDOUT`) without stalling the AXI bus.
