# Benchmarks & ARM NEON Impact (`S5L8947X` / `j33iap`)

Measured on live `S5L8947X` (`j33iap`) hardware running mainline **Linux 6.12.79** and **Linux 7.3.1** with the CPU frequency governor pinned to `performance` at **`850 MHz`** (`850000 kHz`).

## 1. Test Environment

| Parameter | Baseline (`#192`) | ARM NEON + `armv7` (`#196`) |
|---|---|---|
| **SoC / CPU** | `S5L8947X` Cortex-A9 r2p8 @ 850 MHz | `S5L8947X` Cortex-A9 r2p8 @ 850 MHz |
| **SIMD / FPU Hardware** | VFPv3-D32 + ARM NEON MPE (`half thumb fastmult vfp edsp thumbee neon vfpv3 tls vfpd32`) | VFPv3-D32 + ARM NEON MPE (`half thumb fastmult vfp edsp thumbee neon vfpv3 tls vfpd32`) |
| **Cache Hierarchy** | 32 KB L1 I/D + 512 KB PL310 L2 (erratum 769419 enabled) | 32 KB L1 I/D + 512 KB PL310 L2 (erratum 769419 enabled) |
| **Kernel Crypto (`arch/arm/crypto/`)** | Configured as `=m` (not shipped in initramfs); only generic C + `s5l8947x-aes` / `s5l8947x-sha` active | Built-in (`=y`): `sha1-neon`, `sha256-neon`, `sha512-neon`, `blake2b-neon`, `blake2s-arm`, `chacha20-neon`, `poly1305-neon`, `aes-arm`, `aes-neonbs`, plus `AF_ALG` sockets |
| **Userspace Rootfs** | Alpine 3.21 `armhf` (ARMv6 + VFPv2 baseline) | Alpine 3.21 `armv7` (ARMv7-A Thumb-2 + VFPv3/NEON) |
| **Static Tools (`scripts/tools/`)** | `-O2` (scalar ARM) | `-O2 -mcpu=cortex-a9 -mfpu=neon` + explicit NEON intrinsics (`sgxcarve`, `wavplay`, `l2bench`) |
| **Boot Bundle Size** | `10,982,752` bytes | `10,981,984` bytes (`-768` bytes) |
| **Hardware Test Suite (`hwtest`)** | 101 passed | **106 passed, 0 failed, 2 skipped** |

---

## 2. Cache & Memory Hierarchy (`/bin/l2bench`)

### Dependent Pointer-Chase Latency (`32-byte` cache line stride)

| Working Set | Tier | Latency (`850 MHz`) | Approx. CPU Cycles |
|---|---|---|---|
| **16 KB** | L1 Data Cache (32 KB) | **`4.7 ns`** | ~4 cycles |
| **32 KB** | L1 Data Cache boundary | **`4.8 ns`** | ~4 cycles |
| **64 KB** | PL310 L2 Cache (512 KB) | **`45.3 ns`** | ~39 cycles |
| **128 KB** | PL310 L2 Cache | **`53.7 ns`** | ~46 cycles |
| **256 KB** | PL310 L2 Cache | **`58.5 ns`** | ~50 cycles |
| **384 KB** | PL310 L2 Cache | **`59.8 ns`** | ~51 cycles |
| **512 KB** | PL310 L2 Cache boundary | **`62.8 – 64.5 ns`** | ~54 cycles |
| **768 KB** | L2 -> DRAM transition | **`115.4 ns`** | ~98 cycles |
| **1024 KB** | LPDDR2 DRAM | **`135.2 ns`** | ~115 cycles |
| **2048 KB** | LPDDR2 DRAM | **`153.5 ns`** | ~130 cycles |
| **4096 KB** | LPDDR2 DRAM | **`159.0 – 159.8 ns`** | ~135 cycles |

### Sequential Streaming Read Bandwidth (`l2bench`)

Compiling `seq_read_mbs()` in `scripts/tools/l2bench.c` with `-O3 -mcpu=cortex-a9 -mfpu=neon` vectorizes the cache-line stride accumulation into wide NEON `vld2.32` / `vadd.i32` loads, keeping the Cortex-A9 Media Processing Engine and PL310 L2 prefetchers saturated:

| Span | Baseline (`-O2` Scalar GPR) | Post-NEON (`-O3 -mcpu=cortex-a9 -mfpu=neon`) | Speedup |
|---|---|---|---|
| **256 KB Sequential Read (PL310 L2)** | `1,763 – 1,819 MB/s` | **`3,648 – 3,728 MB/s`** | **`2.05x`** (`+1,909 MB/s`) |
| **16 MB Sequential Read (DRAM)** | `678 – 684 MB/s` | **`1,596 MB/s`** | **`2.34x`** (`+912 MB/s`) |

---

## 3. Kernel Cryptographic Subsystem (`AF_ALG`, 16 MiB Transfers)

Measured through `AF_ALG` sockets (`64 KiB` chunks over `16 MiB` per algorithm) on kernel `#196`, comparing generic C (`-generic`), ARMv7 scalar assembly (`-arm` / `-asm`), the on-chip hardware engines (`s5l8947x-sha` and `s5l8947x-aes`), and ARM NEON (`-neon` / `-neonbs`):

| Algorithm | Generic C (`-generic`) | ARMv7 Scalar (`-arm` / `-asm`) | S5L8947X Hardware (`-s5l8947x`) | ARM NEON (`-neon` / `-neonbs`) | NEON vs. Generic | Highest-Priority Active Driver |
|---|---|---|---|---|---|---|
| **SHA-1** | `33.1 MB/s` | `57.0 MB/s` (`1.72x`) | — | **`90.5 MB/s`** | **`2.73x`** | `sha1-neon` (`prio 250`) |
| **SHA-256** | `28.3 MB/s` | `35.8 MB/s` (`1.26x`) | `15.3 – 15.6 MB/s` *(PIO)* | **`48.8 MB/s`** | **`1.72x`** *(3.19x vs HW)* | `sha256-neon` (`prio 250`) |
| **SHA-512** | `12.4 MB/s` | `16.6 MB/s` (`1.34x`) | `16.5 – 17.0 MB/s` *(PIO)* | **`31.6 MB/s`** | **`2.55x`** *(1.92x vs HW)* | `sha512-s5l8947x` / `sha512-neon` (`prio 300`) |
| **BLAKE2b-512** | `29.6 MB/s` | — | — | **`58.3 MB/s`** | **`1.97x`** | `blake2b-512-neon` (`prio 200`) |
| **Poly1305** | `98.8 MB/s` | `171.2 MB/s` (`1.73x`) | — | **`221.4 MB/s`** | **`2.24x`** | `poly1305-neon` (`prio 200`) |
| **ChaCha20** | `43.2 MB/s` | `63.0 MB/s` (`1.46x`) | — | **`73.1 MB/s`** | **`1.69x`** | `chacha20-neon` (`prio 300`) |
| **AES-128-CBC** | `17.8 MB/s` | `18.6 MB/s` (`1.05x`) | **`70.3 MB/s`** *(CDMA)* | **`23.1 MB/s`** *(bit-sliced)* | **`1.30x`** *(HW is `3.95x`)* | `s5l8947x-cbc-aes` (`prio 300`) |
| **AES-128-CTR** | `17.8 MB/s` *(base)* | `18.6 MB/s` *(base)* | — *(ECB/CBC only)* | **`33.9 MB/s`** *(bit-sliced)* | **`1.90x`** | `ctr-aes-neonbs` (`prio 250`) |
| **CRC32C** | `188.0 MB/s` | — | — | — | — | `crc32c-generic` (`prio 100`) |

### Hardware vs. NEON Trade-offs on `S5L8947X`

1. **Why Hardware AES (`s5l8947x-aes`) wins for CBC/ECB (`70.3 MB/s` vs. `23.1 MB/s`):**
   - The `s5l8947x-aes` engine streams 16 KiB buffers directly to and from DRAM over CDMA channels 1 and 2 (`prio 300`), offloading the CPU completely.
   - However, the hardware AES block only supports `ECB` and `CBC` modes (`128`/`192`/`256`-bit keys) and serializes on a single hardware mutex. Bit-sliced NEON AES (`ctr-aes-neonbs` at `33.9 MB/s`, `xts-aes-neonbs`, `cbc-aes-neonbs` at `prio 250`) and scalar `aes-arm` (`cmac(aes-arm)` at `prio 200` for Wi-Fi WPA2/WPA3 management frame protection) accelerate all modes not handled by the CDMA engine.
2. **Why ARM NEON wins for SHA-2 (`48.8 MB/s` SHA-256 and `31.6 MB/s` SHA-512 vs. `15.3 – 17.0 MB/s`):**
   - Although the `s5l8947x-sha` core at `0x30200000` hashes a single block in hardware at ~47 MB/s (SHA-256) and ~76 MB/s (SHA-512), CDMA channel 4 cannot feed its FIFO, so the CPU must push every 32-bit word over uncached MMIO (`writel_relaxed`).
   - Operating directly on L1/L2 cached memory in 128-bit NEON registers (`q0`–`q15`) avoids the MMIO bus bottleneck, delivering **`3.19x`** higher SHA-256 throughput (`48.8 MB/s`) and **`1.92x`** higher SHA-512 throughput (`31.6 MB/s`).

---

## 4. Userspace SIMD & `armv7` Rootfs Workloads

### Scalar vs. ARM NEON Microbenchmarks (Cortex-A9 @ 850 MHz)

| Workload | Scalar ARM (`-O2`) | ARM NEON (`-mcpu=cortex-a9 -mfpu=neon`) | Speedup | Notes |
|---|---|---|---|---|
| **Adler-32 Checksum (`sgxcarve.c`, 128 MiB)** | `230.8 MB/s` | **`397.0 MB/s`** | **`1.72x`** | Vectorized 16-byte `vld1q_u8` / `vpaddlq_u8` / `vmlal_u16` accumulation (`crc=7db5bc6e` bit-exact) |
| **720p `RGB565 -> XRGB8888` Scanout (`1280x720`)** | `194.4 MB/s` (`55.3 fps`) | **`344.7 MB/s` (`98.0 fps`)** | **`1.77x`** | 8-pixel `vld1q_u16` unpack + `vst4_u8` interleaved store; crosses 60 Hz real-time threshold |
| **Stereo 16-bit Resampler (`wavplay.c`, 44.1->48 kHz)** | `153.4 MB/s` | **`174.1 MB/s`** | **`1.14x`** | 4-frame NEON `vtrn_s32` / `vsubl_s16` / `vmulq_s32` / `vshrn_n_s32` linear interpolation |
| **Unrolled GPR32 Streaming Read (L2 256 KB)** | `1,819 MB/s` | **`1,972.9 MB/s`** | **`1.08x`** | `-mcpu=cortex-a9` scheduling + software prefetch (`pld`) |
| **Unrolled GPR32 Streaming Read (DRAM 16 MB)** | `683 MB/s` | **`823.9 MB/s`** | **`1.21x`** | `-mcpu=cortex-a9` scheduling + software prefetch (`pld`) |

### Userspace CLI Utilities (`armhf` vs. `armv7` Rootfs, 32 MiB `/dev/urandom` in `/tmp`)

| Command | Baseline (`armhf` Rootfs) | Post-NEON (`armv7` Rootfs) | Improvement |
|---|---|---|---|
| **`gzip -1 -c` (32 MiB)** | `11.30 s` (`2.83 MB/s`) | **`10.28 s` (`3.11 MB/s`)** | **`+9.9%`** (`-1.02 s`) |
| **`sha512sum` (32 MiB)** | `9.47 s` (`10.14 MB/s`) | **`9.06 s` (`10.60 MB/s`)** | **`+4.5%`** (`-0.41 s`) |
| **`md5sum` (32 MiB)** | `1.26 s` (`76.19 MB/s`) | **`1.21 s` (`79.34 MB/s`)** | **`+4.1%`** (`-0.05 s`) |
| **`sha1sum` (32 MiB)** | `2.22 s` (`43.24 MB/s`) | **`2.16 s` (`44.44 MB/s`)** | **`+2.8%`** (`-0.06 s`) |
| **`sha256sum` (32 MiB)** | `4.93 s` (`19.47 MB/s`) | **`4.90 s` (`19.59 MB/s`)** | **`+0.6%`** (`-0.03 s`) |

---

## 5. End-to-End Storage, Decompression & Boot Carvers

| Workload | Measured Time | Throughput / Rate | Subsystem Path |
|---|---|---|---|
| **`/dev/nand0p1` Raw Block Read (32 MiB, cold cache)** | `2.38 s` | **`13.45 MB/s`** | `s5l8947x-fmi` (CDMA ch 5/6) + `s5l8947x-ftl` |
| **HFS+ `decmpfs` Transparent Inflate (`/usr/sbin` + `/usr/bin`, 64 MiB)** | `4.02 s` | **`15.87 MB/s`** | `s5l8947x-fmi` + `hfsplus` `decmpfs` (zlib/LZVN) |
| **`sgxcarve` (LZSS inflate + NEON Adler-32 -> `sgx543.fw`)** | `324 ms` / iter | **`22.68 MB/s`** *(input)* | Unwraps and inflates 12.6 MB system image, verifies Adler-32, extracts 12 SGX regions (`92,328` B) |
| **`hcdcarve` (`BCM4334B0.hcd` extraction)** | `209.6 ms` / iter | **`25.36 MB/s`** *(input)* | Scans binary for board-specific HCI patch (`49,316` B) |
| **Total USB DFU -> Linux Console Boot Time** | **`+2.20 s`** *(kernel -> `system ready`, fast `stage2`)* | **`26.36 s`** *(baseline cold DFU)* | Pre-kernel 1080p splash + seamless HDMI handover + SLC NAND (`18.6 MB/s`) + `usr-local.sqsh` loop mount |

---

## 6. SGX543 3D GPU & OpenGL ES 2.0 Benchmarks (`glcubes` at `1920x1080`)

Measured with `sgx-gl glcubes` (10×10×10 grid = **1,000 textured, shaded 3D cubes**, **36,000 vertices / 12,000 triangles per frame**, trilinear mipmapped `64x64` RGBA texture, 24-bit depth buffer, `1920x1080` resolution over 120 frames):

| Metric (`1920x1080`, 120 Frames) | Baseline | Optimized (`e427398`) | Improvement |
|---|---:|---:|---|
| **Startup & Asset Load (`--screen` `load_ms`)** | `1384.2 ms` | **`427.7 ms`** | **`3.24x` faster** (`-956.5 ms`) |
| **EGL/GBM Context Init (`context`)** | `1137.7 ms` | **`266.5 ms`** | **`4.27x` faster** |
| **Texture & Mipmap Setup (`assets`)** | `144.2 ms` | **`64.2 ms`** | **`2.25x` faster** |
| **GPU Render Time (`avg_ms` / `min_ms`)** | `47.11 ms` / `46.50 ms` | **`20.59 ms` / `17.90 ms`** | **`2.29x` faster** |
| **GPU Render Throughput (`render_fps`)** | `21.23 fps` | **`48.57 fps`** *(peak `55.9 fps`)* | **`2.29x` higher FPS** |
| **HDMI Presented FPS (`--screen` at `1080p30`)** | `14.01 fps` (`8.566 s`) | **`25.00 fps`** (`4.800 s`) | **`1.78x` higher FPS** |
| **Offscreen `1920x1080` (`avg_ms` / `min_ms`)** | ~`47.1 ms` (`21.2 fps`) | **`20.41 ms` / `17.39 ms` (`48.98 fps`)** | **`2.31x` higher FPS** |

### Key Driver & Compiler Optimizations (`e427398`)

1. **USSE Compiler (`F16` Vector Fragment Path, `V32MAD` Fusion & Register Packing):**
   - Extended `f16_colour()` (`sgx_compiler.c`, `usse_hop_swz()`) to support 4-wide `F16` `V16NMAD` with arbitrary channel swizzles and hardware constants (`0.0, 1.0, 2.0, 0.5`), compiling `glcubes`'s fragment shader into **3 instructions and 2 temporaries** instead of 15 `F32` instructions and 14 temporaries.
   - Added `nir_op_ffma` lowering (`usse_fmad()`) with single-use `fadd`+`fmul` chain reassociation (`fuse_ffma()`), reducing 4×4 matrix transforms (`u_mvp * vec4(a_pos, 1.0)`) from 24 instructions to 12 `FMAD`s, and eliminating unread attribute channel `LIMM`s in `vs_prologue()`.
   - Packed scalar `V32NMAD` and `V32MAD` temporaries into both even and odd register lanes (`define_aligned(..., 1)`), halving register pressure.
2. **TBDR Tile & Bandwidth Optimizations:**
   - Pointed the hardware background object (`bg[0..3]`) at the cached 64-byte 4×4 dummy texture when a render starts with a full-screen clear or an opaque full-screen quad (`bg_cleared`), eliminating an **8.3 MiB DRAM read** per `1920x1080` pass and skipping the redundant `zs_quad` depth-fill pass.
3. **Vertex & Index Streaming:**
   - Expanded the pre-built identity index buffer (`BUILT_IDX_COUNT`) from `8,190` to `65,535` vertices and added a static repacked multi-VBO GPU BO cache in `gpu_vs_draw()`, eliminating per-frame CPU index generation and 1.15 MiB/frame of strided CPU vertex copies.
4. **Startup, Mipmaps & DRM EDID Caching:**
   - Removed redundant write-combined `memset` on freshly allocated kernel shmem GEM BOs, added a write-only `PIPE_MAP_WRITE` fast path in `cpu_blit()` for `glGenerateMipmap()`, and cached parsed HDMI EDID across `get_modes()` calls in `s5l8947x_drm.c`.

---

## 7. Linux 6.12.79 vs. Linux 7.3.1 Kernel Migration Comparison

Measured back-to-back on the same `S5L8947X` (`j33iap`) device with the CPU governor pinned to `performance` at **`850 MHz`** (`850,000,643 Hz` calibrated against the 24 MHz watchdog counter, `1700.00 BogoMIPS`), comparing **Linux `6.12.79` (`#277`)** and **Linux `7.3.1` (`#1`)**:

### Boot, Bundle Size & Hardware Verification (`hwtest`)

| Metric | Linux `6.12.79` (`#277`) | Linux `7.3.1` (`#1`) | Notes |
|---|---|---|---|
| **CPU Clock / BogoMIPS** | `850,000 kHz` / `1700.00` | `850,000 kHz` / `1700.00` | Cortex-A9 r2p8 + PL310 L2 (`512 KB`, 16-way) |
| **Boot Bundle Size (`boot-bundle-s5l8947x-j33iap.bin`)** | `12,869,056 B` (`12.27 MiB`) | `12,919,872 B` (`12.32 MiB`) | `+50.8 KiB` (`+0.39%`) with full Mesa/GBM/GLESv2 rootfs |
| **Stage 5 Kernel Boot (`0x80000000` -> USB Console)** | ~`14.7 s` | **`14.733 s`** | XZ decompress + kernel init + initramfs + USB CDC-ACM ready |
| **Hardware Test Suite (`/bin/hwtest`)** | **127 passed, 0 failed, 2 skipped** | **127 passed, 0 failed, 2 skipped** | Full parity across CPU, AIC, CDMA, FMI/FTL, AES/SHA, DRM/HDMI, SGX543, M2SC, JPEG, H264, USB, Wi-Fi/BT, and Audio |

### Cache & Memory Hierarchy (`l2bench` & `neonbench`)

| Benchmark | Linux `6.12.79` (`#277`) | Linux `7.3.1` (`#1`) | Delta |
|---|---:|---:|---:|
| **L1 Latency (16 KB pointer chase)** | `4.7 ns` | `4.7 ns` | `0.0%` |
| **L1 Boundary Latency (32 KB pointer chase)** | `4.8 ns` | `4.8 ns` | `0.0%` |
| **PL310 L2 Latency (64 KB / 128 KB / 256 KB)** | `43.2 / 58.6 / 65.2 ns` | `42.5 / 59.1 / 67.4 ns` | Within thermal/cache-placement variance |
| **PL310 L2 Boundary Latency (512 KB)** | `68.5 ns` | `72.7 ns` | Within L2 replacement variance |
| **LPDDR2 DRAM Latency (4 MB / 16 MB pointer chase)** | `159.4 / 158.9 ns` | **`157.8 / 157.5 ns`** | **`-0.9%` (lower latency)** |
| **`l2bench` 256 KB Sequential Read (PL310 L2)** | `3,676 MB/s` | **`3,727 MB/s`** | **`+1.4%`** |
| **`l2bench` 16 MB Sequential Read (LPDDR2 DRAM)** | `1,615 MB/s` | **`1,625 MB/s`** | **`+0.6%`** |
| **`neonbench` L2 256 KB Read (Scalar / GPR32 / NEON)** | `454.9 / 851.5 / 1,033.8 MB/s` | **`455.2 / 856.3 / 1,041.6 MB/s`** | **`+0.8%` NEON** |
| **`neonbench` DRAM 16 MB Read (Scalar / GPR32 / NEON)** | `305.5 / 393.2 / 507.2 MB/s` | **`306.8 / 394.5 / 506.7 MB/s`** | Parity (`-0.1%` NEON, `+0.3%` GPR32) |

### Storage, HFS+ Decompression & Firmware Carvers

| Workload | Linux `6.12.79` (`#277`) | Linux `7.3.1` (`#1`) | Delta |
|---|---:|---:|---:|
| **`/dev/nand0p1` Raw Block Read (32 MiB, cold cache)** | `13.11 MB/s` (`2.44 s`) | **`13.39 MB/s` (`2.39 s`)** | **`+2.1%`** |
| **HFS+ `decmpfs` Read (`/usr/sbin` + `/usr/bin`)** | `15.72 MB/s` (`4.06 s`) | `15.56 MB/s` (`4.10 s`) | `-1.0%` |
| **`sgxcarve` (LZSS + NEON Adler-32, 10 iters)** | `25.17 MB/s` (`292.0 ms/iter`) | `24.66 MB/s` (`298.0 ms/iter`) | `-2.0%` (`crc=7db5bc6e` bit-exact) |
| **`sgxcarve` Scalar (`-O2`, 10 iters)** | `23.48 MB/s` (`313.0 ms/iter`) | `23.26 MB/s` (`316.0 ms/iter`) | `-0.9%` |
| **`hcdcarve` (`BCM4334B0.hcd`, 50 iters)** | `25.14 MB/s` (`211.4 ms/iter`) | `24.31 MB/s` (`218.6 ms/iter`) | `-3.3%` |

### Userspace CLI & NEON SIMD Microbenchmarks

| Workload | Linux `6.12.79` (`#277`) | Linux `7.3.1` (`#1`) | Delta |
|---|---:|---:|---:|
| **`sha256sum` (32 MiB)** | `19.47 MB/s` (`4.93 s`) | `19.24 MB/s` (`4.99 s`) | `-1.2%` |
| **`sha512sum` (32 MiB)** | `10.55 MB/s` (`9.10 s`) | `10.53 MB/s` (`9.12 s`) | `-0.2%` |
| **`sha1sum` (32 MiB)** | `44.04 MB/s` (`2.18 s`) | **`44.44 MB/s` (`2.16 s`)** | **`+0.9%`** |
| **`md5sum` (32 MiB)** | `78.69 MB/s` (`1.22 s`) | **`80.00 MB/s` (`1.20 s`)** | **`+1.7%`** |
| **`gzip -1 -c` (32 MiB)** | `3.10 MB/s` (`10.31 s`) | `3.02 MB/s` (`10.61 s`) | `-2.6%` |
| **Adler-32 (`128 MiB`, Scalar -> NEON)** | `229.8 -> 378.8 MB/s` (`1.65x`) | **`233.6 -> 383.3 MB/s` (`1.64x`)** | **`+1.2%` NEON** |
| **Audio Resampler (`44.1 -> 48 kHz`, Scalar -> NEON)** | `137.8 -> 177.8 MB/s` (`1.29x`) | **`137.2 -> 179.1 MB/s` (`1.31x`)** | **`+0.7%` NEON** |
| **720p `RGB565 -> XRGB8888` (Scalar -> NEON)** | `248.7 MB/s (70.7 fps) -> 494.9 MB/s (140.8 fps)` | **`251.3 MB/s (71.5 fps) -> 505.4 MB/s (143.8 fps)`** | **`+2.1%` NEON (`+3.0 fps`)** |

### Kernel Cryptographic Subsystem (`AF_ALG`, 16 MiB Transfers)

In Linux `7.x`, the upstream kernel consolidated architecture-specific NEON/ARM crypto modules (`sha1-neon`, `sha256-neon`, `sha512-neon`, `blake2b-neon`, `chacha20-neon`, `aes-arm`) into the unified `lib/crypto/` library (`*-lib` drivers) and introduced `/proc/sys/crypto/af_alg_restrict = 1` by default (which restricts userspace `AF_ALG` sockets from binding directly to low-level driver cra_driver_names unless set to `0`):

| Algorithm (`16 MiB` via `AF_ALG`) | Linux `6.12.79` (`#277`) | Linux `7.3.1` (`#1`) | Notes / Driver Change |
|---|---:|---:|---|
| **SHA-1 (ARM NEON)** | `94.1 MB/s` (`sha1-neon`) | `92.4 MB/s` (`sha1-lib`) | Consolidated into `lib/crypto/arm/sha1.h` (`prio 300`) |
| **SHA-256 (ARM NEON)** | `49.8 MB/s` (`sha256-neon`) | `49.0 MB/s` (`sha256-lib`) | Consolidated into `lib/crypto/arm/sha256.h` (`prio 300`) |
| **SHA-256 (`s5l8947x-sha` PIO)** | `15.4 MB/s` (`sha256-s5l8947x`) | `15.4 MB/s` (`sha256-s5l8947x`) | Identical hardware PIO throughput (`prio 150`) |
| **SHA-512 (ARM NEON)** | `31.6 MB/s` (`sha512-neon`) | `31.2 MB/s` (`sha512-lib`) | Consolidated into `lib/crypto/arm/sha512.h` (`prio 300`) |
| **SHA-512 (`s5l8947x-sha` PIO)** | `16.4 MB/s` (`sha512-s5l8947x`) | `16.4 MB/s` (`sha512-s5l8947x`) | Raised to `prio 350` so `s5l8947x-sha` stays default for SHA-512 |
| **BLAKE2b-512 (ARM NEON)** | `57.3 MB/s` (`blake2b-512-neon`) | `57.1 MB/s` (`blake2b-512-lib`) | Consolidated into `lib/crypto/arm/blake2b.h` (`prio 300`) |
| **AES-128-CBC (ARM NEON bit-sliced)** | `22.9 MB/s` (`cbc-aes-neonbs`) | `22.8 MB/s` (`cbc-aes-neonbs`) | Identical (`prio 250`) |
| **AES-128-CBC (ARM Scalar / Library)** | `19.0 MB/s` (`cbc(aes-arm)`) | **`22.7 MB/s` (`cbc-aes-lib`)** | **`+19.5%` faster** via `CONFIG_CRYPTO_LIB_AES_CBC` (`prio 200`) |
| **AES-128-CTR (ARM NEON bit-sliced)** | `33.0 MB/s` (`ctr-aes-neonbs`) | `33.0 MB/s` (`ctr-aes-neonbs`) | Identical (`prio 250`) |
| **ChaCha20 (ARM NEON)** | `73.9 MB/s` (`chacha20-neon`) | `71.3 MB/s` (`chacha20-lib`) | Consolidated into `lib/crypto/arm/chacha.h` (`prio 300`) |
| **AES-128-CBC (`s5l8947x-aes` CDMA)** | `70.8 MB/s` (`s5l8947x-aes`) | In-kernel CDMA (`prio 300`) | Registered for in-kernel FTL/DM-Crypt consumers; masked from userspace `AF_ALG` by `CRYPTO_ALG_KERN_DRIVER_ONLY` in 7.x |

---

## 8. Full-Stack Lean & Performance Optimization Impact (`Linux 7.3.1 #3`)

Measured on live `S5L8947X` (`j33iap`) hardware at **`850 MHz`** (`performance` governor) comparing the pre-audit `Linux 7.3.1 #1` baseline against the post-audit `Linux 7.3.1 #3` build (Phases 1–5: zero-media rootfs, 64-bit `apple-aic1-time` clocksource, batched SGX543 MMU mapping, trimmed `sgx_pb_image()` zeroing, lazy `tvshell` surface allocation, and single-pass `hcdcarve`):

### Image Footprint, Boot Time & System Memory

| Metric | Pre-Audit (`7.3.1 #1`) | Post-Audit (`7.3.1 #3`) | Improvement |
|---|---:|---:|---|
| **Initramfs Archive (`initramfs-s5l8947x.cpio`)** | `140.0 MB` | **`25.0 MB`** | **`-82.1%`** (`-115.0 MB`, zero bundled media) |
| **Rootfs SquashFS (`out/rootfs.sqsh`)** | `431.0 MB` | **`53.0 MB`** | **`-87.7%`** (`-378.0 MB`) |
| **`/usr/local` SquashFS (`build/usr-local.sqsh`)** | `369.0 MB` | **`56.0 MB`** | **`-84.8%`** (`-313.0 MB`) |
| **Uncompressed Kernel `out/Image`** | `158.0 MB` | **`41.0 MB`** | **`-74.1%`** (`-117.0 MB`) |
| **Compressed `zImage` & DFU Boot Bundle** | `21.0 MB` | **`12.0 MB`** | **`-42.9%`** (`-9.0 MB`) |
| **Stage 3 DFU Bundle Upload Time** | ~`17.8 s` | **`10.120 s`** | **`1.76x` faster** (`-7.7 s`) |
| **Stage 5 Kernel + Initramfs -> USB Console** | `14.733 s` | **`13.367 s`** | **`-1.366 s` (`+9.3%` faster)** |
| **System Free / Available RAM (`free -m`)** | ~`171 MB` free | **`286 MB` free / `349 MB` available** | **`+115 MB` usable RAM** (`53 MB` total used) |
| **`tvshell` Resident Memory (`VmRSS` at 720p60)** | ~`48,000 kB` | **`9,668 kB` (`9.4 MB`)** | **`4.96x` lower RSS** (`-38.3 MB`, lazy card/tile decode) |

### SGX543 GPU, Boot Carvers & Subsystem Benchmarks (`850 MHz`)

| Workload | Pre-Audit (`7.3.1 #1`) | Post-Audit (`7.3.1 #3`) | Improvement |
|---|---:|---:|---|
| **`glcubes` `1920x1080` Context Init (`context`)** | `266.5 ms` | **`80.0 ms`** | **`3.33x` faster** (`-186.5 ms`; batched MMU + 324 KiB PB init) |
| **`glcubes` `1920x1080` Total Startup (`load_ms`)** | `427.7 ms` | **`237.1 ms`** | **`1.80x` faster** (`-190.6 ms`; `5.84x` vs original `1384.2 ms`) |
| **`glcubes` `1920x1080` Render (`avg_ms` / `min_ms`)** | `20.41 ms` / `17.39 ms` | **`19.76 ms` / `17.02 ms`** | **`50.60 fps`** (`+3.3%`, crosses 50 FPS at 1080p) |
| **`glcubes` `1280x720` Offscreen (`avg_ms` / `fps`)** | — | **`10.20 ms` (`8.82 ms` min) / `98.01 fps`** | `203.6 ms` total load (`81.8 ms` context) |
| **`glcubes` `1280x720` HDMI `--screen` (`720p60`)** | — | **`96.84 fps` render / `51.44 fps` presented** | `286.7 ms` load, `9.11 ms` avg DRM flip |
| **`sgx2d` Hardware 2D Blit (64×64 -> 720p + 360p)** | — | **`17.00 ms` / process** | Pre-initialized static 2D blit templates in `sgx_blit_init()` |
| **`hcdcarve` (`BCM4334B0.hcd`, 50 iters)** | `218.6 ms/iter` (`24.31 MB/s`) | **`159.4 ms/iter` (`33.34 MB/s`)** | **`+37.1%` faster** (`1.37x`) + **32x less RAM** (1-bit `visited` bitmap) |
| **`sgxcarve` (`sgx.fw`, 10 iters)** | `298.0 ms/iter` (`24.66 MB/s`) | **`303.0 ms/iter` (`24.25 MB/s`)** | Shared `carve_common.h` (`crc=7db5bc6e` bit-exact) |
| **`jitterseed` Early Entropy Seeder (`2048` samples)** | ~`38 ms/iter` | **`10.0 ms/iter`** | **`3.8x` faster** (halved `clock_gettime` calls + `apple-aic1-time`) |
| **Encrypted HFS+ Read (`/mnt/data`, 32 MiB cold)** | — | **`12.17 MB/s` (`2.63 s`)** | `s5l8947x-fmi` + `s5l8947x-ftl` + `s5l8947x-aes` CDMA (`91%` of raw NAND) |
| **L1 / L2 / DRAM Latency & Bandwidth (`l2bench`)** | `4.7 / 67.4 / 157.8 ns`, `3727 / 1625 MB/s` | **`4.7 / 67.9 / 159.2 ns`, `3719 / 1614 MB/s`** | Full parity |
| **Hardware Test Suite (`/bin/hwtest`)** | `127 passed, 0 failed` | **`127 passed, 0 failed`** | Zero kernel warnings or regressions |
