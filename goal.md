# Goal: Eliminate Stage 3 Black Screen, Zero Disk Swapping (In-RAM Compressed VRAM), Zero Audio Underruns & Locked 60 FPS

## 1. Ruthless Audit of Present Codebase (`d43fe26`) Against Previous `goal.md`

*(Note: `report.md` was not found on disk—if it is open in an unsaved editor tab, save it and we will merge any extra notes immediately. Below is the objective codebase + empirical trace audit explaining every symptom reported: persistent Stage 3 black screen during real play, audio underruns, and FPS losses.)*

| Previous Goal Deliverable | Actual Status | Honest Post-Mortem & Measured Evidence |
|---|---|---|
| **Stage 3 Black-Screen Resolution (Natural S1 $\to$ S2 $\to$ S3 Play)** | **NOT Met (Partial)** | Fixed the `nand.h` `m_raBuf` aliasing bug for `START_STAGE=2` up to frame `4,843`, **but** natural play into full Stage 3 still breaks due to **4 active bugs** detailed in Section 2 (`draws == 0` black clears during `212`-frame transition, cyclic-scan LRU collapse once Stage 3's `1,064`-page working set exceeds `768` RAM pages, `pak_s2`/`pak_s3` bucket boundary mismatch at `f=25,672..25,884`, and `useDirectUpload` failing whenever `!draws.empty()`). |
| **Zero Gameplay Disk I/O & `>= 6.0 MB` Free RAM** | **NOT Met** | `51,654` blocks (`14.7%+` speculative static-follow bloat) inflated `default.xbe` to `13.27 MB`, forcing `kRetailMaxAllocPages` **down** from `928` (`29 MB`) to `768` (`24 MB`, `minFree = 3.3 MB`). Because Stage 1 needs `890` pages (`27.8 MB`) and Stage 3 needs `1,203` pages (`37.6 MB`), `Nv2aCv1kRenderer` **continuously thrashes `Z:\mushivrm.bin` on the HDD during active gameplay** (`2,087` swaps in Stage 1 alone; catastrophic cyclic thrashing in Stage 3!). |
| **Zero Audio Underruns at `<= 43 ms` Latency** | **NOT Met** | While `sfx_adpcm.bin` (`480 KB`) eliminated SFX MPEG-2 spikes, **two major underrun sources remain**: (1) `Ymz770::EnsureChannelWindow` (`ymz770.cpp:108-115`) performs **synchronous `1.0 MB` DVD `NtReadFile` calls directly inside the real-time `AudioThreadLoop()`**, stalling audio for `100–350 ms` on every BGM/boss track change; (2) `50–85 ms` HDD swap + CPU frame stalls exceed the `42.7 ms` (`4 × 512`) ring cushion (`87` underruns recorded on Stage 1 Boss). |
| **Locked 60 FPS (`DroppedVblPct <= 1.0%`) Across Stages 1–3** | **NOT Met** | Early Stage 1 hit `59.35 FPS` (`0.61%` dropped VBlanks), **but** Stage 1 Boss dropped to `57.61 FPS` (`3.94%` dropped, `MaxTot = 82.7 ms`) and Stage 3 dropped to **`53.04 FPS` (`11.67%` dropped, `CPU AVG = 14.15 ms`)** because: (1) `73/74` SH-3 TUs still compile with `-Oz` (`blocks_hot.cpp` only covers `114` of `8,810` functions), (2) zero gameplay/bullet/blitter hot loops were rewritten in native C++, and (3) human play modes/shots (`Maniac`, `Ultra`, `M/S-Power`, deaths/continues) were never recorded into `blocks.txt`, triggering slow `InterpretBlock()` fallbacks! |
| **`nxdk` Zero-Interrupt `pbkit`, `R5G6B5`, No-Z, `XVideoFreeFB`** | **Met** | `GpuWait` dropped from `2,059 µs` $\to$ **`237 µs`** and `PbSubmit` to **`19 µs`**; saved `+4.31 MB` of contiguous `nxdk` RAM. |
| **NV2A Hardware Render-to-Texture Off-Screen Tilemap** | **NOT Met** | Skipped in previous pass; `BlitVramToVram` still burns `1.2–1.4 ms/frame` on the CPU. |

---

## 2. Exact Root Causes of Remaining Black Screen, Underruns & FPS Losses

### A. Why the Screen Still Goes Black (Stage 2 $\to$ Stage 3 & Transitions)
1. **Unconditional Black Clear on `0`-Draw Frames & Persistent VRAM Loss (`nv2a_cv1k_renderer.h:232-235`):**
   - Frame-by-frame tracing (`f=25,672..25,884`, `212` consecutive frames between Stage 2 Boss clear and Stage 3 start, and `f=91,000` in Stage 5) proves CV1000 submits **`0` draws** while expecting the CRT scanout (`Blitter::Picture` at `(scrollX, scrollY)`) to hold the last rendered VRAM image or transition screen. `Nv2aCv1kRenderer::Render()` **unconditionally clears the backbuffer to black (`0xFF000000`)** even when `onScreenDraws == 0`!
2. **Cyclic-Scan LRU Swap Collapse on `Z:\mushivrm.bin` (`1,064` Active Read Pages vs `768` RAM Slots):**
   - Once Stage 3's Leviathan body streams in (`f=25,891..30,000`), Stage 3 reads **`1,064` VRAM pages (`33.2 MB`) every frame** (`1,203` union = `37.6 MB`). Scanning `1,064` pages cyclically through a `768`-page strict-LRU cache causes **near-100% cache misses on the cycling set**—issuing hundreds of synchronous `32 KB` `NtWriteFile`/`NtReadFile` calls per frame to `Z:\mushivrm.bin` on FATX HDD (`100–500+ ms/frame`), freezing the screen during/after the black Stage 3 fade-in! Worse, if `Z:\` / `E:\` swap write fails or fills cluster limits, `AllocateOrEvictPage` (`line 744`) `memset`s evicted pages to `0` (transparent black).
3. **Stage 3 Initial Load Happens While `kStageOff == 1` (`f=25,672..25,884`):**
   - During `f=25,672..25,884`, the SH-3 loads Stage 3's initial graphics *before* incrementing `kStageOff` (`0x0C1ECF9C`) from `1` to `2` at `f=25,885`—and during Stage start/continue when `ShipState == 0`, `InPlay()` becomes `false` and `main.cpp:212` swaps in `pak_menu.bin`! And throughout `f=25,891..26,900+`, **100% of Stage 3's streaming uploads (`65,536` px every 6 frames, then `2,048` px every frame) occur AFTER `draws`**, completely bypassing `useDirectUpload` (`native->draws.empty() == false`)!

### B. Why Audio Underruns Still Occur
1. **Blocking `1.0 MB` DVD Reads Inside the Real-Time Audio Thread (`ymz770.cpp:108-115`):**
   - `EnsureChannelWindow` runs inside `XboxAudio::AudioThreadLoop()`. Every time a looped BGM track starts (up to `983 KB` in `sound_rom.bin`), the real-time audio thread blocks on a synchronous `1.0 MB` DVD `NtReadFile` (`100–350 ms`), starving AC'97 DMA (`CIV` catches `LVI` $\to$ `DCH` halt & audible glitch/underrun).
2. **IDE/ATA Bus Contention + Short `42.7 ms` Ring Cushion (`xbox_audio.h:28-30`):**
   - Heavy `Z:\mushivrm.bin` HDD swapping + `D:\mushisam` DVD reads contend on the Xbox's single IDE bus and stall the CPU for `50–85 ms`, overflowing the `42.7 ms` (`4 × 512`) buffer cushion.

### C. Why FPS Drops Happen (`53.0 FPS` in Stage 3, `57.6 FPS` in Stage 1 Boss)
1. **Disk Swap Stalls in `Render()` (`MaxRender = 24–50 ms`):** Synchronous `32 KB` `Z:\` HDD swaps inside `GetPageRead` / `GetPageWrite` stall the main loop for `8–40 ms`.
2. **SH-3 CPU Bottleneck (`14.15 ms` AVG in Stage 3):**
   - Only `114` functions (`2.2%`) are in `blocks_hot.cpp` (`-O3`); **97.8% of functions (`8,696` functions, including Stage 2–5 bullet/Leviathan code!) are compiled at `-Oz`**!
   - Unrecorded human-play branches (Maniac/Ultra modes, M-Power/S-Power shots, player death/bomb/continue/name-entry) hit the slow `SH3::InterpretBlock()` interpreter!
   - `BlitVramToVram` (`1.2–1.4 ms/frame`) + `AtlasCopy` (`0.7–1.3 ms/frame`) still eat `~2.5 ms/frame` of PIII CPU time.

---

## 3. Next Optimization Roadmap & Architectural Fixes

### Pillar 1 — Zero-Disk-Swap In-RAM Compressed Cold VRAM Tier (Delete `Z:\mushivrm.bin` Forever!)
1. **Two-Tier 100%-RAM VRAM Pool (`Hot Uncompressed` + `Cold Fast-RLE16/LZ4 Compressed`):**
   - Eliminate `Z:\mushivrm.bin` / `E:\mushivrm.bin` **completely** (`0` HDD writes, `0` HDD reads, `0` FATX cluster stalls ever!).
   - Keep **`640` hot uncompressed `128×128` `A1R5G5B5` pages (`20.0 MB`)** in O(1) direct pointers, backed by an **in-RAM word-aligned RLE16/LZ4 compressed cold store** holding all remaining logical pages (`up to 1,408 cold pages`).
   - Cave CV1000 `A1R5G5B5` sprite/tile pages contain large transparent (`0x0000`) and flat-color runs, compressing **3×–5×** (`18 MB` raw cold pages $\to$ **`~4.0–5.5 MB` in RAM**!) and decompressing in **`~25 µs` in L2 cache** vs **`10,000+ µs` on HDD** (`400×` faster!).
   - Add **scan-resistant Clock / 2Q eviction** (pages read in two consecutive frames go to the Protected queue; one-shot streaming uploads start in Probation) so Stage 3's cyclic `1,064`-page Leviathan scan never evicts active sprite sheets!
2. **True Ordered Zero-Copy Upload References (Even When `!draws.empty()`!):**
   - Instead of copying `65,536` pixels into `NativeList::pixels` whenever `draws` precede an `Upload` (`f=25,891..26,900`), store `{ramOffset, dstX, dstY, w, h}` in `NativeUpload` (when SH-3 does not overwrite the upload list buffer mid-frame—or copy into a fixed `256 KB` static ring buffer), completely eliminating dynamic `std::vector` heap allocations during Stage 3!
3. **Preserve Display Frame on `0`-Draw Transition Frames (`nv2a_cv1k_renderer.h`):**
   - When `list.draws.empty()` (or `onScreenDraws == 0` while `scrollX, scrollY` points to valid VRAM), either skip `pb_finished()` buffer clear/flip (holding the last presented frontbuffer cleanly, zero black flicker!) OR composite the `320×240` `(scrollX, scrollY)` window from VRAM pages just like real CV1000 hardware (`Blitter::Picture`)!

### Pillar 2 — Prune Speculative Code Bloat, Harvest All Modes/Shots, & Expand `-O3` Hot Code (`+3–4 MB` RAM + `2×` Faster Stage 3 CPU)
1. **Multi-Mode / Multi-Shot Complete SH-3 Block Harvest + Zero Speculative Bloat:**
   - Extend `MushisamFakePlay` / `mushisam_parity_test.cpp` to sweep **all 3 game modes (`Original`, `Maniac`, `Ultra`), all 3 shot types (`W-Power`, `M-Power`, `S-Power`), player death/respawn, bomb bursts, Continue, Game Over, and Stages 1–5 + Bosses** with **`recordStatic = 0` (or `1` strictly inside `0x0C002000..0x0C163FFF`)** so ONLY genuine executable blocks are compiled—slashing `default.xbe` size by **`~3 MB`** while achieving **`interpBlk == 0` for real human players**!
2. **5-Stage Profile-Guided `-O3` Hot Bank (`Top 600` Functions Across All 5 Stages!):**
   - Re-run `-DMUSHISAM_PROFILE_HOT` across the **full `100,000`-frame 5-stage + Boss trace** (our previous `hot_pcs.txt` was recorded ONLY on Stage 1–2, which is why **Stage 3's Leviathan & Stage 4–5 functions were all compiled at slow `-Oz`**!!).
   - Put the **top `600` functions (`>= 98.5%` of dynamic SH-3 cycles across ALL 5 stages)** into `blocks_hot.cpp` compiled with `-O3 -funroll-loops`, cutting Stage 3 SH-3 CPU time from `14.15 ms` down to **`~7–8 ms`**!
3. **Native C++20 / SSE1 / MMX Rewrites of Top Gameplay Loops (`mushisam_native.h`):**
   - Inspect and rewrite the top 3–5 pure SH-3 gameplay hotspots (`kHitTest = 0x0C05B110` player-vs-bullet point-in-box loop over `kBulletCount`, `0x0C05A9C0` shot/laser-vs-enemy AABB collision pass, and `0x0C160E1C` sprite/blitter list emitter) in native x86/SSE1 C++ verified against `mushisam_parity_test --parity` (`0` mismatches!).

### Pillar 3 — Decoupled Zero-Stall Audio I/O (`0` Underruns Ever!)
1. **Remove ALL Synchronous DVD `NtReadFile` Calls from `AudioThreadLoop()` (`ymz770.cpp` + `xbox_audio.h`):**
   - Never call `NtReadFile` inside the real-time `THREAD_PRIORITY_HIGHEST` audio mixer thread!
   - Move `sound_rom.bin` BGM/stream prefetching onto a **dedicated low-priority I/O worker thread** (or pre-stage `64 KB` double-buffered ring chunks asynchronously when `WriteRegister` queues a phrase command on the main thread), so `AudioThreadLoop()` reads **100% from RAM** (`0 µs` disk wait ever!).
2. **Rock-Solid Hardware AC'97 Ring (`6 × 512` = `64 ms` Cushion, `0` Underruns):**
   - Increase in-flight AC'97 descriptors from `4 × 512` (`42.7 ms`) to `5 × 512` (`53.3 ms`) or `6 × 512` (`64.0 ms` — still `< 4 frames` arcade feel) with non-blocking RAM-only synthesis so even worst-case transition frames produce **`0` underruns across a full 5-stage playthrough**.

### Pillar 4 — Seamless Stage-Transition Asset Preloading & Further `nxdk` Fast Paths
1. **Look-Ahead / Persistent `MPAK` Transition Table (`main.cpp` + `nand.h`):**
   - Keep a compact unified index (or attach `pak_s{N+1}.bin` as soon as Boss `N` dies / `draws == 0` transition starts, *before* `kStageOff` increments!) and never switch back to `pak_menu.bin` mid-stage when `ShipState` momentarily hits `0` during cutscenes, respawns, or continues!
2. **Inline `pbkit` Pushbuffer Macros & Skip Per-Frame `pb_reset()` Spin (`nxdk/lib/pbkit`):**
   - Inline `pb_begin()` / `pb_end()` (`*(pb_DmaUserAddr + 0x40/4) = ...` with a single `_mm_sfence()` instead of flushing `NV_PFB_WC_CACHE` MMIO spinloop `8–12` times per frame at `pbkit.c:269-270`!) and only wrap `pb_jump_to_head()` when `pb_Put` approaches `pb_Tail - 64 KB`, saving another **`200–400 µs/frame`** of MMIO bus stalls!

---

## 4. Strict Verification Gates
1. **`0` Mismatches in Host Differential Parity:** `mushisam_parity_test.exe --parity` passes across `5,000+` frames including native C++ collision/blitter hooks.
2. **Natural End-to-End Xemu Playthrough (Stage 1 $\to$ Stage 2 $\to$ Stage 3 Leviathan $\to$ Stage 3 Boss):**
   - Verified via live Xemu playthrough **from Stage 1 through Stage 2 Boss into full Stage 3 Leviathan (`f > 28,000`)**—not just stage-select jump—confirming **`0` black-screen hangs/flickers**, `Swaps == 0` (zero disk swaps!), and `minFree >= 4.0 MB`.
3. **Zero Gameplay Audio Underruns & Locked 60 FPS (`decode_perf.py` v4):**
   - `AudioUnderruns == 0` across all active gameplay & BGM/boss transitions.
   - `EffectiveFps >= 58.5–60.0 FPS` (`DroppedVblPct <= 1.5%`) in Stage 1 Boss, Stage 2 Boss, and full Stage 3 Leviathan scenes!
