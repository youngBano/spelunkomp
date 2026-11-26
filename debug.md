# Visual Sprite & Tilemap Glitch Investigation & Root-Cause Fix (`debug.md`)

## 1. Symptom Summary
During gameplay and scrolling background transitions in *Mushihimesama* (`port/nxdk/mushisam`), sprites and background map tiles exhibited frequent visual mismatches:
- Wrong sprites appearing in place of earlier sprites/HUD digits on frames with VRAM uploads.
- Stale, missing, or mismatched scrolling background map tiles.
- Occasional 1-pixel edge bleed / hash collisions on large or tightly packed rects.

---

## 2. Empirical Diagnosis (`mushisam_parity_test.exe --bench --frames 4500`)

Instrumenting `NativeList` steps across 4,500 frames of boot, attract, and Stage 1 gameplay exposed **four concrete root causes** in [`nv2a_cv1k_renderer.h`](port/nxdk/mushisam/nv2a_cv1k_renderer.h):

| Counter / Probe | Measured Count (4,500 frames) | Impact |
| :--- | :---: | :--- |
| **`MidFrameUpAfterDraw`** (`step.upload` occurring *after* `step.draw`s in the same frame) | **`19,008 / 21,355` (`89.0%`)** | **Primary sprite-swap glitch:** `ResetAtlasCache()` reset `shelfX_ = 0, shelfY_ = 0` mid-frame *before* `SubmitDraws()` ran, overwriting earlier quads' texels at `(0, 0)` with new sprites! |
| **`FarOffscreen` VRAM Draws** (`d=(7456..7519, 32..223)` while screen buffers are at `(32,32)` & `(416,32)`) | **`62,084` (`~13.8 / frame`)** | **Primary tilemap mismatch:** *Mushihimesama* disables blitter clipping (`0xC000 -> 0`) to composite `32×32` background tiles into an off-screen VRAM ring buffer at `x = 7456..8191`, then blits that cache onto the screen. Dropping off-screen `NativeDraw`s left stale/zero tiles in `pageData_`! |
| **Hash Key Bitmask & Probe Overflow** | `w & 0x3FF` / `h & 0x3FF` + `64`-probe reset | `1024`-px dimensions wrapped to `0` (`0x3FF`), and probe/shelf wrap called `ResetAtlasCache()` mid-frame on top of queued vertices + performed uncached WC readback (`vbCur_[i].v += deltaBias`). |
| **0-Pixel Shelf Gutter + `0.001f` UV Inset** | Every adjacent shelf rect | `0.001f` is smaller than NV2A's `1/256` fixed-point UV interpolator precision, allowing 1-pixel edge bleed from neighboring sprites under `1.5×` YOKO scaling or bilinear filtering. |

### Sample Off-Screen Tilemap Composite Trace (Frame 1678)
```text
[FarOff f=1678] scr=(416,32) clip=(32,32) d=(7456..7487, 32..63)  src=(768,0) flg=0x140
[FarOff f=1678] scr=(416,32) clip=(32,32) d=(7488..7519, 32..63)  src=(768,0) flg=0x140
[FarOff f=1678] scr=(416,32) clip=(32,32) d=(7456..7487, 64..95)  src=(768,0) flg=0x140
[FarOff f=1678] scr=(416,32) clip=(32,32) d=(7488..7519, 64..95)  src=(768,0) flg=0x140
```

---

## 3. Root-Cause Fixes Implemented in [`nv2a_cv1k_renderer.h`](port/nxdk/mushisam/nv2a_cv1k_renderer.h)

1. **Decoupled Hash Invalidation (`InvalidateRectHash()`) from Shelf Reset:**
   - `shelfX_ = 0; shelfY_ = 0; atlasPhase_ = (atlasPhase_ + 1) & 3;` now executes **once at the start of `Render()`** (after `while (pb_busy())` confirms `PGRAPH` finished reading the previous frame), eliminating both mid-frame `(0, 0)` overwrites and Write-Combined vertex buffer readbacks.
   - Mid-frame `NativeUpload`s and off-screen VRAM blits call **`InvalidateRectHash()` (`++rectEpoch_` only)**—preserving all previously packed sprites in the current frame's atlas while allocating fresh shelf coordinates for subsequent draws.
   - Expanded the dynamic NV2A `A1R5G5B5` shelf atlas to **`1024 × 2048` (`4 MB`)** with an **`8,192`-slot (`128`-probe)** `Mix64` hash table and added `FlushMidFrameAndResetShelf()` (GPU kick + `pb_busy()` fence) so even an extreme >2,048-quad frame can never overwrite queued texels.
2. **Off-Screen VRAM-to-VRAM Compositor (`BlitVramToVram`):**
   - Draws targeting VRAM outside both screen double-buffers (`clipX/Y ± 32` and `scrollX/Y ± 32`, such as `x = 7456..8191`) now execute `BlitVramToVram(d)` directly into the sparse `128×128` `A1R5G5B5` VRAM page pool (supporting opaque fast-`memcpy`, bit-15 transparency, X/Y flips, 5-bit tint, and all 64 CV1000 blend modes) and bump `rectEpoch_`.
3. **Collision-Free 64-Bit Rect Key & 1-Pixel Gutter:**
   - Widened width/height fields in `key` to **12 bits (`0..4095`)** (`w << 40 | h << 28 | srcX0 << 13 | srcY0`) with zero bit overlap.
   - Added a **1-pixel zeroed gutter** (`w + 1`, `h + 1`) between shelf rects and increased UV inset to `0.02f` (nearest) / `0.25f` (bilinear) above NV2A's fixed-point sub-texel threshold.

---

## 4. Reproduction & Verification Commands

* **Host Draw/Parity Diagnostic:**
  ```bash
  MSYSTEM=MINGW64 CHERE_INVOKING=1 C:/msys64/usr/bin/bash.exe -lc \
    'export PATH="/mingw64/bin:/usr/bin:$PATH"; cd port/nxdk/mushisam/tests && make -j$(nproc) && cd ../../../.. && ./port/nxdk/mushisam/tests/mushisam_parity_test.exe --bench --frames 4500'
  ```
* **Rebuild Xbox XBE / XISO:**
  ```bash
  MSYSTEM=MINGW64 CHERE_INVOKING=1 CLEAN=0 C:/msys64/usr/bin/bash.exe -l port/nxdk/mushisam/build_xbox.sh FAKE_PLAY=1
  ```
* **Dump Live Xemu Telemetry:**
  ```bash
  python port/nxdk/mushisam/tools/gdb_dump.py 0x0040de60 384 perf_dump.bin
  python port/nxdk/mushisam/tools/decode_perf.py perf_dump.bin
  ```
