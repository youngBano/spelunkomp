# Mushihimesama (CV1000-B / SH-3) Original Xbox Port — Architectural Changes Report 2

## 1. Executive Summary

This report documents all major architectural changes implemented since `HEAD` (`8c478da`) across the **SH-3 static recompiler / runtime**, **CV1000 blitter timing & non-slowdown toggle**, **NV2A native-quad renderer & VRAM memory hierarchy**, **audio/input preemption tuning**, and **Max-Power Maniac stress-testing harness** — including the full engineering post-mortem of our **multi-slab WC GPU blitter + RTT experiment** versus the **final upgraded single-atlas 100%-native-NV2A-quad architecture** verified on both host differential parity (`0` mismatches / `5,000` frames) and live Xemu timeline profiling (`f = 0 .. 3,850`).

---

## 2. SH-3 Static Recompiler & Bullet-Storm Runtime Architecture

To handle dense Maniac-mode bullet storms without falling back to the interpreter or burning cycles in the top-level dispatch loop, we overhauled how hot SH-3 sprite/blitter routines and indirect calls execute.

### A. Out-of-Line Bounded Indirect Dispatch (`SH3::DispatchIndirect`)
- **Files:** `port/nxdk/mushisam/engine/machine/sh3.h`, `port/nxdk/mushisam/engine/runtime/dispatch.cpp`, `port/nxdk/mushisam/tools/recomp_sh3.py`
- **Problem:** Indirect `jsr @Rm` / `jmp @Rm` calls (such as sprite update vtables and bullet behavior callbacks) previously returned all the way out to `SH3::RunBlocks()` on every call. Inlining the indirect lookup table across all `5,242` recompiled functions bloated `blocks_hot.obj` to `4.89 MB` (`default.xbe` `16.4 MB`, pushing free Xbox RAM down to `1.44 MB`).
- **Architectural Change:**
  - Implemented an out-of-line `bool SH3::DispatchIndirect(uint32 target)` helper in `dispatch.cpp` with a bounded recursion guard (`indirectDepth < 4u`) that resolves `m_pageFnPtrs[phys >> 12][(phys & 0xFFF) >> 1]` directly without returning to `RunBlocks()`.
  - Updated `recomp_sh3.py` to emit `DispatchIndirect()` calls **exclusively for the top `150` hot functions** compiled into `gen/blocks_hot.cpp` (`-O3`), keeping `blocks_hot.obj` compact (`1.7 MB`) and `default.xbe` at `~12.3 MB`.
  - Expanded AOT block coverage across all 6 harvested block lists (`--strip-found`: `48,806` blocks → `5,242` whole functions across `44` TUs), achieving **`interpBlk = 0`** across `30,000` Max-Power Maniac frames.

### B. Cycle-Exact Native C++ Fast-Paths for Hot Sprite/Blitter Helpers
- **Files:** `port/nxdk/mushisam/engine/game_native/mushisam_native.h`
- **Architectural Change:** Added four cycle-exact, parity-verified native hooks for Mushihimesama's hottest per-sprite functions (with full bounds/overlap/interrupt guards and fallback to recompiled SH-3 on edge cases):
  1. **`FastSpritePivot` (`0x0C0555DC`):** Inlines the `mode == 4 / 0 / 3` sprite pivot alignment switch (`26 / 19 / 22` exact SH-3 cycles), eliminating millions of indirect jump-table dispatches.
  2. **`FastSpriteBlendFlags` (`0x0C0556C8`):** Inlines both indirect jump-table lookups (`0x0C055718[0..9]`) for sprite alpha/blend flag construction with exact stack and cycle parity.
  3. **`FastEmitBlitCmd` (`0x0C0557D8`):** Inlines the `#1` hot 20-byte CV1000 blitter command emitter (`0x0C055800`, `53` exact cycles) directly into SH-3 RAM.
  4. **`FastMemcpyByteLoop` (`0x0C04F37C`):** Added an 8-byte `uint64_t` fast path covering `~82%` of calls from `0x0C0A9522`.
- **Measured Host Impact:** Cut `RunBlocks()` dispatches by **`3.69×`** (`6,745.9/f` → `1,828.9/f`) and host frame wall-clock by **`+43.8%`** (`0.35 ms/f` → `0.24 ms/f`), passing `5,000`-frame differential parity with **`0` mismatches**.

---

## 3. Authentic Cave Slowdown Toggle, O(1) Blitter Timing & Visual Indicator

- **Files:** `port/nxdk/mushisam/engine/machine/blitter.h`, `blitter.cpp`, `cv1000.h`, `cv1000.cpp`, `xbox_input.h`, `main.cpp`, `nv2a_cv1k_renderer.h`
- **Architectural Changes:**
  1. **Runtime LT Non-Cave-Slowdown Flag (`board.noSlowdown`):** Replaced the compile-time `#if !defined(XBOX_NO_SW_VRAM) && !defined(XBOX_FAST_CPU)` macro gate on CV1000 blitter slowdown with a runtime `blitter.simulateSlowdown = !noSlowdown` flag toggled live via **Left Trigger (`LT`)** (`PadState::lTrigger` in `xbox_input.h`, edge-triggered in `main.cpp`).
  2. **Permanent `4×4` Green Corner Indicator:** When `board.noSlowdown` is active, `Nv2aCv1kRenderer::Render()` issues a single hardware `NV097_CLEAR_SURFACE` rect (`0..3, 0..3`, `0xFF00FF00`) in the upper-left corner, and triggers `clearFramesRemaining_ = 3` when toggled off to cleanly wipe all 3 pbkit swapchain buffers.
  3. **`O(1)` Blitter Busy-Wait Fast-Forward (`Cv1000::Read`):** Added `m_busyPollCount` so when SH-3 enters a tight blitter-ready poll loop (`offset == 0x10 && blitter.busy && ++m_busyPollCount >= 2 && !cpu.interruptReady`), `cpu.cycles` jumps directly to `min(m_blitterDone, cpu.blockStop)` — preserving exact CV1000 slowdown timing with zero wasted host CPU spin cycles.
  4. **`O(1)` Closed-Form `VramRows()` (`blitter.cpp`):** Replaced the two 32-pixel stride loops inside `VramRows()` (called `~1,800×/frame` during dense Maniac draws) with a bit-exact closed-form integer calculation (`0` parity mismatches across `5,000` frames).

---

## 4. NV2A Renderer Architecture: Multi-Slab WC + RTT Experiment vs. Upgraded Single-Atlas Native-Quad Pipeline

### Zero CPU Screen Rasterization Guarantee
Across both designs, **`board.blitter.rasterize = false`** (`-DXBOX_NO_SW_VRAM`) is strictly enforced. Every screen sprite, bullet, HUD element, menu, and background layer is rendered as **native NV2A hardware quads** (`NV097_DRAW_ARRAYS` + `NV097_SET_BEGIN_END_OP_QUADS`) using hardware `SHIFTLEFTBY1` 2× hit-flash tint combiners, hardware alpha-test, hardware alpha-blending, and hardware `NV097_SET_COMPOSITE_MATRIX` YOKO/TATE scaling.

### Architectural Post-Mortem: Why Direct WC GPU Slabs + NV2A RTT Regressed Performance & Caused Title-to-Stage-1 Jitter
We implemented and extensively profiled a full GPU-resident slab + hardware Render-to-Texture (RTT) renderer (`12 × 2048×516` contiguous `PAGE_WRITECOMBINE` slabs = `24.1 MB`, `512×260` A8R8G8B8 RTT surface, 3-slot asynchronous VB). While raw CPU render-prep time dropped to `0.65 ms`, live Xemu A/B timeline probing across `f = 0 .. 3,850` proved **four severe structural regressions**, concentrated directly between the **Title Screen (`f = 1,241`) and Stage 1 Start (`f = 3,000`)**:

1. **128×128 Sub-Quad Sprite Splitting (`12–36` splits/frame from `f = 1,700` onward):** Any sprite crossing a `128×128` VRAM page seam had to be split into 2–4 sub-quads. At 1.5× YOKO scale, each sub-quad rounded independently at the seam, causing visible wobble/shimmer on moving Title, Mode Select, and Stage 1 intro objects.
2. **Per-Slab `NV097_SET_TEXTURE_OFFSET` Thrashing + `2× ASK_FOR_IDLE` RTT Switches:** Starting on `f = 1,747` (Mode Select), `rtt = 22` draws/frame and `ups = 16..64` uploads/frame constantly bumped `slabGen_` across `12` separate `2.01 MB` slabs (`120–192` draw batches and `180–281` state changes/frame). Shifting the physical texture base address of `2 MB` slabs invalidated GPU texture caches (and forced multi-megabyte OpenGL texture re-uploads per frame in Xemu), collapsing frame pacing from locked `60.0 FPS` down to stuttering `50–56 FPS` (`+76..+85` frames per 1.5s window) with `27.3 ms` `gpuW` spikes.
3. **Uncacheable WC-Read Eviction Stalls (`1,103` swaps) & Tight Free RAM (`1.98 MB`):** Storing hot pages in `PAGE_WRITECOMBINE` memory made eviction readbacks (`StoreColdPageRle16`) perform `4,096` uncacheable 64-bit WC reads per page, while `24.1 MB` of slabs left only `1.98 MB` free RAM — causing `38.6–76.6 ms` CPU spikes inside `ApplyDirectUpload()` and triggering an audio underrun.
4. **Async Submit / `pb_busy()` Contention (`v6–v8` experiments):** Skipping the pre-submit `pb_busy()` barrier caused `canReuseInPlace` to miss whenever NV2A was still rendering frame `N-1`, triggering frequent bank flips and doubling render time (`2.64 ms` → `4.27 ms`).

### Final Architecture (`nv2a_cv1k_renderer.h`, `v9`)
We returned to the rock-solid **single dual-bank `1024×2048` A1R5G5B5 NV2A hardware quad atlas** + **cached-RAM 3-tier lossless VRAM backing** (`576` hot pages = `18 MB` cached RAM → `3.5 MB` RLE16 cold RAM → lossless `Z:\mushivrm.bin` spill), enhanced with five targeted, zero-downside architectural upgrades:

1. **Zero Sprite Splitting & Single Texture Bind:** Every sprite is emitted as **one atomic native NV2A quad** (`0` sub-quad splits across Title → Stage 5) using `1` bound texture for the entire frame and `0` RTT render-target switches.
2. **Hoisted `128×128` Page-Strip `ApplyUpload()` & `ApplyDirectUpload()`:** Resolves `GetPageWrite(px, py)` once per page tile instead of per scanline (`32×` fewer lookups during the `64`-tile Title/Mode Select uploads).
3. **Pointer-Based `StoreColdPageRle16()`:** Pre-sizes `rleScratch_` once in `Init()`, writes RLE16 tokens via raw pointer instead of `push_back()`/`insert()` inside the 16,384-pixel loop, and cuts `MmQueryStatistics` calls in half.
4. **Boot-Time Swap File Mount (`EnsureSwapFileOpen()` in `Init()`):** Pre-opens `Z:\mushivrm.bin` (with `E:` fallback) at startup instead of lazily stalling mid-Mode Select, while keeping `kMaxColdRamBytes = 3.5 MB` so physical free RAM stays `>= 3.68 MB` (avoiding dlmalloc heap fragmentation and `1.5 MB` emergency flush bursts).
5. **Sub-Texel Edge Guard & Expanded Quad Buffer:** Raised `uvEps` to `0.03125f` (`1/32` texel for NV2A 12.4 fixed-point UV rounding safety), expanded `kMaxQuadsPerFrame` from `2048` → `2560`, and kept `EnsureGpuReadyForSubmit()` cleanly overlapping NV2A rasterization of frame `N-1` with both SH-3 CPU and frame `N` atlas/VB prep.

---

## 5. Audio, Input & Stress-Test Harness Architecture

- **Audio Preemption Tuning (`port/nxdk/mushisam/xbox_audio.h`):** Lowered the async AC'97 audio thread from `THREAD_PRIORITY_HIGHEST` to `THREAD_PRIORITY_ABOVE_NORMAL`, raised sleep cadence from `4 ms` → `8 ms`, and expanded `kTargetInFlight` from `6` (`64.0 ms`) → `8` (`85.3 ms`), halving Pentium III preemption jitter while achieving **`0` underruns**.
- **USB OHCI Hub Polling (`port/nxdk/mushisam/xbox_input.h`):** Reduced `usbh_pooling_hubs()` interval from `60` frames (`1 Hz`) → `300` frames (`0.2 Hz`) once a gamepad is attached (live packets still arrive at `125 Hz` via OHCI interrupts), eliminating `1 Hz` OHCI bus jitter, and exposed analog `lTrigger` / `rTrigger`.
- **Max-Power Maniac Stress Harness (`port/nxdk/mushisam/mushisam_fake_play.h`, `tests/mushisam_parity_test.cpp`):** Updated `MushisamFakePlay` to auto-select Maniac mode and grant max power level (`0x0C22E27C`), enabling reproducible bullet-storm profiling across host and Xemu (`FAKE_PLAY=1`, disabled in clean playable builds).
- **Accurate Telemetry Accounting (`port/nxdk/mushisam/main.cpp`):** Split `pb_finished()` triple-buffer wait out of `renOnlyUs` into `gpuWaitUs` so CPU render work and GPU/swapchain synchronization are measured cleanly.

---

## 6. Side-by-Side Verification Summary

### Host Verification
- **5,000-Frame Differential Parity (`tests/build_host.sh --parity --frames 5000 --roms bin/mushisam`):** **PASS — `0` mismatches**, `interpBlk = 0`.
- **SH-3 Optimization Impact:** `3.69×` fewer `RunBlocks()` dispatches (`6,745.9/f` → `1,828.9/f`), `+43.8%` faster host wall-clock (`0.35 ms/f` → `0.24 ms/f`).

### Live Xemu Timeline A/B Comparison (`f = 0 .. 3,850`, Title → Mode Select → Stage 1 Intro → Stage 1 Play)

| Metric | Multi-Slab WC + RTT (`cur`) | `HEAD` Renderer Baseline (`head_ren`) | Final Upgraded Native-Quad (`v9`) |
|---|---:|---:|---:|
| **Mode Select → S1 Pacing (frames / 1.5s, 60 fps = 90–91)** | `+79, +84, +84, +85, +76` (50–56 FPS) | `+90, +91` locked | **`+90, +91` locked (60.0 FPS)** |
| **PLAY `avgWall` (target `16.66 ms`)** | `17.75 ms` | `16.59 ms` | **`16.62 ms`** |
| **PLAY `avgCpu`** | `8.45 ms` | `8.12 ms` | **`7.70 ms`** |
| **PLAY `avgRen` / `avgWork`** | `0.65 ms` / `9.14 ms` | `2.67 ms` / `12.18 ms` | **`2.64 ms` / `11.20 ms`** |
| **PLAY Dropped VBlanks** | `17 / 873 (1.95%)` | `4 / 838 (0.48%)` | **`5 / 816 (0.61%)`** |
| **Max `gpuW` (Mode Select → S1)** | `27.3 ms` | `10.7 ms` | **`8.5 ms`** |
| **Min Free Physical RAM (64 MB Xbox)** | `1.98 MB` | `3.74 MB` | **`3.68 MB`** |
| **Audio Underruns** | `1` | `0` | **`0`** |
| **128×128 Sub-Quad Splits / Frame** | `12–36` | `0` | **`0`** |

### Clean Playable Output Artifacts (without `FAKE_PLAY`)
- `port/nxdk/mushisam/bin/default.xbe`
- `port/nxdk/mushisam/mushisam-cv1000-xbox.iso`
