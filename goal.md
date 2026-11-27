# Goal: Zero-Lag Full-Game Completion (Stages 1–5 + Bosses), Stage 3 Black-Screen Fix & Deep `nxdk` Kernel/GPU Optimization

## 1. Honest Audit Against Previous `goal.md` (`914968f`)

| Previous Goal Pillar / Deliverable | Status | Objective Codebase Evidence |
|---|---|---|
| **Pillar 1.1–1.2:** Real-time `KeQuerySystemTime` + `KeQueryPerformanceCounter` + `pb_get_vbl_counter()` v4 profiler | **Matched** | Implemented in `port/nxdk/mushisam/xbox_profiler.h` and `tools/decode_perf.py`; logs true wall-clock `WallUs`, `VblDelta`, `DroppedVblPct`, and `EffectiveFps`. |
| **Pillar 1.3:** 1:1 adaptive VBlank pacing (`vblNow == lastVbl`) | **Matched** | Implemented in `port/nxdk/mushisam/main.cpp:233-238`; eliminates late-frame `16.6 ms -> 33.3 ms` double-sleep halving. |
| **Pillar 1.4:** Throttled USB OHCI hub polling (`usbh_pooling_hubs`) & `!InPlay` EEPROM flush | **Matched** | `xbox_input.h:60-63` polls OHCI root hub once per 60 frames once XID pad is attached; `main.cpp:207-209` gates EEPROM writes to `!inPlayNow`. |
| **Pillar 2.2:** Streamed `sound_rom.bin` + 1 MB looped BGM cache + 8-ch `Ymz770` | **Partial** | Saved `+7.5 MB` RAM (`ymz770.h:87-129`), **but** offline pre-baked `sfx_adpcm.bin` was **not** built—`ImaAdpcmCache` (`adpcm_cache.h:15`) still dynamically allocates up to `2.0 MB` of `std::vector` heap across stages. |
| **Pillar 2.1 & 2.3:** Menu + Stages 1–5 `MPAK` binaries (`pak_menu`, `pak_s1..s5`) & zero gameplay disk I/O | **NOT Matched** | Only `pak_s1.bin` (`8.1 MB`) and `pak_s2.bin` (`10.5 MB`) exist. `pak_menu.bin` and `pak_s3..s5.bin` were **never generated**, causing Stage 3+ to hammer `mushisam_u2` and trigger a 60 Hz failing `CreateFileA` storm. |
| **Pillar 3:** Full 5-stage AOT SH-3 harvest, `-O3` top-500 hot bank, inline `JSR`, and native C++ hot-loop rewrites (`0x0C160E1C`, bullet/collision loops) | **Partial / NOT Matched** | 32-bit `sint32 rem` countdown landed (`3,500`-frame parity = `0` mismatches), **but** `blocks.txt` stops at `frame 18,000` (mid-Stage 2), leaving **86.7M interpreted block fallbacks** across Stage 2 Boss & Stages 3–5; `0x0C160E1C` and bullet loops were not rewritten in C++; cold TUs stayed `-Oz` (`4.68%` cumulative dropped VBlanks vs `<= 1.0%` target). |
| **Pillar 4:** 4 MB dual-bank sprite atlas + GPU overlap + MMX blit + NV2A Render-to-Texture off-screen tilemap | **Partial** | 4 MB dual-bank (`2 × 1024 × 1024`) atlas, deferred `pb_busy()` (`GpuWait` `2,059 µs -> 565 µs`), and 4-px MMX blit landed, **but** off-screen `BlitVramToVram` (`x=7456..8191`, `21.4` draws/frame) still runs in software on the CPU (`~1.5 ms/frame`). |
| **Pillar 5:** `< 45 ms` low-latency audio (`3 × 512` frames) with `0` underruns | **NOT Matched** | Reverted to `4 × 1024` (`85.3 ms`, `Sleep(6)`) in `xbox_audio.h:28-30` because `512`-frame polling with `Sleep(2)` caused `18` underruns and preempted the single PIII core. |

---

## 2. Empirical Root-Cause Diagnosis: Why Screen Goes Black on Stage 3

A `72,000`-frame host instrumentation pass across **Boot/Menu + Stages 1–5** exposed the exact working-set profile of every stage:

| Bucket | NAND Pages Read | VRAM Read | VRAM Written (`Upload`) | Active VRAM Union | Interpreted SH-3 Blocks |
|---|---:|---:|---:|---:|---:|
| **Boot / Menu** | `19,421` (`39.12 MB`) | `167` (`5.2 MB`) | `1,250` (`39.1 MB`) | `1,252` (`39.1 MB`) | `0` |
| **Stage 1** (`stage=0`) | `4,014` (`8.08 MB`) | `849` (`26.5 MB`) | `266` (`8.3 MB`) | `857` (`26.8 MB`) | `0` |
| **Stage 2** (`stage=1`) | `5,222` (`10.52 MB`) | `751` (`23.5 MB`) | `342` (`10.7 MB`) | `768` (`24.0 MB`) | `3.43M` (Boss) |
| **Stage 3** (`stage=2`) | **`10,141` (`20.43 MB`)** | **`1,064` (`33.2 MB`)** | **`658` (`20.6 MB`)** | **`1,203` (`37.6 MB`)** | **`42.80M` (`~2,140/f`)** |
| **Stage 4** (`stage=3`) | `7,517` (`15.14 MB`) | `896` (`28.0 MB`) | `490` (`15.3 MB`) | `932` (`29.1 MB`) | `38.99M` (`~1,700/f`) |
| **Stage 5** (`stage=4`) | `8,701` (`17.53 MB`) | `471` (`14.7 MB`) | `564` (`17.6 MB`) | `919` (`28.7 MB`) | *(continues)* |

Stage 3 (the giant walking Leviathan) turns black due to **three compounding failures** firing simultaneously during the black fade-out transition between Stage 2 Boss clear and Stage 3 start (`f ≈ 25,800..26,800`):

1. **Fatal Heap OOM in `Blitter::Upload()` -> `NativeList::pixels` (`blitter.cpp:198`) While Screen Is Faded to Black:**
   - Stage 1 telemetry already showed only **`3.9 MB` free RAM** on a 64 MB retail Xbox *before* `ImaAdpcmCache` dynamically allocates up to **`2.0 MB`** across Stages 1–2 (`~1.9 MB` free RAM left).
   - Stage 3 uploads **`20.6 MB` (`658` pages)** of new Leviathan graphics. During Stage 3's black load transition, `Blitter::Upload()` buffers all uploaded pixels for the frame inside `std::vector<uint16_t> NativeList::pixels` (`native->pixels.resize(...)`). Doubling `std::vector` capacity (`2 MB -> 4 MB -> 8 MB` while holding both old and new buffers) exceeds the `< 2 MB` remaining heap; `malloc` returns `NULL`, and under `-fno-exceptions`, `dst[i] = Word(address)` (`blitter.cpp:207`) dereferences `nullptr` and hard-freezes while the screen is black.
   - Even if `pixels.resize` were avoided, Stage 3's active VRAM union is **`1,203` pages (`37.6 MB`)**, exceeding `kRetailMaxAllocPages = 928` (`29.0 MB`) by **`275` pages (`8.6 MB`)**—which either zeroed Stage 3 graphics to black (before `914968f`) or triggers heavy synchronous `Z:\mushivrm.bin` HDD swap thrashing (after `914968f`).
2. **60 Hz Infinite Failing DVD `CreateFileA("D:\\mushisam\\pak_s3.bin")` Storm (`main.cpp:210-216`):**
   - On Stage 3 (`curStage == 2`), `curStage != loadedStagePack` (`2 != 1`) calls `board.nand.AttachStagePack("D:\\mushisam\\pak_s3.bin")`.
   - `AttachStagePack()` (`nand.h:45`) detaches `pak_s2.bin`, calls `CreateFileA` on nonexistent `pak_s3.bin`, and returns `false` **without updating `loadedStagePack`**.
   - As a result, `main.cpp` hammers the DVD filesystem with a failing `NtCreateFile` directory lookup **every single frame (60 Hz forever)** right while Stage 3 also streams `10,141` uncached NAND pages across 4,000 gameplay frames (`f=26,000..30,000`).
3. **Uncompiled SH-3 Interpreter Explosion (`42.8 Million` Interpreted Blocks in Stage 3):**
   - Because `gen/blocks.txt` ended at `frame 18,000`, Stage 2 Boss and all of Stage 3 fall back to `SH3::InterpretBlock()` (`2,140–4,890` interpreted blocks/frame), multiplying SH-3 CPU cost by 3–5× during Stage 3 load and gameplay.

---

## 3. Execution Pillars for Next Pass

### Pillar 1 — Fix Stage 3 Crash, Eliminate `NativeList::pixels`, & Complete Menu + Stages 1–5 Packs
1. **Zero-Copy Direct `Blitter::Upload` Streaming (Save `+4–8 MB` Transient Heap, Fix Stage 3 OOM):**
   - Add a direct upload sink callback/interface on `Blitter` (`onUploadRow` / `Nv2aCv1kRenderer::WriteUploadRect`) so `0x2000` blitter uploads write 16-bit pixels **directly from `board.ram` into sparse 128×128 VRAM pages** (ordered accurately via `NativeList::steps` or staged page writes without buffering megabytes in `NativeList::pixels`). Shrink `NativeList::pixels` capacity to `0` bytes (`std::vector<uint16_t>().swap(nativeList.pixels)`).
2. **Fix `loadedStagePack` Retry Bug & Build Complete `MPAK` Suite (`pak_menu.bin`, `pak_s1..s5.bin`):**
   - Fix `main.cpp:210-216` so `loadedStagePack = curStage` is recorded on transition attempt (never retrying a missing file at 60 Hz) and attach `pak_menu.bin` when `!inPlayNow`.
   - Extend `tools/build_stage_packs.py` using the `72,000+`-frame full-game trace (plus boss clear & attract/menu sweep) to generate `pak_menu.bin`, `pak_s1.bin`, `pak_s2.bin`, `pak_s3.bin`, `pak_s4.bin`, and `pak_s5.bin` (including Stage 3's mid-stage Leviathan waves `f=26,000..30,000` so **zero** scattered `mushisam_u2` seeks occur mid-stage).
3. **Offline Pre-Baked `sfx_adpcm.bin` (Eliminate `2.0 MB` Dynamic `ImaAdpcmCache` Heap):**
   - Extend `tools/build_audio_pack.py` to decode all short non-looped YMZ770 SFX phrases (`<= 32,768` samples) offline into a compact `D:\mushisam\sfx_adpcm.bin` (`~0.8–1.2 MB` flat allocation loaded once at boot), deleting runtime `m_captureBuf` / `std::vector` heap growth and first-play MPEG-2 decode spikes.

### Pillar 2 — Direct `nxdk` Library Customizations (`nxdk/lib/...`)
Modify the local `nxdk` submodule sources (`C:\Users\lenovo\Documents\ree\nxdk`) and rebuild the `nxdk` static libraries used by this project:

1. **Reclaim `+4.31 MB` of Wasted Contiguous Physical RAM in `nxdk` (`hal/video.c` + `pbkit/pbkit.c`):**
   - **Free Dead Boot Framebuffer (`+1.23 MB`):** Add `XVideoFreeFB()` in `nxdk/lib/hal/video.c` (or release `framebufferMemory` once `pb_show_front_screen()` switches `PCRTC_START` to `pb_FBAddr[0]`), reclaiming the dead `640×480×4B` buffer noted at `pbkit.c:2879`.
   - **Optional No-Z Mode in `pbkit` (`+1.23 MB` + Zero Z-Tile Overhead):** Add `pb_set_depth_stencil_enable(bool enable)` in `nxdk/lib/pbkit/pbkit.h` & `pbkit.c` (`pbkit.c:3048`). When disabled (2D sprite engine with `DEPTH_TEST_ENABLE = 0`), skip `MmAllocateContiguousMemoryEx(DSSize)` (`1.23 MB`) and skip `pb_assign_tile(1, ...)` Z-compression tag traffic.
   - **16-bit `R5G6B5` Scanout Mode (`+1.85 MB` + 50% Less NV2A Framebuffer ROP Bandwidth):** CV1000 native output is 15-bit RGB555 (`A1R5G5B5`); switching display + `pbkit` triple-buffers from 32-bit `A8R8G8B8` (`3 × 1.23 MB = 3.69 MB`) to 16-bit `R5G6B5` (`3 × 614 KB = 1.84 MB`) via `XVideoSetMode(640, 480, 16, REFRESH_DEFAULT)` + `pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false)` saves another **`+1.85 MB`** of contiguous RAM and halves backbuffer clear/fill memory bandwidth (verify tint/blend visual parity; keep 32-bit toggleable via compile flag if desired).
   - **Expand Hot Sparse VRAM Pool to `1,184–1,216` Pages (`37.0–38.0 MB`):** Feed the reclaimed `+6.3 MB` (`+4.3 MB` `nxdk` + `+2.0 MB` `ImaAdpcmCache`/upload heap) directly into `kRetailMaxAllocPages`, raising hot VRAM residency from `928` (`29.0 MB`) to **`~1,184+` pages (`>= 37.0 MB`)** so even Stage 3's giant `1,203`-page Leviathan fits almost 100% in RAM with near-zero `Z:\mushivrm.bin` swaps!
2. **Zero-Interrupt `pbkit` Render-Target Switch & Pushbuffer Fast Path (`nxdk/lib/pbkit/pbkit.c`):**
   - **Eliminate 16 GPU-to-CPU Hardware Interrupts/Frame (`set_draw_buffer`):** `pbkit.c:1638-1721` currently emits `WAIT_MAKESPACE` (full 3D pipeline idle stall) + **8 `FIRE_INTERRUPT (PB_SETOUTER)` IRQ3/DPC traps** *twice per frame* (`pb_finished()` and `pb_target_back_buffer()`) just to rewrite `NV_PRAMIN` DMA context addresses. Since `pb_init()` already configures `DMA_COLOR` (`Channel 9`, `pbkit.c:2665`) across `0..MAXRAM`, replace `set_draw_buffer()`'s `PB_SETOUTER` storm with direct pushbuffer methods: `NV097_SET_SURFACE_PITCH` + `NV097_SET_SURFACE_COLOR_OFFSET = pb_FBAddr[idx] & 0x03FFFFFF` (**2 DWORDs, `0.01 µs`, zero IRQ3 traps, zero pipeline stalls**).
   - **Disable Default `MULTISAMPLE` & `ZCULL` in `pb_init()` (`pbkit.c:3219-3233`):** Turn off `NV20_TCL_PRIMITIVE_3D_MULTISAMPLE` and `CULL_ENABLE` for pure 2D quad rasterization, and avoid unnecessary `pb_jump_to_head()` busy-wait spins (`pbkit.c:1525`) until `pb_Put` nears `pb_Tail - 64 KB`.
3. **Single-Syscall NT File I/O & ThinLTO + Pentium III `nxdk` Archive Build (`nxdk/Makefile`):**
   - Replace two-syscall `SetFilePointerEx` + `ReadFile`/`WriteFile` pairs (`nxdk/lib/winapi/fileio.c:259`) in `nand.h`, `ymz770.cpp`, and `nv2a_cv1k_renderer.h` with direct `NtReadFile` / `NtWriteFile` passing `&ByteOffset` in **one** kernel call (halving syscall overhead and making file reads atomic/thread-safe).
   - Build `nxdk` C libraries (`libpbkit.lib`, `libhal.lib`, `libusb.lib`, `libwinapi.lib`, `libc.lib`) with `-O2 -march=pentium3 -msse -mmmx -mfpmath=sse -fomit-frame-pointer -flto=thin` so `pb_begin`, `pb_end`, `pb_busy`, and `XAudioProvideSamples` inline across library boundaries into `main.exe`.

### Pillar 3 — Full 5-Stage + Boss SH-3 AOT Recompilation & Native C++ Hot-Loop Rewrites
1. **Harvest 100% of Stages 1–5 + Bosses + Attract/Menu (`0` Interpreted Blocks):**
   - Run an `85,000+`-frame `MushisamFakePlay` sweep (plus Stage-Select jumps via `kBeginStage = 0x0C05D4E0` for all 5 stages, bosses, and game-over/menu paths) to record every reachable SH-3 block into `gen/blocks.txt` (`interpBlk == 0` across all 5 stages!).
   - Clamp static branch following (`FollowBlock` / `recomp_sh3.py`) strictly to valid executable code ranges and safe `RamOracle` intersection across Boot + Stages 1–5 so data words are never mis-decoded as `ILLEGAL` or over-folded.
2. **Native C++20 / MMX / SSE1 Rewrites of Top SH-3 Bottlenecks (`engine/native_cv1k/mushisam_native.h`):**
   - Profile the full `85,000`-frame 5-stage run with `-DMUSHISAM_PROFILE_HOT` and intercept the top SH-3 CPU hotspots (`0x0C160E1C` blitter command emitter + top bullet-update, hitbox-collision, and sprite-sort loops) with verified native x86/SSE1 C++ implementations.
   - Inline direct/hot `JSR` targets inside `recomp_sh3.py` and expand `blocks_hot.cpp` (`-O3`) to cover **>= 95%** of total dynamic SH-3 cycles while keeping `main.exe` well within our expanded RAM budget.

### Pillar 4 — NV2A GPU Render-to-Texture Off-Screen Tilemap & `< 32 ms` Event-Driven Audio
1. **Hardware NV2A Render-to-Texture for `x=7456..8191` Off-Screen Tilemap Cache (`~1.5 ms/frame` Saved):**
   - Using our new zero-interrupt `NV097_SET_SURFACE_COLOR_OFFSET` fast path, allocate a dedicated contiguous `A1R5G5B5` / `R5G6B5` NV2A surface for the CV1000 off-screen scrolling tilemap ring (`x=7456..8191`, `y=0..511`), routing off-screen tilemap composition quads to NV2A PGRAPH instead of CPU `BlitVramToVram()`. Keep software `BlitVramToVram` only as an automatic fallback for any rare self-overlapping or unmapped off-screen region.
2. **Event-Driven `< 32 ms` AC'97 Audio (`3 × 512` Frames, `0` Underruns, Minimal CPU Preemption):**
   - With `sfx_adpcm.bin` eliminating runtime SFX MPEG-2 spikes and `NtReadFile(&ByteOffset)` streamlining BGM streaming, upgrade `xbox_audio.h` from blind `Sleep(6)` polling (`85.3 ms`) to AC'97 buffer-completion IRQ / waitable kernel timer wakeup with **`3 × 512` stereo frames (`32.0 ms` latency)** and `0` underruns.

---

## 4. Strict Verification & Acceptance Criteria
1. **Bit-Exact / Cycle-Accurate Host Differential Parity:** `mushisam_parity_test.exe --parity` passes with **`0` mismatches** across Boot, Menu, and Stages 1–5 (including native C++ hotspot hooks).
2. **Stage 3 Black-Screen Resolution (Live Xemu Verification):** Live automated playthrough transitions cleanly from Stage 2 Boss -> Stage 3 -> Stage 3 Leviathan -> Stage 3 Boss -> Stage 4 -> Stage 5 with **visible gameplay (verified via Xemu screenshots on Stage 3!)**, **`0` black-screen hangs/crashes**, and **`0` missing/corrupted sprites or tiles**.
3. **Zero Uncompiled SH-3 Code & Zero Gameplay Disk Stutter:**
   - `interpBlk == 0` (or `< 5` one-shot transition blocks total) across all 5 stages on Xbox.
   - `0` failed `CreateFileA` calls and `0` mid-stage `mushisam_u2` cache misses once a stage's `pak_s*.bin` finishes initial load; `>= 6.0 MB` free Xbox physical RAM headroom maintained throughout Stages 1–5.
4. **Real-Time 60 Hz Wall-Clock & Audio Targets (`decode_perf.py` v4):**
   - `EffectiveFps >= 59.4 FPS` (`DroppedVblPct <= 1.0%`) measured by real-time `KeQuerySystemTime` + `pb_get_vbl_counter()` across Stages 1, 2, and 3 heavy bullet/Leviathan scenes, with `AudioUnderruns == 0` at `<= 32–43 ms` latency.
