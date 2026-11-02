# Boot Sequence Improvement Plan (`S5L8947X` / `j33iap`)

Plan for booting from the internal NAND with a minimal USB loader, a raw uncompressed kernel, an uncompressed read-only root filesystem, and a GUI architecture whose logo is the compositor's first frame.

**Decision:** the stock OS no longer needs to boot. After a one-time migration, Linux owns the NAND data area and the stock flash translation format is retired from the boot path. The regions Linux still reads (key store, factory identity, hardware description) are preserved and never written, and the firmware Linux needs is copied off the stock volumes before they are erased.

Baseline measurements come from [benchmarks.md](benchmarks.md), commit history (`9328006`, `856de65`, `a2d9233`), and the current build outputs; estimates that still need on-device confirmation are marked *(est.)*. NAND geometry and protocol details are in [nand-fmi-and-crypto.md](nand-fmi-and-crypto.md); driver paths are in [drivers-and-porting.md](drivers-and-porting.md).

---

## 1. Current Boot Chain

```text
stage-1 USB loader ──DFU──▶ entry-stub.S ──copy 11.5 MB──▶ boot-loader.c
   (USB, ~1.2 MB/s)              (0x80000000)                │ PMGR → 850 MHz
                                                             │ HDMI 1080p30
                                                             │ splash (logo)
                                                             ▼
                                     zImage (XZ, 40.47 MB Image) ──▶ kernel
                                                             │ initramfs (25.05 MB cpio)
                                                             │ /init → stage2.sh
                                                             ▼
                                   FTL map rebuild, keybag, firmware copies,
                                   Mesa/Weston untar, dropbear, eth, BT, wifi
```

| Item | Value |
|---|---|
| Boot bundle (`out/boot-bundle-s5l8947x-j33iap.bin`) | `11,577,248 B` (11.04 MiB) |
| `zImage` / embedded XZ payload | `11,532,632 B` / `11,504,184 B` |
| Uncompressed kernel `Image` (with embedded initramfs) | `40,469,940 B` (38.60 MiB) |
| Embedded initramfs (uncompressed cpio) | `25,052,672 B` (23.89 MiB), 705 files |
| Initramfs share of the bundle | ≈ `7.0 MiB` of 11.04 MiB (XZ of the cpio) |
| Kernel text/data/bss without initramfs | `15,417,268 B` (14.70 MiB), 979 `=y` symbols |
| Static test/diagnostic tools in the initramfs | `8,921,176 B` (8.51 MiB; 19 binaries, ~450–515 KiB each) |
| Mesa / Weston tarballs unpacked every boot | `19.08 MiB` + `10.16 MiB` (`29.24 MiB` total) |
| Physical NAND sequential read throughput | **13.45 MB/s** (`nand0p1`, [benchmarks.md](benchmarks.md)); **9.36 MB/s** (`8.9 MiB/s`, encrypted `nand0p2`); **15.1 MB/s** bus timing ([nand-fmi-and-crypto.md](nand-fmi-and-crypto.md)); **15.4 MB/s** IRQ / **18.6 MB/s** polling on 1,024 physical 16 KiB pages (commit `9328006`) |
| FTL translation map rebuild | **4.32 s** (`4324 ms`, `575,245` blocks mapped, `1,026` tables, `6` open, `3` free superblocks; up to **6.1 s** when scanning unindexed blocks) |
| Measured end-to-end | **26.36 s** DFU→USB console; **14.06 s** from jump ([benchmarks.md](benchmarks.md)); **`+6.39 s` kernel uptime to `system ready`** after Phase 0/5 quick wins (down from `+22.68 s`) |

> **Note on the 47 MB/s figure in `progress.md`:** Commit `9328006` measured 1,024 physical single-page reads (`16 MiB`) in `1.09 s` with IRQ (`15.39 MB/s`) and `0.90 s` polling (`18.64 MB/s`). The `47 MB/s` figure from a 32 MB read on `/dev/nand0` included unallocated logical blocks (`FTL_UNMAPPED`), which `ftl_copy()` zero-fills in DRAM without touching the NAND. All read-time budgets in this plan use the physical **15–18 MB/s** rate.

---

## 2. Where the Time Goes

| # | Phase | Time | Notes |
|---|---|---|---|
| 0 | Exploit + stage-1 handoff | ~2.0–2.5 s | independent of payload size |
| 1 | **DFU upload of 11.5 MB** | **~9.5–10 s** | ~1.2 MB/s, the Full-Speed bulk ceiling (19×64 B per 1 ms frame) |
| 2 | Quiesce + jump | ~0.3 s | |
| 3 | Entry stub: cache flush + uncached 11.5 MB copy | ~0.1–0.3 s *(est.)* | `scripts/boot/entry-stub.S` copies with MMU/caches off at 212.5 MHz |
| 4 | Loader: PMGR 850 MHz, HDMI mode set, splash | ~0.1–0.4 s *(est.)* | `pmgr-init.c`, `hdmi-boot.h`, `splash.c` |
| 5 | **XZ decompress 40.47 MB** | **~4–8 s** *(est.)* | `dict=128 MiB`, ARM BCJ |
| 6 | Kernel initcalls + 1080p fbcon output | ~2–5 s *(est.)* | `loglevel=8 ignore_loglevel`, `console=tty0` scrolling 8.3 MB/frame |
| 7 | Initramfs unpack (24 MB into ramfs) | ~0.2–0.5 s *(est.)* | 705 files |
| 8 | Stage 1: mounts, `jitterseed`, forced USB re-enumeration | ~0.4–1.2 s | `initramfs/init.sh` toggles `soft_connect` and polls up to 150× |
| 9 | **FTL translation map rebuild** | **6.1 s** | measured; every root or state access waits for it |
| 10 | Stage 2 → SSH-ready | ~3–5 s *(est.)* | see below |

Stage 2 (`initramfs/stage2.sh`) brings up `/dev/ttyGS0` at line 70 (where the 26.36 s benchmark stops), and only *then* blocks on: waiting for `/dev/nand0p1` (the 6.1 s map rebuild), mounting `Data` and loading the keybag, carving/copying firmware, **unpacking the 29.2 MiB Mesa and Weston tarballs into ramfs on every boot** (≥ 2.2 s of NAND reads at 13.45 MB/s plus untar), a `sync`, and finally starting `dropbear`. Real time to SSH and GUI readiness is therefore ~35–38 s.

**Key insight:** the same bytes are paid for three times — once over USB, once through the XZ decoder, and once as a ramfs copy — and the GUI payload a fourth time in stage 2. On top of that, every access to persistent storage waits ~6.1 s for the stock-format map rebuild.

### 2.1 Everything Uncompressed on NAND (Kernel and Root)

Once the kernel and root filesystem live on internal NAND instead of a 1.2 MB/s USB link, compression is the wrong trade:

1. **Kernel (`Image`):** A trimmed uncompressed `Image` (~7–9 MiB) reads sequentially from NAND over CDMA in **~0.45–0.55 s** at 15–18 MB/s. Adding a pre-kernel decompressor adds code, memory copies, and CPU decode latency that eat most of the I/O savings.
2. **Root filesystem (uncompressed `squashfs`):** Built with `mksquashfs ... -noI -noD -noF -noX -b 131072`. Unlike an initramfs (which unpacks all 25 MiB into RAM upfront), `squashfs` over `ubiblock` pages in only the 128 KiB blocks actually touched during boot (~6–10 MiB to reach the first compositor frame, or **~0.4–0.6 s** of NAND reads) with **zero CPU decompression overhead** and no extra kernel decompressor symbols. With 256 MiB per root slot in SLC mode, an uncompressed Alpine + Mesa + Weston root (~55–70 MiB) fits with > 3.5× headroom. *(Phase 3 benchmarks uncompressed vs LZ4 on hardware to record the exact delta.)*

### 2.2 Why the Stock Flash Format Leaves the Boot Path

Keeping the stock translation layer and carving boot space out of it fails on boot time and on robustness:

| Problem | Effect |
|---|---|
| Map rebuild by scanning at every attach | **4.32–6.1 s** on the critical path of any NAND-backed root or state |
| Tight free-superblock headroom & GC pressure | Data area is 1,047 superblocks × 8 MiB = `8,376 MiB` raw vs `8,000,000,000 B` (`7,629.4 MiB`) logical `LwVM` capacity (`~747 MiB` raw-minus-logical overprovisioning). Most closed superblocks hold a mix of live and stale LBAs awaiting GC, leaving only **3 free superblocks** (`1,026` tables, `6` open, `3` free measured in `dmesg`); reserving 11 superblocks for raw boot would starve GC |
| Reserved raw superblocks look like FTL superblocks | `ftl_build()` treats any block with page 0 programmed as FTL data (`s5l8947x-ftl.c:568–600`), requiring special-case exclusion ranges in the scanner and allocator |
| Root on `Data` (`nand0p2` loop file) | Plaintext `System` (`nand0p1`) reads at **13.45 MB/s**, while encrypted `Data` (`nand0p2`, per-4 KiB AES-CBC) measures **9.36 MB/s** (`8.9 MiB/s`) and shares a mutable HFS+ volume with read-write state |
| Custom GC complexity | Commit `856de65` (2026-10-31) fixed open-superblock leaks, low-space recursion, and two retired superblocks returning status `0x45` on erase, but `hfsplus` still lacks TRIM/discard so deleted files remain live to GC |
| MLC paired pages | A power cut while programming an upper page can corrupt its paired lower page in the same block |

Replacing the data area with **SLC-mode partitions**, a **raw extent-mapped boot partition**, and **split UBI devices** (`ubi_sys` for critical-path boot and `ubi_data` attached after the first GUI frame) removes every issue above. Power-cut update safety for the root filesystem comes from **A/B volume separation + atomic manifest page append** (not from `ubiupdatevol`, which wipes the target volume before writing), while UBIFS journalling and atomic LEB changes (`UBI_IOCEBCH`) protect mutable state.

---

## 3. Target Design

Design rules, in priority order:

1. **The USB payload is a loader, not an OS image.** Target ≤ 16 KiB.
2. **Linux owns the NAND data area.** The stock format is retired by a one-time migration. The preserved regions (§4.2) are never written.
3. **Everything Linux writes is programmed in SLC mode** (row bit 20): no paired-page loss, faster page sense (`tR`), better retention and read-disturb margin, and ~4.09 GiB of usable space. *Verified on spare blocks in Phase 1 before anything depends on it.*
4. **The loader is read-only.** It never programs or erases NAND. All writes happen in Linux, through the installer or UBI.
5. **The host is the boot manager.** Every boot is tethered over USB, so the host chooses the slot and profile; the loader only validates and falls back.
6. **Everything on the critical boot path is uncompressed.** Raw kernel `Image` and uncompressed read-only `squashfs` root (`-noI -noD -noF -noX`); mutable state in UBIFS. No initramfs, no tarballs, no XZ/LZ4/zstd in the default boot path.
7. **Seamless HDMI handover into the GUI layer.** The loader sets 1080p30 and scans out a black buffer inside the reserved `fb_region`; the built-in DRM driver detects that the HDMI PHY is already locked and takes over scanout on the compositor's first commit without resetting the PHY or dropping the HDMI link.
8. **Critical-path storage is isolated from bulk data storage.** The boot UBI device (`ubi_sys`, ~1.2 GiB) holds only `rootfs_a`, `rootfs_b`, `firmware`, and `state`; the multi-gigabyte `data` partition (`ubi_data`, ~2.8 GiB) attaches asynchronously *after* the first GUI frame so even a full UBI scan after a hard power cut stays under ~0.7 s.
9. **Updates are A/B and verified.** The current USB bundle stays as the permanent recovery image and keeps full MLC/FTL support for backups and restoration.

### 3.1 Target Boot Flow

```text
host: exploit + DFU loader (~8–14 KiB, slot/profile word patched in)
   ▼
USB loader (runs from SRAM/DRAM with caches on)
   │  PMGR → 850 MHz (with W0 = 0x01220002 peripheral divider fix)
   │  HDMI mode set (1080p30, black in 0x9e400000) ─ TV locks in parallel
   │  FMI: reset, READ ID, async power state, SLC page reads (CDMA ch 5/6)
   │  manifest (copy 0 / copy 1, highest valid sequence), CRC-32 check
   │  read raw Image → 0x80008000, DTB → 0x82000000 (well above kernel _end/.bss)
   ▼
raw Linux Image (no initramfs; FMI, UBI+fastmap, ubiblock, squashfs, UBIFS, DRM built in)
   │  FMI reads factory MACs/serial from preserved SLC blocks into live DT at probe
   │  UBI attach of ubi_sys (~608 PEBs) from fastmap (~0.1 s; ≤ 0.7 s full-scan fallback)
   │  root = uncompressed squashfs on /dev/ubiblock0_0 (slot A) or /dev/ubiblock0_1 (slot B)
   ▼
PID 1 (critical path):
   │  mount /dev, /proc, /sys, /run; start watchdog
   │  mount state (UBIFS) + /etc, /var overlays + firmware (/lib/firmware)
   │  start compositor / GUI splash → DRM flips RGBOUT_BASE without PHY reset → FIRST FRAME
   ▼
Background (parallel, post-first-frame):
   │  ubiattach ubi_data (/data)
   │  load Wi-Fi / Bluetooth / V4L2 modules; start dropbear, network, NTP
```

### 3.2 Display Handover and GUI Layer Architecture

The pre-kernel splash exists today partly to hide how long the TV takes to lock onto a new HDMI signal (often 1–3 s on a 4K TV *(est., measure)*). If the first mode set happens in userspace, that lock time adds directly to time-to-logo. To make the visual transition seamless and robust for the GUI layer:

1. **Stage-1 Black Scanout (`BOOT_HDMI=1`):** The loader programs `rgbout`, `out`, and `hdmi` for `1920x1080@30` (matching commit `a2d9233`) and zeros the `1920×1080×4` (`8.29 MB`) scanout region at `0x9e400000`. No polygon splash code or logo assets are linked into the production loader.
2. **Keep `fb_region` Reserved (`no-map`), Drop `simple-framebuffer` and `fbcon`:**
   - Because `rgbout` DMA continuously reads `0x9e400000..0x9e7e8fff` while the kernel boots, **`fb_region: framebuffer@9e400000` (`reg = <0x9e400000 0x800000>; no-map;`) MUST remain in `reserved-memory`** whenever `BOOT_HDMI=1` so the kernel page allocator never overwrites live scanout memory.
   - Only the `framebuffer0` (`simple-framebuffer`) DT node and `CONFIG_FRAMEBUFFER_CONSOLE` / `drm_fbdev_dma_setup()` are disabled so the kernel neither scrolls text at 8.3 MB/line nor performs an fbdev modeset at probe time.
3. **Skip-Reset Handover in `s5l8947x_drm.c` (Phase 5 Deliverable):**
   - *Current state:* `s5l_pipe_enable()` (`drivers/gpu/drm/s5l8947x/s5l8947x_drm.c:1073`) unconditionally calls `s5l_hdmi_stop()`, pulses `OUT_RESET`, and calls `s5l_hdmi_start()` (pulsing `HDMI_PHY_RSTOUT` and waiting 33 ms), which drops the HDMI clock and forces the TV to re-lock.
   - *Phase 5 change:* At `s5l_probe()` / first atomic enable, check if `HDMI_PHY_STATUS & 1` is already locked and the active timing registers match the requested mode (`1920x1080@30`). When they match, **skip `s5l_hdmi_stop()`, `OUT_RESET`, and `s5l_hdmi_start()`**, adopt the running pipe state, and simply point `RGBOUT_BASE` at the compositor's first DRM dumb/GBM buffer on the next vblank.
4. **Two-Tier GUI Startup (Fast Logo → Full Compositor):**
   - *Tier 1 (Immediate DRM Master / Splash):* A tiny DRM/KMS client (or Weston's first frame) presents the boot logo into a DRM buffer as soon as `/dev/dri/card0` is ready, before SGX microkernel init or shader compilation.
   - *Tier 2 (Accelerated Desktop / App Layer):* Weston (or a dedicated KMS/EGL shell) initializes GBM/EGL on `/dev/dri/renderD128` (`apple_sgx`, which lazily boots the SGX543 microkernel on first context creation using `/lib/firmware/apple/sgx543.fw` from the pre-carved `firmware` volume) and cross-fades from the logo into the interactive UI.
   - *Watchdog & Fail-Safe Visual Recovery:* If the compositor crashes or fails to post a frame within 5 s, the supervisor falls back to a small KMS diagnostic status screen showing IP/USB status on `/dev/dri/card0`.

### 3.3 Loader Minimalism Ladder

**Prerequisite bug fix (`scripts/boot/pmgr-init.c`):** The current pre-kernel loader writes `PMGR_PERF_STATE2_W0 = 0x01220001U` (peripheral divider `1`), which doubles the UART baud clock when switching to 850 MHz (`drivers/cpufreq/s5l8947x-cpufreq.c:56–60` uses `0x01220002U` to keep divider `2`). Phase 0 fixes `pmgr-init.c` to write `0x01220002U` before any UART timestamps or console output are used.

| Module | Size (est.) | Boot-time effect | Decision |
|---|---|---|---|
| Entry stub (mask IRQ/FIQ, self-relocate with caches on) | ~0.5 KiB | < 1 ms (14 KiB vs 11.5 MB) | keep |
| PMGR CPU boost 212.5 → 850 MHz (`W0 = 0x01220002U` fix) | ~0.8 KiB | saves seconds of kernel init | keep |
| HDMI mode set, black scanout (`memset` `0x9e400000`) | ~3–4 KiB | ~0.05 s; hides the TV lock time | keep (`BOOT_HDMI=1`) |
| Splash/logo polygon drawing (`splash.o` is `17.2 KiB` today) | ~17 KiB | ~0.1–0.2 s | **drop** (`BOOT_SPLASH=1` for debug) |
| FMI init + READ ID + async power feature + SLC page read | ~2–3 KiB | required | keep |
| Manifest parse, slot select from host word, fallback | ~1 KiB | ~0 | keep |
| Page read: PIO drain | ~1.5 KiB | ~0.7–0.9 s for 8 MiB *(est.)* | Phase 2 step 1 |
| Page read: CDMA data/meta channels 5/6 | ~2–3 KiB | **~0.45–0.55 s** for 8 MiB (at 15–18 MB/s) | Phase 2 step 2 |
| CRC-32 per extent | ~0.5 KiB | ~0.03–0.05 s | keep |
| SHA-256 | ~2–3 KiB | ~0.3 s in software | drop — not a security boundary |
| FDT fixup of `bootargs` | ~2 KiB | ~ms | drop — each slot carries a prod and a debug DTB |
| Boot-attempt log / NAND writes | — | — | **drop** — the loader never writes NAND |

Without `splash.o`, `boot-start.o + pmgr-init.o + boot-loader.o` is already **2.7 KiB** today; adding FMI read, manifest, and CRC-32 yields **~8–12 KiB** total (~10 ms over Full-Speed USB).

### 3.4 Host-Side Boot Control

The host script that runs the exploit and uploads the loader patches one parameter word into the loader image before upload:

| Field | Values |
|---|---|
| slot | `auto` (manifest's active slot), `a`, `b`, `golden` |
| profile | `prod` or `debug` DTB of the chosen slot |
| flags | `BOOT_HDMI`, verbose UART output |

Rollback is a host decision: if the USB console (or SSH) does not appear within a timeout, the host retries with the previous slot, then `golden`, then the USB recovery bundle. No boot counter is ever written to NAND.

---

## 4. NAND Layout and Driver Architecture

### 4.1 Geometry (from [nand-fmi-and-crypto.md](nand-fmi-and-crypto.md))

- 2 CAUs × 1,064 blocks/CAU, 16,448 B/page (16 KiB data + 64 B metadata)
- MLC: 256 pages/block (4 MiB per block); **SLC: 128 pages/block (2 MiB per block)**
- `row = (slc << 20) | (cau << 19) | (block << 8) | page`
- Block `0` of each CAU: boot pages (partition descriptor) and the stock first-stage loader
- Blocks `1048..1063` of each CAU (`KS_RESERVED_BLOCKS = 16`): SLC reserved region (key store, `SCfg` factory identity, hardware description tree, stock settings)
- **Data area: blocks `1..1047` × 2 CAUs = 2,094 blocks**, ≈ **4.09 GiB (`4,188 MiB`) in SLC mode** (8.18 GiB in MLC mode)

### 4.2 Preserved Regions and Cold-Boot DFU Entry

| Region | Why it stays | Linux use |
|---|---|---|
| Block `0`, both CAUs | Boot pages locate the reserved SLC partitions; first-stage loader header determines cold-boot behavior | Read-only during normal operation |
| Blocks `1048..1063`, both CAUs | Volume/class keys (`Face`), factory identity (`SCfg`: serial, Ethernet/Wi-Fi/Bluetooth MACs), hardware description tree | Read-only: `s5l8947x-fmi` reads `SCfg` at probe; keys used only when mounting backups via `s5l8947x-ftl.ko` |

**Reconciling cold-boot DFU entry with Block 0 preservation:**
- If DFU entry uses **external MCU / IR reset (Method B)** or manual recovery entry, Block `0` is never modified at any time.
- If automatic cold-boot DFU entry uses **first-stage header invalidation (Method A)**, that is a single, explicit 1-page operation on Block `0` with a verified backup image (`research/nand/bootarea_ce0_r0-127.bin`), and is completely separate from the Linux MTD/installer path. Under no circumstances may `s5l8947x-fmi` MTD partitions or `nandboot-install` address Block `0` or Blocks `1048..1063`.

### 4.3 Dual-Mode `s5l8947x-fmi` Driver and Contiguous Interleaved MTD Mapping

Currently, `s5l8947x-fmi.c` exposes a single CAU-major MTD (`CAU 0 blocks 0..1063`, then `CAU 1 blocks 0..1063`) with `mtd->type = MTD_MLCNANDFLASH`, `erasesize = 4 MiB`, and `mtd->numeraseregions = 2`. That creates three incompatibilities if changed globally:
1. **Non-contiguous partitions:** CAU 0's preserved blocks (`1048..1063`) and CAU 1's Block `0` sit right in the middle of the linear MTD offset space between CAU 0 block `1047` and CAU 1 block `1`.
2. **UBI rejection:** Mainline UBI (`drivers/mtd/ubi/build.c:605, 898`) refuses to attach if `mtd->numeraseregions != 0` or if `mtd->type == MTD_MLCNANDFLASH` without `MTD_SLC_ON_MLC_EMULATION`.
3. **FTL / restore compatibility:** `ftl_usable()` (`drivers/mtd/s5l8947x-ftl.c:2381`) requires `mtd->type == MTD_MLCNANDFLASH`, `erasesize = 4 MiB`, and `numeraseregions = 2`.

**Solution — Explicit `slc_mode` in `s5l8947x-fmi.c`:**

Add a module parameter / DT property (`s5l8947x_fmi.slc_mode=0|1` / `apple,slc-data-mode`):

1. **Mode 0 — Raw Full-Chip MLC Mode (`slc_mode=0`, used by the USB recovery bundle):**
   - Exposes the entire chip (`2 CAUs × 1,064 blocks × 256 pages`, `MTD_MLCNANDFLASH`, `erasesize = 4 MiB`, `numeraseregions = 2`), identical to today.
   - `s5l8947x-ftl.ko` loads cleanly to mount stock `System`/`Data` volumes or verify restored backups.
   - Extend `s5l_fmi_mtd_erase()` to honor an SLC flag (or debugfs/ioctl helper) so Phase 1 testing and the Phase 4 migration tool can erase and program SLC blocks before rebooting into Mode 1.
2. **Mode 1 — Interleaved SLC Data-Area Mode (`slc_mode=1`, used by production NAND-boot DTBs):**
   - Decouples `fmi->pages_per_eb = 128` from `fmi->page_bits = 8` (`s5l_fmi_identify()` keeps `fmi->pages = 256` for hardware validation, while SLC mode uses `128` pages/block and keeps `block << 8` in `s5l_fmi_row()`).
   - Maps **only the 2,094 data-area blocks (`1..1047` of each CAU)** in **block-major (CAU-interleaved) order**:
     $$\text{eb} = \lfloor\text{page} / 128\rfloor \in [0, 2093], \quad \text{cau} = \text{eb} \bmod 2, \quad \text{phys\_block} = 1 + \lfloor\text{eb} / 2\rfloor, \quad \text{pg} = \text{page} \bmod 128$$
     $$\text{row} = \text{BIT}(20) \mid (\text{cau} \ll 19) \mid (\text{phys\_block} \ll 8) \mid \text{pg}$$
   - Sets `mtd->type = MTD_NANDFLASH` and `mtd->flags = MTD_CAP_NANDFLASH` (not `MTD_SLC_ON_MLC_EMULATION`, which is for software paired-page emulation in `mtdcore.c` whereas PPN 1.5 performs hardware SLC via `BIT(20)`), `mtd->erasesize = 2 MiB`, `mtd->size = 2094 * 2 MiB` (`4,188 MiB`), and `mtd->numeraseregions = 0`.
   - Passes `slc = true` to `s5l_fmi_read_one()`, `s5l_fmi_write_one()`, and `s5l_fmi_erase_one()`.
   - Adds a 1-page bounce cache (`fmi->cached_row`) so back-to-back partial page reads (such as UBI reading the 64 B EC header at offset 0 of page 0) do not trigger duplicate DMA reads.
   - Synchronously reads `SCfg` (serial, model, `emac`, `wmac`, `bmac`) from the preserved SLC region (`blocks 1048..1063`) during `s5l_fmi_probe()` and patches the live DT (`local-mac-address` / `local-bd-address`) *before* `eth0`, `brcmfmac`, or `hci_uart_bcm` probe.

### 4.4 Contiguous Partitions (`boot`, `ubi_sys`, `ubi_data`)

With the interleaved SLC data-area mapping (`eb = 0..2093`), standard DT `fixed-partitions` map cleanly to contiguous physical block ranges across both CAUs, and **splitting UBI into `ubi_sys` and `ubi_data` keeps cold-boot attach fast even after an unclean power-off**:

| Partition | Interleaved EBs | Physical Blocks | Size | Role |
|---|---|---|---|---|
| `boot` | `0..31` (32 EBs) | `1..16` of CAU 0 & CAU 1 | `64 MiB` | Raw: 2 manifest blocks (`eb 0..1`), kernel slots A / B / golden (`6 EBs = 12 MiB` each), `12 EBs` spare pool |
| `ubi_sys` | `32..639` (608 EBs) | `17..320` of CAU 0 & CAU 1 | `1,216 MiB` | **Critical-path UBI (`ubi0`):** attached at boot (`rootfs_a`, `rootfs_b`, `firmware`, `state`) |
| `ubi_data` | `640..2093` (1,454 EBs) | `321..1047` of CAU 0 & CAU 1 | `2,908 MiB` | **Bulk-storage UBI (`ubi1`):** attached in the background *after* the first compositor frame (`data`) |

**Why splitting `ubi_sys` (608 PEBs) and `ubi_data` (1,454 PEBs) matters:**
- UBI fastmap attaches in **~0.05–0.15 s**, but fastmap is invalidated whenever a power cut happens while an active UBIFS volume has borrowed PEBs from the fastmap pool.
- A full scan of all 2,062 PEBs reads EC (page 0) and VID (page 1) headers (`2,062–4,124` page reads × `~0.9 ms` = **~2.0–3.5 s**).
- By isolating `ubi_sys` to **608 PEBs** (where `rootfs_a`, `rootfs_b`, and `firmware` are static/read-only and `state` sees only rare config writes), `ubi_sys`'s fastmap is almost always clean, and even a worst-case full scan after a hard power cut during a `/etc` or `/var` write scans at most 608 PEBs (**~0.5–0.7 s**). Meanwhile, heavy writes on `ubi_data` (shader caches, media, logs) never affect boot time to the GUI.

### 4.5 Boot Partition Format and PPN Bad-Block Handling

- **Manifest**: two copies, in `eb 0` (CAU 0 block 1) and `eb 1` (CAU 1 block 1). Each copy is one 16 KiB page: magic, format version, sequence number, active slot, and per slot a list of extents (`cau, block, first page, page count, CRC-32`) for the `Image` and both DTBs, plus a header CRC. Each new manifest is written to the next free page of its block; when a block fills, the installer erases the *other* copy's block first and never both at once. The loader reads both blocks and takes the highest sequence with a valid CRC.
- **Slots**: each slot owns 6 blocks (`12 MiB`, for a ≤ 9 MiB kernel + DTBs + spare). Extents name physical `(cau, block)` explicitly, so any retired block is skipped by the installer and the loader needs zero bad-block logic.
- **Bad-block handling (using PPN hardware status, no custom page-0 OOB BBT):**
  - The managed PPN 1.5 package tracks and retires its own bad blocks internally, returning status `0x45` (`MN_ST_FAIL` / uncorrectable) when a retired block is erased or read (2 blocks on this board already return `0x45`, per commit `856de65`). There is no page-0 OOB bad-block table, and inventing one would both double page-0 reads during UBI scan and fail when a block dies after page 0 was already programmed.
  - However, UBI requires `ubi->bad_allowed = 1` (`mtd->_block_isbad != NULL` and `mtd->_block_markbad != NULL`) to retire a block that fails erase with `-EIO`; otherwise `__erase_worker()` (`drivers/mtd/ubi/wl.c:1177–1197`) switches UBI into read-only mode (`ubi_ro_mode`).
  - Therefore, `s5l8947x-fmi` keeps an in-memory bitmap of blocks that returned `0x45` / `-EIO`: `_block_isbad()` checks the bitmap (no NAND read required), and `_block_markbad()` sets the bit in the bitmap and returns `0` so UBI cleanly retires those blocks using its `beb_rsvd_pebs` pool.
- **Read refresh**: the loader ignores `MN_ST_REFRESH` (`0x42`); Linux checks each boot slot's read status after boot and re-writes any slot whose pages report refresh. On UBI partitions, `s5l8947x-fmi` reports `MN_ST_REFRESH` as `1` bitflip (`-EUCLEAN` with `bitflip_threshold = 1`), triggering UBI's background scrubber automatically.

### 4.6 UBI Volumes (Pinned Volume IDs)

Volume IDs are pinned explicitly at `ubimkvol` time (`-n <vol_id>`) because mainline `ubiblock` names its block device `/dev/ubiblock<ubi_num>_<vol_id>` (`drivers/mtd/ubi/block.c:418`):

| UBI Device | `vol_id` | Name | Type | Block Device | Size | Contents |
|---|---|---|---|---|---|---|
| `ubi0` (`ubi_sys`) | `0` | `rootfs_a` | static | `/dev/ubiblock0_0` | `256 MiB` | Uncompressed `squashfs` (`-noI -noD -noF -noX`), paired with kernel slot A |
| `ubi0` (`ubi_sys`) | `1` | `rootfs_b` | static | `/dev/ubiblock0_1` | `256 MiB` | Uncompressed `squashfs`, paired with kernel slot B |
| `ubi0` (`ubi_sys`) | `2` | `firmware` | static | `/dev/ubiblock0_2` | `64 MiB` | Uncompressed `squashfs`: pre-carved firmware (`sgx543.fw`, `vxd390.fw`, `iop.fw`, `gl-event.pds`, `brcm/*`) |
| `ubi0` (`ubi_sys`) | `3` | `state` | dynamic | — (`ubi0:state`) | `512 MiB` | UBIFS: SSH host keys, Wi-Fi/BT pairings, `/etc` and `/var` overlay upper |
| `ubi1` (`ubi_data`) | `0` | `data` | dynamic | — (`ubi1:data`) | `~2.7 GiB` | UBIFS (autoresize): user/app data, Mesa shader cache, diagnostic tools |

- **Slot A DTB `bootargs`:** `... ubi.mtd=ubi_sys ubi.block=0,rootfs_a ubi.block=0,firmware root=/dev/ubiblock0_0 rootfstype=squashfs ro rootwait`
- **Slot B DTB `bootargs`:** `... ubi.mtd=ubi_sys ubi.block=0,rootfs_b ubi.block=0,firmware root=/dev/ubiblock0_1 rootfstype=squashfs ro rootwait`

**Fallback if SLC mode cannot be used on data-area blocks:** keep `s5l8947x-fmi` in MLC mode (`4 MiB` blocks) for read-only raw `squashfs` partitions (`rootfs_a`, `rootfs_b`, `firmware` mapped directly via `mtdblock` or `dm-linear` without UBI, since UBI refuses MLC without SLC emulation), and place mutable `state` in a small SLC partition. Phase 1 resolves this before any migration code runs.

---

## 5. One-Time Migration and Verified Restore Path

The migration runs from the USB recovery bundle (everything in RAM, `s5l8947x-fmi` in `slc_mode=0`).

1. **Back up.** Keep the verified 2026-10-30 raw backup (`research/nand/backup/`, verified by `MANIFEST.sha256`). Take a fresh logical archive of `System` (`nand0p1`) and `Data` (`nand0p2`) over Ethernet (`eth0`), since `Data` has been written since 2026-10-30.
2. **Build and test a real restore path before erasing.**
   - Today `research/nand/README.md` notes there is no automated restore script, and streaming 8.92 GB over Full-Speed USB (1.2 MB/s) would take ~2 hours.
   - Build a `nand-restore` tool that streams raw backup chunks over **100 Mbps Ethernet (`eth0`, ~11 MB/s = ~13 minutes for the full chip)** and programs MLC pages + 64 B OOB.
   - Validate `nand-restore` in Phase 1 on a 4-superblock (`32 MiB`) test window (including both free and closed TOC superblocks): erase, corrupt, restore from the backup chunks over `eth0`, reload `s5l8947x-ftl.ko`, and verify that the FTL translation map, `nand0p1`, and `nand0p2` mount and pass `hwtest nand` identically. If any check fails, stop.
3. **Extract what Linux needs** into a migration bundle on the host:
   - pre-carved firmware files (`/lib/firmware/brcm/brcmfmac4334.bin`, `brcmfmac4334.txt`, `BCM4334B0.hcd`, `/lib/firmware/apple/sgx543.fw`, `vxd390.fw`, `iop.fw`, `gl-event.pds`) plus their raw source files from `System` for archival;
   - Linux persistent state from `Data`: SSH host keys, Bluetooth pairings, Wi-Fi `wpa_supplicant.conf`;
   - a copy of the `SCfg` factory identity and hardware description tree, for reference only (the originals remain untouched in blocks `1048..1063`).
4. **Switch `s5l8947x-fmi` to `slc_mode=1` and erase the data area** (`blocks 1..1047` on both CAUs) in SLC mode; any block returning `0x45` is recorded in the bad-block bitmap.
5. **Create the layout**: write kernel slot A, slot B, golden, and the two manifest blocks in `boot`; `ubiformat` `ubi_sys` and `ubi_data` with fastmap; create the pinned volumes (`-n 0..3` on `ubi0`, `-n 0` on `ubi1`); write `rootfs_a`, `rootfs_b`, `firmware`, and `state`.
6. **Verify** every volume and slot by reading it back and comparing SHA-256 hashes, then reboot into Slot A via the minimal USB loader.

After migration, `s5l8947x-ftl.ko` stays built in the USB recovery bundle for reading backups or running `nand-restore`, and is omitted from the production NAND kernel.

---

## 6. Kernel, Userspace and GUI

### 6.1 Kernel

- **Raw `Image` and safe DTB placement:** Build `arch/arm/boot/Image`, not `zImage`. The loader places `Image` at `0x80008000` and places the selected DTB at **`0x82000000` (`+32 MiB`)** — well above the kernel's `_end` (including its uninitialized `.bss` segment, which `arch/arm/kernel/head.S` zeroes in-place before reading the DTB) — then jumps with `r0 = 0`, `r1 = 0xffffffff`, `r2 = 0x82000000`.
- **No `CONFIG_INITRAMFS_SOURCE`** in the production NAND kernel.
- **Explicit Kernel Config Additions (`config/s5l8947x.config`):**
  - Already `=y`: `CONFIG_MTD_UBI=y`, `CONFIG_UBIFS_FS=y`, `CONFIG_SQUASHFS=y`, `CONFIG_OVERLAY_FS=y`, `CONFIG_DRM_S5L8947X=y`.
  - **Add:** `CONFIG_MTD_UBI_FASTMAP=y`, `CONFIG_MTD_UBI_BLOCK=y`.
  - **Disable in prod:** `CONFIG_FRAMEBUFFER_CONSOLE=n`, `CONFIG_DRM_FBDEV_EMULATION=n`.
  - **Modules (`=m`), loaded after first GUI frame:** `brcmfmac`, `hci_uart`, `btbcm`, `s5l8947x-m2sc`, `s5l8947x-jpeg`, `s5l8947x-vxd`, `s5l8947x-iop`; `apple_sgx` built-in (`=y`) so `/dev/dri/renderD128` is immediately present for the compositor (its microkernel boots lazily on first EGL context creation).
- **Purpose-built defconfig** instead of `multi_v7_defconfig` + trim: today `15.42 MB` with `979` `=y` symbols; target **7–9 MiB** (`~0.45–0.55 s` DMA read from NAND).
- **Production `bootargs` (Slot A prod DTB):**
  ```text
  quiet loglevel=4 console=ttyGS0 ubi.mtd=ubi_sys ubi.block=0,rootfs_a ubi.block=0,firmware root=/dev/ubiblock0_0 rootfstype=squashfs ro rootwait lpj=<measured>
  ```
  - Includes **`rootwait`** so VFS waits if `late_initcall(ubi_init_attach)` / `ubiblock` creation finishes asynchronously.
  - Uses **`console=ttyGS0`** (not `ttySAC0`, which is `uart0` at `0x32500000`, the system MCU bootstrap link).
  - **Debug DTB `bootargs`:** adds `earlycon=s3c6400,0x32500000 console=ttySAC3,115200n8 ignore_loglevel initcall_debug` (`ttySAC3` is `uart3` at `0x32800000`, the dedicated boot-console test pad).

### 6.2 Userspace and Robust GUI Startup

- **Root (`rootfs_a` / `rootfs_b`):** Uncompressed `squashfs` (`mksquashfs ... -noI -noD -noF -noX -b 131072`) with Mesa (`pvr` / `kmsro` GBM/EGL) and Weston pre-installed at build time. Zero boot-time untarring.
- **Pre-carved `/lib/firmware` (`firmware` volume):** Mounted read-only from `/dev/ubiblock0_2` at `/lib/firmware`. Contains `apple/sgx543.fw`, `apple/gl-event.pds`, `apple/vxd390.fw`, `apple/iop.fw`, and `brcm/*`. No boot ever mounts the stock volumes or runs a carver binary.
- **Mutable `/etc` and `/var`:** `overlayfs` backed by `/mnt/state/etc` and `/mnt/state/var` on `ubi0:state` (`UBIFS`); `/tmp` and `/run` on `tmpfs`.
- **Two-Stage PID 1 Critical Path (GUI First, Everything Else Parallel):**
  1. **Synchronous Critical Path (< 0.3 s):**
     - Mount `/proc`, `/sys`, `/dev` (`devtmpfs`), `/run` (`tmpfs`).
     - Open `/dev/watchdog` immediately (inheriting the hardware watchdog without gap).
     - Mount `ubi0:state` at `/mnt/state` (`-t ubifs -o rw,noatime`), mount `/etc` and `/var` overlays, and mount `/dev/ubiblock0_2` at `/lib/firmware` (`-o ro`).
     - Launch the **GUI service supervisor** immediately:
       - Displays the first frame (boot logo) on `/dev/dri/card0` (taking over smoothly from the Stage-1 black scanout without an HDMI mode reset) and brings up the accelerated Weston / EGL shell using `/dev/dri/renderD128` and `/dev/input/event0` (the IR remote MCU, which is built-in and ready before userspace).
  2. **Asynchronous Post-Frame Services (in parallel with GUI rendering):**
     - Run `ubiattach -m <ubi_data>` and mount `ubi1:data` at `/data` (pointing `MESA_SHADER_CACHE_DIR=/data/cache/mesa`).
     - Bring up `eth0`, `usb0`, `dropbear`, `ntpd`, and load wireless/media kernel modules (`brcmfmac`, `hci_uart`, V4L2).

---

## 7. Phased Plan

### Phase 0 — Fix Loader Clock Bug, Measure Baseline, Quick Wins (1–2 days, no NAND layout changes)

- **Fix `scripts/boot/pmgr-init.c:61`:** change `PMGR_PERF_STATE2_W0` from `0x01220001U` to `0x01220002U` so the 850 MHz boost preserves the peripheral clock divider (`2`) and UART baud rates stay accurate.
- **Record missing baseline numbers on hardware:**
  - `/proc/uptime` markers across `init.sh` and `stage2.sh`;
  - exact `dmesg` `translation map: ... free superblocks, ... ms` line;
  - sequential read throughput on encrypted `/dev/nand0p2` (`Data`) vs plaintext `/dev/nand0p1` (`System`, `13.45 MB/s`);
  - TV HDMI lock time (from Stage-1 mode set to visible picture);
  - kernel `lpj=` value from `dmesg` (`Calibrating delay loop...`).
- **Quick wins on the current USB path:**
  - unpack `mesa-sgx-install.tar` and `weston-armv7.tar` once into a persistent directory on `Data` (`/mnt/data/linux/opt-overlay`) instead of untarring 29.2 MiB into RAM on every boot;
  - move the 8.51 MiB of static test/carver binaries out of the initramfs onto `Data` after first provisioning;
  - keep `fb_region` (`no-map`) reserved, drop `framebuffer0`/`console=tty0` and `ignore_loglevel`.
- **Acceptance:** `pmgr-init.c` UART clock verified; all baseline numbers recorded in `docs/benchmarks.md`; USB boot time reduced to ~14–16 s with zero NAND layout changes.

### Phase 1 — SLC Mode, UBI Split, and Full Network Restore Validation (2–4 days, non-destructive)

Use 4 currently free FTL superblocks (`32 MiB` raw MLC / `16 MiB` SLC) with `s5l8947x-ftl` unloaded:

- Implement `slc_mode` in `s5l8947x-fmi.c` (decoupled `pages_per_eb = 128`, `page_bits = 8`, SLC erase/program/read, 1-page bounce cache `cached_row`, and in-RAM `0x45` bad-block bitmap for `_block_isbad`/`_block_markbad`).
- Test SLC erase, program, and read across all 128 pages of the test blocks; verify read status (`0x40` OK) and retention across cold power cycles.
- Attach a test UBI device (`CONFIG_MTD_UBI_FASTMAP=y`, `CONFIG_MTD_UBI_BLOCK=y`) over those blocks; measure fastmap attach vs full-scan attach per PEB; test UBIFS under simulated power cuts; verify that injecting `-EIO` on erase retires a PEB cleanly via `_block_markbad` without triggering `ubi_ro_mode`.
- Build `nand-restore` (streaming backup chunks over `eth0` at ~11 MB/s) and test restoring the 4 superblocks back to their exact MLC data + 64 B OOB from `research/nand/backup/`, then reload `s5l8947x-ftl.ko` and verify `hwtest nand` passes 16/16.
- **Acceptance:** SLC mode proven on data-area blocks; UBI fastmap, full-scan, and bad-block retirement verified; `nand-restore` over `eth0` restores stock MLC blocks bit-for-bit and `hwtest nand` passes.

### Phase 2 — Minimal NAND Loader (3–5 days)

- Create `scripts/nandboot/`: reuse `boot-start.S`, `entry-stub.S` (with caches enabled before relocation), fixed `pmgr-init.c`, and the mode-set + black-memset path of `hdmi-boot.h`; add `fmi-read.c` (reset, `MN_FEATURE_POWER = MN_POWER_ASYNC`, SLC page read), `manifest.c`, and `crc32.c`.
- Test loading a raw `Image` (`0x80008000`) + DTB (`0x82000000`) from the Phase 1 test blocks: PIO first, then CDMA channels 5/6.
- Add host-side parameter word patching (slot `auto`/`a`/`b`/`golden`, profile `prod`/`debug`) and timeout fallback.
- **Acceptance:** jump → first kernel message under **0.9 s (PIO)** or **0.6 s (CDMA)** for an 8 MiB `Image`; a corrupted slot automatically falls back to the secondary slot; raw `Image` boots cleanly with `r2 = 0x82000000`.

### Phase 3 — Uncompressed SquashFS Root, Pre-Carved Firmware, and Fast Init (2–3 days)

- Move `ftl_read_syscfg()` into `s5l_fmi_probe()` so MAC addresses and serial are populated in the live DT before network/BT drivers probe.
- Update `scripts/build-alpine-rootfs.sh` to build an uncompressed `squashfs` (`-noI -noD -noF -noX -b 131072`) with Mesa and Weston pre-installed and no static test/carver bloat (benchmark against `-comp lz4 -Xhc` to record both numbers).
- Build the `firmware` `squashfs` volume (`64 MiB`) containing all pre-carved firmware blobs.
- Implement the two-stage PID 1 init (synchronous critical path to compositor + asynchronous `ubi_data` attach and network/wireless bring-up).
- **Acceptance:** boots to Weston and SSH from a test UBI volume with no initramfs, no boot-time untar, and no stock volume access.

### Phase 4 — One-Time Migration (1–2 days)

- Build `nand-migrate` implementing §5, run from the USB recovery bundle.
- **Acceptance:** NAND-only boot of Slot A (`ubiblock0_0`) and Slot B (`ubiblock0_1`); SSH host keys, Bluetooth pairings, and Wi-Fi config preserved in `ubi0:state`; MACs and serial populated at FMI probe; all firmware loads from `/dev/ubiblock0_2`; `ubi1:data` mounts in the background.

### Phase 5 — Seamless DRM Handover and GUI Polish (2–3 days)

- Update `s5l8947x_drm.c` (`s5l_pipe_enable`) to detect when the HDMI PHY is already locked at `1920x1080@30` from Stage-1 and take over scanout by flipping `RGBOUT_BASE` on the compositor's first commit without calling `s5l_hdmi_stop()`, `OUT_RESET`, or `s5l_hdmi_start()`.
- Drop `drm_fbdev_dma_setup()` and `CONFIG_FRAMEBUFFER_CONSOLE` in production builds so the screen stays rock-solid black until the compositor's first frame (the logo) appears.
- Add compositor watchdog fallback (renders a diagnostic status screen on `/dev/dri/card0` if the GUI exits unexpectedly).
- **Acceptance:** black → compositor logo → accelerated UI with zero HDMI link drop, zero TV re-lock banner, and zero console flash; time-to-first-frame recorded.

### Phase 6 — A/B Updates and Recovery (2 days)

- Build `nandboot-install`: writes the inactive kernel slot in `boot` and the inactive root volume (`rootfs_b` or `rootfs_a` via `ubiupdatevol`), reads back and verifies SHA-256 hashes, then appends a new manifest page switching the active slot.
- Golden slot updated only by an explicit `--golden` flag.
- **Acceptance:** power cut at any point during `nandboot-install` leaves the active slot and its paired `rootfs` untouched and bootable; host script falls back automatically if a newly activated slot fails to reach readiness.

Total: roughly **2.5–3 focused weeks**; Phase 0 lands immediate boot-time and clock fixes without touching the NAND layout.

---

## 8. Build and Tooling Changes

| Path | Change |
|---|---|
| `scripts/boot/pmgr-init.c` | Fix `PMGR_PERF_STATE2_W0` from `0x01220001U` to `0x01220002U` (keep peripheral divider `2` at 850 MHz) |
| `scripts/build-kernel.sh` | Emit `out/Image` and per-slot prod/debug DTBs (`slot-a`, `slot-b`); keep `zImage` + initramfs for the USB recovery bundle |
| `scripts/build-bootslot.sh` (new) | Pack slot image: `Image` + prod/debug DTBs + extent CRCs |
| `scripts/boot/` | Remains the self-contained USB recovery path; `splash.c` only under `BOOT_SPLASH=1` |
| `scripts/nandboot/` (new) | Minimal ~8–12 KiB loader: stub, PMGR, HDMI black scanout, FMI SLC CDMA read, manifest, CRC-32, jump (`r2 = 0x82000000`) |
| `scripts/velliangiri.sh` | Support NAND loader upload with patched parameter word, slot/profile selection, and timeout fallback to recovery bundle |
| `scripts/build-alpine-rootfs.sh` | Emit uncompressed `squashfs` root (`-noI -noD -noF -noX`) with Mesa and Weston installed at build time; no tarballs |
| `scripts/tools/nand-restore`, `nand-migrate`, `nandboot-install` (new) | Network-streamed full MLC restore (`eth0`), §5 migration, and verified A/B slot + volume installer |
| `drivers/mtd/devices/s5l8947x-fmi.c` | Dual mode (`slc_mode=0` full-chip MLC for FTL/recovery; `slc_mode=1` CAU-interleaved SLC over blocks `1..1047` with `numeraseregions = 0`), 1-page bounce cache, `0x45` bad-block bitmap (`_block_isbad`/`_block_markbad`), refresh → `-EUCLEAN`, synchronous `SCfg` MAC/serial DT patch at probe |
| `drivers/mtd/s5l8947x-ftl.c` | Built into USB recovery bundle (`slc_mode=0`) for backups and `nand-restore`; omitted from production NAND kernel |
| `drivers/gpu/drm/s5l8947x/s5l8947x_drm.c` | Skip `s5l_hdmi_stop`/`OUT_RESET`/`s5l_hdmi_start` when PHY is already locked at target mode; flip `RGBOUT_BASE` seamlessly on first commit |
| `arch/arm/boot/dts/apple/s5l8947x-j33iap.dts` | Keep `fb_region` (`no-map` at `0x9e400000`); disable `framebuffer0` unless `BOOT_SPLASH=1`; add `boot`, `ubi_sys`, `ubi_data` partitions and per-slot prod/debug `bootargs` (`rootwait`, `ttyGS0` / `ttySAC3`, `/dev/ubiblock0_0` vs `/dev/ubiblock0_1`) |

---

## 9. Expected Budget

| Phase | Today (Measured / Est.) | Target (NAND Boot) |
|---|---|---|
| Exploit + stage-1 handoff | ~2.2 s | ~2.2 s (fixed; < 1.5 s with dedicated MCU injector) |
| USB payload upload (1.2 MB/s) | ~9.5 s (11.58 MB) | **~0.01 s** (8–12 KiB loader) |
| Loader: PMGR + HDMI 1080p30 black scanout | ~0.1–0.4 s | **~0.05 s** (no polygon splash) |
| Loader: read ~8 MiB `Image` + DTB from NAND | — | **~0.45–0.55 s** (CDMA at 15–18 MB/s) / ~0.8 s (PIO) |
| XZ decompress | ~4–8 s | **0 s** (raw `Image`) |
| Kernel init (trimmed, quiet, no fbcon, `lpj=`) | ~2–5 s | **~1.2–2.0 s** *(est.)* |
| Initramfs unpack | ~0.3 s | **0 s** |
| Critical-path storage attach (`ubi_sys`, 608 PEBs) | **6.1 s** (FTL map) | **~0.1 s** (fastmap) / **~0.6 s** (worst-case full scan after power cut) |
| Root (`squashfs`) + `state` (`UBIFS`) + `firmware` mount | — | **~0.1 s** |
| Mesa/Weston untar + firmware carving | ~2.5–3.5 s | **0 s** (pre-installed in `rootfs` & `firmware` volumes) |
| Compositor first frame (logo via seamless DRM flip) | ~2 s (after modeset) | **~0.8–1.2 s** (no HDMI PHY re-lock) |
| **Total from DFU to first GUI frame** | **~32–36 s** (`26.36 s` to early USB console) | **~5.0–6.8 s** (fastmap) / **~5.5–7.3 s** (after unclean power cut) |

Because the loader sets 1080p30 at `t ≈ 2.25 s`, the TV's 1–3 s HDMI lock completes in parallel with the 0.5 s NAND read and 1.5 s kernel boot, so the display is already locked and ready when the compositor commits its first frame.

---

## 10. Risks and Validation

- **SLC mode on data-area blocks** is the foundation; it is proven on the preserved region (`1048..1063`) today and validated on data-area blocks in Phase 1 before any migration happens. Fallback in §4.6.
- **Irreversible erase:** migration does not start until `nand-restore` (§5 step 2) is verified end-to-end over `eth0` on multi-superblock test data and both the 2026-10-30 raw backup and fresh logical backups are verified.
- **Preserved regions:** `slc_mode=1` in `s5l8947x-fmi` maps only physical blocks `1..1047`; Block `0` and Blocks `1048..1063` of both CAUs are outside the MTD address space, and `nandboot-install` additionally rejects any raw row outside `1..1047`.
- **Hard power-off resilience (`ubi_sys` vs `ubi_data`):** splitting `ubi_sys` (608 PEBs) from `ubi_data` (1,454 PEBs) bounds worst-case full-scan time after a power cut to ~0.6 s and prevents application writes on `/data` from ever dirtying `ubi_sys`'s fastmap.
- **PPN retired blocks and status handling:** `MN_ST_REFRESH` (`0x42`) → `-EUCLEAN` (UBI scrubs the PEB), uncorrectable read (`0x43..0x45`) → `-EBADMSG`, sticky `MN_ST_GENERAL_ERROR` (`BIT(4)`) → package reset + `MN_POWER_ASYNC` retry, and erase failure (`0x45` / `-EIO`) → recorded in `s5l8947x-fmi`'s bad-block bitmap via `_block_markbad` so UBI retires the PEB cleanly without entering read-only mode.
- **Display and `fb_region` safety:** `fb_region` (`0x9e400000`, `8 MiB`, `no-map`) stays reserved while `BOOT_HDMI=1` so kernel allocations never corrupt the live black scanout; if the kernel panics before DRM probes, the `debug` DTB profile enables `earlycon` + `ttySAC3` and `BOOT_SPLASH=1` (`simple-framebuffer`).
- **Recovery:** the self-contained USB recovery bundle (`zImage` + initramfs + `slc_mode=0` + `s5l8947x-ftl.ko`) is the permanent unbrick and restore path and is built alongside every release.
- **Clean-room:** all loader, driver, and tooling code is freshly authored `GPL-2.0-only`; carved firmware lives only on the device's `firmware` volume and in `research/`, never in the repository.

---

## 11. Open Decisions

1. **Uncompressed vs LZ4 `squashfs` root** — default is uncompressed (`-noI -noD -noF -noX`); Phase 3 benchmarks both to measure whether LZ4 saves or costs milliseconds at ~15–18 MB/s NAND speed.
2. **Loader HDMI mode set** — keep (`BOOT_HDMI=1` with `fb_region` reserved) unless Phase 0 shows the TV locks in < 0.5 s.
3. **GUI stack and input:** Tier-1 DRM splash + Weston (`apple_sgx` KMS/GBM/EGL) with the built-in IR remote (`s5l8947x-mcu`) today, expanding to USB HID once OTG host mode lands.
4. **Cold-boot DFU entry method:** keep external trigger / Method B (zero Block `0` writes) vs one-time Block `0` header invalidation (Method A).

---

## 12. Measurement Method

Every estimate in this document is replaced by a measured number before the plan is considered complete. Per phase, record: USB transfer time, loader time (UART timestamps with `0x01220002` divider fix), first kernel message, `ubi_sys` fastmap vs full-scan attach (`dmesg`), root mount, first compositor frame on HDMI, SSH-ready, and the wall-clock time until the TV shows the logo. `CONFIG_PRINTK_TIME=y` timestamps the kernel side; `scripts/bootgraph.pl` with `initcall_debug` (debug profile) splits kernel init.
