// Native Original Xbox runner for statically-recompiled Mushihimesama (Cave CV1000-B / SH-3):
// zero SDL2, zero 64 MB software VRAM (sparse 128x128 A1R5G5B5 pages + direct NV2A hardware quads),
// streaming 132 MB NAND from D:\mushisam\mushisam_u2 (64 KB LRU cache), native AC'97 XAudio,
// native USB XID gamepad, native MCPX 60.0184 Hz timer, GDB profiler, and R3/FAKE_PLAY mode.
#include "machine/cv1000.h"
#include "native_cv1k/xbox_eeprom.h"
#include "mushisam_fake_play.h"
#include "nv2a_cv1k_renderer.h"
#include "xbox_audio.h"
#include "xbox_input.h"
#include "xbox_profiler.h"
#include "xbox_timer.h"

#include <hal/debug.h>
#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

void SwapWords(uint8_t *data, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
    }
}

bool LoadFileSwapped(const char *path, std::vector<uint8_t> &dst, size_t offset = 0) {
    // Direct Win32/Xbox ReadFile bypasses nxdk pdclib's 1 KB byte-by-byte fread() loop (10,000x fewer syscalls).
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h);
        return false;
    }
    const size_t n = static_cast<size_t>(sz.QuadPart);
    if (offset + n > dst.size()) {
        dst.resize(offset + n);
    }
    DWORD got = 0;
    const BOOL ok = ReadFile(h, dst.data() + offset, static_cast<DWORD>(n), &got, nullptr);
    CloseHandle(h);
    if (!ok || static_cast<size_t>(got) != n) {
        return false;
    }
    SwapWords(dst.data() + offset, n);
    return true;
}

bool LoadMushisamRoms(spelunk::Cv1000 &board) {
    board.boardD = false; // CV1000-B (8 MB RAM, 2 MB U4)
    if (!LoadFileSwapped("D:\\mushisam\\mushisam_u4", board.program, 0)) {
        debugPrint("Mushihimesama: failed D:\\mushisam\\mushisam_u4\n");
        return false;
    }
    if (!LoadFileSwapped("D:\\mushisam\\u23", board.soundRom, 0)) {
        debugPrint("Mushihimesama: failed D:\\mushisam\\u23\n");
        return false;
    }
    if (!LoadFileSwapped("D:\\mushisam\\u24", board.soundRom, 0x400000)) {
        debugPrint("Mushihimesama: failed D:\\mushisam\\u24\n");
        return false;
    }
    if (!board.nand.AttachFile("D:\\mushisam\\mushisam_u2")) {
        debugPrint("Mushihimesama: failed streaming D:\\mushisam\\mushisam_u2\n");
        return false;
    }
    return true;
}

} // namespace

int main(void) {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    // Heap-allocate large CV1000/renderer objects so the 64 KB Xbox main thread stack never overflows.
    auto boardPtr = std::make_unique<spelunk::Cv1000>();
    auto &board = *boardPtr;
    if (!LoadMushisamRoms(board)) {
        while (true) {
            Sleep(1000);
        }
    }

    auto nativeListPtr = std::make_unique<spelunk::NativeList>();
    auto &nativeList = *nativeListPtr;
    nativeList.Reserve();
    board.blitter.rasterize = false;
    board.blitter.native = &nativeList;
    board.cpu.mode = spelunk::SH3::Mode::Native;
    board.cpu.InstallTranslations();
    board.Reset();

    spelunk_xbox::XboxEeprom eeprom;
    eeprom.Load(board.rtc);

    if (pb_init() != 0) {
        debugPrint("Mushihimesama: pb_init failed\n");
        while (true) {
            Sleep(1000);
        }
    }
    pb_show_front_screen();

    auto inputPtr = std::make_unique<spelunk_xbox::XboxInput>();
    auto &input = *inputPtr;
    input.Init();

    auto audioPtr = std::make_unique<spelunk_xbox::XboxAudio>();
    auto &audio = *audioPtr;
    audio.Init(board.sound.sampleRate);

    auto rendererPtr = std::make_unique<spelunk_xbox::Nv2aCv1kRenderer>();
    auto &renderer = *rendererPtr;
    if (!renderer.Init()) {
        debugPrint("Mushihimesama: NV2A CV1000 renderer init failed\n");
        while (true) {
            Sleep(1000);
        }
    }

    spelunk_xbox::XboxTimer timer;
    timer.Init();
    spelunk_xbox::g_xboxPerf.Reset();
    spelunk_xbox::g_xboxPerf.rdtscPerUs = static_cast<uint32_t>(timer.RdtscPerUs());
    spelunk_xbox::g_xboxPerf.SampleRamStats();
    spelunk_xbox::P3Pmc::Configure(spelunk_xbox::P3Pmc::kEvtIfuIfetchMiss,
                                   spelunk_xbox::P3Pmc::kEvtL2LinesIn);
    bool programFreed = false;

    spelunk_xbox::MushisamFakePlay fakePlay;
    fakePlay.Reset();
#ifdef XBOX_FAKE_PLAY
    fakePlay.enabled = true;
#endif

    bool tateMode = false;
    bool bilinear = false;
    bool integerScale = false;
    bool prevRb = false;
    bool prevLb = false;
    bool prevL3 = false;
    bool prevR3 = false;

    while (true) {
        spelunk_xbox::PadState pad{};
        input.Poll(pad);

        bool displayChanged = false;
        if (pad.black && !prevRb) {
            tateMode = !tateMode;
            displayChanged = true;
        }
        prevRb = pad.black;
        if (pad.white && !prevLb) {
            bilinear = !bilinear;
            displayChanged = true;
        }
        prevLb = pad.white;
        if (pad.l3 && !prevL3) {
            integerScale = !integerScale;
            displayChanged = true;
        }
        prevL3 = pad.l3;
        if (pad.r3 && !prevR3) {
            fakePlay.enabled = !fakePlay.enabled;
            fakePlay.Reset();
        }
        prevR3 = pad.r3;

        // Overlap SH-3 CPU emulation of frame N with NV2A GPU rasterization of frame N-1.
        uint64_t cpuTicks = 0;
        bool stepped = false;
        if (timer.ConsumeArcadeFrame()) {
            uint32_t p1 = 0;
            uint32_t sys = 0;
            if (fakePlay.enabled) {
                fakePlay.Update(board, p1, sys);
            } else {
                if (pad.up) p1 |= spelunk::kInUp;
                if (pad.down) p1 |= spelunk::kInDown;
                if (pad.left) p1 |= spelunk::kInLeft;
                if (pad.right) p1 |= spelunk::kInRight;
                if (pad.a) p1 |= spelunk::kInButton1;
                if (pad.b) p1 |= spelunk::kInButton2;
                if (pad.x) p1 |= spelunk::kInButton3;
                if (pad.y) p1 |= spelunk::kInButton4;
                if (pad.back) sys |= spelunk::kSysCoin1;
                if (pad.start) sys |= spelunk::kSysStart1;
            }
            board.inputs[0] = p1;
            board.system = sys;

            const uint64_t tCpu0 = spelunk_xbox::XboxTimer::NowTicks();
            const uint64_t pmc0_a = spelunk_xbox::P3Pmc::Read(0);
            const uint64_t pmc1_a = spelunk_xbox::P3Pmc::Read(1);
            board.RunFrame();
            const uint64_t pmc0_b = spelunk_xbox::P3Pmc::Read(0) - pmc0_a;
            const uint64_t pmc1_b = spelunk_xbox::P3Pmc::Read(1) - pmc1_a;
            if (!programFreed && board.frame >= 60u && (board.cpu.PC & 0xFF800000u) == 0x0C000000u) {
                // Reclaim 2 MB U4 flash buffer after boot copy completes (saves 2 MB on 64 MB retail Xbox).
                std::vector<uint8_t>().swap(board.program);
                programFreed = true;
            }
            if ((board.frame & 127u) == 0u) {
                eeprom.FlushIfDirty(board.rtc);
            }
            if (board.sound.rateChanged) {
                audio.SetSampleRate(board.sound.sampleRate);
                board.sound.rateChanged = false;
            }
            if (!board.sound.out.empty()) {
                audio.PushMono(board.sound.out.data(), board.sound.out.size());
                board.sound.out.clear();
            }
            cpuTicks = spelunk_xbox::XboxTimer::NowTicks() - tCpu0;
            stepped = true;
            (void)pmc0_b;
            (void)pmc1_b;
        }

        if (stepped || displayChanged) {
            const uint64_t tGpu0 = spelunk_xbox::XboxTimer::NowTicks();
            while (pb_busy()) {
            }
            const uint64_t gpuTicks = spelunk_xbox::XboxTimer::NowTicks() - tGpu0;

            pb_reset();
            pb_target_back_buffer();

            const uint64_t tRen0 = spelunk_xbox::XboxTimer::NowTicks();
            renderer.Render(board.blitter, nativeList, tateMode, bilinear, integerScale);
            while (pb_finished()) {
            }
            const uint64_t renTicks = spelunk_xbox::XboxTimer::NowTicks() - tRen0;

            if (stepped) {
                spelunk_xbox::g_xboxPerf.RecordFrame(
                    static_cast<uint32_t>(timer.TicksToMicros(cpuTicks)),
                    static_cast<uint32_t>(timer.TicksToMicros(renTicks)),
                    static_cast<uint32_t>(timer.TicksToMicros(gpuTicks)),
                    static_cast<uint32_t>(timer.RdtscToMicros(board.profSh3Tsc)),
                    static_cast<uint32_t>(timer.RdtscToMicros(board.profAudioTsc)),
                    static_cast<uint32_t>(timer.RdtscToMicros(board.profBlitterTsc)),
                    board.cpu.nativeBlocks,
                    board.cpu.interpretedBlocks,
                    spelunk_xbox::MushisamFakePlay::InPlay(board) ? 1u : 0u,
                    spelunk_xbox::MushisamFakePlay::Stage(board),
                    renderer.ActiveVramPages(),
                    static_cast<uint32_t>(board.nand.reads),
                    static_cast<uint32_t>(board.nand.cacheMisses),
                    static_cast<uint32_t>(renderer.LastDrawCount()),
                    static_cast<uint32_t>(renderer.LastQuadCount()),
                    renderer.LastBatchCount(),
                    renderer.LastStateChanges(),
                    renderer.LastUploadCount(),
                    renderer.SwapCount(),
                    spelunk_xbox::P3Pmc::Read(0),
                    spelunk_xbox::P3Pmc::Read(1));
            }
        }

        pb_wait_for_vbl();
    }

    renderer.Shutdown();
    audio.Shutdown();
    input.Shutdown();
    pb_kill();
    return 0;
}
