# S5L8947X NAND Flash (FMI / PPN / FTL) & Hardware Crypto Architecture

Clean-room hardware and on-disk format specification for the `S5L8947X` Flash Memory Interface (`drivers/mtd/devices/s5l8947x-fmi.c`), Flash Translation Layer & `LwVM` partition mapper (`drivers/mtd/s5l8947x-ftl.c`), and CDMA AES engine (`drivers/crypto/s5l8947x-aes.c`).

---

## 1. FMI Controller (`0x31200000` / `0x31300000`)

### 1.1 MMIO Windows, Clocks & CDMA

Each channel `B` (`FMI0 = 0x31200000`, `FMI1 = 0x31300000`) exposes three MMIO sub-windows:

| Sub-Window | Role |
|---|---|
| `B + 0x00000` | Host interface: transfer configuration, command start, interrupt status, Data (`+0x14`) & Metadata (`+0x18`) FIFOs |
| `B + 0x40000` | Flash-bus sequencer (`FMC`): chip-enable (`CE`), command bytes, address cycles, bus timing, status match |
| `B + 0x80000` | Hardware BCH ECC engine (used for 512-byte boot pages; main-area PPN pages use on-package ECC) |

- **PMGR Clock Gates:** `FMI0` = gates `47, 48` (`0x3f1010c4`, `0x3f1010c8`, reset `bit 31` on gate 47); `FMI1` = gates `49, 50` (`0x3f1010cc`, `0x3f1010d0`).
- **Interrupts:** `FMI0` = IRQ `33`, `FMI1` = IRQ `34`.
- **CDMA Channels (`FMI0`):**
  - Data FIFO (`0x31200014`): CDMA channel `5`, request `0` (32-bit width, burst 1).
  - Metadata FIFO (`0x31200018`): CDMA channel `6`, request `1` (8-bit width, burst 1).

### 1.2 Host Interface Registers (`B + 0x00000`)

| Offset | Description |
|---|---|
| `0x00` | Transfer config: `0` = raw single, `3` = PPN logical page (`4 KiB` data + `16 B` metadata), `5` = raw page stream, `0xf0` = 512-byte BCH-30 boot page. |
| `0x04` | Command: `3` = start read transfer, `5` = start write transfer, `6` = reset host & FIFOs. |
| `0x0c` | Status (`W1C`): `bit 1` = transfer done (`0x2`). |
| `0x10` | Interrupt enable (`0x100` = sequencer status match, `0x2` = host transfer done). |
| `0x14` | 32-bit Data FIFO (PIO or CDMA channel 5). |
| `0x18` | 8-bit Metadata FIFO (PIO or CDMA channel 6). |
| `0x1c` | FIFO readiness: `bits [2:0]` = metadata FIFO level, `bits [4:3]` = data FIFO ready. |
| `0x34` | Transfer shape: `bits [7:0]` = sector count, `bits [18:8]` = bytes per sector, `bits [24:19]` & `bits [30:25]` = metadata bytes per logical page (`4 \| (16 << 19) \| (16 << 25) \| (1024 << 8)` for a 4 KiB PPN logical page). |

### 1.3 Sequencer Registers (`B + 0x40000`)

| Offset | Description |
|---|---|
| `0x40000` | Control: `1` = enable async mode. |
| `0x40008` | Bus timing: `bits [15:12]` = `tRP` (`RE#` low clocks − 1), `bits [11:8]` = `tREH` (`RE#` high clocks − 1), `bits [7:0]` = write timing. Computed from PPN parameter page at ~110 MHz FMI clock (`0x10ff` -> `15.1 MB/s` read throughput; `0xffff` = safe reset default). |
| `0x4000c` | Chip Enable (`CE`): one-hot `1 << (ce & 7)`, `0` = deassert all. |
| `0x40010` | Sequencer start (`GO`): `bit 0` = `cmd1`, `bit 1` = `cmd2`, `bit 3` = address cycles (`0x1`, `0x3`, `0x9`, `0xb`), `0x50` = poll status match. |
| `0x40014` | Command bytes: `cmd1` in `bits [7:0]`, `cmd2` in `bits [15:8]`. |
| `0x40018` | Address bytes `0..3` (`row` for 4-byte PPN commands). |
| `0x4001c` | Address bytes `4..7`. |
| `0x40020` | Address cycle count minus 1 (`0` = 1 byte, `3` = 4 bytes). |
| `0x40040` | Sequencer interrupt enable (`0x20` = status match). |
| `0x40044` | Sequencer status (`W1C`): `bit 0` = `cmd1` done, `bit 1` = `cmd2` done, `bit 3` = address done, `bit 5` = status match. |
| `0x40048` | Last NAND status byte read from the bus. |
| `0x4004c` | Status match mask & value: `mask \| (value << 8)` (`0x4040` = wait for `bit 6` ready). |

---

## 2. PPN 1.5 Flash Protocol & Page Layout

The flash package on `FMI0 CE0` is a Toshiba PPN 1.5 device (`READ ID = 50 50 4e 01 05 05 00 00`, manufacturer ID `98 00 01 44 19 01`, firmware `080100P`).

### 2.1 PPN Command Sequences

1. **Reset (`0xff`):** `CE` on -> `cmd1 = 0xff`, `GO = 1` -> wait `bit 0` -> `CE` off.
2. **Set Power State (Required after reset):** Set feature `0x0180` = `2` (async active mode) via `cmd1 = 0xef`, 2-byte address `0x0180` (`GO = 9`), write `0x00000002` to data FIFO (`host 0x00 = 5`, `0x34 = 0x401`, `0x04 = 5`), `cmd1 = 0xe7`, poll operation status (`0x77` + `0x7d`). Without this step, page reads fail with sticky status `0x51`.
3. **Read Device Parameters (`512 B`, magic `"PPN Device Info"`):** `cmd1 = 0x92`, 1-byte addr `0x00`, `cmd2 = 0x97`, poll operation status, `cmd1 = 0x7a` (data out), read 512 bytes.
   - Geometry: `2` CAUs × `1,064` blocks/CAU × `256` MLC pages/block (`128` SLC pages/block) × `16,448` bytes/page (`16,384` data + `64` metadata) = `8.92 GB` raw (`544,768` pages).
4. **Row Addressing:**
   - `row = (slc_flag << 20) | (cau << 19) | (block << 8) | page`
   - `bit 20` (`0x100000`) selects SLC mode (`128` pages/block, used for reserved blocks `1048..1063`).
5. **Single-Page DMA Read (`16 KiB` data + `64 B` metadata):**
   - Issue `cmd1 = 0x0a`, 4-byte `row`, `cmd2 = 0x37` (`GO = 0xb`).
   - Poll operation status (`cmd1 = 0x77`, `cmd2 = 0x7d`, `GO = 3`, then status match `0x4040`):
     - `0x40` / `0x42` = clean / refresh OK; `0x49` = blank (`0xff`); `0x43..0x45` = uncorrectable.
   - Issue `cmd1 = 0x7a` (`GO = 1`).
   - Start CDMA channel 5 (`16,384 B` from `0x31200014`) and channel 6 (`64 B` from `0x31200018`), and step the host transfer 4 times (`host 0x00 = 3`, `0x34 = 4 | (16 << 19) | (16 << 25) | (1024 << 8)`, `0x04 = 3`, wait `0x0c bit 1`).
   - Finish with `cmd1 = 0x77` (`GO = 1`), `host 0x04 = 6`, `CE` off.
   - On the raw bus, each 16,448-byte page consists of **4 logical pages × 4,112 bytes**, where each logical page is `[1,024 B data][16 B metadata][3,072 B data]`.

---

## 3. FTL Translation Layer & `LwVM` Partition Map

### 3.1 16-Byte Logical Page Metadata (`type 0x01`)

Each 4 KiB logical page carries a 16-byte unwhitened metadata record:

| Offset | Type | Field |
|---|---|---|
| `+0x00` | `u8` | Page type (`0x01` = user data, `0x02` = FTL index page, `0x1c` = superblock Table of Contents `TOC`). |
| `+0x01` | `u8` | Flags (`bit 0` = garbage-collection destination on `TOC` pages). |
| `+0x02` | `u16` | Superblock sequence number (`seq`). |
| `+0x04` | `u32` | Superblock generation counter (`gen`). |
| `+0x08` | `u32` | 4 KiB Logical Block Address (`lba`, `< 0x1dcd65` = `1,953,125` blocks = `8,000,000,000` bytes). |
| `+0x0c` | `u32` | Initial write generation. |

- **Superblocks & TOCs:** A superblock stripes across block `b` of CAU 0 and CAU 1 in `page -> CAU -> logical_page` order. Closed superblocks store a Table of Contents (`type 0x1c`, 4 or 8 logical pages) in their final 1–2 pages containing run records `{flags, generation, lba, count}`. Open superblocks (without a trailing TOC) are scanned page-by-page. The newest copy of any `lba` is selected by `(run_gen, toc_gen, toc_seq, physical_pos)`.

### 3.2 `LwVM` Volume Header (Logical Block 0)

Logical block 0 of `/dev/nand0` holds a `LwVM` partition map:
- **Header (`+0x000`):** 16-byte type GUID `6a9088cf-8afd-630a-e351-e24887e0b98b`, 16-byte media UUID at `+0x10`, `u64` media size at `+0x20`, `u32` partition count (`2`) at `+0x28`.
- **Partition Records (`+0x200`, `0x80` bytes each):**
  - Partition 0 (`System`, `/dev/nand0p1`): plaintext HFSX (`~1.40 GB`), `attributes = 0`.
  - Partition 1 (`Data`, `/dev/nand0p2`): AES-256-CBC encrypted HFSX (`~6.60 GB`), `attributes & BIT_ULL(48)`.
- **8 MiB Chunk Map (`+0x800`):** `1,024` `u16` entries (`partition_idx << 12 | partition_chunk_idx`; `0xf...` = unallocated/header).

---

## 4. CDMA AES Engine (`0x37801000`) & Key Hierarchy

### 4.1 Hardware AES Register Interface

- **Clock Gate:** PMGR gate `28` (`0x3f101078`).
- **Key Disable Status (`0x37800000`):** `bit 0` = hardware device key disabled, `bit 1..2` = group keys disabled. Must read `0` on `bit 0` for volume/keybag key derivation.
- **Context 1 Registers (`0x37801000`, requires CDMA channel 1 enabled):**
  - `+0x00` (`CTRL`): `bit 8` = always `1`; `bit 16` = `1` encrypt / `0` decrypt; `bit 17` = `1` CBC / `0` ECB; `bits [19:18]` = key size (`0` = 128-bit, `1` = 192-bit, `2` = 256-bit); `bit 20` = `1` software key / `0` hardware key (`bits [22:21] = 0` selects the per-device hardware key).
  - `+0x10..0x1c`: 128-bit IV (4 little-endian words).
  - `+0x20..0x3c`: 128/192/256-bit software key (4–8 little-endian words).
- **DMA Execution:** CDMA channel 1 (`MEM_TO_DEV`, descriptor control `0x02030103`, `CSR = 0x021c0180`) feeds plaintext/ciphertext into the engine; CDMA channel 2 (`DEV_TO_MEM`, descriptor control `0x02000103`, `CSR = 0x021c0080`) writes the result back to memory. Trigger both with `CSR |= 1`.

### 4.2 SLC Key Store (`Face`) & Volume Decryption (`LwVM`)

1. **Locating the SLC Key Store:**
   - Boot-page row 0 (`CAU 0, block 0, page 0`, 3 × 512 B with 30-bit BCH, magic `'ndrG'` = `0x4772646e`) defines the `'plog'` partition (`flags = 0x108` -> SLC mode `row bit 20`), mapped via the `'rAps'` spare table (`+0x250`) and `'sbus'` substitution table (`+0x35c`) to four SLC blocks near the top of CAU 0/1 (`1055..1057`).
   - Within the active SLC block, SLC pages `0..15` carry `'nepo'` headers, pages `16..111` carry 96 redundant copies of the 1,024-byte key store, and pages `112..127` carry `'solc'` trailers.
2. **Unobfuscating the `Face` Container:**
   - XOR bytes `0x00..0x0f` with bytes `0x10..0x1f`, then zero bytes `0x10..0x1f`.
   - Verify magic `0x46616365` (`'Face'`) at `+0x00` and the CRC-32 at `+0x3c`.
   - Parse `'kL'` (`0x4c6b`) TLV lockers at `+0x40..0x3ff`: `'LwVM'` (`80 B`), `'BAG1'` (`52 B`), `'Dkey'` (`40 B`), `'DONE'`.
3. **`LwVM` Volume Key & Per-LBA IV:**
   - Derive wrapping key `k_wrap = AES-128-CBC_encrypt(hw_device_key, iv=0, seed=183e99676bb03c546fa468f51c0cbd49)`.
   - Decrypt the 80-byte `'LwVM'` locker payload with `AES-128-CBC_decrypt(k_wrap, iv=0)` to extract the 32-byte `Data` partition AES-256 key (`vol_key` at `+0x30..0x4f`).
   - Each 4 KiB block on `/dev/nand0p2` at whole-disk offset `lba = disk_byte_offset >> 12` is decrypted with `AES-256-CBC(vol_key, iv=LFSR(lba))`, where `LFSR(x)` steps a 4-word Galois LFSR (`poly = 0x80000061`) 4 times.

### 4.3 HFSX Per-File Content Protection (`com.apple.system.cprotect`)

Files on `/dev/nand0p2` carry a v4 `com.apple.system.cprotect` extended attribute containing a protection class (`1..4`) and a 40-byte RFC 3394 wrapped per-file key (`persistent_key` at `+0x24`):
- **Deriving Key `0x835`:** `k_835 = AES-128-CBC_encrypt(hw_device_key, iv=0, 0x01 × 16)`.
- **Class 4 (`Dkey`):** Unwrap the 40-byte `'Dkey'` locker with `k_835` via RFC 3394 AES-128 Key Wrap to obtain the 32-byte class 4 key.
- **Classes 1–3 (`/keybags/systembag.kb`):**
  - Decrypt `_MKBPAYLOAD` inside `/keybags/systembag.kb` using the last 32 bytes of the `'BAG1'` locker (`AES-256-CBC`, `iv = _MKBIV`).
  - Derive the 32-byte passcode key from `PBKDF2-HMAC-SHA1("", SALT, 1)` tangled across `ITER` (`50,000`) rounds of hardware device-key AES-CBC encryption in 4 KiB batches (`/sys/block/nand0/keybag`).
  - For each class key with `WRAP = 3`, RFC 3394 unwrap `WPKY` (`40 -> 32 B`) with the passcode key, then decrypt with `AES-128-CBC(k_835, iv=0)`.
- **File Data Fork Decryption:** Unwrap the file's 40-byte `persistent_key` with its 32-byte class key via RFC 3394 AES-256 Key Wrap to obtain the 32-byte file key `fkey`. Decrypt each 4 KiB file block at `file_offset = b * 4096` with `AES-256-CBC(fkey, iv)`, where `iv = AES-128-ECB_encrypt(SHA1(fkey)[0..15], LFSR(file_offset))`.
