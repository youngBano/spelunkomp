# Goal: Comprehensive Native Rebuild of *Mushihimesama* (`mushisam`) for Original Xbox

## 1. Mission & Architectural Vision

Transform [`port/nxdk/mushisam`](port/nxdk/mushisam) (*Mushihimesama* `2004/10/12 MASTER VER.`, Cave CV1000-B) from a **basic-block-translated arcade board emulator** into a **ground-up native Original Xbox executable** built with `nxdk`.

Right now, even with static basic-block translation (`interpretedBlocks == 0`), the runtime behaves like an emulator at every boundary: it dispatches **~30,091 indirect basic-block calls per frame** across **~9.8 MB** of generated `.obj` code (thrashing the Pentium III's 16 KB L1i and 128 KB L2 caches), routes hardware accesses through virtual `Bus` methods and big-endian byte-swaps, bit-bangs NAND DMA and serial EEPROM registers, decodes MPEG-like AMM audio streams on the CPU, and copies 16-bit VRAM pages from a 14 MB CPU RAM pool (with synchronous `Z:\` HDD swap stalls) into a scratch texture every frame.

This rebuild replaces that entire stack across four pillars:
1. **Whole-Binary Game Rewriting & Pentium III Codegen:** Lift the SH-3 binary into whole-function AOT C++20 routines with register-promoted locals, constant-folded PC-relative pools, direct calls/branches, `.text.hot`/`.text.cold` cache partitioning, and hand-written native C++20/SSE1/MMX replacements for the game's hottest routines (collision/hit-testing, bullet update & trig math, blitter list generation, and timer/idle loops).
2. **Nativizing All CV1000-B Compatibility Layers:** Eliminate virtual `Bus` indirection, replace SH-3 NAND + `DMAC0` port polling with direct native FATX asset streaming, offload YMZ770C/AMM audio onto **Xbox MCPX APU hardware ADPCM/PCM voices**, and bridge USB XID input, RTC-9701, and FATX EEPROM saves directly.
3. **Bare-Metal NV2A GPU Hardware Registers & Pipelines:** Keep CV1000 `8192×4096` `A1R5G5B5` graphics resident in contiguous NV2A physical memory (zero HDD VRAM swapping, zero per-frame CPU pixel copies), offload vertex transformation/rotation/clipping to a **custom NV2A Kelvin programmable vertex shader**, implement exact CV1000 5-bit `0x..2x` hit-flash tint overdrive in **NV2A Register Combiners**, and synchronize CPU/GPU frames via **hardware Kelvin semaphores**.
4. **Hardware-Level Profiling & Differential Verification:** Instrument Pentium III Ring-0 hardware performance counters (`rdpmc` L1i/L1d/L2/branch misses + `rdtsc`), NV2A `PGRAPH` hardware notifier timestamps, per-routine hotspot rankings, and automated 5-stage `FAKE_PLAY` benchmarks alongside frame-exact `StateHash()` and pixel parity checks against the reference CV1000 model.

---

## 2. Empirical Baseline vs. Rebuild Targets

### 2.1 Measured Baseline (`perf_dump.bin`, 2,075 frames, 733 MHz scale)

Captured from [`port/nxdk/mushisam/xbox_profiler.h`](port/nxdk/mushisam/xbox_profiler.h) (`g_xboxPerf`, CV1000 frame budget = **`16,662 µs`** at `60.0184 Hz`):

| Metric | Current Baseline | Root Bottleneck Identified in Codebase | Rebuild Target |
| :--- | :--- | :--- | :--- |
| **Avg Total Frame** | **`16,697.1 µs`** (Max `77,314 µs`) | Exceeds `16,662 µs` budget on average; **`1,037 / 2,075` (`50.0%`) host lag frames** | **`< 7,500 µs` avg**, **`< 13,500 µs` p99**, **`0` host lag frames** |
| **SH-3 Game Binary (`profSh3Tsc`)** | **`15,001.7 µs`** (Max `76,961 µs`) | **97.2% of CPU time:** `30,091` `Lookup(PC)` dispatches/frame, `9.8 MB` code size, memory-bound `c.R[]`, dead `c.at` stores, unwired `FastTmuPollWrapper` | **`< 3,500 µs` avg**, **`< 6,000 µs` peak** |
| **YMZ770C / AMM Audio (`profAudioTsc`)** | **`216.9 µs` avg** (Max `3,628 µs`) | Software MPEG Layer-II polyphase synthesis filterbank bursts on PIII + DPC software resampler | **`< 100 µs` avg**, **`< 250 µs` peak** (MCPX APU hardware voices) |
| **CV1000 Blitter Parse (`profBlitterTsc`)** | **`211.9 µs` avg** (Max `1,350 µs`) | Double-copying uploads (`ram` -> `NativeList::pixels` -> `pageData_` -> `atlas_`) | **`< 120 µs` avg**, **`< 350 µs` peak** (zero-copy direct GPU stream) |
| **NV2A Render + Submit (`renderUs`)** | **`1,236.5 µs` avg** (Max `31,912 µs`) | `activeVramPages = 448/448` saturated -> synchronous `Z:\mushivrm.bin` HDD swap + CPU `CopyVramRectToAtlas` + 80B/quad CPU float math | **`< 800 µs` avg**, **`< 1,800 µs` peak**, **`0` disk swaps** |
| **NAND Streaming (`nandMisses`)** | **`18,213 / 18,427` misses** | SH-3 byte/DMA emulation + synchronous `ReadFile` stalls (`77.1 ms` worst frame) | **`0` gameplay stalls**, direct native chunk loader |
| **Retail RAM Footprint** | **~56–60 MB** (tight, `448` CPU pages + `9.8 MB` `.text`) | Duplicate `k_XXXXXXXX[]` ROM arrays (~1 MB), bloated block TUs, CPU-side VRAM mirror | **`<= 48 MB` peak** on stock **64 MB** retail Xbox |

### 2.2 Target Hardware Envelope (Stock Retail Original Xbox)

* **CPU:** 733 MHz Intel Pentium III (Coppermine-128): 32-bit `i686`, MMX + SSE1 (**no SSE2**), **16 KB L1i / 16 KB L1d / 128 KB unified L2** (32-byte cache lines), 133 MHz 64-bit FSB (~1.06 GB/s peak shared with GPU/UMA).
* **GPU:** 233 MHz NVIDIA NV2A (`NV097` / `PGRAPH` / dual Kelvin programmable vertex units, 4 pixel pipelines × 2 texture units, 2-stage + final Register Combiners, hardware DMA pushbuffers & vertex arrays).
* **APU:** NVIDIA MCPX (`NV1A` DirectSound/AC'97: 256 hardware 2D voices, hardware Xbox IMA ADPCM decompression, 48 kHz hardware SRC/mixing).
* **Memory:** **64 MB Unified DDR SDRAM** (~6.4 GB/s internal, shared across kernel, `.xbe` image, SH-3 8 MB RAM, GPU framebuffers/textures/pushbuffers, and audio voices). Optional runtime unlocks when `MmQueryStatistics()` detects **128 MB**.

---

## 3. Pillar 1: Rewriting the Game Binary for Optimal Xbox Execution

### 3.1 Whole-Function AOT Static Recompiler (`tools/recomp_sh3.py`)

Replace the naive basic-block translator ([`port/nxdk/mushisam/tools/translate.py`](port/nxdk/mushisam/tools/translate.py)) with an optimizing whole-binary SH-3 -> Pentium III C++20 static recompiler:

1. **Full Post-Boot Binary & Call-Graph Lifting:**
   * Snapshot the deterministic post-boot executable region in CV1000-B RAM (`0x0C000000 .. codeAddrEnd`, copied once from `mushisam_u4`) plus the U4 boot loader (`0x00000000`), augmented by dynamic indirect-jump target logs (`JSR @Rn`, `JMP @Rn`, `BSRF`, `BRAF`) harvested across all 5 stages, attract mode, 2P mode, continues, name entry, and service menu.
   * Partition code into **whole functions** (entry at `BSR`/`JSR`/vector targets, internal basic blocks joined by labels) rather than cutting a separate C++ function at every `BT`/`BF`/`BTS`/`BFS`/`BRA`.
2. **Eliminate Basic-Block Dispatch Overhead (`30,091 -> < 1,500` dispatches/frame):**
   * Lower intra-function `BT`, `BF`, `BTS`, `BFS`, and `BRA` into direct C++ `if (T) goto L_0C0xxxxx;` branches with cycle checks hoisted only onto **backward loop edges** (`if (cycles >= blockStop || interruptReady) { c.PC = ...; return; }`).
   * Lower static `BSR` and constant-pool `JSR @Rn` calls into **direct C++ function calls** (returning immediately if `c.PC != expectedReturnPc` or `interruptReady`), leveraging the Pentium III hardware Return Stack Buffer (RSB).
   * Build a flat, cache-aligned **computed-jump table** for switch tables (`BRAF`/`JMP @Rn`) and a zero-branch 2-level page/halfword table for residual indirect calls—eliminating `t->epoch`, `t->valid`, and `std::memcmp` checks for immutable post-boot code.
3. **SSA Register Promotion & Constant Pool Folding:**
   * Promote `R[0..15]`, `T`, `PR`, `MACL`, `MACH`, and local `cycles` into **local C++ variables** inside each compiled routine (spilling back to `SH3 &c` only before non-inlined calls/exits) so Clang allocates `eax`, `ecx`, `edx`, `ebx`, `esi`, `edi`, `ebp` across entire loops instead of reloading `c.R[n]` from memory on every opcode.
   * **Fold PC-relative literal loads at compile time:** In SH-3, every 32-bit constant or function/RAM address is loaded via `MOV.L @(disp,PC), Rn` or `MOV.W @(disp,PC), Rn`. Currently `translate.py` emits `c.Read32(0x0C0024A0u)` at runtime! For all read-only code/rodata addresses, fold these directly into immediate `0xXXXXXXXXu` constants (turning indirect constant-pool `JSR`s into known static calls and known RAM/MMIO addresses into constant-address accesses!).
4. **Strip Dead Compatibility Baggage & Shrink `.text` by >60%:**
   * Delete all `static const uint8_t k_XXXXXXXX[]` verification byte arrays (~1 MB of dead data) and all `c.at = 0x...;` / `if (c.timing)` emissions in Xbox release builds.
   * Inline all remaining hot opcodes (`DMULS`, `DMULU`, `MUL_L`, `MULS_W`, `MULU_W`, `SHAD`, `SHLD`, `EXTS`, `EXTU`, `SWAP_B`, `SWAP_W`, `XTRCT`) directly in the recompiler instead of out-of-line `sh3_ops.h` helper calls.
   * **Profile-Guided Hot/Cold Code Partitioning:** Use block execution counts from `FAKE_PLAY` profiling to emit the top 99% hot gameplay functions into `blocks_hot.cpp` (`-O3 -march=pentium3 -msse -fomit-frame-pointer -falign-functions=16` in `.text.hot`) and cold init/service/attract functions into `blocks_cold.cpp` (`-Oz` in `.text.cold`), keeping the active frame working set tight inside Coppermine's 16 KB L1i / 128 KB L2 cache.

### 3.2 Native C++20 / SSE1 / MMX Rewrites of Hot Game Subsystems (`engine/game_native/`)

Go beyond instruction-by-instruction translation by **rewriting *Mushihimesama*'s hottest SH-3 engine routines natively for x86/SSE1**, verified frame-for-frame against the SH-3 reference via differential execution:

1. **Frame-Sync, Idle, & Delay Spin Loops (Immediate Win):**
   * Wire and inline exact O(1) cycle/timer fast-forwards for `kIdleLoop` (`0x0C04A2AA` TMU poll reading `0xFFFFFE98`/`A4`/`B0` and RAM flag `0x0C0024D8`) and delay loops (`0x0C056756`, `0x0C056768`), fixing the currently disconnected `FastTmuPollWrapper` in [`dispatch.cpp`](port/nxdk/mushisam/engine/runtime/dispatch.cpp#L208-L233).
2. **Native SIMD Collision & Hit-Testing Pass (`0x0C05A9C0` & `kHitTest` `0x0C05B110`):**
   * Using the verified data layouts from [`spelunkomp/games/mushisam/src/mushisam.h`](spelunkomp/games/mushisam/src/mushisam.h) (`kShipRecord` `0x0C1ED350`, `kShipBox` `0x0C1ED400`, `kBullets` `0x0C1CA084` + `0x62000`/`0x72000`, `kBodyHeaders` `0x0C26C030`/`0x0C23FC10`, `kShotHeaders` `0x0C2681B0`, `kLaserHeaders` `0x0C2D32A0`), rewrite the per-frame bullet-vs-ship and shot/laser-vs-enemy collision sweeps in native C++20 + **Pentium III SSE1 / MMX** (testing 4 bullets/boxes per iteration with branchless masks while preserving exact first-hit ordering and exact side-effects on hit).
3. **Native Bullet Mover, Fixed-Point Math, & Sprite/Blitter List Emitter:**
   * Profile and nativize the active bullet update loop (`0x0C08D410`+), fixed-point `16.16` `sin`/`cos`/`atan2`/`sqrt` table lookups, and the routine that walks active sprites/tiles to emit CV1000 blitter words into RAM—emitting either exact RAM list words (when slowdown/parity checking is active) or pushing `NativeDraw` primitives straight into the NV2A draw stream.
4. **Native Memory & Decompression Intrinsics:**
   * Intercept standard SH-3 `memcpy`/`memset`/word-fill and graphics unpack routines in RAM with `xbox_asm.S` MMX/SSE1 streaming kernels.

---

## 4. Pillar 2: Nativizing All CV1000-B Compatibility Layers

Eliminate register-level board emulation overhead across every peripheral in [`engine/machine/`](port/nxdk/mushisam/engine/machine):

### 4.1 Devirtualized Flat Memory & Direct MMIO Dispatch
* **Remove `class Bus` virtual calls (`Cv1000::Read`, `Write`, `Fetch`, `PortRead`, `PortWrite`):** Bind `SH3` directly to concrete inline CV1000 accessors.
* **Compile-Time Address Routing:** When the AOT recompiler folds constant base addresses (`0x0Cxxxxxx` RAM, `0x180000xx` Blitter, `0x104000xx` Sound, `0xFFFFFExx` / `0xA40000xx` SH-3 on-chip), emit **direct inline loads/stores or peripheral calls** with zero runtime `IsRam(a)` range branching.
* **Fast Unaligned Big-Endian Access:** Ensure `Read16`/`Read32`/`Write16`/`Write32` lower to a single masked base+index load/store + hardware `rolw $8` / `bswapl` (`2` instructions total, zero `NoteCodeWrite()` checks on post-boot data writes).
* **Host-Endian Native Shadow / Direct Struct Views (for Nativized Subsystems):** Provide zero-copy `be32`/`be16` wrapper views over `ram` so native C++ game routines (`game_native/`) read and mutate game state seamlessly without serialization glue.

### 4.2 Native Asset Loader & Pre-Indexed NAND (`mushisam_u2`) Pipeline
* **Routine-Level Loader Hook:** Identify and hook *Mushihimesama*'s NAND page-read + DMA0 transfer function (`SAR0 = 0xB0000000`, `2,112`-byte pages) so stage/boss loads copy requested page ranges straight from the backing file/cache into `ram + dstPhys` in a single native call, bypassing thousands of `0x10000000`/`0x10000001`/`0x10000002` command/address writes and `CHCR0` poll iterations.
* **Aligned Sector I/O & Stage Pre-Warming:** Read `D:\mushisam\mushisam_u2` using sector-aligned (`2,048` / `4,096` B) `FILE_FLAG_NO_BUFFERING` / large `256 KB` sequential read-ahead buffers, and pre-warm Stage `N` NAND pages into a compact RAM ring cache at `kBeginStage` (`0x0C05D4E0`) so **zero synchronous DVD/HDD reads ever occur during active gameplay frames**.

### 4.3 Native Audio Engine on Xbox MCPX Hardware Voices
* **Boot/Offline AMM -> Xbox IMA ADPCM Transcode Cache:** At first boot (cached persistently to `E:\mushisam\audio_adpcm.pak` or `Z:\mushisnd.pak`), decode all valid phrases referenced in `u23` + `u24` (`8 MB` sound ROM) once using fast float/SSE1 `AmmDecoder`, and encode them into **native Xbox 4-bit IMA ADPCM** (or 16-bit PCM for tiny SFX), freeing the `8 MB` `soundRom` vector from retail RAM if desired!
* **Direct MCPX APU Voice Offload (`xbox_mcpx_audio.h`):** Map the YMZ770C's 16 channels onto **16 dedicated hardware MCPX DirectSound / AC'97 voices** (using `nxdk` DirectSound / direct MCPX voice descriptors):
  * Hardware decodes ADPCM, resamples arbitrary phrase rates (`16 kHz` / `22.05 kHz` / `32 kHz` / `44.1 kHz`) to `48 kHz`, applies per-channel volume (`latchVolume2`), stereo pan (`latchPan`), and loop points (`c.loop`) at **zero Pentium III DSP cost**.
  * Run only the tiny YMZ770 register sequencer (`Ymz770::RunSequencer`, ~50 lines of integer state) on the CPU to trigger/stop/modulate hardware voices at exact frame/sub-frame timestamps.
  * Keep software `Ymz770` + `AmmDecoder` under `#ifdef MUSHISAM_REF_AUDIO` for bit-exact host regression tests.

### 4.4 Native Input, RTC-9701, & Persistent FATX Saves
* **Zero-Latency Input Latching:** Poll USB XID gamepads (`xbox_input.h`) directly at the exact cycle *Mushihimesama* reads JAMMA Ports C/D/L (`0xA4000124`/`126`/`134`), minimizing input-to-photon latency to **< 1 frame**.
* **Native FATX EEPROM & High-Score Persistence:** Persist the 128-Byte `Rtc9701` EEPROM (`rtc.eeprom`) and any programmed NAND score/config blocks atomically to `E:\UDATA\4356314B\eeprom.bin` on modification, and feed real Xbox clock time into `Rtc9701`.

### 4.5 Fused Native Blitter & Authentic CV1000 Slowdown Model
* **Zero-Copy Upload & Draw Streaming:** Fuse [`Blitter::Upload`](port/nxdk/mushisam/engine/machine/blitter.cpp#L179) and [`Blitter::DrawOp`](port/nxdk/mushisam/engine/machine/blitter.cpp#L240) directly into `Nv2aCv1kRenderer`:
  * `Upload` (`0x2000`) byte-swaps (`pshufw`/`pshuflw` or `rolw $8` MMX burst) straight from `ram` into the destination NV2A VRAM texture page—eliminating `NativeList::pixels` heap copies completely.
  * `DrawOp` (`0x1000`) emits compact hardware vertices/quads directly into the write-combined NV2A DMA buffer.
* **Toggleable Cycle-Calibrated Arcade Slowdown (`SlowdownMode::BoardAccurate` vs `Unlocked`):**
  * Preserve `Blitter` busy-time accounting (`m_vramClocks` via branchless `VramRows` lookup table + `lastStolenCpuCycles` + `IRQ1` scheduling) and optional lightweight per-block SH-3 SDRAM contention weights so *Mushihimesama*'s iconic Stage 3–5 bullet-hell slowdown matches real CV1000-B hardware **without** the 600 KB `Sh3Paths` interpreter overhead, while host Xbox frame presentation stays locked at a tear-free 60 Hz.

---

## 5. Pillar 3: Bare-Metal NV2A GPU Hardware Registers & Pipelines

Re-architect [`port/nxdk/mushisam/nv2a_cv1k_renderer.h`](port/nxdk/mushisam/nv2a_cv1k_renderer.h) to exploit the NV2A (`NV097` / `KELVIN` / `PGRAPH` / `NV039`) directly:

### 5.1 GPU-Resident CV1000 VRAM Architecture (Zero `Z:\` HDD Swapping)
* **Eliminate CPU `pageData_` & Synchronous Disk Swap:** Remove the 14 MB CPU `std::vector<uint16_t>` page pool and `SwapOutPage`/`SwapInPage` (`Z:\mushivrm.bin`), which caused the `31.9 ms` render spike in `perf_dump.bin`.
* **Multi-Page Contiguous NV2A `A1R5G5B5` Sheet Pool + Persistent Sprite Atlas:**
  * Allocate a dedicated **16–20 MB contiguous Write-Combined physical pool** (`MmAllocateContiguousMemoryEx(..., PAGE_READWRITE | PAGE_WRITECOMBINE)`) divided into NV2A-addressable `2048×1024` / `1024×1024` `LU_IMAGE_A1R5G5B5` texture banks (4 texture stages available simultaneously on NV2A!).
  * Map CV1000 `8192×4096` VRAM regions (tile/sprite sheets uploaded via `0x2000`) directly into GPU-resident texture banks with generation tags—static stage graphics uploaded at stage start are **never copied by the CPU again** for the rest of the stage!
  * For dynamic off-screen VRAM-to-VRAM render-target blits (if the game composites into an off-screen VRAM region other than `clipX, clipY` and later draws from it), execute **GPU-side DMA copies (`NV039_MEMORY_TO_MEMORY_FORMAT`)** or render-to-texture passes on `PGRAPH` instead of CPU readbacks.

### 5.2 Programmable NV2A Kelvin Vertex Shader (`NV097_LOAD_TRANSFORM_PROGRAM`)
* **Switch `NV097_SET_TRANSFORM_EXECUTION_MODE` from Fixed-Function (`0`) to Programmable (`PROGRAM`):**
  * Write and assemble a compact **NV2A Kelvin vertex shader** (~8–12 microcode instructions) loaded once at `Init()`.
  * Upload per-frame/per-mode constants (`scrX`, `scrY`, viewport scale/offset, YOKO 90° CCW vs TATE 0° basis vectors, atlas `1/W`/`1/H` or linear texcoord bias) via `NV097_SET_TRANSFORM_CONSTANT_LOAD` (`c[0..3]`).
* **Compact 12-Byte Hardware Vertex Format (`60%` Less FSB Bandwidth):**
  * Replace the current 20-byte float `Nv2aVertex` (`x, y, u, v, diffuse` = `80 bytes/quad` computed on the PIII FPU) with a **12-byte integer vertex** (`int16_t x, y; int16_t u, v; uint32_t packedTint;` = `48 bytes/quad`) configured via `NV097_SET_VERTEX_DATA_ARRAY_FORMAT`:
    * Attribute `0` (`POS`): `TYPE_S32K`, `SIZE_2`, stride `12`
    * Attribute `3` (`DIFFUSE`): `TYPE_UB_D3D`, `SIZE_4`, stride `12`
    * Attribute `9` (`TEX0`): `TYPE_S32K`, `SIZE_2`, stride `12`
  * Pair with a **pre-built static 16-bit index buffer** (`0,1,2, 0,2,3 ...` or `NV097_DRAW_ARRAYS` `OP_QUADS`) in contiguous memory for maximum Kelvin vertex-cache reuse.

### 5.3 Exact CV1000 5-Bit Color & `2x` Overdrive Tint in NV2A Register Combiners
* **Fix the `Scale5To8` Hit-Flash Clamping Bug:**
  * On real CV1000 hardware, tint channels are **6-bit (`0..63`)** where `32` (`0x20`) = `1.0x` unchanged (`x * 32 / 31` clamped) and `33..63` = **`1.0x..2.0x` brightness overdrive** (used constantly in *Mushihimesama* when enemies/bosses flash bright white/yellow/red under laser fire!). Currently `Scale5To8` clamps `v >= 31` to `255`, destroying all `>1.0x` hit flashes!
  * **Hardware Register Combiner Fix:** Encode tint channels as `min(tint * 4, 255)` (`tint = 32 -> 128 = 0.502`), multiply `TEX0.rgb * DIFFUSE.rgb` in **Stage 0 Combiner (`ICW`)**, and set **Stage 0 Output (`OCW`) to `NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY1` (`SCALE_BY_TWO`, clamped `0..1`)**! That gives hardware-exact `0x..2x` saturated RGB overdrive in a **single NV2A combiner cycle** at zero CPU cost!
* **Exact 64-Mode CV1000 Hardware Blending & Batch Coalescing:**
  * Keep `NV097_SET_ALPHA_TEST_ENABLE` (`GREATER, 0`) on `A1R5G5B5` bit 15 for `kTransparent` draws.
  * Coalesce consecutive draws—and optionally reorder **spatially disjoint** draws (tracked via fast 32×32 screen-tile occupancy bitmasks, `20` `uint32_t`s for `320×240`)—that share `(textureBank, alphaTest, blendOn, sFactor, dFactor, blendColor)` to slash `pb_push1` state switches per frame by **3–5×**.
* **Hardware Scissor, Scanline Pass, & Training Overlays:**
  * Enforce the CV1000 `320×240` viewport window in hardware via `NV097_SET_SURFACE_CLIP_HORIZONTAL` / `VERTICAL` + `NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_HORIZ(0)` / `VERT(0)`.
  * Optional **zero-extra-pass CRT scanline filter** using **Texture Stage 1** (a tiny `1×2` or `2×2` repeating `A8R8G8B8` scanline/grille mask texture modulated in **Register Combiner Stage 1**) + optional NV2A `OP_LINE_LOOP`/`OP_QUADS` hitbox overlay (`HitBoxes` from [`game.cpp`](spelunkomp/games/mushisam/src/game.cpp)) toggled via controller combo.

### 5.4 Hardware Kelvin Semaphore / PCRTC Async Pipelining
* Replace CPU busy-spin loops (`while (pb_busy()) {}` and `while (pb_finished()) {}` in [`main.cpp`](port/nxdk/mushisam/main.cpp#L210-L220)) with **triple-buffered pushbuffer/VB slots** gated by `NV097_SET_SEMAPHORE_OFFSET` + `NV097_BACK_END_WRITE_SEMAPHORE_RELEASE` and `pb_wait_for_vbl()`, eliminating MMIO bus contention while `PGRAPH` is rasterizing.

---

## 6. Pillar 4: Profiling, Telemetry & Verification Infrastructure

Every architectural change must be driven and validated by hardware telemetry—never guesswork.

### 6.1 Multi-Level Xbox & Host Profiler (`xbox_profiler.h` v2)
1. **Sub-Microsecond Subsystem Timers (`rdtsc` + MCPX `KeQueryPerformanceCounter`):**
   * Break every frame into: `sh3HotUs`, `sh3NativeHleUs`, `blitterParseUs`, `audioSeqUs`, `nandLoadUs`, `nv2aBuildUs`, `nv2aSubmitUs`, `gpuBusyUs`, and `vblWaitUs`, with rolling averages, `p95`/`p99`/`max` watermarks, and per-stage (`Stage 1..5` + Bosses) buckets in `g_xboxPerf` (`0x50455246` `"PERF"`).
2. **Pentium III Ring-0 Hardware Performance Counters (`rdpmc` on Real Xbox):**
   * Add optional Ring-0 MSR configuration (`wrmsr` `0x186`/`0x187`, `rdpmc` `0`/`1`) on Coppermine silicon to record per-frame:
     * **Mode A (Frontend/Code):** `IFU_IFETCH_MISS` (`0x81`, L1i misses) + `ITLB_MISS` (`0x85`)
     * **Mode B (Data/L2):** `DCU_LINES_IN` (`0x45`, L1d fills) + `L2_LINES_IN` (`0x24`, L2 misses to DDR RAM)
     * **Mode C (Control/SIMD):** `BR_MISS_PRED_RETIRED` (`0xC5`) + `INST_RETIRED` (`0xC0`, true IPC calculation!)
3. **NV2A GPU Hardware Execution Timestamps:**
   * Bracket each frame's pushbuffer with `NV097_BACK_END_WRITE_SEMAPHORE_RELEASE` / notifier writes or DPC VBlank timestamps to measure **true `PGRAPH` GPU rasterization time** independently of CPU submission time, alongside `drawCalls`, `stateChanges`, `quadsSubmitted`, `vramBytesUploaded`, and `MmQueryStatistics()` free physical pages.
4. **SH-3 Function Hotspot Profiler (Host & Xemu):**
   * Instrument `recomp_sh3` entry/exit counters + `rdtsc` sampling to emit a ranked table (`top_functions.txt`) of every SH-3 function by cumulative cycles, call count, and I-cache footprint—directly guiding Phase 2 native C++/SSE1 rewrites.
5. **Live On-Screen HUD Overlay:**
   * Toggleable lightweight NV2A bitmap-quad debug HUD (**Start + R3** or **L3 + R3**) displaying live FPS, `CPU / GPU / Audio / Blit` ms bars, RAM free MB, Stage/Bullet count, and Slowdown indicator without touching `pb_print` software text buffers.

### 6.2 Automated 5-Stage `FAKE_PLAY` Benchmark & Differential Parity Suite
* **Deterministic 5-Stage Stress Harness (`mushisam_fake_play.h` + Stage Warp):**
  * Extend [`mushisam_fake_play.h`](port/nxdk/mushisam/mushisam_fake_play.h) using [`mushisam.h`](spelunkomp/games/mushisam/src/mushisam.h) stage-start facts (`kNewGame = 0x0C05BCB0`, `kBeginStage = 0x0C05D4E0`, `kBullets = 0x0C1CA084`) to deterministically sweep **all 5 stages + midboss/boss bullet-hell peaks** (`Maniac` / `Ultra` modes!) in a single automated Xemu/host benchmark run (`make FAKE_PLAY=1 STAGE_SWEEP=1`), dumping `perf_dump.bin` via GDB.
* **Differential Reference Oracle (`tests/mushisam_parity_test.cpp`):**
  * Run the AOT whole-function recompiler + HLE native rewrites side-by-side against the reference `SH3` block interpreter + software `Blitter::Picture()`:
    * **Logic Gate:** `100%` identical `StateHash()` (or documented exact equivalence for float/audio-decoupled paths) across `10,000+` frames of scripted play across Stages 1–5.
    * **Visual Gate:** Compare NV2A / reference `A1R5G5B5` + `2x` overdrive tint output against `Blitter::Picture()`, verifying `0` unintended pixel/clipping/flip/blend regressions (and confirming `tint > 32` overdrive hit-flashes render accurately!).

---

## 7. Memory Budget Plan (Stock 64 MB Retail Xbox)

| Allocation | Current Size | Post-Rebuild Size | Notes |
| :--- | :---: | :---: | :--- |
| **Xbox Kernel + OS Overhead** | ~6.0 MB | ~6.0 MB | Fixed retail kernel reservation |
| **`.xbe` Binary (`.text` + `.rdata` + `.data`)** | ~10.5 MB | **~4.5 MB** | Whole-function AOT + stripped `k_XXXX[]` arrays + `.text.cold` `-Oz` |
| **CV1000-B Main RAM (`board.ram`)** | 8.0 MB | **8.0 MB** | Exact `0x0C000000` 8 MB SH-3 RAM |
| **Program Flash (`mushisam_u4`)** | 2.0 MB | **0.0–0.5 MB** | Freed after boot copy into `ram` (only tiny boot vector stub needed) |
| **Sound Assets (`u23`+`u24` -> Xbox ADPCM)** | 8.0 MB | **~2.5 MB** | Transcoded 4-bit IMA ADPCM voice bank; `8 MB` raw `soundRom` freed |
| **NAND Stage Read-Ahead / Page Cache** | 0.7 MB | **1.5 MB** | Sector-aligned pre-warmed stage cache (`0` mid-stage stalls) |
| **NV2A Framebuffers + Pushbuffer + DMA VBs** | ~4.5 MB | **~4.2 MB** | `640×480×32` triple-buffer + `12 B/vert` triple DMA VB |
| **GPU-Resident CV1000 `A1R5G5B5` VRAM Pool** | 16.0 MB (14M CPU + 2M GPU) | **18.0 MB (GPU only)** | Single unified WC physical texture pool; **zero CPU mirror, zero HDD swap** |
| **Total Peak Retail Footprint** | **~55.7 MB** (fragile) | **~45.2 MB** | **~18.8 MB free headroom** on unmodded 64 MB retail consoles! |

*(On **128 MB** modded/dev Xboxes detected via `MmQueryStatistics()`, automatically unlock a full 64 MB `8192×4096` linear VRAM mirror + instant save-state / rewind ring buffers.)*

---

## 8. Phased Execution Roadmap & Strict Exit Gates

### Phase 0: Telemetry v2, Host Parity Harness & Quick Wiring Fixes
* **Scope:**
  1. Upgrade [`xbox_profiler.h`](port/nxdk/mushisam/xbox_profiler.h) with fine-grained subsystem timers, Ring-0 `rdpmc` hooks, NV2A GPU semaphore timestamps, `MmQueryStatistics()` RAM tracking, and SH-3 per-function cycle profiling.
  2. Build the host differential parity & benchmark runner (`tests/mushisam_parity_test.cpp`) + 5-stage `MushisamFakePlay` sweep (`Ultra` mode bullet stress).
  3. Wire up the currently disconnected `FastTmuPollWrapper` (`0x0C04A2AA`) and `FastDelay1/2Wrapper` (`0x0C056756`/`68`) in `InstallTranslations()` and free `board.program` (`2 MB`) post-boot.
* **Exit Gate:** Automated host parity check passes (`100%` `StateHash` match over 5,000 frames); baseline `top_functions.txt` hotspot table and 5-stage `perf_dump.bin` captured.

### Phase 1: Whole-Binary AOT Function Recompiler (`tools/recomp_sh3.py`)
* **Scope:**
  1. Implement CFG + call-graph discovery over post-boot SH-3 RAM + dynamic indirect target logs across all 5 stages/modes.
  2. Emit whole-function C++20 routines with local register promotion (`R0..R15`, `T`, `PR`, `MACL`/`MACH`, `cycles`), compile-time PC-relative literal folding (`MOV.L/W @(disp,PC)`), intra-function `goto` branches, direct C++ calls for static targets, inlined `sh3_ops`, and zero `k_XXXX[]` arrays / `c.at` stores.
  3. Split generated code into `blocks_hot.cpp` (`-O3`) and `blocks_cold.cpp` (`-Oz`).
* **Exit Gate:** `StateHash()` matches reference 100%; block dispatches/frame drop by **>85%** (`< 3,000/frame`); `.xbe` size drops below **5.5 MB**; `profSh3Tsc` drops below **`6,000 µs` avg**.

### Phase 2: Native C++20 / SSE1 Game Subsystem Rewrites (`engine/game_native/`)
* **Scope:**
  1. Rewrite the top profiler-ranked SH-3 routines in native Pentium III C++20 + SSE1/MMX: collision & hit-testing (`0x0C05A9C0`, `0x0C05B110`), bullet update/spawn (`0x0C08D410`+), fixed-point trig/math helpers, sprite/blitter list builders, and RAM copy/clear/unpack routines.
  2. Validate each rewritten function individually via shadow/differential state comparison against the SH-3 code it replaces.
* **Exit Gate:** `0` gameplay/collision divergences in `mushisam_parity_test` across all 5 stages (`Original`, `Maniac`, `Ultra`); `profSh3Tsc` + `sh3NativeHleUs` combined drops to **`< 3,500 µs` avg** (`< 6,000 µs` worst bullet-hell peak).

### Phase 3: Nativizing CV1000-B Compatibility Layers (`engine/native_cv1k/`)
* **Scope:**
  1. Devirtualize `Bus` and inline compile-time memory/MMIO routing in `SH3` + `Cv1000`.
  2. Hook NAND + `DMAC0` loader routines for direct, sector-aligned, stage-prewarmed FATX asset reads (`0` mid-stage I/O stalls).
  3. Implement boot-cached `u23`+`u24` AMM -> Xbox IMA ADPCM transcoding + **16-voice hardware MCPX APU backend** (`xbox_mcpx_audio.h`), freeing the `8 MB` `soundRom` buffer.
  4. Wire cycle-exact USB XID input latching, RTC-9701 system time, FATX `E:\UDATA` EEPROM persistence, and toggleable integer LUT arcade slowdown (`BoardAccurate` vs `Unlocked`).
* **Exit Gate:** Audio CPU cost `< 100 µs/frame` with clean 48 kHz polyphonic BGM + SFX; `0` frame spikes during stage/boss transitions; total RAM usage `< 48 MB`.

### Phase 4: Bare-Metal NV2A Kelvin & Register Combiner Pipeline (`nv2a_cv1k_renderer.*`)
* **Scope:**
  1. Replace the 14 MB CPU `pageData_` pool + `Z:\` swap file with persistent **GPU-resident `A1R5G5B5` texture banks** + GPU `NV039` DMA blits for dynamic VRAM-to-VRAM copies.
  2. Fuse `Blitter::Upload` (MMX byte-swap straight to WC VRAM) and `Blitter::DrawOp` (direct 12-byte compact vertex emission, zero `NativeList::pixels`).
  3. Implement the **NV2A Kelvin programmable vertex shader** (`POS` + `TEX0` `int16x2` transform/rotation/clip), **Register Combiner `SHIFTLEFTBY1` `2x` overdrive tint fix**, disjoint-tile batch coalescing, hardware scissor, optional Stage-1 CRT scanlines, NV2A hitbox overlay, and **Kelvin semaphore async triple-buffering**.
* **Exit Gate:** `renderUs < 800 µs` avg (`< 1,800 µs` peak), `0` disk swaps, `0` pixel errors (with verified `2x` hit-flash overdrive), and tear-free 60 Hz YOKO/TATE output in Xemu VRAM captures.

### Phase 5: Whole-Game Stress Validation & Hardware Sign-Off
* **Scope:**
  1. Run a continuous **30,000-frame automated `FAKE_PLAY` `Ultra` sweep** across Stages 1–5 + True Last Boss loop transitions in Xemu (and real 64 MB Xbox hardware with `rdpmc` counters enabled).
  2. Verify controller hotkeys (YOKO/TATE, Nearest/Bilinear, Integer/Aspect, Scanlines, Hitboxes, Board-Accurate Slowdown toggle, Debug HUD, EEPROM save/reload across reboot).
* **Exit Gate:** **`0` host lag frames (`> 16,662 µs`)** across all 5 stages, `< 48 MB` peak RAM on 64 MB retail config, clean audio, and deterministic game logic parity.

---

## 9. Build, Benchmark & Verification Commands

* **Build Xbox `.xbe` & XISO (`nxdk` via MSYS2 MinGW64):**
  ```bash
  taskkill /F /IM xemu.exe 2>/dev/null || true
  MSYSTEM=MINGW64 bash -lc 'cd port/nxdk/mushisam && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make -j$(nproc)'
  ```

* **Launch Automated 5-Stage Benchmark in Xemu (with GDB stub):**
  ```bash
  MSYSTEM=MINGW64 bash -lc 'cd port/nxdk/mushisam && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make FAKE_PLAY=1 STAGE_SWEEP=1 -j$(nproc)'
  "${XEMU_BIN:-xemu}" -dvd_path port/nxdk/mushisam/mushisam-cv1000-xbox.iso -s > xemu_stdio.log 2>&1
  ```

* **Dump Live `g_xboxPerf` Telemetry (`perf_dump.bin`) via GDB:**
  ```bash
  MSYSTEM=MINGW64 bash -lc 'gdb -batch \
    -ex "file port/nxdk/mushisam/main.exe" \
    -ex "target remote localhost:1234" \
    -ex "dump binary memory perf_dump.bin &spelunk_xbox::g_xboxPerf ((char*)&spelunk_xbox::g_xboxPerf + sizeof(spelunk_xbox::g_xboxPerf))" \
    -ex "detach" -ex "quit"'
  ```

---

## 10. Definition of Done

The rebuild is complete when **all** of the following hold simultaneously:
1. **Native Binary Execution:** `mushisam` runs from whole-function AOT-compiled C++20 + native SSE1/MMX game subsystem rewrites with `0` interpreted blocks during gameplay, `< 3,000` block/function dispatches per frame, and `< 3.5 ms` average CPU game-logic time (`< 6.0 ms` peak in `Ultra` Stage 3–5 bullet hell).
2. **Nativized CV1000 Layers:** Virtual `Bus` dispatch, byte-by-byte NAND DMA polling, CPU AMM audio filterbank spikes, and serial EEPROM bit-banging are replaced by direct inline memory/IO routing, pre-warmed native FATX streaming (`0` gameplay I/O stalls), 16-voice MCPX APU hardware audio (`< 0.1 ms` CPU), and persistent `E:\UDATA` saves.
3. **Optimal NV2A Hardware Pipeline:** All CV1000 VRAM sheets reside in contiguous NV2A `A1R5G5B5` physical memory (`0` `Z:\` swap stalls, `0` `NativeList::pixels` CPU copies), vertices are transformed by a programmable NV2A Kelvin vertex shader (`12 B/vert`), `0x..2x` hit-flash overdrive tints execute in NV2A Register Combiners, and CPU/GPU frames overlap via hardware Kelvin semaphores (`< 0.8 ms` avg CPU render submit).
4. **Verified Zero-Lag Retail Stability & Accuracy:** Sustained **60.0184 Hz / 60 FPS (`0` host lag frames > `16.662 ms`)** across a full 5-stage `Ultra` stress run within **`<= 48 MB` peak RAM** on a stock 64 MB Original Xbox, with automated `StateHash()` and visual parity checks passing.
