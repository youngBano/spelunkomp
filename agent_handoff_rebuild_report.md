# Agent Handoff & Rebuild Report: `spelunkomp-xbox`

## 1. Purpose
This report gives an incoming agent everything needed to rebuild, debug, and complete **`spelunkomp-xbox`** (porting `tmcleroy/spelunkomp` to the Original Xbox via `nxdk`) while preserving all current progress.

## 2. Project Scope & Target
- **Upstream:** `tmcleroy/spelunkomp` (static recompilation suite for 26 Cave arcade shoot 'em ups across M68000, PGM, and CV1000/SH-3).
- **Target Platform:** Original Xbox (32-bit `i386` Pentium III 733 MHz, NVIDIA NV2A 233 MHz GPU, 64 MB retail / 128 MB dev unified DDR SDRAM, FATX HDD/DVD) via `nxdk`.
- **Roadmap:** `docs/ROADMAP.md`
- **Active Debugging Checklist:** `debugging.md`

## 3. Workspace & Key Files
- **Upstream Source:** `spelunkomp/` (or `external/spelunkomp/` after repo restructure)
- **nxdk Toolchain:** `nxdk/` (referenced via `NXDK_DIR`)
- **Xbox Port Core:** `port/nxdk/core/`
  - `Makefile` — builds `bin/default.xbe` and `spelunkomp-cave68k-core.iso` (17 Cave 68000 TUs; `embedded_roms.S` removed).
  - `main.cpp` — loads `ddonpach` ROMs (`b1.u27`, `b2.u26`, `u50`-`u53`, etc.) from `D:\ddonpach\` via `ReadFileBytes`, initializes `pbkit` + `Nv2aPresenter`, runs 60 Hz `pb_wait_for_vbl()` loop, SDL 2 controller input, and 44.1 kHz mono audio.
  - `nv2a_presenter.h` — `spelunk_xbox::Nv2aPresenter` (`MmGetPhysicalAddress(tex_)`, `NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8`, YOKO/TATE via `RB`, Nearest/Bilinear via `LB`, Integer/Aspect-Fill via `L3`).
- **Logs:** `xemu_com1.log` (guest COM1 `0x3F8`), `xemu_serial.log` (Xemu stdout/stderr), `gdb_output.log`.

## 4. Current Progress (Keep & Build On This)
1. **Zero-SDL2 Native Static-Recompiled Pentium III `-O3` Build:** `port/nxdk/core` drops `NXDK_SDL = y` completely, links all **11,974 statically-recompiled native 68000 blocks** (`spelunkomp/work/ddonpach/gen/native_000..037.cpp` + `native_table.cpp`, `M68K::Mode::Native`), compiles with `-O3 -march=pentium3 -mtune=pentium3 -mmmx -msse -mfpmath=sse -fomit-frame-pointer`, and produces `bin/default.xbe` (6.1 MB) and `spelunkomp-cave68k-core.iso` (26 MB).
2. **Locked 60.0 FPS Live Execution in Xemu:** GDB sampling of `frameNum` (`0x626058`) verified **`742 → 862` (+120 frames in 2.0s = 60.0 FPS VSync-locked)** with zero crashes or pushbuffer stalls.
3. **Optimized & Visually Verified NV2A Presentation (`nv2a_presenter.h`):** Ping-pong double-buffered WC textures (`tex_[2]`), SSE1 `movntps`/`prefetchnta` streaming upload (zero L1/L2 cache pollution), one-time static NV097 state push + precomputed NDC/UV vertices, and dirty-tracked `pb_fill` clearing. Live Xemu VRAM dump (`xemu_screen.png`) confirms crisp, upright 90° CCW YOKO rendering of the full `WARNING` + `©ATLUS/CAVE 1995 1996` screen (`34,680` lit pixels, 15 colors, zero R/B swap).
4. **Native Xbox Input & Audio (Zero SDL2):** Replaced SDL2 GameController with native USB XID (`port/nxdk/core/xbox_input.h`) and SDL2 Audio with native AC'97 DMA `XAudio` (`port/nxdk/core/xbox_audio.h`, lock-free SPSC ring buffer + DPC FPU-safe integer 16.16 fixed-point 44.1→48 kHz mono→stereo resampler).
5. **Unit & Core Validation Tests:** Both `port/nxdk/tests/wide_math_test.cpp` and `port/nxdk/tests/core_validation_test.cpp` (**27/27 checks**) pass.

## 5. Exact Build, Launch & Debug Commands
Always stop Xemu before rebuilding to avoid ISO file locks:
```bash
taskkill /F /IM xemu.exe 2>/dev/null || true
MSYSTEM=MINGW64 bash -lc 'cd port/nxdk/core && eval "$(../../../nxdk/bin/activate -s)" && /usr/bin/make'
```

Launch Xemu with COM1 serial file and GDB stub (`localhost:1234`):
```bash
"${XEMU_BIN:-xemu}" \
  -dvd_path port/nxdk/core/spelunkomp-cave68k-core.iso \
  -serial file:xemu_com1.log \
  -s > xemu_serial.log 2>&1
```

Attach GDB with `main.exe` symbols (`nxdk` does not bundle `gdb`; use MSYS2 MinGW64 `gdb`):
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

## 6. Active Blocker & Immediate Next Steps
- **Status:** Core boot, ROM load, `pbkit`/`Nv2aPresenter` init, and continuous frame execution (`frameNum = 313+`) are now verified live in Xemu via GDB (`gdb_output.log`).
- **Remaining Gaps:**
  - `xemu_com1.log` stays 0 bytes under retail `Complex_4627.bin` BIOS (passing `-device lpc47m157` causes Xemu 0.8.139 to exit).
  - Unoptimized `-O0` interpreter build runs at ~8 FPS in Xemu (`289 -> 313` in 3s).
- **Immediate Next Steps:**
  1. Add `-O2` to `NXDK_CXXFLAGS` in `port/nxdk/core/Makefile` (and evaluate generated static-recomp blocks) to hit locked 60 Hz pacing.
  2. Visually confirm NV2A textured-quad output, colors, `RB`/`LB`/`L3` toggles, SDL 2 controller input, and 44.1 kHz audio in Xemu (see `debugging_guide_unified.md`).
  3. Optionally add a debug BIOS or `pb_print` HUD overlay for on-screen frame/FPS diagnostics.

## 7. Remaining Roadmap & Repo Hygiene
1. **Repo Restructure:** Initialize root Git repo with `.gitignore` (excluding `*.iso`, `*.xbe`, `*.exe`, `*.obj`, `*.log`, ROMs), convert `spelunkomp/` to a Git submodule, split `nv2a_presenter.h` into `.h`/`.cpp`, and move build/debug commands into `scripts/`.
2. **Phase 2:** Port `spelunkomp` hitbox overlays as lightweight NV2A line/quad primitives.
3. **Phase 3:** Normalize save-state, config, and ROM/NAND paths to FATX (`D:\`, `E:\`).
4. **Phase 4:** Profile RAM for 64 MB retail target (45–52 MB peak), add HDD streaming for PGM/CV1000, and detect 128 MB consoles (`MmQueryStatistics`) for extended rewind.
5. **Phase 5:** Real Original Xbox hardware validation (1.0–1.6) and Cave slowdown parity benchmarks.