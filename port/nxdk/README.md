# Original Xbox bootstrap

This directory contains the first nxdk build target. It initializes a 640x480 video mode and periodically emits a debug string. It is a build/package smoke test only; it does not yet include game code, ROM support, or an emulator boot test.

## Build

From an MSYS2 MinGW64 shell with nxdk's prerequisites installed, run:

```sh
eval "$(../../nxdk/bin/activate -s)"
make
```

Run the build in this directory. nxdk writes the executable to `bin/default.xbe` and the configured ISO to `spelunkomp-xbox-bootstrap.iso`.

## Cross-compile probe (2026-11-24)

A clean nxdk build completed and regenerated `bin/default.xbe` plus the ISO. `file` identified the XBE as `spelunkomp-xbox-bootstrap`; its first four bytes are `XBEH`. This verifies build/package output only, not an emulator or console boot.

A direct nxdk Clang compile probe against the upstream Cave 68000 sources found:

- Compiles: `machine/m68k.cpp`, `machine/cave_video.cpp`, `machine/eeprom93c46.cpp`, `runtime/recomp68k.cpp`, `machine/cave68k.cpp`, `machine/ymz280b.cpp`, and `machine/okim6295.cpp`.
- Replaced the Cave audio timing code's unsupported `unsigned __int128` arithmetic with bounded-multiplier 64-bit mul/div helpers in `machine/wide_math.h`. `tests/wide_math_test.cpp` compares floor/ceiling results against a host 128-bit reference, including large cycle counts; it passes under Clang with warnings treated as errors.
- Added `runtime/romset_files.cpp` as the ROM-source filesystem boundary. On nxdk it uses `GetFileAttributes` and `FindFirstFile`/`FindNextFile`; on desktop it keeps POSIX directory scanning. The host regression test passes, and both `runtime/romset_files.cpp` and `runtime/romset.cpp` now compile with nxdk. nxdk's zlib headers require its `-DZ_SOLO` define.
- Built nxdk's `samples/winapi_filefind` sample successfully as an XBE and ISO. It demonstrates mounting a FATX drive with `nxMountDrive` and listing it with `FindFirstFile`/`FindNextFile`; this is a build check, not a boot test.
- `machine/sh3_ops.h` still uses `__int128`; that code is outside the Cave 68000 target and remains a later non-Cave portability issue.

The host CMake build of the `ddonpachi` headless target succeeds, but cannot run its board without a user ROM set (`no ddonpachi set in rom`). Xemu v0.8.139 is available and has been used to boot the separate Cave-core ISO.

## Core-link smoke target (2026-11-24)

`core/Makefile` now links the Cave 68000 board core into its own XBE and packages `core/spelunkomp-cave68k-core.iso`, leaving this bootstrap independent. The build completed with 17 core translation units and produced `core/bin/default.xbe` (376,832 bytes); `llvm-nm -u core/bin/machine/cave_video.obj` showed no unresolved TLS-destructor reference. Core intermediates are stored in `core/obj/`, so the ISO contains only `default.xbe` (983,040 bytes), not build objects or dependency files. This is still only a link/package smoke test for gameplay: the XBE does not yet execute frames, load ROMs, or present gameplay. Under Xemu, the XBE reached its entry point and `SetBoard for target `ddonpach`` took the success path (GDB stopped at `main` address `0x110b4`). Continuing then left the CPU in kernel code at `0x800151ef`; return from the success `debugPrint` and arrival at the smoke target's steady loop are not yet confirmed. Hardware boot remains unverified.

Xemu v0.8.139 loads the core ISO and reaches the XBE entry point under the i386 GDB stub (`0x631f0`). A GDB breakpoint confirmed `Cave68k::SetBoard("ddonpachi")` takes its success path (`main` at `0x110b4`). Execution continued into kernel code, but the success `debugPrint` return and steady-loop arrival are not confirmed. The target still has no ROM loading, frame loop, or gameplay presenter. Next, isolate the post-board debug/output path, then add a minimal game loop and ROM/video/audio integration. FATX behavior remains untested at runtime. Keep this core target separate from the bootstrap until runtime validation; defer SDL3 and NV2A presenter work until the Cave core runs.

The host regression tests can be run from the `spelunkomp` directory with:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror ../port/nxdk/tests/wide_math_test.cpp -o wide_math_test.exe
./wide_math_test.exe
clang++ -std=c++20 -Wall -Wextra -Werror tests/romset_files_test.cpp runtime/romset_files.cpp -o romset_files_test.exe
./romset_files_test.exe
```
