# Project Goal: `spelunkomp-xbox` (Original Xbox Port via `nxdk`)

## 1. Vision & Objective

Port **`tmcleroy/spelunkomp`**—a static recompilation suite covering 26 Cave arcade shoot 'em ups across Motorola 68000, PGM, and Hitachi SH-3 (CV1000) arcade hardware—to the **Original Xbox** using the open-source **`nxdk`** toolchain.

Unlike dynamic CPU emulation, static recompilation translates arcade machine code ahead-of-time into native C++20. By pairing the recompiled game logic with a zero-overhead, bare-metal NV2A GPU presenter (`pbkit` + `xgu`), this port aims to deliver **full-speed gameplay with cycle-accurate Cave slowdown** on original 2001 console hardware.

---

## 2. Target Hardware & Platform Constraints

All architectural decisions must respect the Original Xbox's fixed hardware envelope and `nxdk`'s Ring 0 execution model:

| Subsystem | Original Xbox Specification | Engineering Implication |
| :--- | :--- | :--- |
| **CPU** | Intel Pentium III (32-bit `i386`, 733 MHz, 128 KB L2 cache) | Same little-endian x86 family as desktop targets, but requires strict 32-bit addressing and cache-friendly code layout. |
| **GPU** | NVIDIA NV2A (233 MHz, `NV097` / `PGRAPH` 3D engine) | No Vulkan, D3D12, or desktop shaders. Driven directly via DMA pushbuffers (`pbkit` + `xgu`). |
| **Memory** | **64 MB Unified DDR SDRAM** (Retail) / **128 MB** (Dev/Modded) | Shared between CPU, `.xbe` binary, GPU textures/framebuffers, and audio buffers. Tightest system bottleneck. |
| **Storage** | IDE HDD (`E:\`, `F:\`) & DVD-ROM (`D:\`) using **FATX** | Max 42-character filenames, restricted ASCII character set, and slower sequential read speeds than modern SSDs. |
| **OS / SDK** | `nxdk` (Clang/LLVM C++20, SDL 2, `pbkit`, Xbox kernel APIs) | Single-process Ring 0 flat physical memory map; no virtual memory paging or SDL 3 support. |

---

## 3. Core Technical Strategy

### A. Bare-Metal NV2A Graphics Backend (`pbkit` + `xgu`)
Replace `spelunkomp`'s desktop `SDL_GPU` presenter and runtime `shaderc` / `.slangp` shader pipeline with a lightweight hardware presenter built directly on **`pbkit`** and **`xgu`**:
* **Hardware Initialization & Scanout:** Use `pbkit` (`pb_init()`, `pb_wait_for_vbl()`, `pb_finished()`) to configure NV2A DMA channels, `PCRTC` video modes (`480i`, `480p`, `720p`), and double/triple-buffered render targets.
* **Direct Pushbuffer Emission:** Use `xgu` inline helpers over `pb_begin()` / `pb_end()` pushbuffer pointers to configure `NV097` texture stages and draw textured quads with near-zero CPU overhead.
* **Zero-Cost TATE Rotation & Scaling:** Upload the native `240×320` (or `320×240`) arcade framebuffer into contiguous/swizzled NV2A texture memory and perform 90°/270° TATE rotation, aspect-ratio correction, and point/bilinear filtering entirely in GPU vertex/texture coordinates.
* **Optional Scanline Pass:** Replace heavy `.slangp` CRT presets with an optional single-pass NV2A register-combiner scanline/aperture-grille blend.

### B. SDL 2 Platform Shim (`nxdk`)
Backport `spelunkomp`'s `recomp-common` platform layer from **SDL 3** to **`nxdk`'s SDL 2**:
* **Input:** Map Xbox controllers (Duke / Controller S) and USB arcade sticks via `SDL_GameController` / `SDL_Joystick`, preserving full button remapping and training-mode hotkeys.
* **Audio:** Route synthesized/decoded PCM audio streams (`YMZ280B`, `ICS2115`, `OKI M6295`, etc.) through SDL 2's audio callback with tuned buffer sizes to prevent underruns without introducing input latency.
* **Configuration & UI:** Replace desktop launcher dependencies with an embedded, controller-driven Xbox menu overlay rendered via NV2A textured quads/bitmap fonts.

### C. 32-Bit Memory Arena Adaptation
* Replace 64-bit virtual address space reservations (`mmap` / `VirtualAlloc`) with compact, deterministic 32-bit memory arenas allocated via Xbox kernel contiguous/heap allocators (`MmAllocateContiguousMemory` for DMA-accessible GPU buffers, standard heap for CPU RAM).
* Audit pointer casts and address translation masks in the recompiled output to guarantee 32-bit pointer safety.

### D. Tiered Memory Management (64 MB vs. 128 MB)
* **64 MB Retail Baseline:** Target ~45–52 MB total peak memory usage (leaving safe headroom for kernel/GPU DMA buffers). Disable or cap RAM-heavy rewind ring buffers by default on 64 MB consoles.
* **Asset Streaming:** For larger PGM and CV1000 ROM/NAND datasets (64 MB–128+ MB), stream graphics/audio banks on demand from the Xbox HDD rather than preloading entire ROM archives into RAM.
* **128 MB Console Tier:** Detect available physical RAM at boot (`MmQueryStatistics`) and automatically unlock extended rewind buffers and full in-memory caching when running on 128 MB RAM-upgraded Xboxes.

---

## 4. Phased Development Milestones

### Phase 1: `nxdk` Build Integration & Headless Boot

**Progress (2026-11-24):** A separate nxdk XBE now compiles and links the Cave 68000 board core (17 translation units). `main.cpp` now loads the extracted `ddonpach` ROMs (`b1.u27`, `b2.u26`, `u50`-`u53`, `u60`-`u62`, `u6`-`u7`, `eeprom-ddonpach.bin`) from `D:\ddonpach` (packaged into the 20 MB `spelunkomp-cave68k-core.iso`), runs `board.Reset()`, and executes 10 frames via `board.RunFrame()`. Xemu v0.8.139 boots the 20 MB ISO cleanly. Framebuffer blitting/rendering, controller input, audio, and real hardware boot remain to be wired and verified.

- [ ] Set up `nxdk` build configuration (`Makefile` / CMake toolchain) compiling `recomp-common` and a single M68000 target (e.g., *DoDonPachi* or *ESP Ra.De.*) with Clang C++20.
- [ ] Stub out `SDL_GPU`, `shaderc`, and desktop file dialogs.
- [ ] Patch 64-bit memory mapping calls to 32-bit flat allocations.
- [ ] Package a valid `default.xbe` and verify headless game-loop execution via serial/debug logging in **Xemu**.

### Phase 2: Low-Level NV2A Presenter (`pbkit` + `xgu`)

**Progress (2026-11-24):** Wired `pbkit` initialization (`pb_init`, `pb_show_front_screen`, `pb_wait_for_vbl`, `pb_reset`, `pb_finished`) and per-frame `board.Picture()` -> `pb_back_buffer()` CPU blitting (ABGR8888 -> XRGB8888 with 90° CCW upright rotation centered on 640×480) directly into `port/nxdk/core/main.cpp`. Built `default.xbe` and `spelunkomp-cave68k-core.iso` cleanly and launched in Xemu. Next: upgrade CPU blit to NV2A `xgu` textured quad with YOKO/TATE toggle and integer scaling, plus SDL 2 controller input.

- [x] Implement `pbkit` initialization, framebuffer clearing, and VSync frame presentation.
- [x] Implement dynamic texture upload for the arcade video output (`ARGB8888` / `RGB565`) via `xgu` (implemented `spelunk_xbox::Nv2aPresenter` in `port/nxdk/core/nv2a_presenter.h` using contiguous write-combined `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8` texture bound directly to stage 0 with zero CPU pixel-swapping).
- [x] Render the game quad with support for horizontal (YOKO) and vertical (TATE) orientations, integer scaling, and bilinear/nearest toggle (wired in `port/nxdk/core/nv2a_presenter.h` and `main.cpp`: `RB` = YOKO/TATE, `LB` = nearest/bilinear, `L3` = integer/aspect-fill scaling; built cleanly and launched in Xemu).
- [ ] Port `spelunkomp`'s hitbox overlay rendering as lightweight NV2A line/quad primitives.

### Phase 3: Audio, Input, & FATX Filesystem
- [x] Implement SDL 2 controller polling and customizable button mapping (wired `SDL_INIT_GAMECONTROLLER` in `port/nxdk/core/main.cpp`: D-pad/left-stick -> directional, A/B/X/Y -> Buttons 1-4, Back -> Coin 1, Start -> Start 1, RB -> YOKO/TATE orientation toggle).
- [x] Integrate SDL 2 PCM audio output (`SDL_INIT_AUDIO`, `SDL_OpenAudioDevice` at 44.1 kHz 16-bit mono, per-frame `board.SoundOut()` -> `SDL_QueueAudio` with queue cap and buffer clear in `port/nxdk/core/main.cpp`; pitch/timing lock at 60 Hz to be verified on Xemu/hardware).
- [ ] Normalize all save-state, config, and ROM/NAND asset paths to FATX-compliant naming on `D:\` and `E:\`.

### Phase 4: Memory Optimization & Multi-Hardware Expansion
- [ ] Profile RAM consumption across M68000 titles on a stock **64 MB** console; eliminate leaks and cap rewind history for retail safety.
- [ ] Implement HDD streaming for PGM and CV1000 titles (*Ketsui*, *DoDonPachi DaiOuJou*, *Mushihimesama*, *ESPGaluda II*, etc.).
- [ ] Add runtime detection for **128 MB** consoles to enable full-length rewind buffers.

### Phase 5: Hardware Validation & Accuracy Tuning
- [ ] Test on real Original Xbox hardware (1.0–1.6 revisions) across 480i/480p component and RGB outputs.
- [ ] Benchmark frame times during high-bullet-density sequences to verify authentic Cave slowdown parity with zero unintended host-side frame drops.

---

## 5. Non-Goals (Out of Scope)

* **Runtime Shader Compilation:** `shaderc` and multi-pass `.slangp` shaders will not be ported due to NV2A hardware limits and RAM constraints.
* **Unbounded Rewind on 64 MB Units:** Multi-minute uncompressed state rewind will not be enabled on unmodded 64 MB consoles if it risks out-of-memory kernel panics.
* **Desktop Launcher Parity:** External GUI launchers are out of scope in favor of a native, controller-friendly Xbox title/game-select menu.

---

## 6. Definition of Done (v0.1 MVP)

The initial MVP milestone is complete when:
1. At least one M68000 Cave title builds cleanly into a standalone `default.xbe` via `nxdk`.
2. The title boots and runs reliably on a stock **64 MB Original Xbox** (as well as Xemu) at a locked 60 Hz refresh rate with synchronized audio.
3. Both YOKO and TATE screen orientations render cleanly through the `pbkit` + `xgu` NV2A backend with responsive controller input.