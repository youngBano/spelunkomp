# Subgoal: Debug & Verify Current Stage (`spelunkomp-xbox`)

## Scope
Validate the end-to-end `ddonpach` (`Cave68k`) runtime on Original Xbox / Xemu before moving to Phase 2 hitbox overlays and Phase 3 FATX persistence:
- **ISO:** `port/nxdk/core/spelunkomp-cave68k-core.iso`
- **Entrypoint:** `port/nxdk/core/main.cpp`
- **ROM Loading:** `D:\ddonpach\` via `ReadFileBytes` in `main.cpp` (`embedded_roms.S` removed)
- **Presenter:** `port/nxdk/core/nv2a_presenter.h`

## Build & Launch Commands
Set environment variables as needed (`NXDK_DIR`, `XEMU_BIN`, `MSYS2_BIN`).

- **Rebuild (MSYS2 MinGW64):**
  ```bash
  taskkill /F /IM xemu.exe 2>/dev/null || true
  MSYSTEM=MINGW64 bash -lc 'cd port/nxdk/core && rm -f main.obj main.exe bin/default.xbe spelunkomp-cave68k-core.iso && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make && ls -lh bin/default.xbe spelunkomp-cave68k-core.iso'
  ```

- **Run on Xemu (with COM1 serial file & GDB stub):**
  ```bash
  "${XEMU_BIN:-xemu}" \
    -dvd_path port/nxdk/core/spelunkomp-cave68k-core.iso \
    -serial file:xemu_com1.log \
    -s > xemu_serial.log 2>&1
  ```

- **Attach GDB with Symbols (MSYS2 MinGW64):**
  ```bash
  export PATH="/mingw64/bin:$PATH"
  gdb -batch \
    -ex "set pagination off" \
    -ex "file port/nxdk/core/main.exe" \
    -ex "set architecture i386" \
    -ex "target remote localhost:1234" \
    -ex "info registers eip esp" \
    -ex "bt 20" \
    -ex "continue" > gdb_output.log 2>&1
  ```

## Verification Checklist (Exit Criteria)

### 1. Boot, ROM Load & Frame Pacing (`main.cpp`)
- [x] Verify `ddonpach/` ROM files are packaged inside `spelunkomp-cave68k-core.iso`.
- [x] `board.SetBoard("ddonpach")` and `LoadDdonpachRoms(board)` (`D:\ddonpach\` via `ReadFileBytes`) succeed (verified via GDB backtrace + live `frameNum` progression in Xemu).
- [x] `pb_init()` and `presenter.Init()` succeed without hanging.
- [x] Main loop sustains **locked 60.0 Hz** via `pb_wait_for_vbl()` with no freeze, crash, or pushbuffer stall (`frameNum` at `0x626058` verified `742 -> 862` = 120 frames in 2.0s in Xemu with native statically-recompiled 68k blocks and zero SDL2).

### 2. NV2A Textured-Quad Presentation (`nv2a_presenter.h`)
- [ ] Serial framebuffer flush (`FB frame=... nonBlack=... hash=0x...` + 32x12 ASCII luminance preview) confirms non-black, changing frames in `xemu_com1.log` without relying on screenshots *(COM1 silent under retail BIOS; verified directly via live NV2A VRAM dump to `xemu_screen.png`)*.
- [x] Framebuffer renders visibly on Xemu (non-black, non-corrupted — DoDonPachi Warning screen verified in `xemu_screen.png`).
- [x] Colors are accurate under `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8` (no R/B channel swap — red/orange `WARNING` and purple/white body text verified).
- [x] Default **YOKO** mode (`tateMode = false`) renders upright 240x320 (90 deg CCW) centered on 640x480 (`360x480` = 172,800 pixels).
- [ ] **RB** toggles **TATE** mode (`tateMode = true`, native 320x240 unrotated).
- [ ] **LB** toggles **Nearest / Bilinear** filtering (`NV097_SET_TEXTURE_FILTER`).
- [ ] **L3** toggles **Aspect-Fill / Integer Scaling** (`240x320` in YOKO, `640x480` 2x in TATE).

### 3. Native Xbox USB XID Controller Input (`xbox_input.h`, zero SDL2)
- [ ] Hotplug / initial open detects controller (`usbh_core_init` + `usbh_xid_init`).
- [ ] **Back** inserts coin (`kSysCoin1`), **Start** starts P1 (`kSysStart1`).
- [ ] **D-pad / Left Stick** (`+/-8000` deadzone) moves ship in all 8 directions.
- [ ] **A / B / X / Y** map to `kInButton1..4` (shot, bomb, etc.), **Black (RB)** / **White (LB)** / **L3** toggle TATE / Bilinear / Integer scale.

### 4. Native Xbox AC'97 `XAudio` PCM Audio (`xbox_audio.h`, zero SDL2)
- [x] `XAudioInit(16, 2, ...)` + `XAudioPlay()` initialize cleanly on Xemu with zero hangs/crashes (integer 16.16 fixed-point 44.1 kHz mono → 48 kHz stereo resampler in DPC, FPU/SSE-safe).
- [ ] Audio is audible with accurate pitch and no distortion/crackling.
- [x] Lock-free SPSC ring buffer with 250 ms cap (`kMaxQueued = 11025`) prevents latency drift and `pcm.clear()` prevents RAM growth.

## Ranked Hypotheses (NV2A Textured Quad)
1. **Unnormalized UVs for Linear (`LU_IMAGE`) Textures:** `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8` requires pixel-space UVs (`0.0f..320.0f`, `0.0f..240.0f`) in `NV097_SET_TEXCOORD0_2F`, not `0.0f..1.0f`. Normalized UVs sample only the top-left 1x1 pixel (often black). Also verify `NV097_SET_TEXTURE_CONTROL1` (pitch `<< 16`) and `NV097_SET_TEXTURE_IMAGE_RECT` (`(w << 16) | h`).
2. **Transform / Viewport mode mismatch:** `pb_init()` defaults may leave `NV097_SET_TRANSFORM_EXECUTION_MODE` in fixed-function or programmable mode with a projection/viewport transform that clips screen-space `(0..640, 0..480)` vertices passed to `NV097_SET_VERTEX4F`.
3. **Register combiner / final combiner bitfield mismatch:** `NV097_SET_COMBINER_SPECULAR_FOG_CW0` (`0x00000008u`) and `CW1` (`0x00002000u`) may not route `TEX0` RGB and `1.0` alpha properly if stage 0 combiner or CW0/CW1 bit shifts differ.
4. **Texture control / pitch / format:** `NV097_SET_TEXTURE_CONTROL0` (`ENABLE` without `MIN_LOD`/`MAX_LOD` or `ALPHAKILL`) or `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8` (`0x3F`) handling in NV2A/Xemu.
5. **Cache / write-combining coherency:** `tex_` is allocated with `PAGE_WRITECOMBINE`; emit `__asm__ volatile("sfence" ::: "memory")` after `board.Picture(presenter.TexBuffer())` finishes writing and before kicking the pushbuffer.

## Findings (Updated 2026-11-26)
- **Root-Cause Game-Breaking Bug Fixed (`main.cpp`):** `board.SetBoard("ddonpach")` was previously called **before** `LoadDdonpachRoms(board)`. Because `Cave68k::SetBoard()` maps `m_parts[0..N]` from `board.program.size()`, calling it while `program` was empty left all program ROM pages unmapped (`Part::None`), causing every 68000 data read from ROM to return `0xFFFF` and crashing the game into illegal instruction `F3C0 at 00FFFF`. Reordering to `LoadDdonpachRoms()` -> `SetBoard("ddonpach")` -> `Reset()` (matching upstream `romset.cpp`) completely resolved this (`174` sprite lists by frame 300, matching `spelunk-headless.exe`).
- **Cache-Oriented Architecture (`nv2a_cave_renderer.h`, `xbox_audio.h`, `xbox_input.h`):**
  - Restructured `Nv2aCaveRenderer` for the Pentium III Coppermine-128 cache hierarchy (16 KB L1D, 128 KB L2, 32-byte lines): single-pass bucketed emission (`spriteQuads_[0..3]`, `tileQuads_[0..3]`, pre-reserved — cuts `layerRam` and `sprites` reads by 75%), packed 8-byte `uint64_t tileCache_[8192]` (64 KB instead of 128 KB, 4 slots/line), O(1) 2,048-slot epoch-tagged open-addressed `spriteHash_` (16 B/slot, eliminates O(N²) linear scan), division-free precomputed affine NDC transform, and `alignas(32)` 64-byte `Quad` layout.
  - Added `alignas(32)` separation in `XboxAudio` (`ringRead_` vs `ringWrite_` + `ring_[]`) and `XboxInput` (`raw_`/`hasPacket_` vs `padDev_`) to eliminate interrupt/main-thread false sharing.
  - Verified live in Xemu: **full VSync-locked speed** (`frameNum` `1070 → 1195` in 2s) with pixel-accurate output (`xemu_screen.png`, `39,120` lit pixels, 15 colors).
- **Interpreter Stripping & Pentium III Assembly Kernels (`no_interpreters.cpp`, `xbox_asm.S`, `xbox_asm.h`):**
  - Stripped all 10 unused Z80/OKI/FM/SH-3 interpreter source files (`z80.cpp`, `cave_z80.cpp`, `recomp_z80.cpp`, `no_z80_blocks.cpp`, `okim6295.cpp`, `ymfm_*.cpp`, `no_blocks.cpp`) from `Makefile`, shrinking `default.xbe` to 6.0 MB (running native statically-recompiled 68k blocks via `M68K::Mode::Native`).
  - Reimplemented 5 hot-path kernels in hand-tuned SafeSEH i686/MMX/SSE1 assembly (`xbox_asm.S`): 4×-unrolled `prefetcht0` palette dirty hash (`xbox_hash_palette_64k_asm`), MMX + `prefetchnta` 32K 15-bit→32-bit palette converter (`xbox_convert_palette_32k_mmx`), branchless `cmovz` 8×8 tile & sprite row unpackers into WC VRAM (`xbox_unpack_tile8x8_asm`, `xbox_unpack_sprite_row_asm`), and pure integer i386 16.16 fixed-point AC'97 DPC audio resampler (`xbox_resample_mono_to_stereo_asm`).
  - Verified live in Xemu: **60.0 FPS VSync-locked** (`frameNum` at `0x60eef0`: `700 → 820` in 2.0s) and crisp color-accurate VRAM output (`xemu_screen.png`, `34,755` lit pixels, 15 colors).
- **Host Core Validation Suite (`port/nxdk/tests/core_validation_test.cpp`):** 27 automated checks (plus `wide_math_test.cpp`) verifying ROM load parity, 300-frame execution (`pictures=174`, `uniqueColors=13`), opaque `A8B8G8R8` alpha, PCM audio, `StateHash` + `KeepNow`/`PutBack` determinism, and NV2A geometry/register bitfields — **27/27 passing**.
- **Serial Logging (`xemu_com1.log`):** Remains 0 bytes under retail `Complex_4627.bin` BIOS (passing duplicate `-device lpc47m157` causes Xemu 0.8.139 to exit). Live GDB memory + VRAM inspection (`0xFD600800` `PCRTC_START`, `0x1330e8` `frameNum`) provides deterministic verification.