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
    if (!board.sound.AttachRomFile("D:\\mushisam\\sound_rom.bin")) {
        debugPrint("Mushihimesama: failed streaming D:\\mushisam\\sound_rom.bin\n");
        return false;
    }
    board.sound.LoadSfxAdpcmPack("D:\\mushisam\\sfx_adpcm.bin");
    if (!board.nand.AttachFile("D:\\mushisam\\mushisam_u2")) {
        debugPrint("Mushihimesama: failed streaming D:\\mushisam\\mushisam_u2\n");
        return false;
    }
    return true;
}

} // namespace

int main(void) {
    // 16-bit R5G6B5 display matches CV1000's native 15-bit RGB555 output while saving +1.85 MB RAM
    // and halving NV2A ROP bandwidth vs 32-bit A8R8G8B8!
    XVideoSetMode(640, 480, 16, REFRESH_DEFAULT);

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

    pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, false);
#ifdef XBOX_CUSTOM_NXDK
    pb_set_depth_stencil_enable(false); // 2D CV1000 renderer uses no Z/stencil: saves +1.23 MB contiguous RAM!
#endif
    if (pb_init() != 0) {
        debugPrint("Mushihimesama: pb_init failed\n");
        while (true) {
            Sleep(1000);
        }
    }
    pb_show_front_screen();
#ifdef XBOX_CUSTOM_NXDK
    XVideoFreeFB(); // Reclaim dead boot framebuffer after PCRTC_START switches to pb_FBAddr[0] (+0.61-1.23 MB)!
#endif

    auto inputPtr = std::make_unique<spelunk_xbox::XboxInput>();
    auto &input = *inputPtr;
    input.Init();

    auto audioPtr = std::make_unique<spelunk_xbox::XboxAudio>();
    auto &audio = *audioPtr;
    if (audio.Init(board.sound.sampleRate)) {
        board.onSoundWrite = &spelunk_xbox::XboxAudio::SoundWriteCallback;
        board.onSoundWriteContext = &audio;
        audio.StartAsync(&board.sound);
    }

    auto rendererPtr = std::make_unique<spelunk_xbox::Nv2aCv1kRenderer>();
    auto &renderer = *rendererPtr;
    if (!renderer.Init()) {
        debugPrint("Mushihimesama: NV2A CV1000 renderer init failed\n");
        while (true) {
            Sleep(1000);
        }
    }
    board.blitter.onDirectUpload = &spelunk_xbox::Nv2aCv1kRenderer::DirectUploadCallback;
    board.blitter.onDirectUploadCtx = &renderer;

    spelunk_xbox::XboxTimer timer;
    timer.Init();
    spelunk_xbox::g_xboxPerf.Reset();
    spelunk_xbox::g_xboxPerf.rdtscPerUs = static_cast<uint32_t>(timer.RdtscPerUs());
    spelunk_xbox::g_xboxPerf.SampleRamStats();
    spelunk_xbox::P3Pmc::Configure(spelunk_xbox::P3Pmc::kEvtIfuIfetchMiss,
                                   spelunk_xbox::P3Pmc::kEvtL2LinesIn);

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
    uint32_t loadedStagePack = 0xFFFFFFFFu;
    bool everInPlay = false;
    uint32_t outOfPlayFrames = 0;
    uint32_t lastVbl = pb_get_vbl_counter();
    uint64_t lastWallTick = spelunk_xbox::XboxTimer::NowTicks();

    while (true) {
        const uint64_t tLoop0 = spelunk_xbox::XboxTimer::NowTicks();
        spelunk_xbox::PadState pad{};
        input.Poll(pad);

        if (pad.black && !prevRb) tateMode = !tateMode;
        prevRb = pad.black;
        if (pad.white && !prevLb) bilinear = !bilinear;
        prevLb = pad.white;
        if (pad.l3 && !prevL3) integerScale = !integerScale;
        prevL3 = pad.l3;
        if (pad.r3 && !prevR3) {
            fakePlay.enabled = !fakePlay.enabled;
            fakePlay.Reset();
        }
        prevR3 = pad.r3;

        // 1:1 Hardware VBlank-locked arcade frame (overlaps SH-3 CPU frame N with NV2A GPU frame N-1).
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
        // Keep board.program (2 MB mushisam_u4) resident: Stage 3+ script/pattern code reads U4 directly!
        const bool inPlayNow = spelunk_xbox::MushisamFakePlay::InPlay(board);
        const uint32_t curStage = spelunk_xbox::MushisamFakePlay::Stage(board);
        // Never write EEPROM to HDD during active gameplay (zero gameplay disk I/O).
        if (!inPlayNow && (board.frame & 127u) == 0u) {
            eeprom.FlushIfDirty(board.rtc);
        }
        if (inPlayNow) {
            everInPlay = true;
            outOfPlayFrames = 0;
        } else {
            ++outOfPlayFrames;
        }
        // Keep stage pack attached across brief ShipState == 0 stage transitions/respawns/continues!
        // Only switch back to pak_menu.bin after > 300 consecutive non-play frames at stage 0 (true attract/title).
        uint32_t desiredPack = loadedStagePack;
        if (!everInPlay || (curStage == 0u && !inPlayNow && outOfPlayFrames > 300u)) {
            desiredPack = 0u;
        } else {
            desiredPack = curStage + 1u;
        }
        if (desiredPack != loadedStagePack) {
            loadedStagePack = desiredPack;
            char packPath[64];
            if (desiredPack == 0u) {
                std::snprintf(packPath, sizeof(packPath), "D:\\mushisam\\pak_menu.bin");
            } else {
                std::snprintf(packPath, sizeof(packPath), "D:\\mushisam\\pak_s%u.bin", desiredPack);
            }
            board.nand.AttachStagePack(packPath);
        }
        const uint64_t cpuTicks = spelunk_xbox::XboxTimer::NowTicks() - tCpu0;
        (void)pmc0_b;
        (void)pmc1_b;

        // Overlap NV2A rasterization of frame N-1 with BOTH SH-3 CPU AND CPU-side tilemap/atlas prep of frame N!
        const uint64_t tRen0 = spelunk_xbox::XboxTimer::NowTicks();
        if (renderer.Render(board.blitter, nativeList, tateMode, bilinear, integerScale)) {
            while (pb_finished()) {
            }
        }
        const uint64_t renTotalTicks = spelunk_xbox::XboxTimer::NowTicks() - tRen0;
        const uint32_t gpuWaitUs = static_cast<uint32_t>(timer.RdtscToMicros(renderer.LastGpuWaitTsc()));
        const uint32_t renTotalUs = static_cast<uint32_t>(timer.TicksToMicros(renTotalTicks));
        const uint32_t renOnlyUs = (renTotalUs > gpuWaitUs) ? (renTotalUs - gpuWaitUs) : 0u;
        const uint64_t workTicks = spelunk_xbox::XboxTimer::NowTicks() - tLoop0;

        // Adaptive 1:1 VBlank sync: wait ONLY if we finished before the next VBlank; never double-sleep to 33.3 ms!
        uint32_t vblNow = pb_get_vbl_counter();
        if (vblNow == lastVbl) {
            vblNow = pb_wait_for_vbl();
        }
        const uint32_t vblDelta = vblNow - lastVbl;
        lastVbl = vblNow;

        const uint64_t nowWallTick = spelunk_xbox::XboxTimer::NowTicks();
        const uint32_t wallFrameUs = static_cast<uint32_t>(timer.TicksToMicros(nowWallTick - lastWallTick));
        lastWallTick = nowWallTick;
        LARGE_INTEGER sysTime{};
        KeQuerySystemTime(&sysTime);

        spelunk_xbox::g_xboxPerf.RecordFrame(
            static_cast<uint32_t>(timer.TicksToMicros(cpuTicks)),
            renOnlyUs,
            gpuWaitUs,
            static_cast<uint32_t>(timer.RdtscToMicros(board.profSh3Tsc)),
            static_cast<uint32_t>(timer.RdtscToMicros(board.profAudioTsc)),
            static_cast<uint32_t>(timer.RdtscToMicros(board.profBlitterTsc)),
            board.cpu.nativeBlocks,
            board.cpu.interpretedBlocks,
            inPlayNow ? 1u : 0u,
            curStage,
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
            spelunk_xbox::P3Pmc::Read(1),
            audio.QueuedSamples(),
            audio.PeakAmp(),
            audio.Ac97CivLviSr(),
            audio.Underruns(),
            audio.BuffersSubmitted(),
            board.cpu.lastInterpretedPc,
            static_cast<uint32_t>(timer.RdtscToMicros(renderer.LastBlitVramTsc())),
            static_cast<uint32_t>(timer.RdtscToMicros(renderer.LastAtlasCopyTsc())),
            static_cast<uint32_t>(timer.RdtscToMicros(renderer.LastPbSubmitTsc())),
            renderer.LastAtlasHits(),
            renderer.LastAtlasMisses(),
            wallFrameUs,
            static_cast<uint32_t>(timer.TicksToMicros(workTicks)),
            vblDelta,
            static_cast<uint64_t>(sysTime.QuadPart));
    }

    renderer.Shutdown();
    audio.Shutdown();
    input.Shutdown();
    pb_kill();
    return 0;
}
