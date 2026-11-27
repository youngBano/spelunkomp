# `spelunkomp-xbox` Rebuild & Handoff Report

## 1. Purpose
This document gives another agent everything needed to rebuild the entire `spelunkomp-xbox` project scope from scratch while preserving all current progress, build fixes, and debugging findings.

## 2. Project Scope & Target
- **Upstream:** `tmcleroy/spelunkomp` (static recompilation suite for 26 Cave arcade shoot 'em ups across M68000, PGM, and CV1000/SH-3).
- **Target Platform:** Original Xbox (32-bit `i386` Pentium III 733 MHz, NVIDIA NV2A 233 MHz GPU, 64 MB retail / 128 MB dev unified DDR SDRAM, FATX HDD/DVD) via `nxdk`.
- **Roadmap:** [project_goals_technical_roadmap.md](file:///C:/Users/lenovo/Documents/ree/project_goals_technical_roadmap.md)
- **Active Debugging Checklist:** [debugging.md](file:///C:/Users/lenovo/Documents/ree/debugging.md)

## 3. Workspace Layout & Key Files
- **Workspace Root:** `C:/Users/lenovo/Documents/ree/`
- **nxdk Toolchain:** `C:/Users/lenovo/Documents/ree/nxdk/`
- **Xbox Port Core:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/`
  - `Makefile` — builds `bin/default.xbe` and `spelunkomp-cave68k-core.iso` (17 Cave 68000 translation units; `embedded_roms.S` removed).
  - `main.cpp` — loads `ddonpach` ROMs (`b1.u27`, `b2.u26`, `u50`-`u53`, etc.) from `D:\ddonpach\` via `ReadFileBytes`, initializes `pbkit` + `Nv2aPresenter`, runs 60 Hz `pb_wait_for_vbl()` loop, SDL 2 `SDL_INIT_GAMECONTROLLER` input, and SDL 2 `44100 Hz` `AUDIO_S16LSB` mono audio.
  - `nv2a_presenter.h` — `spelunk_xbox::Nv2aPresenter` using contiguous `PAGE_WRITECOMBINE` texture (`MmGetPhysicalAddress(tex_)`), `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8`, explicit fixed-function screen-space state, YOKO/TATE (`RB`), nearest/bilinear (`LB`), integer/aspect-fill (`L3`).
- **Logs:** `C:/Users/lenovo/Documents/ree/xemu_com1.log`, `C:/Users/lenovo/Documents/ree/xemu_serial.log`, `C:/Users/lenovo/Documents/ree/gdb_output.log`

## 4. Environment & Build/Run Commands
- **Build (must use `MSYSTEM=MINGW64`):**
  ```bash
  taskkill /F /IM xemu.exe 2>/dev/null || true
  MSYSTEM=MINGW64 C:/msys64/usr/bin/bash.exe -lc 'cd /c/Users/lenovo/Documents/ree/port/nxdk/core && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make'
  ```
  *(Note: Stop Xemu before rebuilding to avoid `Device or resource busy` on the ISO.)*
- **Launch Xemu:**
  ```bash
  /c/Users/lenovo/AppData/Local/Microsoft/WinGet/Links/xemu -dvd_path C:/Users/lenovo/Documents/ree/port/nxdk/core/spelunkomp-cave68k-core.iso -serial file:C:/Users/lenovo/Documents/ree/xemu_com1.log -s > C:/Users/lenovo/Documents/ree/xemu_serial.log 2>&1
  ```
- **Attach GDB (`nxdk` does not bundle `gdb`; use MSYS2 MinGW64 `gdb`):**
  ```bash
  export PATH="/mingw64/bin:$PATH"
  gdb -batch -ex "set pagination off" -ex "set architecture i386" -ex "target remote localhost:1234" -ex "info registers" -ex "bt" -ex "continue" > C:/Users/lenovo/Documents/ree/gdb_output.log 2>&1
  ```

## 5. Current Progress & Verified State
- **Phase 1 (Build & Boot):** `default.xbe` and `spelunkomp-cave68k-core.iso` build cleanly. `gdb` attached to Xemu `-s` (`localhost:1234`) confirms the XBE is loaded (`0x10000 = 0x48454258` `XBEH`) and executing in user-space (`eip = 0x14642`, stack `0x145d2` -> `0x148dc` -> `0x14893` in `gdb_output.log`).
- **Phase 2 (NV2A Presenter):** `Nv2aPresenter` implemented in `nv2a_presenter.h` with `RB`/`LB`/`L3` toggles; runtime rendering in Xemu still needs verification.
- **Phase 3 (Audio & Input):** SDL 2 controller polling and 44.1 kHz PCM audio wired in `main.cpp`; runtime verification pending.

## 6. Current Blocker & Immediate Next Steps
1. **Diagnose 0-byte `xemu_com1.log`:** `SerialPrintf` (`OutB(0x3F8, ...)`) is not appearing in `xemu_com1.log` under `-serial file:...`. Test `-serial tcp:127.0.0.1:4444,server,nowait` or verify LPC47M157 SuperIO COM1 initialization so `FB frame=... nonBlack=... hash=0x...` serial framebuffer flush can be read without screenshots.
2. **Complete `debugging.md` Exit Criteria:** Verify `ddonpach` 60 Hz pacing, NV2A textured-quad presentation, YOKO/TATE/filter/scaling toggles, SDL 2 controller input, and 44.1 kHz audio.
3. **Continue Roadmap:** Port hitbox overlays (Phase 2), FATX `D:\` / `E:\` path normalization (Phase 3), 64 MB/128 MB memory profiling & HDD streaming (Phase 4), and real hardware validation (Phase 5).
4. **Pending Repo Housekeeping:** Add executables, ROMs, and copyrighted content to `.gitignore` in `C:/Users/lenovo/Documents/ree/spelunkomp/` and set up the user's fork.
