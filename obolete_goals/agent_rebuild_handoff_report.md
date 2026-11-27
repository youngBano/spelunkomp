# Agent Handoff & Rebuild Report: `spelunkomp-xbox`

## 1. Purpose
This report gives an incoming agent everything needed to rebuild, debug, and complete **`spelunkomp-xbox`** (porting `tmcleroy/spelunkomp` to the Original Xbox via `nxdk`) while preserving all current progress.

## 2. Workspace & Key Files
- **Project Root:** `C:/Users/lenovo/Documents/ree`
- **Upstream Source:** `C:/Users/lenovo/Documents/ree/spelunkomp/` (run `git` commands inside this directory, not `ree/`)
- **Xbox Port Core:** `C:/Users/lenovo/Documents/ree/port/nxdk/core/`
  - `Makefile` — builds `bin/default.xbe` and `spelunkomp-cave68k-core.iso` (17 Cave 68000 TUs; `embedded_roms.S` removed)
  - `main.cpp` — `ReadFileBytes` ROM loading from `D:\ddonpach\`, `pbkit` loop, SDL 2 controller + 44.1 kHz mono audio, `SerialPrintf` + serial framebuffer flush
  - `nv2a_presenter.h` — `spelunk_xbox::Nv2aPresenter` (`MmGetPhysicalAddress(tex_)`, `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8`, YOKO/TATE, Nearest/Bilinear, Integer/Aspect-Fill)
- **nxdk Toolchain:** `C:/Users/lenovo/Documents/ree/nxdk/`
- **Roadmaps & Logs:**
  - `C:/Users/lenovo/Documents/ree/project_goals_technical_roadmap.md`
  - `C:/Users/lenovo/Documents/ree/debugging.md`
  - `C:/Users/lenovo/Documents/ree/xemu_com1.log` (guest COM1 `0x3F8` output)
  - `C:/Users/lenovo/Documents/ree/xemu_serial.log` (Xemu stdout/stderr)

## 3. Current Progress (Keep & Build On This)
1. **Clean `nxdk` Build:** `port/nxdk/core` compiles and links `bin/default.xbe` and `spelunkomp-cave68k-core.iso` cleanly.
2. **ROM Loading:** Switched from `embedded_roms.S` (21 MB XBE) to runtime `ReadFileBytes` from `D:\ddonpach\` in `main.cpp`.
3. **NV2A Presenter (`nv2a_presenter.h`):** Implemented textured-quad presentation with `RB` (YOKO/TATE), `LB` (Nearest/Bilinear), and `L3` (Integer/Aspect-Fill) toggles.
4. **SDL 2 Input & Audio (`main.cpp`):** Controller polling (`SDL_INIT_GAMECONTROLLER`) and 44.1 kHz `AUDIO_S16LSB` mono output (`SDL_QueueAudio` with 250 ms cap) are wired.
5. **Unit Tests:** `wide_math_test.cpp` passes.

## 4. Exact Build, Launch & Debug Commands
Always stop Xemu before rebuilding to avoid ISO file locks:
```bash
taskkill /F /IM xemu.exe 2>/dev/null || true
MSYSTEM=MINGW64 C:/msys64/usr/bin/bash.exe -lc 'cd /c/Users/lenovo/Documents/ree/port/nxdk/core && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make'
```
Launch Xemu with COM1 serial file and GDB stub (`localhost:1234`):
```bash
/c/Users/lenovo/AppData/Local/Microsoft/WinGet/Links/xemu -dvd_path C:/Users/lenovo/Documents/ree/port/nxdk/core/spelunkomp-cave68k-core.iso -serial file:C:/Users/lenovo/Documents/ree/xemu_com1.log -s > C:/Users/lenovo/Documents/ree/xemu_serial.log 2>&1
```
Attach GDB (`nxdk` does not bundle `gdb`; use MSYS2 MinGW64 `C:/msys64/mingw64/bin/gdb.exe`, GNU gdb 18.1):
```bash
export PATH="/mingw64/bin:$PATH"
gdb -batch -ex "set pagination off" -ex "set architecture i386" -ex "target remote localhost:1234" -ex "info registers" -ex "bt" -ex "continue" > C:/Users/lenovo/Documents/ree/gdb_output.log 2>&1
```

## 5. Active Blocker & Immediate Next Steps
- **Blocker:** XBE loads (`0x10000 = XBEH`) and executes, but `xemu_com1.log` is 0 bytes (`SerialPrintf` / `OutB(0x3F8, ...)` produces no output).
- **Immediate Next Steps:**
  1. Attach `gdb` to `localhost:1234` while Xemu runs and inspect `eip`/backtrace to see if execution reaches `main()`, hangs in `pb_init()`/`LoadDdonpachRoms()`, or if COM1 (`0x3F8`) needs LPC47M157 SuperIO initialization.
  2. Verify serial framebuffer flush (`FB frame=... nonBlack=... hash=0x...`) in `xemu_com1.log` / `xemu_serial.log` (do not rely on screenshots for debugging).
  3. Complete the exit criteria in `debugging.md` (60 Hz pacing, YOKO/TATE/`RB`/`LB`/`L3`, SDL 2 input, 44.1 kHz audio).

## 6. Remaining Full-Scope Roadmap
1. **Phase 2:** Port `spelunkomp` hitbox overlays as lightweight NV2A line/quad primitives.
2. **Phase 3:** Normalize save-state, config, and ROM/NAND paths to FATX (`D:\`, `E:\`).
3. **Phase 4:** Profile RAM for 64 MB retail target (45-52 MB peak), add HDD streaming for PGM/CV1000, and detect 128 MB consoles (`MmQueryStatistics`) for extended rewind.
4. **Phase 5:** Real Original Xbox hardware validation (1.0-1.6) and Cave slowdown parity benchmarks.
5. **Repo Hygiene:** Add executables, ROMs, and copyrighted content to `.gitignore` and fork `spelunkomp`.
