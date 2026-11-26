// Whole-game automated fake gameplay timing & Cave slowdown benchmark for DoDonPachi.
// Exercises real scenes across Stage 1..6 using DdpFakePlay and XboxPerfStats against the
// standalone port/nxdk/core/engine/ and native static-recompiled 68k blocks.
#include "ddp_fake_play.h"
#include "machine/cave68k.h"
#include "xbox_profiler.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path &path) {
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> out(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (sz > 0) out.resize(std::fread(out.data(), 1, out.size(), f));
    std::fclose(f);
    return out;
}

bool LoadRoms(spelunk::Cave68k &board, const std::filesystem::path &dir) {
    const auto even = ReadFileBytes(dir / "b1.u27");
    const auto odd = ReadFileBytes(dir / "b2.u26");
    if (even.empty() || even.size() != odd.size()) return false;
    board.program.resize(even.size() * 2);
    for (size_t i = 0; i < even.size(); ++i) {
        board.program[i * 2] = even[i];
        board.program[i * 2 + 1] = odd[i];
    }
    std::vector<uint8_t> packed;
    for (const char *n : {"u50.bin", "u51.bin", "u52.bin", "u53.bin"}) {
        auto p = ReadFileBytes(dir / n);
        if (p.empty()) return false;
        packed.insert(packed.end(), p.begin(), p.end());
    }
    for (size_t i = 0; i + 1 < packed.size(); i += 2) std::swap(packed[i], packed[i + 1]);
    size_t spSize = 1;
    while (spSize < packed.size() * 2) spSize <<= 1;
    board.video.spritePixels.assign(spSize, 0);
    for (size_t i = 0; i < packed.size(); ++i) {
        board.video.spritePixels[i * 2] = packed[i] >> 4;
        board.video.spritePixels[i * 2 + 1] = packed[i] & 15;
    }
    const char *ln[] = {"u60.bin", "u61.bin", "u62.bin"};
    board.video.layers = 3;
    for (int n = 0; n < 3; ++n) {
        const auto l = ReadFileBytes(dir / ln[n]);
        if (l.empty()) return false;
        auto &k = board.video.kinds[n];
        if (n == 2) {
            k.pixels.resize(l.size());
            for (size_t a = 0; a + 1 < l.size(); a += 2) {
                k.pixels[a] = static_cast<uint8_t>((l[a + 1] & 0xF0) | (l[a] >> 4));
                k.pixels[a + 1] = static_cast<uint8_t>(((l[a + 1] & 15) << 4) | (l[a] & 15));
            }
            k.only8 = true;
        } else {
            k.pixels.resize(l.size() * 2);
            for (size_t a = 0; a < l.size(); ++a) {
                k.pixels[a * 2] = l[a] >> 4;
                k.pixels[a * 2 + 1] = l[a] & 15;
            }
            k.only8 = false;
        }
        k.colourBase = 0x4000;
        k.colourStep = 256;
        k.colourMask = 0x3F;
    }
    for (const char *n : {"u6.bin", "u7.bin"}) {
        auto p = ReadFileBytes(dir / n);
        if (p.empty()) return false;
        board.soundRom.insert(board.soundRom.end(), p.begin(), p.end());
    }
    const auto eep = ReadFileBytes(dir / "eeprom-ddonpach.bin");
    if (eep.size() == 128) {
        for (int i = 0; i < 64; ++i) {
            board.eeprom.words[i] = static_cast<uint16_t>((eep[i * 2] << 8) | eep[i * 2 + 1]);
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv) {
    const std::filesystem::path romDir =
        (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::path("port/nxdk/core/bin/ddonpach");

    spelunk::Cave68k board;
    if (!LoadRoms(board, romDir) || !board.SetBoard("ddonpach")) {
        std::fprintf(stderr, "Failed to load ddonpach ROMs from %s\n", romDir.string().c_str());
        return 1;
    }

    spelunk::NativeFrame68k nativeFrame;
    nativeFrame.zeroCopy = true;
    board.video.rasterize = false;
    board.video.native = &nativeFrame;
    board.mode = spelunk::M68K::Mode::Native;
    board.Reset();

    // Fast-forward boot straight to title (matches game.cfg title_frame = 2940).
    board.noPicture = true;
    for (int f = 0; f < 2940; ++f) {
        board.RunFrame();
        board.SoundOut().clear();
    }
    board.noPicture = false;
    board.Redraw();

    spelunk_xbox::DdpFakePlay fakePlay;
    fakePlay.Reset();
    fakePlay.enabled = true;
    fakePlay.invincible = true;
    fakePlay.stageHopFrames = 900; // ~15s of heavy gameplay per stage across Stage 1..6

    spelunk_xbox::XboxPerfStats perf;
    perf.Reset();

    constexpr int kTotalFrames = 7000;
    uint32_t maxSpritesSeen = 0;
    uint32_t stagesVisitedMask = 0;

    for (int f = 0; f < kTotalFrames; ++f) {
        uint32_t p1 = 0, sys = 0;
        fakePlay.Update(board, p1, sys);
        board.inputs[0] = p1;
        board.system = sys;

        const uint32_t prevPictures = board.pictures;
        const auto t0 = std::chrono::steady_clock::now();
        board.RunFrame();
        board.SoundOut().clear();
        const auto t1 = std::chrono::steady_clock::now();
        const uint32_t cpuUs = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        const uint32_t mode = spelunk_xbox::DdpFakePlay::Mode(board);
        const uint32_t stage = spelunk_xbox::DdpFakePlay::Stage(board);
        const uint32_t loop = spelunk_xbox::DdpFakePlay::Loop(board);
        const uint32_t spCount = static_cast<uint32_t>(nativeFrame.sprites.size());
        if (spCount > maxSpritesSeen) maxSpritesSeen = spCount;
        if (mode == 7u && stage < 6u) {
            stagesVisitedMask |= (1u << stage);
        }

        perf.RecordFrame(cpuUs, 0, 0, mode, stage, loop, spCount, 0, board.pictures != prevPictures);
    }

    const double avgCpuUs = perf.frames ? static_cast<double>(perf.sumCpuUs) / perf.frames : 0.0;
    std::printf("=== WHOLE-GAME FAKE PLAY TIMING REPORT (%u frames, budget=%u us) ===\n",
                perf.frames, spelunk_xbox::XboxPerfStats::kCaveBudgetUs);
    std::printf("  Avg CPU: %.1f us | Max CPU: %u us (frame %u, stage %u, sprites %u)\n",
                avgCpuUs, perf.maxCpuUs, perf.worstFrame, perf.worstStage + 1, perf.worstSprites);
    std::printf("  Host Lag Frames (>17408us): %u | Authentic Cave Slowdown Frames: %u | Max Sprites: %u\n",
                perf.lagFrames, perf.caveSlowdownFrames, maxSpritesSeen);
    std::printf("  Stages Visited Mask: 0x%02X\n", stagesVisitedMask);
    for (int s = 0; s < 12; ++s) {
        if (perf.stageFrames[s] > 0) {
            std::printf("  Loop %d Stage %d: frames=%4u lag=%u maxCpu=%u us\n",
                        (s / 6) + 1, (s % 6) + 1, perf.stageFrames[s], perf.stageLagFrames[s], perf.stageMaxCpuUs[s]);
        }
    }

    return (stagesVisitedMask != 0 && maxSpritesSeen >= 30 && perf.lagFrames == 0) ? 0 : 1;
}
