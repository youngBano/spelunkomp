# Mushihimesama (CV1000-B / SH-3) Original Xbox Port — Debugging & Optimization Report

## 1. Executive Summary

This report documents the root-cause debugging of the **Stage 3 black screen / freeze**, **HDD VRAM swap thrashing**, **audio underruns**, and **mid-stage FPS drops**, together with the fixes and live Xemu/host verification results across a full continuous **Boot → Stage 1 → Stage 1 Boss → Stage 2 → Stage 2 Boss → Stage 3 Leviathan → Stage 3 Boss → Stage 4** playthrough (`48,949` frames).

---

## 2. Root Cause of the Stage 3 Black Screen / Infinite Loop

### Observed Symptom
- Even after fixing NAND read-ahead buffer aliasing (`m_raBuf`), removing disk swapping, and holding the frontbuffer on `0`-draw frames, natural gameplay into Stage 3 still hit a hard **`Draws = 0`** state around **`f ≈ 28,076`** and again during **Stage 3 Boss (`f ≈ 39,200`)**.
- Live telemetry showed:
  - `Draws = 0`, `Quads = 0`, `nandReads` frozen at `39,368`
  - SH-3 dispatches pinned at **`45,433` dispatches/frame**
  - `Last Interpreted PC = 0x0C096A10` / `0x0C098350`
- Before the `0`-draw frontbuffer hold, `Nv2aCv1kRenderer` cleared the backbuffer to solid black on every `0`-draw frame; with the hold, it froze on the last rendered frame.

### Live GDB & Register Proof (Xemu)
Sampling live x86 EIPs and stack frames via GDB RSP (`127.0.0.1:1234`) against `main.map` pinpointed the exact spin loop:
- `EIP` samples repeatedly hit `F_0C096990`, `F_0C096A9C`, `SH3::ReadSlow`, and `Cv1000::Read`.
- Dumping the live `SH3` struct (`&board.cpu` at `0x7FF00010`) revealed:
  - `PC    = 0x0C096A04`
  - `R[2]  = 0x00113004`
  - `R[8]  = 0x0C27B780`
  - `R[13] = 0x00113006`
- Inspecting `L_0C096A04` in `gen/blocks_hot.cpp`:
  ```cpp
  L_0C096A04:
      r2 = recomp::R32(c, r8 + 56u, rem); // base pointer in U4 flash ROM
      r3 = recomp::R32(c, r8 + 60u, rem); // offset (+2 per iteration)
      r2 += r3;                           // 0x00113004 -> U4 flash range (< 0x400000)!
      r4 = (int16)recomp::R16(c, r2, rem);
      T = ((r4 & r4) == 0u);              // expects 0x0000 terminator
  ```

### Why It Only Happened on Xbox (and Never on Host)
1. `port/nxdk/mushisam/main.cpp` previously freed `board.program` (`2.0 MB` `mushisam_u4` flash ROM) at frame 60 (`std::vector<uint8_t>().swap(board.program)`), assuming U4 was only used during boot.
2. In `engine/machine/cv1000.cpp`, `Cv1000::Read(address < 0x400000)` returned `0xFFFFFFFF` whenever `program.empty()` was true.
3. Because Stage 3's script/pattern engine (`F_0C096990`) reads directly from `0x00000000..0x001FFFFF`, every read returned `0xFFFF != 0`, so the loop never encountered its `0x0000` terminator and scanned through the entire U4 address space at `45,433` dispatches/frame with `Draws = 0`.
4. **Host Reproduction:** A minimal host harness freeing `board.program` at frame 60 reproduced the exact Stage 3 infinite loop at **`f = 28,081`** (`PC = 0x0C096AAC`, `R2 = 0x00040E44`, `R13 = 0x00040E45`, `Draws = 0`).

### Fix
- Removed `programFreed` / `board.program` deallocation in `port/nxdk/mushisam/main.cpp` so `mushisam_u4` (`2.0 MB`) stays permanently resident in RAM.
- Tuned the hot VRAM pool cap in `port/nxdk/mushisam/nv2a_cv1k_renderer.h` from `704` (`22.0 MB`) to `640` pages (`20.0 MB` hot uncompressed + `5.0 MB` RLE16 cold RAM tier) to preserve `~3.0 MB` physical RAM headroom on 64 MB retail Xbox.

---

## 3. Additional Bugs & Bottlenecks Identified & Resolved

### A. HDD VRAM Swap Thrashing (`Z:\mushivrm.bin`) & `0`-Draw Black Clears
- **Cause:** Stage 3's working set (`~1,064` active pages) exceeded the fixed RAM page cap, causing continuous synchronous `32 KB` `NtWriteFile`/`NtReadFile` HDD swaps to `Z:\mushivrm.bin` under strict LRU eviction, plus unconditional backbuffer black clears during `0`-draw CV1000 transition frames.
- **Fix (`port/nxdk/mushisam/nv2a_cv1k_renderer.h`, `main.cpp`):**
  - Deleted `Z:\mushivrm.bin` / `E:\mushivrm.bin` disk swapping completely (`0` HDD swaps ever).
  - Implemented a two-tier 100%-RAM VRAM pool: `640` hot uncompressed `128×128` `A1R5G5B5` pages (`20.0 MB`) + `5.0 MB` lossless word-aligned **RLE16 compressed cold tier** (`StoreColdPageRle16` / `LoadColdPageRle16`) with O(1) scan-resistant 2-bit Clock eviction (`pageRef_[]`, `clockHand_`).
  - `Render()` now returns `bool` and holds the previous presented frontbuffer on `0`-draw transition frames.
  - Kept the active stage asset pack attached across brief `ShipState == 0` transitions/respawns/continues.

### B. Mid-Frame NV2A Atlas Overflows (`FlushMidFrameAndResetShelf()` `10 ms` Stalls)
- **Cause:** Live GDB profiling caught `FlushMidFrameAndResetShelf()` executing `while (pb_busy()) {}` mid-frame during Stage 2–3 (`ren = 15.9 ms` while `blitVram + atlasCopy + pbSub = 5.97 ms`). Scrolling/updated VRAM rects with the same `(srcX0, srcY0, w, h)` allocated new shelf rows every time `pageGen_` incremented, overflowing the 1024-row bank and evicting cached static sprites.
- **Fix (`port/nxdk/mushisam/nv2a_cv1k_renderer.h`):**
  - Added `RectHashSlot::lastFrame` and per-frame `frameId_`.
  - When a `RectHashSlot` matches `key` with a stale `pageGen` and NV2A is finished with its previous frame (`cand.lastFrame < frameId_` and `(cand.lastFrame + 2u <= frameId_ || gpuReadyThisFrame_ || !pb_busy())`), the renderer overwrites `(cand.u0, cand.v0)` **in-place** with zero shelf growth and zero bank flushes.
  - Charged `FlushMidFrameAndResetShelf()`'s `pb_busy()` wait to `lastGpuWaitTsc_` for accurate telemetry.
  - **Measured Impact:** Stage 1 atlas hit rate rose to **`95.4%`**, Stage 1 `stageMaxRenMs` dropped `37.7 ms` → **`24.2 ms`** (`-36%`), and Stage 2 `stageMaxRenMs` dropped `53.1 ms` → **`16.8 ms`** (`-68%`).

### C. Audio Underruns from Synchronous BGM DVD Reads
- **Cause:** `Ymz770::EnsureChannelWindow()` executed synchronous `1.0 MB` DVD `NtReadFile` calls directly inside the real-time `AudioThreadLoop()`, starving AC'97 DMA on BGM/boss transitions.
- **Fix (`engine/machine/ymz770.h`, `ymz770.cpp`, `xbox_audio.h`):**
  - Pre-allocated the `1.0 MB` BGM buffer once; `EnsureChannelWindow()` reads only `64 KB` synchronously (`< 3 ms`) while a `THREAD_PRIORITY_BELOW_NORMAL` background `BgmPrefetchLoop()` streams the remaining `~920 KB` asynchronously.
  - Expanded AC'97 ring to `12 × 512` (`6 × 512` = `64.0 ms` in-flight cushion).
  - **Measured Impact:** Dropped from `87` underruns in Stage 1 Boss alone to **`10` underruns over `80,438` AC'97 buffers (`99.988%` clean across `11.8` minutes of Stages 1–4)**.

### D. Speculative SH-3 Code Bloat, Cold Stage 3–5 Functions & Interpreter Fallbacks
- **Cause:** `blocks_hot.cpp` (`-O3`) previously covered only `114` Stage 1–2 functions while `8,696` functions (including Stage 3–5) compiled at `-Oz`; speculative `found` blocks bloated `default.xbe` to `13.27 MB`, and `36` Stage 2–4 basic blocks were missing from the AOT registry (`56,236` interpreter fallbacks).
- **Fix (`tools/recomp_sh3.py`, `nxdk/lib/pbkit/pbkit.c`):**
  - Profiled `100,000` frames across all 5 stages (`work/hot_pcs.txt`, `763` hot PCs) and harvested the `36` missing Stage 2–4 blocks (`work/extra_blocks_s3.txt`).
  - Added `--strip-found` to `recomp_sh3.py`: `46,103` blocks → **`5,001` whole functions in `40` TUs** (`blocks_hot.cpp` tripled from `114` → **`359`** 5-stage `-O3` functions; `default.xbe` reduced to `12.43 MB`).
  - Updated `pb_reset()` in `nxdk/lib/pbkit/pbkit.c` to skip `pb_jump_to_head()` unless `(pb_Tail - pb_Put) <= 16384`.

---

## 4. Verification Summary

| Verification Gate | Result |
|---|---|
| **Host Differential Parity (`mushisam_parity_test.exe --parity --frames 5000`)** | **PASS** — `0` mismatches over `5,000` frames |
| **Host 50,000-Frame Stage 1–4 Bench** | **PASS** — `3,938.2 FPS` (`0.25 ms/frame`), **`interpBlk = 0`** across `319,993,882` native dispatches |
| **Live Xemu Natural Playthrough (Boot → S1 → S2 → S3 → S4)** | **PASS** — `48,949` frames (`Stage 1: 10,141`, `Stage 2: 15,743`, `Stage 3: 20,921`, `Stage 4: 2,144`), **`0` black screens/freezes**, `maxDisp = 24,111`, **`0` interpreted blocks across `44,057` play frames** |
| **Live R5G6B5 Framebuffer (Stage 3 Boss & Stage 4)** | `172,800 / 307,200` non-zero (`56.25%` = `100%` YOKO playfield), `4,880–5,865` unique colors |
| **Performance & Memory (64 MB Retail Xemu)** | `Stage 1` dropped VBlanks **`0.27%`**, `Stage 1+2` **`0.48%`**, `Stages 1–4` `PlayAvg` **`7.83 ms` CPU / `3.49 ms` render / `12.91 ms` total** (`62.26 FPS` real-time), **`0` disk swaps**, `minFree = 2.95 MB` |
| **Audio (Live Xemu Stages 1–4)** | `10` underruns / `80,438` AC'97 buffers (`99.988%` clean over `11.8` min) |
| **Clean Playable Build** | `bin/default.xbe` (`12 MB`) & `mushisam-cv1000-xbox.iso` (`275 MB`) built without `FAKE_PLAY`/`START_STAGE` |
