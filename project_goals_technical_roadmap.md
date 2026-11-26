# Project Goal: `spelunkomp-xbox` (Original Xbox Port via `nxdk`)

## 1. Vision & Objective

Port **`tmcleroy/spelunkomp`**—a static recompilation suite covering 26 Cave arcade shoot 'em ups across Motorola 68000, PGM, and Hitachi SH-3 (CV1000) arcade hardware—to the **Original Xbox** using the open-source **`nxdk`** toolchain.

Unlike dynamic CPU emulation, static recompilation translates arcade machine code ahead-of-time into native C++20. By pairing the recompiled game logic with a zero-overhead, bare-metal NV2A GPU presenter (`pbkit` + `xgu`), this port aims to deliver **full-speed gameplay with cycle-accurate Cave slowdown** on original 2001 console hardware.

---

## 2. Target Hardware & Platform Constraints

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
* **Hardware Initialization & Scanout:** Use `pbkit` (`pb_init()`, `pb_wait_for_vbl()`, `pb_finished()`) to configure NV2A DMA channels, `PCRTC` video modes (`480i`, `480p`, `720p`), and double/triple-buffered render targets.
* **Direct Pushbuffer Emission:** Use `xgu` / `NV097` inline helpers over `pb_begin()` / `pb_end()` pushbuffer pointers to configure texture stages and draw textured quads with near-zero CPU overhead.
* **Zero-Cost TATE Rotation & Scaling:** Upload the native `240×320` (or `320×240`) arcade framebuffer into contiguous NV2A texture memory and perform 90°/270° TATE rotation, aspect-ratio correction, and point/bilinear filtering entirely in GPU vertex/texture coordinates.
* **Optional Scanline Pass:** Replace heavy `.slangp` CRT presets with an optional single-pass NV2A register-combiner scanline/aperture-grille blend.

### B. SDL 2 Platform Shim (`nxdk`)
* **Input:** Map Xbox controllers (Duke / Controller S) and USB arcade sticks via `SDL_GameController` / `SDL_Joystick`, preserving full button remapping and training-mode hotkeys.
* **Audio:** Route synthesized/decoded PCM audio streams (`YMZ280B`, `ICS2115`, `OKI M6295`, etc.) through SDL 2's audio queue/callback with tuned buffer sizes to prevent underruns without introducing input latency.
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

**Progress (2026-11-26):** Dropped SDL2 completely in favor of native `nxdk` APIs (`xbox_audio.h` AC'97 DMA `XAudio` + `xbox_input.h` USB XID gamepad) and linked all **11,974 statically-recompiled native 68000 blocks** (`M68K::Mode::Native`, `-O3 -march=pentium3 -msse`). Live Xemu + GDB + VRAM dump (`xemu_screen.png`) confirms **locked 60.0 FPS** (`742 → 862` = 120 frames in 2.0s) and crisp upright 90° CCW YOKO rendering of DoDonPachi's full `WARNING` + `©ATLUS/CAVE 1995 1996` screen. Host validation (`core_validation_test.cpp`, 27/27 checks) passes.

- [x] Set up `nxdk` build configuration (`Makefile`) compiling Cave 68000 (`ddonpach`) with Clang C++20 (`-O2`).
- [x] Stub out `SDL_GPU`, `shaderc`, and desktop file dialogs.
- [x] Patch 64-bit memory mapping calls to 32-bit flat allocations.
- [x] Package a valid `default.xbe` and verify live game-loop execution in **Xemu** *(verified via GDB + NV2A VRAM dump `xemu_screen.png`)*.

### Phase 2: Low-Level NV2A Presenter (`pbkit` + `xgu`)

**Progress (2026-11-26):** `spelunk_xbox::Nv2aPresenter` in `port/nxdk/core/nv2a_presenter.h` is **visually verified in Xemu** (`xemu_screen.png`): upright 360×480 YOKO quad centered on 640×480, accurate `LU_IMAGE_A8B8G8R8` colors, `NV097_SET_DEPTH_MASK = 0`, `MAX_LOD_CLAMP = 0xFFF`, stages 1–3 disabled, and stage 0 combiner OCW zeroed.

- [x] Implement `pbkit` initialization, framebuffer clearing, and VSync frame presentation.
- [x] Verify dynamic texture upload (`LU_IMAGE_A8B8G8R8`) and non-black quad rendering in **Xemu** *(verified via `xemu_screen.png`)* .
- [ ] Verify YOKO/TATE (`RB`), nearest/bilinear (`LB`), and integer/aspect-fill (`L3`) modes on **Xemu** *(wired; visual verification pending)*.
- [ ] Port `spelunkomp`'s hitbox overlay rendering as lightweight NV2A line/quad primitives.

### Phase 3: Audio, Input, & FATX Filesystem
- [ ] Verify SDL 2 controller polling (`SDL_INIT_GAMECONTROLLER`) and button mapping in **Xemu** *(wired in `main.cpp`; runtime verification pending)*.
- [ ] Verify SDL 2 PCM audio output (44.1 kHz 16-bit mono, `SDL_QueueAudio`, 250 ms cap) for pitch and 60 Hz timing lock *(wired in `main.cpp`; runtime verification pending)*.
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
1. At least one M68000 Cave title (`ddonpach`) builds cleanly into a standalone `default.xbe` via `nxdk`.
2. The title boots and runs reliably on a stock **64 MB Original Xbox** (as well as Xemu) at a locked 60 Hz refresh rate with synchronized audio.
3. Both YOKO and TATE screen orientations render cleanly through the `pbkit` + `xgu` NV2A backend with responsive controller input.