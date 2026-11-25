# Subgoal: Debug & Verify Current Stage (`spelunkomp-xbox`)

## Scope
Validate the end-to-end `ddonpach` (`Cave68k`) runtime on Original Xbox / Xemu before moving to Phase 2 hitbox overlays and Phase 3 FATX persistence:
- **ISO:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/spelunkomp-cave68k-core.iso`
- **Entrypoint:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/main.cpp`
- **Presenter:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/nv2a_presenter.h`

## Build & Launch Commands
- **Rebuild (MSYS2 MinGW64):**
  ```bash
  taskkill /F /IM xemu.exe 2>/dev/null || true
  MSYSTEM=MINGW64 C:/msys64/usr/bin/bash.exe -lc 'cd /c/Users/lenovo/Documents/ree/port/nxdk/core && rm -f main.obj main.exe bin/default.xbe spelunkomp-cave68k-core.iso && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make && ls -lh bin/default.xbe spelunkomp-cave68k-core.iso'
  ```
- **Run on Xemu (with serial log):**
  ```bash
  /c/Users/lenovo/AppData/Local/Microsoft/WinGet/Links/xemu -dvd_path C:/Users/lenovo/Documents/ree/port/nxdk/core/spelunkomp-cave68k-core.iso -serial stdio
  ```

## Verification Checklist (Exit Criteria)

### 1. Boot, ROM Load & Frame Pacing (`main.cpp`)
- [ ] `board.SetBoard("ddonpach")` and `LoadDdonpachRoms(board, "D:\\ddonpach")` succeed (`debugPrint` reports loaded program & sprite KB).
- [ ] `pb_init()` and `presenter.Init()` succeed without hanging.
- [ ] Main loop sustains 60 Hz via `pb_wait_for_vbl()` with no freeze, crash, or pushbuffer stall (`pb_busy()` / `pb_finished()`).

### 2. NV2A Textured-Quad Presentation (`nv2a_presenter.h`)
- [ ] Framebuffer renders visibly on Xemu (non-black, non-corrupted).
- [ ] Colors are accurate under `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8` (no R/B channel swap).
- [ ] Default **YOKO** mode (`tateMode = false`) renders upright 240x320 (90 deg CCW) centered on 640x480.
- [ ] **RB** toggles **TATE** mode (`tateMode = true`, native 320x240 unrotated).
- [ ] **LB** toggles **Nearest / Bilinear** filtering (`NV097_SET_TEXTURE_FILTER`).
- [ ] **L3** toggles **Aspect-Fill / Integer Scaling** (`240x320` in YOKO, `640x480` 2x in TATE).

### 3. SDL 2 Controller Input (`main.cpp`)
- [ ] Hotplug / initial open detects controller (`SDL_INIT_GAMECONTROLLER`).
- [ ] **Back** inserts coin (`kSysCoin1`), **Start** starts P1 (`kSysStart1`).
- [ ] **D-pad / Left Stick** (`+/-8000` deadzone) moves ship in all 8 directions.
- [ ] **A / B / X / Y** map to `kInButton1..4` (shot, bomb, etc.).

### 4. SDL 2 PCM Audio (`main.cpp`)
- [ ] `SDL_OpenAudioDevice` opens at 44,100 Hz, `AUDIO_S16LSB`, mono (`samples = 1024`).
- [ ] Audio is audible with accurate pitch and no distortion/crackling.
- [ ] Queue cap (`44100 * sizeof(int16_t) / 4`, 250 ms) prevents latency drift and `pcm.clear()` prevents RAM growth.

## Ranked Hypotheses (NV2A Textured Quad)
1. **Transform / Viewport mode mismatch:** `pb_init()` defaults may leave `NV097_SET_TRANSFORM_EXECUTION_MODE` in fixed-function or programmable mode with a projection/viewport transform that clips screen-space `(0..640, 0..480)` vertices passed to `NV097_SET_VERTEX4F`.
2. **Register combiner / final combiner bitfield mismatch:** `NV097_SET_COMBINER_SPECULAR_FOG_CW0` (`0x00000008u`) and `CW1` (`0x00002000u`) may not route `TEX0` RGB and `1.0` alpha properly if stage 0 combiner or CW0/CW1 bit shifts differ.
3. **Texture control / pitch / format:** `NV097_SET_TEXTURE_CONTROL0` (`ENABLE` without `MIN_LOD`/`MAX_LOD` or `ALPHAKILL`) or `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8` (`0x3F`) handling in NV2A/Xemu.
4. **Cache / write-combining coherency:** `tex_` is allocated with `PAGE_WRITECOMBINE`; verify CPU writes from `board.Picture(presenter.TexBuffer())` are flushed before GPU DMA read.

## Findings
*(To be populated during investigation)*
