# Mushihimesama (`port/nxdk/mushisam`) — Original Xbox Optimization Roadmap

## Context & Constraints
* **Target Hardware:** Original Xbox (733 MHz Pentium III Coppermine, 16 KB L1i / 16 KB L1d / 128 KB L2 cache, SSE1 only — **no SSE2**, 64 MB unified RAM, NV2A GPU).
* **Goal:** Reach a stable **60 FPS (16.66 ms/frame)** on real hardware without breaking game logic or exceeding the 64 MB RAM budget.
* **Rule:** Do not rewrite the SH-3 static recompiler or discard the repo. Optimize the runtime, audio, blitter bookkeeping, and toolchain flags in strict phases, measuring with `xbox_profiler.h` (`rdtsc`) after every phase.

---

## Target Frame Budget (16.66 ms)

| Subsystem | Target Time | Primary Optimization Lever |
| :--- | :--- | :--- |
| **SH-3 Recompiled Logic** | **4–6 ms** | Confirm `Mode::Native`, strip `bus_timing.h`, `-Os`/`-O2 -march=pentium3` |
| **Blitter List + NV2A Submit** | **3–5 ms** | Skip `VramRows()` timing math, pre-reserve `NativeList`, batch NV2A draws |
| **YMZ770C / AMM Audio** | **1–2 ms** (or **<0.5 ms**) | Convert `AmmDecoder` to `float` (SSE1) / fixed-point, or offline ADPCM |
| **NAND Streaming + Overhead** | **<1 ms** | Expand `Nand::kCacheSlots` using reclaimed RAM |
| **Headroom (Spikes / VSync)** | **3–6 ms** | Validate on real Xbox silicon (not just `xemu`) |

---

## Phase 0: Baseline & Sanity Check
**Objective:** Establish an exact 4-way frame-time breakdown and confirm static recompilation is actually executing.

### Tasks
1. **Verify native block linking:**
   * Inspect `port/nxdk/mushisam/Makefile` and confirm generated SH-3 block `.cpp` files are linked instead of `engine/runtime/no_blocks.cpp`.
   * Inspect `port/nxdk/mushisam/main.cpp` and confirm `cpu.mode` is set to `SH3::Mode::Native` (default in `engine/machine/sh3.h` is `Mode::Interpreter`).
   * Log `cpu.nativeBlocks` vs. `cpu.interpretedBlocks` after 60 frames.
2. **Capture a 4-way `rdtsc` profile** (using `xbox_profiler.h`):
   * `cpu.Run()`
   * `sound.RunTo()`
   * `Blitter::Run()` (`NativeList` construction)
   * `nv2a_cv1k_renderer.h` submit + present
3. **Run an audio-off A/B test:**
   * Temporarily guard `sound.RunTo(...)` in `engine/machine/cv1000.cpp` and record the FPS/frame-time delta.

### Exit Criteria
* `cpu.interpretedBlocks` is near zero during gameplay.
* Baseline milliseconds per subsystem are recorded before any optimization commits.

---

## Phase 1: High-Impact Quick Wins
**Objective:** Eliminate the three largest known CPU and RAM drains in the current codebase.

### 1.1 Fix `AmmDecoder` 64-bit `double` Math (`engine/machine/amm_decoder.h`, `amm_decoder.cpp`)
* **Problem:** `AmmDecoder::Synthesise()` runs an MPEG-1 Layer II synthesis filterbank (`32 * bandsUsed` + `32 * 16` MACs per 32 samples, per channel) in 64-bit `double`. Coppermine has no SSE2, forcing slow scalar x87 FPU instructions.
* **Action (Option A — Fast in-place):**
  * Change `m_history`, `Tables::scale`, `Tables::cosine`, `Tables::tap`, `Tables::Q::c`, and `Synthesise()` / `DecodeFrame()` locals from `double` to `float`.
  * Compile `amm_decoder.cpp` with `-O2 -march=pentium3 -msse -mfpmath=sse -ffast-math` (or vectorize the inner 32-element dot products with SSE1 intrinsics).
* **Action (Option B — Best long-term):**
  * Convert `Synthesise()` to 32-bit fixed-point `int32_t`, **or** pre-decode the 8 MB `soundRom` offline into Xbox ADPCM/PCM and offload mixing to the Xbox APU.

### 1.2 Strip `bus_timing.h` on Xbox (`engine/machine/sh3.h`, `bus_timing.h`)
* **Problem:** Even when `SH3::timing == false`, `Sh3Paths timingPaths` is a direct member of `SH3`, allocating 8,192 `Memo` structs and 131,072 `uint32_t` write-tracking entries in RAM at startup. Additionally, `Read8/16/32` and `Write8/16/32` check `if (timing)` and `if (watching)` on every memory access.
* **Action:**
  * Introduce a build macro `-DXBOX_FAST_CPU`.
  * Under `#ifdef XBOX_FAST_CPU`, omit `Sh3Timing memoryTiming` and `Sh3Paths timingPaths` (and `TimeAccess`/`TimeBranch`/`PollLoop`) from `SH3`, and compile out `if (timing)` and `if (watching)` branches in `Read8/16/32` and `Write8/16/32`.
  * Stub `timingPaths.PutBack(...)` / `timingPaths.clock` references in `cv1000.cpp` under the same macro.

### 1.3 Trim `Blitter` Bookkeeping & Allocations (`engine/machine/blitter.cpp`, `blitter.h`)
* **Problem:** Even with `XBOX_NO_SW_VRAM` enabled, `Blitter::DrawOp()` executes `VramRows()` and 64-bit `m_vramClocks` slowdown calculations on every sprite draw, and `NativeList` vectors can reallocate mid-frame.
* **Action:**
  * Under `#ifdef XBOX_NO_SW_VRAM` (or `#ifdef XBOX_FAST_CPU`), bypass `VramRows()` and `m_vramClocks` timing math in `Blitter::DrawOp()` unless board-accurate slowdown is explicitly toggled on.
  * Pre-`reserve()` `native->steps`, `native->draws`, `native->uploads`, and `native->pixels` once at initialization so `Clear()` retains capacity with zero per-frame heap allocations.

### Exit Criteria
* Re-run the Phase 0 4-way profile.
* Verify measurable drops in `sound.RunTo()`, `cpu.Run()`, `Blitter::Run()`, and total RAM usage.

---

## Phase 2: SH-3 Recomp & Toolchain Tuning
**Objective:** Minimize I-cache thrashing on Coppermine's 16 KB L1i / 128 KB L2 cache and remove remaining per-write overhead.

### Tasks
1. **Split compiler flags (`port/nxdk/mushisam/Makefile`):**
   * **Runtime / audio / renderer TUs** (`cv1000.cpp`, `blitter.cpp`, `amm_decoder.cpp`, `ymz770.cpp`, `main.cpp`): `-O2 -march=pentium3 -msse -mfpmath=sse`.
   * **Generated SH-3 block TUs:** `-Os -march=pentium3` to keep code footprint tight in L1i/L2 (benchmark `-Os` vs. `-O2` on real hardware).
   * Ensure `-ffreestanding` / `-fno-builtin` are **not** preventing `std::memcpy` + `__builtin_bswap16/32` in `SH3::Read16/32` and `Write16/32` from lowering to single `mov`/`bswap` instructions.
   * Enable ThinLTO if supported by the `nxdk` clang/lld toolchain.
2. **Gate `NoteCodeWrite()` (`engine/machine/sh3.h`):**
   * `Write8/16/32` calls `NoteCodeWrite(a)` on every RAM write. If Mushihimesama only writes code during boot/stage load, gate `NoteCodeWrite()` behind a flag or omit it when no code pages are dirty.

### Exit Criteria
* `cpu.Run()` consistently fits within the **4–6 ms** target budget.

---

## Phase 3: NV2A Renderer & NAND Streaming Polish
**Objective:** Eliminate GPU submission stalls and disk I/O hitches.

### Tasks
1. **Audit `port/nxdk/mushisam/nv2a_cv1k_renderer.h`:**
   * Verify `NativeUpload` does not perform redundant per-pixel CPU format conversion or swizzling every frame for static textures.
   * Verify `NativeDraw` quads are batched into a single dynamic pushbuffer/vertex stream without mid-frame CPU/GPU sync stalls.
2. **Tune `Nand` streaming cache (`engine/machine/nand.h`):**
   * Using RAM reclaimed from stripping `Sh3Paths` in Phase 1.2, increase `Nand::kCacheSlots` from `32` (~66 KB) to `256`–`512` (~0.5–1 MB) to reduce `fseek`/`fread` misses during stage transitions.
   * Confirm zero synchronous disk reads occur during active gameplay frames.

### Exit Criteria
* Full 60 FPS (16.66 ms) with headroom on real Original Xbox hardware.
* No audio crackles, frame-time spikes, or out-of-memory crashes across stage transitions.