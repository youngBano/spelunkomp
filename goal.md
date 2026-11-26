# Goal: Zero-Lag 60 FPS Gameplay & Uncompromising Audio for *Mushihimesama* (`mushisam`) on Original Xbox

## 1. Comprehensive Codebase & Telemetry Audit (Baseline at Commit `2886729`)

### 1.1 Why Previous Telemetry Reported "Low/No Lag" Despite Visible Slowdown (Critical Timer & Pacing Audit)

Inspection of [`port/nxdk/mushisam/main.cpp`](port/nxdk/mushisam/main.cpp), [`port/nxdk/mushisam/xbox_timer.h`](port/nxdk/mushisam/xbox_timer.h), and [`port/nxdk/mushisam/xbox_profiler.h`](port/nxdk/mushisam/xbox_profiler.h) identified **four compounding measurement and frame-pacing bugs** that masked real frame drops and slow-motion:

1. **Incomplete Frame-Time Accounting (`xbox_profiler.h:194`, `main.cpp:202-271`):**
   - `totalUs = cpuUs + renderUs + gpuWaitUs` only timed isolated inner blocks. It **excluded** `input.Poll()` (`usbh_pooling_hubs()` synchronous USB OHCI hub register polling), `pb_wait_for_vbl()`, and loop iterations where `ConsumeArcadeFrame()` returned `false`.
2. **Hidden VBlank Quantization Cliff (60 FPS $\to$ 30 FPS Halving):**
   - When `vblWakeOffset (1–2 ms) + inputPoll (0.5–1 ms) + cpuUs (8.5 ms) + renderUs (6.0 ms)` totaled `16.8 ms` wall-clock, `g_xboxPerf` logged `cpuUs + renderUs = 14.5 ms` (`< 16,662 µs` = **"no lag"**), yet the frame missed the `16.683 ms` hardware VBlank by `0.12 ms` and **`pb_wait_for_vbl()` blocked until `33.367 ms` (instant drop to 30 FPS)** without incrementing `lagFrames`.
3. **Dual-Clock Beat-Frequency Judder & Silent Slow-Motion Clamping (`xbox_timer.h:60-73`, `main.cpp:182-271`):**
   - Coupling a `60.0184 Hz` (`16.661562 ms`) software accumulator (`ConsumeArcadeFrame()`) in series with an unconditional `59.94 Hz` (`16.6833 ms`) hardware `pb_wait_for_vbl()` caused two severe artifacts:
     - Whenever a VBlank interval woke slightly under `16.6615 ms`, `ConsumeArcadeFrame()` returned `false` (`stepped = false`) and `main.cpp:271` **immediately called `pb_wait_for_vbl()` a second time**—stalling an entire extra `16.68 ms` (`33.3 ms` judder).
     - Whenever any frame missed VBlank (`33.3 ms`), `accumTicks_` saturated at `maxAccumTicks_ = 2 * ticksPerArcadeFrame_`. Because the loop only stepped at most **1** emulated frame per `pb_wait_for_vbl()`, it could **never drain the backlog**; every subsequent spike clamped `accumTicks_ = maxAccumTicks_`, silently discarding real wall-clock time (**visible slow-motion**).
   - *Empirical proof from live Xemu dumps:* Over `60.0` real wall-clock seconds (`3,600` expected frames), `g_xboxPerf.frames` only advanced `2,599–2,888` frames (**43–48 real-time FPS**) while `AVG Total` falsely reported `8.8–9.2 ms`.
4. **Missing Ground-Truth Hardware VBlank & System Wall-Clock Anchors:**
   - Neither `pb_get_vbl_counter()` (NV2A hardware VBlank interrupt counter) nor `KeQuerySystemTime()` (100 ns real-time system clock) nor end-to-end Present-to-Present wall-clock deltas were tracked. Under emulator load, virtualized guest `RDTSC` and ACPI PMTimer (`KeQueryPerformanceCounter`) can also dilate together unless verified against `KeQuerySystemTime()` and `pb_get_vbl_counter()`.

---

### 1.2 Subsystem-by-Subsystem Bottleneck Audit

| Subsystem | Current Files | Measured State & Remaining Slowdown / Stutter Root Causes |
| :--- | :--- | :--- |
| **1. SH-3 AOT Recompiler & CPU** (`6.4 ms` avg, **`35.4 ms` max**) | [`tools/recomp_sh3.py`](port/nxdk/mushisam/tools/recomp_sh3.py), [`engine/runtime/recomp_rt.h`](port/nxdk/mushisam/engine/runtime/recomp_rt.h), [`Makefile`](port/nxdk/mushisam/Makefile), [`engine/machine/sh3.cpp`](port/nxdk/mushisam/engine/machine/sh3.cpp) | **a) `-Oz` on Boss & Stage 2–5 Code:** Only `blocks_hot.obj` (`113` functions from Stage 1 opening) compiles at `-O3`. All other `3,762` functions—including Stage 1 Midboss/Boss and Stage 2+ bullet scripts—compile in `blocks_000..031.obj` at **`-Oz`**, causing `20–35 ms` CPU spikes when boss patterns activate.<br>**b) 64-bit `uint64 &cyc` Stack Spills on 32-bit Pentium III:** Passing `uint64 &cyc` by reference to every `recomp::R*/W*` spills `cyc` to stack memory and emits 64-bit `add/adc` + `cmp/sbb` across an ISA with only 7 usable GPRs. One CV1000 frame is only `1,706,496` cycles (`< 2^21`)—a 32-bit `int32_t cyclesLeft` countdown fits in 1 GPR.<br>**c) Dual-Branch `SH3::IsRam()`:** Every load/store executes two comparisons + mask instead of a single unsigned `(a - 0x0C000000u) < 0x800000u`.<br>**d) Residual Interpreted & Un-Nativized Loops:** `1,805` blocks at `0x0C09E040` still hit `InterpretBlock()` on Xbox; `JSR @Rn` jump tables exit to `RunBlocks()`; hot routines `0x0C160E1C` (`433M` cycles), `0x0C09DE8C`, `0x0C055470`, `0x0C055800`, and `0x0C05411E` are not yet native C++. |
| **2. NV2A Renderer & Sprite Atlas** (`4.05 ms` avg, **`29.6 ms` max**) | [`nv2a_cv1k_renderer.h`](port/nxdk/mushisam/nv2a_cv1k_renderer.h), [`xbox_asm.S`](port/nxdk/mushisam/xbox_asm.S) | **a) Full-Atlas Wipe Cliff (`29.6 ms` spike):** When `shelfY_ > 768`, `InvalidateRectHash()` wipes **all** cached sprites simultaneously, forcing 400+ sprites/tiles to re-copy from VRAM into write-combined memory in one frame.<br>**b) CPU Off-Screen Tilemap Double-Copy (`~2.5 ms/frame`):** `BlitVramToVram()` composites scrolling background tiles (`x = 7456..8191`, `~20` draws/frame) on the CPU into sparse VRAM pages, and `CopyVramRectToAtlas()` copies those exact pixels *again* on the CPU into the atlas—work the NV2A GPU can execute in `< 0.1 ms` via hardware Render-to-Texture.<br>**c) Tight 64 MB RAM Headroom (`~1.9 MB` free):** `832` VRAM pages (`26 MB`) still leaves minor swap pressure because `board.soundRom` pins `8.0 MB` of RAM. |
| **3. Audio Engine** (`0 µs` main thread, `128 ms` latency) | [`xbox_audio.h`](port/nxdk/mushisam/xbox_audio.h), [`engine/machine/ymz770.cpp`](port/nxdk/mushisam/engine/machine/ymz770.cpp), [`engine/native_cv1k/adpcm_cache.h`](port/nxdk/mushisam/engine/native_cv1k/adpcm_cache.h) | Dedicated `THREAD_PRIORITY_HIGHEST` async thread achieved `0` underruns, **but**:<br>**a) `8.0 MB` `board.soundRom` (`u23+u24`) is pinned permanently in RAM** even though BGM streams sequentially at ~16 KB/s.<br>**b) High Audio Latency (`128 ms`):** `6 × 1024` stereo frames @ 48 kHz delays shot/explosion SFX by ~7.7 frames.<br>**c) Dead Channels `8..15`:** `Ymz770` allocates 16 `AmmDecoder` + `m_block` buffers (`0..15`) when CV1000 hardware only addresses 8 channels (`0..7`). |
| **4. Assets, NAND, & Disk I/O** | [`engine/machine/nand.h`](port/nxdk/mushisam/engine/machine/nand.h), [`engine/native_cv1k/xbox_eeprom.h`](port/nxdk/mushisam/engine/native_cv1k/xbox_eeprom.h), [`tools/build_stage_packs.py`](port/nxdk/mushisam/tools/build_stage_packs.py) | **a) Lazy Stage-Pack Reads:** `AttachStagePack()` still performs `ReadFile` calls *after* `InPlay` becomes `1` instead of preloading before gameplay begins.<br>**b) Incomplete Stage Coverage:** Only `pak_s1.bin` (`8.1 MB`) and `pak_s2.bin` (`10.5 MB`) exist; Menu and Stages 3–5 are not yet split.<br>**c) Mid-Gameplay Disk Writes:** `eeprom.FlushIfDirty()` runs every 128 frames even while `InPlay == 1`, and `Z:\mushivrm.bin` swap file still exists. |
| **5. USB XID Input** | [`xbox_input.h`](port/nxdk/mushisam/xbox_input.h) | `usbh_pooling_hubs()` walks USB OHCI hub registers on **every single frame** even when a gamepad is already connected and delivering interrupt transfers via `OnReadComplete`. |

---

## 2. Mission & Engineering Trade-Offs

Deliver **locked, zero-lag 60 FPS gameplay (`0` dropped VBlanks in active play) with full, low-latency, zero-underrun BGM + SFX audio** on a stock **64 MB retail Original Xbox**.

### Approved Engineering Sacrifices vs. Non-Negotiables
* **Embraced Sacrifices (for Zero Gameplay Lag):**
  1. **1:1 Hardware VBlank Lock (`59.94 Hz` NTSC / `60.00 Hz` PAL vs. `60.0184 Hz` arcade = `-0.13%` speed):** Eliminates dual-clock beat judder and `accumTicks_` slow-motion clamping completely (identical practice to official Cave console ports).
  2. **Short Pre-Stage / Pre-Menu Loading Bursts (`!InPlay`):** Split Menu and Stages 1–5 assets into dedicated binary packs loaded in bulk during stage/menu transitions so **strictly zero disk reads or writes occur during active gameplay (`InPlay == 1`)**.
  3. **Streamed BGM + Pre-Baked SFX Bank (Freeing `~7.5 MB` of `soundRom` RAM):** Stream looped BGM phrases on the async audio thread from a compact file/ring buffer instead of pinning `8.0 MB` `u23+u24` in RAM, reallocating that RAM to hold **100% of active VRAM pages in memory (`0` VRAM swap file)**.
* **Non-Negotiables (Zero Compromise):**
  - **Full BGM + SFX Audio at All Times:** Zero muted channels, zero crackles/underruns (`underruns == 0`), tight arcade SFX latency (`<= 45 ms`), bit-accurate YMZ770C mixing.
  - **Zero Visual Glitches:** Preserve exact sprite/tilemap ordering, transparency, 2x hit-flash tint overdrive, and off-screen tilemap compositing.
  - **64 MB Retail Xbox Compatibility:** Keep `>= 4.0 MB` free physical RAM headroom (`MmQueryStatistics`) at all times on a stock 64 MB Xbox.

---

## 3. Pillar 1: Real-Time Wall-Clock Telemetry & VBlank-Locked Frame Pacing

1. **Ground-Truth Real-Time Profiler (`xbox_profiler.h` v4 & `tools/decode_perf.py`):**
   - Measure **end-to-end Present-to-Present wall-clock microseconds** (`wallFrameUs`) across the **entire** main loop (including `input.Poll()`, `CPU`, `NV2A Render`, `GpuWait`, and `pb_wait_for_vbl()`) using hardware `KeQueryPerformanceCounter()` (`3.579545 MHz` ACPI PMTimer).
   - Cross-check elapsed wall-clock seconds via **`KeQuerySystemTime()`** (`100 ns` system clock) and compute **True Real-Time FPS** (`frames / wallElapsedSec`) inside `XboxPerfStats` so emulator/timer drift can never hide slowdowns.
   - Read NV2A hardware **`pb_get_vbl_counter()`** every frame: record `vblanksElapsed` and `droppedVblanks` (`sum of max(0, vblDelta - 1)` during `inPlay && !nandBusy`). Any missed 60 Hz refresh is now caught with 100% hardware certainty.
   - Base the gameplay-isolated histogram (`playHist[10]`, `p50`/`p95`/`p99`) and `playLagFrames` on **real `wallFrameUs` and `vblDelta > 1`**, not partial inner timers.
2. **1:1 VBlank-Locked Tear-Free Frame Loop (`main.cpp`, `xbox_timer.h`):**
   - Replace the conflicting `ConsumeArcadeFrame()` + unconditional `pb_wait_for_vbl()` pair with a clean **1-frame-per-VBlank pipeline**:
     - Overlap SH-3 CPU frame `N` with NV2A GPU rasterization of frame `N-1`.
     - Wait on `pb_wait_for_vbl()` **only** when finishing early (`vblNow == lastPresentVbl`), flip immediately without sleeping an extra frame if `vblNow > lastPresentVbl` (adaptive tear-free recovery: prevents a `16.9 ms` frame from stalling to `33.3 ms`!).
   - Throttle `usbh_pooling_hubs()` in [`xbox_input.h`](port/nxdk/mushisam/xbox_input.h) to once every 60 frames while `padDev_ != nullptr` (button packets continue arriving every millisecond via OHCI interrupt `OnReadComplete`).

---

## 4. Pillar 2: Reclaiming ~8.5 MB RAM, Zero-Swap VRAM, & Preloaded Stage/Menu Packs

1. **Offline SFX Bank + Streamed BGM ROM (`tools/build_audio_pack.py`, `ymz770.h/.cpp`, `xbox_audio.h`):**
   - Build a compact pre-decoded **4-bit IMA ADPCM SFX Pack (`D:\mushisam\sfx_adpcm.bin`, `~1.0–1.5 MB`)** containing all non-looped YMZ770C SFX phrases at boot, eliminating runtime capture/transcode overhead and first-play AMM decode spikes.
   - Replace the `8.0 MB` in-RAM `board.soundRom` (`u23` + `u24`) with a unified `D:\mushisam\sound_rom.bin` streamed by the async audio thread through a small **per-channel `32 KB` read-ahead cache** (`8` channels × `32 KB` = `256 KB` total RAM, freeing **`~7.5 MB`** of main RAM!).
   - Trim unused `Ymz770` channels `8..15` (`m_decoders`, `m_keptDecoders`, `m_block`), saving another **`~0.5 MB`**.
2. **100% RAM-Resident Sparse VRAM Pool — Delete `Z:\mushivrm.bin` Completely (`nv2a_cv1k_renderer.h`):**
   - Expand `kRetailMaxAllocPages` to **`1,024` pages (`32.0 MB` hot RAM pool)** while still retaining **`>= 4.5 MB` free RAM** on 64 MB retail Xbox.
   - Remove `OpenSwapFile()`, `SwapOutPage()`, and `SwapInPage()` (`Z:\mushivrm.bin` / `E:\mushivrm.bin`) entirely, and add a **Dead-Page Stage Purge** on stage transition so stale previous-stage pages are recycled in O(1) RAM with **zero HDD swap I/O ever**.
3. **Complete Per-Menu & Per-Stage Asset Packs (`pak_menu.bin`, `pak_s1.bin` .. `pak_s5.bin`):**
   - Add deterministic stage-select / 5-stage sweep support in [`mushisam_fake_play.h`](port/nxdk/mushisam/mushisam_fake_play.h) and [`tests/mushisam_parity_test.cpp`](port/nxdk/mushisam/tests/mushisam_parity_test.cpp) to profile and extract **all 5 stages + Bosses + Boot/Menu** via `tools/build_stage_packs.py`.
   - Pre-warm the stage's NAND pack + VRAM uploads in one sequential burst **before** `InPlay` begins (during stage-intro load), and defer `eeprom.FlushIfDirty()` strictly to `!InPlay` (Title / Game Over) so **disk I/O during `InPlay == 1` is strictly zero**.

---

## 5. Pillar 3: SH-3 CPU 2x Speedup (Eliminating All Boss & Bullet-Hell Spikes)

1. **32-Bit `int32_t cyclesLeft` Register Countdown (`tools/recomp_sh3.py`, `engine/runtime/recomp_rt.h`):**
   - Replace stack-spilled `uint64 &cyc` across all generated functions with a register-allocated `int32_t rem` (`c.blockStop - c.cycles`) passed/updated in 32-bit GPRs (`rem -= N; if (rem > 0 && !c.interruptReady) goto L_...`), syncing `c.cycles = c.blockStop - rem` only on MMIO slow paths and function exits.
   - Replace dual-branch `SH3::IsRam(a)` in `recomp_rt.h` with single-comparison `const uint32 off = a - 0x0C000000u; if (off < SH3::kRamSize) [[likely]]`.
2. **5-Stage + Boss Profile-Guided `-O3` Partitioning & Zero Interpreted Blocks:**
   - Harvest all executed blocks and hot PCs across **Boot/Menu + Stages 1–5 + Midbosses/Bosses** (explicitly capturing `0x0C09E040` and any attract/menu blocks so `interpretedBlocks == 0` on Xbox from power-on through Stage 5).
   - Compile the top **500** gameplay & boss functions (`>= 99.5%` of active gameplay cycles) into `blocks_hot_*.cpp` with `-O3`, and compile remaining functions with `-O2` (enabled by our `+8.5 MB` RAM savings—no more slow `-Oz` on boss scripts!).
   - Inline fast `m_pageFnPtrs` dispatch directly at `JSR @Rn` / `JMP @Rn` call sites to avoid bouncing back to `SH3::RunBlocks()`.
3. **Native C++20 Rewrites of Top Hot Routines (`engine/game_native/mushisam_native.h`):**
   - Disassemble, rewrite in native C++20/MMX/SSE1, and parity-verify the hottest remaining SH-3 loops (`0x0C160E1C` graphics/data unpacker, `0x0C09DE8C`, `0x0C055470`, `0x0C055800`, `0x0C05411E`), cutting worst-case boss SH-3 frame time below **`6.5 ms`**.

---

## 6. Pillar 4: Sub-1.5 ms NV2A GPU Hardware Tilemap & Dual-Bank Sprite Atlas

1. **NV2A Hardware Render-to-Texture Off-Screen Tilemap (`nv2a_cv1k_renderer.h`):**
   - Allocate a dedicated `1024 × 512` (`1 MB` `A1R5G5B5` / `X8R8G8B8` swizzled or linear) NV2A surface for CV1000's off-screen background tilemap cache (`x = 7456..8191`).
   - Redirect off-screen tilemap draws (`ux >= 7456`) to render **directly on the NV2A GPU** into that surface (`NV097_SET_SURFACE_COLOR_OFFSET`), and bind that surface directly when drawing background layers to the screen—eliminating `1.2–2.2 ms` of CPU `BlitVramToVram()` and `~1.0 ms` of background `AtlasCopy` every frame!
2. **Dual-Bank (`2 × 1024 × 512`) Generational Shelf Atlas (Zero Wipe Stutter):**
   - Partition the `1024 × 1024` (`2 MB`) sprite atlas into two `1024 × 512` banks (`bankEpoch_[0]`, `bankEpoch_[1]`).
   - When the active bank fills, switch allocation to the opposite bank and invalidate **only** that bank's epoch while keeping all active sprites in the recently filled bank valid—eliminating the `29.6 ms` full-atlas wipe spike completely (`MaxRender < 2.5 ms`).

---

## 7. Pillar 5: Low-Latency, Zero-Compromise Async Audio (`<= 45 ms` Latency)

1. **Tight `512`-Frame AC'97 Hardware Ring (`xbox_audio.h`):**
   - Reconfigure DMA descriptors from `8 × 1024` (`kTargetInFlight = 6`, `128 ms` latency) to **`8 × 512` stereo frames @ 48 kHz (`10.67 ms`/buffer) with `kTargetInFlight = 4` (`42.7 ms` latency)** and `Sleep(2)` polling on the `THREAD_PRIORITY_HIGHEST` audio thread.
2. **Zero-Spike Synthesis:**
   - Serve short SFX directly from the pre-baked RAM `sfx_adpcm.bin` bank (`< 15 µs` per block) and BGM from the per-channel `32 KB` buffered ROM reader, guaranteeing `0` underruns even across stage transitions while cutting SFX latency by **3×**.

---

## 8. Strict Verification Gates & Definition of Done

1. **Host Differential Parity & 5-Stage Sweep (`tests/mushisam_parity_test.exe`):**
   - `3,500`-frame differential parity against `SH3::Mode::Blocks`: **`0` mismatches**.
   - Multi-stage (`Stages 1–5` + Bosses) benchmark: **`interpretedBlocks == 0`**, `YMZ770` audio `peakPlay > 5000`.
2. **Live Xemu Real-Time Hardware Verification (`gdb_dump.py` + `decode_perf.py` v4 across `>= 1,500` gameplay frames):**
   - **Real Wall-Clock FPS (`KeQuerySystemTime` & `wallFrameUs`):** **`59.8–60.1 FPS`** sustained during active gameplay (`inPlay == 1 && !nandBusy`).
   - **Hardware VBlank Drops (`pb_get_vbl_counter`):** **`0` dropped VBlanks** (`<= 1%` tolerance under host OS jitter) during active gameplay; `p50 < 10,000 µs`, `p95 < 13,500 µs`, `p99 < 16,000 µs` (`wallFrameUs` including `input` + `CPU` + `Render`).
   - **Zero Gameplay Disk I/O:** `nandReads` delta **`== 0`**, VRAM swap count **`== 0`** (swap file removed), EEPROM writes **`== 0`** during active gameplay.
   - **Zero-Compromise Live Audio:** `audioPeakAmp > 5000`, `AC97 CIV` advancing continuously, `DCH == 0`, **`audioUnderruns == 0`** from boot through Stage 1–5 sweep.
   - **64 MB Retail RAM Headroom:** `minFreeRam >= 4.0 MB` on stock 64 MB Xbox configuration; `interpretedBlocks == 0` (`lastInterpPc == 0`).
3. **Deliverables:**
   - Rebuild final player-ready retail `bin/default.xbe` and `mushisam-cv1000-xbox.iso` (with `FAKE_PLAY` off by default, toggleable via `R3`), shut down all Xemu processes cleanly, and save a clean local git commit.
