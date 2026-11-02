# S5L8947X Central DMA (CDMA) Register & Descriptor Model

Clean-room hardware specification for the 42-channel `S5L8947X` Central DMA controller (`drivers/dma/s5l8947x-cdma.c`).

---

## 1. Overview, Clocks & Interrupts

- **MMIO Windows:**
  - **Window 0 (`0x37000000`, size `0x2d000`):** Global enable/idle bitmaps (page 0) and 42 per-channel register pages (`0x37000000 + ch * 0x1000`, `ch = 1..42`).
  - **Window 1 (`0x37800000`, size `0x9000`):** Hardware AES filter contexts (`0x37801000`, `0x1000` stride) and key-disable control (`0x37800000`).
- **Clock Gate:** PMGR gate `85` (`0x3f10115c`).
- **Interrupts:** AIC IRQs `51..92` (42 interrupts, one per channel `1..42`).
- **Hardware Version:** `cdma-version = 3`, `max-channel = 42`.

---

## 2. Global Page (`0x37000000`)

| Offset | Direction | Description |
|---|---|---|
| `+0x00`, `+0x04` | W | Set-enable bitmap for channels `0..31` / `32..63` (write `1 << ch` to enable channel clock/state). |
| `+0x08`, `+0x0c` | W | Clear-enable bitmap for channels `0..31` / `32..63` (write `1 << ch` to disable). |
| `+0x10`, `+0x14` | R | Enabled/idle bitmap for channels `0..31` / `32..63`. |

> [!IMPORTANT]
> A disabled channel page (`0x37000000 + ch * 0x1000`) reads back a free-running counter at `+0x00`/`+0x04` and silently drops writes. Always enable the channel in `+0x00`/`+0x04` of page 0 before programming its registers.

---

## 3. Per-Channel Register Page (`0x37000000 + ch * 0x1000`)

| Offset | Name | Width | Description |
|---|---|---|---|
| `+0x00` | `CSR` | 32-bit | Control and Status Register (see §3.1). |
| `+0x04` | `DCR` | 32-bit | Device Configuration Register (see §3.2). |
| `+0x08` | `DAR` | 24-bit | Device Address Register (keeps low 24 bits of the peripheral FIFO physical address; upper window bits `0x31..` / `0x34..` are hardwired per channel). |
| `+0x0c` | `DBR` | 32-bit | Device Byte Count Register (`0` for normal peripheral segment lists; transfer length for AES memory-to-memory). |
| `+0x10` | `MAR` | 32-bit | Memory Address Register (prefetch pointer; advances ahead of the peripheral FIFO drain — **do not** use for ALSA playback residue). |
| `+0x14` | `CAR` | 32-bit | Current Address Register (physical address of the active 32-byte segment descriptor; steps by 32 bytes at real-time peripheral pace). |
| `+0x18` | `ERR` | 32-bit | Error status register. |

### 3.1 `CSR` (`+0x00`) Bit Fields

| Bits | Access | Description |
|---|---|---|
| `0` | WO | `RUN`: start transfer (`1`). |
| `1` | WO | `ABORT`: abort transfer (`1`). |
| `2` | RW | `KICK`: request pause/abort handshake (acknowledged by `bit 21`). |
| `3` | RW | `DONE_IE`: command-list completion interrupt enable. |
| `4` | RW | `ERR_IE`: error interrupt enable. |
| `5` | RW | `PAUSE`: pause transfer (write `0x24`, poll `bit 21` or `!bit 16`). |
| `16` | RO | `ACTIVE`: channel is currently running. |
| `18` | W1C | `ERR`: transfer error status. |
| `19` | W1C | `DONE`: command list reached stop descriptor. |
| `20` | W1C | `SEG_DONE`: segment with control `bit 9` completed. |
| `21` | RO | `KICK_ACK`: handshake acknowledgement for `bit 2`. |
| `23` | RW | `SMALL_FIFO`: set when peripheral FIFO depth `< 128`. |
| `25:24` | RW | `TYPE`: `2` (`0x02000000`) on `cdma-version = 3`. |

> [!WARNING]
> Every write to `CSR` overwrites `DONE_IE` (`bit 3`), `ERR_IE` (`bit 4`), and `TYPE` (`bits 25:24`). When acknowledging `W1C` status bits (`bits 18..20`) in the interrupt handler, always preserve the configuration bits:
> `writel((csr & 0x03800018) | status_bits, chan_base + 0x00)`.

### 3.2 `DCR` (`+0x04`) Bit Fields

| Bits | Description |
|---|---|
| `1` | Direction: `1` = Memory → Device (`MEM_TO_DEV`), `0` = Device → Memory (`DEV_TO_MEM`). |
| `3:2` | Device FIFO width: `0x0` = 8-bit (`1 B`), `0x4` = 16-bit (`2 B`), `0x8` = 32-bit (`4 B`). |
| `6:4` | Burst size in words: `0x00` = 1, `0x10` = 2, `0x20` = 4, `0x30` = 8, `0x40` = 16, `0x50` = 32. |
| `7` | Always `1` (`0x80`) for peripheral transfers. |
| `21:16` | Peripheral request line (`0..63`). |

- **Audio (`I2S` / `S/PDIF` TX):** `DCR = 0x0000008a | (req << 16)` (32-bit width `0x8`, 1-word burst `0x00`, `MEM_TO_DEV` `0x2`, `bit 7` `0x80`). Setting `bit 6` (`0xca`, 16-word burst) overruns the 16-entry audio FIFOs by ~14×.
- **NAND (`FMI0` RX):**
  - Data (`ch 5`, `req 0`, `0x31200014`): `DCR = 0x00000088` (32-bit, burst 1, `DEV_TO_MEM`).
  - Metadata (`ch 6`, `req 1`, `0x31200018`): `DCR = 0x00010080` (8-bit, burst 1, `DEV_TO_MEM`).

---

## 4. 32-Byte Segment Descriptor Ring

Each segment descriptor is 32 bytes aligned in DMA-coherent memory:

| Offset | Field | Description |
|---|---|---|
| `+0x00` | `next` | Physical address of the next 32-byte descriptor (forms a circular ring for cyclic audio or points to a stop entry for scatter-gather). |
| `+0x04` | `control` | `0x02000003` for an active data segment (`bit 9` = `0x200` raises `CSR bit 20` on segment completion; `bit 8` = `0x100` on last segment). **`0x00000000` = Stop Entry** (terminates a scatter-gather command cleanly without raising `ERR`). |
| `+0x08` | `mem_addr` | Physical memory buffer address (`32-bit`). |
| `+0x0c` | `length` | Segment byte length. |
| `+0x10..0x1f` | `reserved` | Software bookkeeping (ignored by hardware). |

---

## 5. Peripheral Channel & Request Table

| Client | Channel | IRQ | Peripheral FIFO (`DAR`) | Direction | Width / Burst | Request Line |
|---|---|---|---|---|---|---|
| AES Mem → Engine | `1` | `51` | — | `MEM_TO_DEV` | — | — |
| AES Engine → Mem | `2` | `52` | — | `DEV_TO_MEM` | — | — |
| FMI0 Data | `5` | `55` | `0x31200014` | `DEV_TO_MEM` | 32-bit / 1 | `0` |
| FMI0 Metadata | `6` | `56` | `0x31200018` | `DEV_TO_MEM` | 8-bit / 1 | `1` |
| FMI1 Data | `7` | `57` | `0x31300014` | `DEV_TO_MEM` | 32-bit / 1 | `2` |
| FMI1 Metadata | `8` | `58` | `0x31300018` | `DEV_TO_MEM` | 8-bit / 1 | `3` |
| I2S0 TX | `23` | `73` | `0x34190010` | `MEM_TO_DEV` | 32-bit / 1 | `1` |
| I2S0 RX | `24` | `74` | `0x34190038` | `DEV_TO_MEM` | 32-bit / 1 | `0` |
| I2S1 TX | `25` | `75` | `0x34191010` | `MEM_TO_DEV` | 32-bit / 1 | `3` |
| I2S2 TX | `27` | `77` | `0x34192010` | `MEM_TO_DEV` | 32-bit / 1 | `5` |
| I2S3 TX | `29` | `79` | `0x34193010` | `MEM_TO_DEV` | 32-bit / 1 | `7` |
| S/PDIF TX | `31` | `81` | `0x34194010` | `MEM_TO_DEV` | 32-bit / 1 | `8` |
