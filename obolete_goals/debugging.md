# Subgoal: Debug & Verify Current Stage (`spelunkomp-xbox`)

## Scope
Validate the end-to-end `ddonpach` (`Cave68k`) runtime on Original Xbox / Xemu before moving to Phase 2 hitbox overlays and Phase 3 FATX persistence:
- **ISO:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/spelunkomp-cave68k-core.iso`
- **Entrypoint:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/main.cpp`
- **ROM Loading:** `D:\ddonpach\` via `ReadFileBytes` in `main.cpp` (`embedded_roms.S` removed)
- **Presenter:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/nv2a_presenter.h`

## Build & Launch Commands
- **Rebuild (MSYS2 MinGW64):**
  ```bash
  taskkill /F /IM xemu.exe 2>/dev/null || true
  MSYSTEM=MINGW64 C:/msys64/usr/bin/bash.exe -lc 'cd /c/Users/lenovo/Documents/ree/port/nxdk/core && rm -f main.obj main.exe bin/default.xbe spelunkomp-cave68k-core.iso && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make && ls -lh bin/default.xbe spelunkomp-cave68k-core.iso'
  ```
- **Run on Xemu (with COM1 serial file & GDB stub, logged to project directory):**
  ```bash
  /c/Users/lenovo/AppData/Local/Microsoft/WinGet/Links/xemu -dvd_path C:/Users/lenovo/Documents/ree/port/nxdk/core/spelunkomp-cave68k-core.iso -serial file:C:/Users/lenovo/Documents/ree/xemu_com1.log -s > C:/Users/lenovo/Documents/ree/xemu_serial.log 2>&1
  ```
- **Attach GDB (MSYS2 MinGW64, logged to project directory):**
  ```bash
  export PATH="/mingw64/bin:$PATH"
  gdb -batch -ex "set pagination off" -ex "set architecture i386" -ex "target remote localhost:1234" -ex "info registers" -ex "bt" -ex "continue" > C:/Users/lenovo/Documents/ree/gdb_output.log 2>&1
  ```

## Verification Checklist (Exit Criteria)

### 1. Boot, ROM Load & Frame Pacing (`main.cpp`)
- [ ] `board.SetBoard("ddonpach")` and `LoadDdonpachRoms(board)` (`D:\ddonpach\` via `ReadFileBytes`) succeed (`debugPrint` / `SerialPrintf` reports loaded program & sprite KB).
- [ ] `pb_init()` and `presenter.Init()` succeed without hanging.
- [ ] Main loop sustains 60 Hz via `pb_wait_for_vbl()` with no freeze, crash, or pushbuffer stall (`pb_busy()` / `pb_finished()`).

### 2. NV2A Textured-Quad Presentation (`nv2a_presenter.h`)
- [ ] Serial framebuffer flush (`FB frame=... nonBlack=... hash=0x...` + 32x12 ASCII luminance preview) confirms non-black, changing frames in `xemu_serial.log` without relying on screenshots.
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
- **Serial Logging (`xemu_com1.log` vs `xemu_serial.log`):** Xemu's `-serial file:C:/Users/lenovo/Documents/ree/xemu_com1.log` writes guest COM1 (`0x3F8`) output to `xemu_com1.log`, while `xemu_serial.log` only captures Xemu's own stdout/stderr.
- **Current Runtime State:** XBE is loaded (`0x10000 = 0x48454258` `XBEH`) and actively executing in user-space (`eip = 0x14642`, stack `0x145d2` -> `0x148dc` -> `0x14893` in latest `gdb_output.log`; earlier samples `0x00025570` -> `0x00045eed` with `140.0f` / `480.0f` screen-space floats). `xemu_com1.log` remains 0 bytes despite `SerialPrintf` (`OutB(0x3F8, ...)`), indicating COM1 (`0x3F8`) output is either uninitialized in LPC47M157 SuperIO or buffered/dropped by `-serial file:` (next step: test `-serial tcp:127.0.0.1:4444,server,nowait` or `-serial stdio`).
- **NV2A Presenter (`nv2a_presenter.h`):** Updated to use `MmGetPhysicalAddress(tex_)` for texture DMA offset and explicit fixed-function screen-space state (`NV097_SET_TRANSFORM_EXECUTION_MODE`, `NV097_SET_COMPOSITE_MATRIX` identity, `NV097_SET_VIEWPORT_OFFSET`/`SCALE`). Runtime rendering in Xemu still needs verification.
- **ROM Loading (`main.cpp`):** Switched back from `embedded_roms.S` (21 MB XBE) to loading `ddonpach` ROMs from `D:\ddonpach\` via `ReadFileBytes` to keep `default.xbe` compact.
- **GDB (`MSYS2 MinGW64`):** `nxdk` does not bundle `gdb`; use `C:/msys64/mingw64/bin/gdb.exe` (`GNU gdb (GDB) 18.1`).
