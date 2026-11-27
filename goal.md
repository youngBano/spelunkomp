# Goal: Full NV2A GPU Blitter Offload (Zero-Copy UMA Page Pool + Hardware RTT Tilemap) & Authentic Cave Slowdown Toggle

## 1. Mission & Engineering Motivation

With the Stage 3 U4 ROM black-screen freeze, lossless RLE16 VRAM backing, and async audio prefetch solved, the remaining CPU render bottleneck is **`3.5 ms` average (and `15–43 ms` spikes in Stage 3)** spent copying CV1000 VRAM pixels on the Pentium III CPU:
- `CopyVramRectToAtlas()` (`1.4–6.6 ms/frame`) copies sprites/tiles out of CPU heap pages into a separate `4 MB` GPU shelf atlas.
- `BlitVramToVram()` (`0.9–1.3 ms/frame`) software-composites the off-screen scrolling background ring buffer (`x = 7456..7903, y = 32..287`) on the CPU.
- Meanwhile, `blitter.cpp` `#if`s out `m_vramClocks` under `-DXBOX_NO_SW_VRAM -DXBOX_FAST_CPU`, so the LT **No-Cave-Slowdown** toggle currently only disables upload bus-stealing rather than toggling authentic CV1000 draw-clock blitter slowdown.

50,000-frame empirical audit across Stages 1–4 (`27,292,473` draws) proves this entire pixel pipeline can move **100% onto the NV2A GPU**:
- **94.2%** of all on-screen draws fit inside a single `128×128` VRAM page (the remaining `5.8%` split into at most `4` sub-quads).
- **100%** of off-screen `BlitVramToVram` compositing (`1,058,794` draws) lies inside a single **`448 × 256`** bounding box `(7456..7903, 32..287)` with `0` flips and `0` overlaps — fitting inside one **`512 × 256` `A8R8G8B8` (`512 KB`) NV2A Render-to-Texture (RTT) surface**.

---

## 2. Implementation Pillars

### Pillar 1 — Zero-Copy UMA GPU-Resident `128×128` VRAM Page Pool (Delete the `4 MB` Shelf Atlas!)
1. **Direct GPU-Resident Contiguous Page Slabs (`nv2a_cv1k_renderer.h`):**
   - Replace the `576` CPU heap `std::vector` pages + `4 MB` `atlasAlloc_` shelf atlas + `256 KB` `rectHash_` with **contiguous write-combined NV2A page slabs** (`640` hot `128×(128+4)` `A1R5G5B5` pages = `21.1 MB`, allocated in `10 × 2.06 MB` slabs via `MmAllocateContiguousMemoryEx`).
   - Deleting the `4 MB` shelf atlas frees `+4.0 MB` of contiguous RAM, allowing `+64` MORE hot VRAM pages (`640` vs `576`) while using **less total RAM** and preserving the **100% lossless 3-tier backing** (`640` hot GPU pages → `3.5 MB` RLE16 cold RAM → lazy lossless `Z:\mushivrm.bin` spill).
2. **Direct Sub-Quad Drawing (`CopyVramRectToAtlas = 0.00 ms`):**
   - Each `DrawOp` emits `1–4` hardware sub-quads split along `128×128` page boundaries, binding `NV097_SET_TEXTURE_OFFSET` directly to the hot page's physical address (`slotBasePhys + (pageGen & 3) * 256`).
   - Unmodified pages keep an identical `(offset, height)` across frames (100% permanent cache hit on both real NV2A and Xemu host GPU); modified pages advance their 4-row guard phase (`pageGen & 3`) to guarantee cache freshness.
   - Mid-frame uploads targeting a page already referenced in the current frame (`slotUsedFrame_[slot] == frameId_`) cleanly flush queued draws before overwriting so painter's order stays 100% exact.
   - Eliminates `CopyVramRectToAtlas()` (`0 µs`), `FlushMidFrameAndResetShelf()` (`0` mid-frame stalls), and `rectHash_` completely!

### Pillar 2 — NV2A Hardware Render-to-Texture (RTT) for Off-Screen `BlitVramToVram` (`0.00 ms` CPU!)
1. **`512 × 256` `A8R8G8B8` Linear RTT Surface (`512 KB`, `x = 7424..7935, y = 32..287`):**
   - Allocate one `512 × 256` `A8R8G8B8` contiguous surface (`pitch = 2048 B`, `512 KB`).
   - Off-screen draws targeting `(7424..7935, 32..287)` switch NV2A's color render target (`NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8`, `NV097_SET_SURFACE_PITCH`, `NV097_SET_SURFACE_COLOR_OFFSET`, identity `COMPOSITE_MATRIX`) and rasterize as hardware NV2A quads (preserving exact `0x00`/`0xFF` alpha, hardware tinting, and hardware blending).
   - Main-pass draws sampling from `(7424..7935, 32..287)` bind the RTT surface as `LU_IMAGE_A8R8G8B8` and draw directly on NV2A — reducing CPU `BlitVramToVram` to **`0.00 ms`**!
   - Rare direct uploads into `(7424..7935, 32..287)` expand `A1R5G5B5 → A8R8G8B8` straight into the RTT surface in exact draw order.

### Pillar 3 — Authentic Cave CV1000 Blitter Slowdown with LT Toggle (`blitter.cpp` + `cv1000.cpp`)
- Gate `Blitter::DrawOp` / `Blitter::Write` `m_vramClocks` & line-fetch timing on `!noSlowdown` (instead of `#if !defined(XBOX_NO_SW_VRAM) && !defined(XBOX_FAST_CPU)`), so default play reproduces authentic Cave CV1000 blitter slowdown while **LT (Left Trigger + 4×4 green corner pixel)** switches to zero-slowdown uncapped mode.

---

## 3. Strict Verification Gates
1. **Host Differential Parity:** `mushisam_parity_test.exe --parity --frames 5000` passes with **`0` mismatches**.
2. **Live Xemu Visual & Framebuffer Verification (Boot → Stage 1 → Stage 2 → Stage 3):**
   - Confirm **`0` missing sprites or tiles** (inspect live R5G6B5 framebuffer pixel coverage & unique colors in Stage 1, Stage 2, and Stage 3, plus confirm zero `BlitVramToVram` / `AtlasCopy` CPU time).
   - Confirm **`Render` time drops from `3.5 ms` avg (`15–43 ms` Stage 3 spikes) to `< 1.0 ms` avg (`< 3 ms` Stage 3!)**.
   - Confirm **Stage 3 (`f > 30,000`) runs smoothly with `0` black screens, `0` freezes, `minFree >= 3.0 MB`, and `~60 FPS`**.
3. **Clean Playable Xbox ISO:** Rebuild `default.xbe` and `mushisam-cv1000-xbox.iso` without `FAKE_PLAY`/`START_STAGE`.
