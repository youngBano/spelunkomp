// Host differential parity test, hotspot profiler, and post-boot RAM/CFG dumper
// for Mushihimesama (Cave CV1000-B / SH-3).
// Build (MSYS2 MinGW64):
//   g++ -std=c++20 -O2 -DXBOX_CV1000_B -DXBOX_NO_SW_VRAM -DXBOX_FAST_CPU \
//       -Iport/nxdk/mushisam -Iport/nxdk/mushisam/engine -Iport/nxdk/mushisam/engine/machine -Iport/nxdk/mushisam/engine/runtime \
//       port/nxdk/mushisam/tests/mushisam_parity_test.cpp \
//       port/nxdk/mushisam/engine/machine/amm_decoder.cpp \
//       port/nxdk/mushisam/engine/machine/blitter.cpp \
//       port/nxdk/mushisam/engine/machine/cv1000.cpp \
//       port/nxdk/mushisam/engine/machine/sh3.cpp \
//       port/nxdk/mushisam/engine/machine/ymz770.cpp \
//       port/nxdk/mushisam/engine/runtime/dispatch.cpp \
//       port/nxdk/mushisam/gen/blocks_*.cpp \
//       port/nxdk/mushisam/gen/registry.cpp \
//       -o port/nxdk/mushisam/tests/mushisam_parity_test.exe
#include "machine/cv1000.h"
#include "mushisam_fake_play.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef MUSHISAM_PROFILE_HOT
namespace spelunk {
std::unordered_map<uint32, uint64> g_hotPcCounts;
std::unordered_map<uint32, uint64> g_hotPcCycles;
}
#endif

namespace {

void SwapWords(uint8_t *data, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        std::swap(data[i], data[i + 1]);
    }
}

bool LoadFileSwapped(const char *path, std::vector<uint8_t> &dst, size_t offset = 0) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    const size_t n = static_cast<size_t>(sz);
    if (offset + n > dst.size()) {
        dst.resize(offset + n);
    }
    const size_t got = std::fread(dst.data() + offset, 1, n, f);
    std::fclose(f);
    if (got != n) {
        return false;
    }
    SwapWords(dst.data() + offset, n);
    return true;
}

bool LoadRoms(spelunk::Cv1000 &board, const std::string &dir) {
    board.boardD = false;
    if (!LoadFileSwapped((dir + "/mushisam_u4").c_str(), board.program, 0)) {
        std::fprintf(stderr, "Failed to load %s/mushisam_u4\n", dir.c_str());
        return false;
    }
    if (!LoadFileSwapped((dir + "/u23").c_str(), board.soundRom, 0)) {
        std::fprintf(stderr, "Failed to load %s/u23\n", dir.c_str());
        return false;
    }
    if (!LoadFileSwapped((dir + "/u24").c_str(), board.soundRom, 0x400000)) {
        std::fprintf(stderr, "Failed to load %s/u24\n", dir.c_str());
        return false;
    }
    if (!board.nand.AttachFile((dir + "/mushisam_u2").c_str())) {
        std::fprintf(stderr, "Failed to attach %s/mushisam_u2\n", dir.c_str());
        return false;
    }
    return true;
}

uint64_t HashNativeList(const spelunk::NativeList &nl) {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(nl.steps.size());
    mix(nl.draws.size());
    mix(nl.uploads.size());
    mix(nl.pixels.size());
    for (const auto &d : nl.draws) {
        mix(static_cast<uint32_t>(d.x0));
        mix(static_cast<uint32_t>(d.y0));
        mix(static_cast<uint32_t>(d.x1));
        mix(static_cast<uint32_t>(d.y1));
        mix(static_cast<uint32_t>(d.sx));
        mix(static_cast<uint32_t>(d.sy));
        mix(static_cast<uint32_t>(d.flags));
        mix(static_cast<uint32_t>(d.colours));
    }
    return h;
}

} // namespace

int main(int argc, char **argv) {
    std::string mode = "parity";
    std::string romDir = "port/nxdk/mushisam/bin/mushisam";
    int frames = 1200;
    std::string dumpRamPath;
    std::string dumpBlocksPath;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--parity") {
            mode = "parity";
        } else if (a == "--bench") {
            mode = "bench";
        } else if (a == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else if (a == "--roms" && i + 1 < argc) {
            romDir = argv[++i];
        } else if (a == "--dump-ram" && i + 1 < argc) {
            dumpRamPath = argv[++i];
        } else if (a == "--record" && i + 1 < argc) {
            dumpBlocksPath = argv[++i];
        }
    }

    if (mode == "parity") {
        auto refPtr = std::make_unique<spelunk::Cv1000>();
        auto natPtr = std::make_unique<spelunk::Cv1000>();
        auto &ref = *refPtr;
        auto &nat = *natPtr;
        if (!LoadRoms(ref, romDir) || !LoadRoms(nat, romDir)) {
            return 2;
        }
        spelunk::NativeList refList, natList;
        refList.Reserve();
        natList.Reserve();
        ref.blitter.rasterize = false;
        ref.blitter.native = &refList;
        nat.blitter.rasterize = false;
        nat.blitter.native = &natList;

        ref.cpu.mode = spelunk::SH3::Mode::Blocks;
        nat.cpu.mode = spelunk::SH3::Mode::Native;
        nat.cpu.InstallTranslations();
        ref.Reset();
        nat.Reset();

        spelunk_xbox::MushisamFakePlay fp;
        fp.Reset();
        fp.enabled = true;

        int mismatches = 0;
        for (int f = 1; f <= frames; ++f) {
            uint32_t p1 = 0, sys = 0;
            fp.Update(ref, p1, sys);
            // Mirror fakeplay's RAM pokes identically on nat so both boards see identical inputs & memory
            if (f > 300) {
                nat.ram[spelunk_xbox::MushisamFakePlay::kCreditsOff] = ref.ram[spelunk_xbox::MushisamFakePlay::kCreditsOff];
                if (spelunk_xbox::MushisamFakePlay::InPlay(ref)) {
                    nat.ram[spelunk_xbox::MushisamFakePlay::kShipsOff] = ref.ram[spelunk_xbox::MushisamFakePlay::kShipsOff];
                    nat.ram[spelunk_xbox::MushisamFakePlay::kBombsOff] = ref.ram[spelunk_xbox::MushisamFakePlay::kBombsOff];
                    std::memcpy(&nat.ram[spelunk_xbox::MushisamFakePlay::kSafeFramesOff],
                                &ref.ram[spelunk_xbox::MushisamFakePlay::kSafeFramesOff], 4);
                }
            }
            ref.inputs[0] = p1;
            ref.system = sys;
            nat.inputs[0] = p1;
            nat.system = sys;

            refList.Clear();
            natList.Clear();
            ref.RunFrame();
            nat.RunFrame();
            ref.sound.out.clear();
            nat.sound.out.clear();

            const uint64_t hRef = ref.StateHash();
            const uint64_t hNat = nat.StateHash();
            const uint64_t dRef = HashNativeList(refList);
            const uint64_t dNat = HashNativeList(natList);
            const int64_t cycDiff = static_cast<int64_t>(nat.cpu.cycles) - static_cast<int64_t>(ref.cpu.cycles);
            const bool exactOk = (hRef == hNat && dRef == dNat);
            const bool frameBoundarySlipOk = (dRef == dNat && std::abs(cycDiff) <= 128 &&
                                              ref.blitter.scrollX == nat.blitter.scrollX &&
                                              ref.blitter.scrollY == nat.blitter.scrollY &&
                                              ref.nand.reads == nat.nand.reads);
            if (!exactOk && !frameBoundarySlipOk) {
                std::printf("MISMATCH at frame %d: state ref=%016llX nat=%016llX | draws ref=%016llX (%zu) nat=%016llX (%zu)\n",
                            f, (unsigned long long)hRef, (unsigned long long)hNat,
                            (unsigned long long)dRef, refList.draws.size(),
                            (unsigned long long)dNat, natList.draws.size());
                std::printf("  ref PC=%08X cyc=%llu | nat PC=%08X cyc=%llu (diff=%lld)\n",
                            ref.cpu.PC, (unsigned long long)ref.cpu.cycles,
                            nat.cpu.PC, (unsigned long long)nat.cpu.cycles,
                            (long long)cycDiff);
                ++mismatches;
                if (mismatches >= 5) break;
            }
            if (f % 300 == 0 || f == frames) {
                std::printf("frame %4d/%d OK | state=%016llX | draws=%4zu | inPlay=%d stage=%u | natBlk=%llu interpBlk=%llu\n",
                            f, frames, (unsigned long long)hNat, natList.draws.size(),
                            spelunk_xbox::MushisamFakePlay::InPlay(nat) ? 1 : 0,
                            spelunk_xbox::MushisamFakePlay::Stage(nat),
                            (unsigned long long)nat.cpu.nativeBlocks,
                            (unsigned long long)nat.cpu.interpretedBlocks);
            }
        }
        std::printf("PARITY RESULT: %s (%d mismatches over %d frames)\n",
                    mismatches == 0 ? "PASS" : "FAIL", mismatches, frames);
        return mismatches == 0 ? 0 : 1;
    }

    // Bench & RAM dump mode
    auto boardPtr = std::make_unique<spelunk::Cv1000>();
    auto &board = *boardPtr;
    if (!LoadRoms(board, romDir)) {
        return 2;
    }
    spelunk::NativeList nl;
    nl.Reserve();
    board.blitter.rasterize = false;
    board.blitter.native = &nl;
    board.cpu.mode = spelunk::SH3::Mode::Native;
    if (!dumpBlocksPath.empty()) {
        board.cpu.recording = true;
        board.cpu.recordStatic = 4;
    }
    board.cpu.InstallTranslations();
    board.Reset();

    spelunk_xbox::MushisamFakePlay fp;
    fp.Reset();
    fp.enabled = true;

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t totalDraws = 0;
    uint64_t totalUploads = 0;
    uint64_t totalUploadPx = 0;
    uint64_t midFrameUploadsAfterDraws = 0;
    uint64_t offscreenDraws = 0;
    uint64_t farOffscreenDraws = 0;
    uint64_t scrollDiffClipFrames = 0;
    uint64_t sumSh3Tsc = 0, sumAudTsc = 0, sumBlitTsc = 0;
    uint32_t audioPeakAll = 0;
    uint32_t audioPeakPlay = 0;
    uint64_t audioTotalSamples = 0;
    uint64_t audioNonZeroSamples = 0;
    bool dumpedRam = false;

    for (int f = 1; f <= frames; ++f) {
        uint32_t p1 = 0, sys = 0;
        fp.Update(board, p1, sys);
        board.inputs[0] = p1;
        board.system = sys;
        nl.Clear();
        board.RunFrame();
        const bool inPlayNow = spelunk_xbox::MushisamFakePlay::InPlay(board);
        audioTotalSamples += board.sound.out.size();
        for (int16_t s : board.sound.out) {
            const uint32_t a = static_cast<uint32_t>(s < 0 ? -static_cast<int32_t>(s) : static_cast<int32_t>(s));
            if (a > 0) ++audioNonZeroSamples;
            if (a > audioPeakAll) audioPeakAll = a;
            if (inPlayNow && a > audioPeakPlay) audioPeakPlay = a;
        }
        board.sound.out.clear();

        totalDraws += nl.draws.size();
        totalUploads += nl.uploads.size();
        totalUploadPx += nl.pixels.size();
        {
            bool seenDraw = false;
            const int scrX = static_cast<int>(board.blitter.clipX & 0x1FFFu);
            const int scrY = static_cast<int>(board.blitter.clipY & 0x0FFFu);
            if (board.blitter.scrollX != board.blitter.clipX || board.blitter.scrollY != board.blitter.clipY) {
                ++scrollDiffClipFrames;
            }
            for (const auto &st : nl.steps) {
                if (st.upload) {
                    if (seenDraw) ++midFrameUploadsAfterDraws;
                } else {
                    seenDraw = true;
                    for (uint32_t i = 0; i < st.count; ++i) {
                        const auto &d = nl.draws[st.first + i];
                        const int dx0 = d.x0 - scrX, dy0 = d.y0 - scrY;
                        const int w = d.x1 - d.x0 + 1, h = d.y1 - d.y0 + 1;
                        if (dx0 + w <= 0 || dx0 >= 320 || dy0 + h <= 0 || dy0 >= 240) {
                            ++offscreenDraws;
                            if (dx0 + w < -64 || dx0 > 384 || dy0 + h < -64 || dy0 > 304) {
                                if (farOffscreenDraws < 12) {
                                    std::printf("  [FarOff f=%d] scr=(%u,%u) clip=(%d,%d) d=(%d..%d, %d..%d) src=(%d,%d) flg=0x%X\n",
                                                f, board.blitter.scrollX, board.blitter.scrollY, scrX, scrY,
                                                d.x0, d.x1, d.y0, d.y1, d.sx + d.x0, d.sy + d.y0, d.flags);
                                }
                                ++farOffscreenDraws;
                            }
                        }
                    }
                }
            }
        }
        sumSh3Tsc += board.profSh3Tsc;
        sumAudTsc += board.profAudioTsc;
        sumBlitTsc += board.profBlitterTsc;

        if (!dumpRamPath.empty() && !dumpedRam && f == 600) {
            FILE *out = std::fopen(dumpRamPath.c_str(), "wb");
            if (out) {
                std::fwrite(board.ram.data(), 1, board.ram.size(), out);
                std::fclose(out);
                std::printf("Dumped %zu bytes of post-boot RAM at frame %d to %s\n",
                            board.ram.size(), f, dumpRamPath.c_str());
                dumpedRam = true;
            }
        }
        if (f % 1000 == 0 || f == frames) {
            std::printf("frame %5d/%d | state=%016llX | inPlay=%d stage=%u | draws=%4zu | natBlk=%llu interpBlk=%llu | nandReads=%llu misses=%llu\n",
                        f, frames, (unsigned long long)board.StateHash(),
                        spelunk_xbox::MushisamFakePlay::InPlay(board) ? 1 : 0,
                        spelunk_xbox::MushisamFakePlay::Stage(board),
                        nl.draws.size(),
                        (unsigned long long)board.cpu.nativeBlocks,
                        (unsigned long long)board.cpu.interpretedBlocks,
                        (unsigned long long)board.nand.reads,
                        (unsigned long long)board.nand.cacheMisses);
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double totalTsc = static_cast<double>(sumSh3Tsc + sumAudTsc + sumBlitTsc);
    std::printf("BENCH: %d frames in %.1f ms (%.2f ms/frame, %.1f FPS)\n",
                frames, ms, ms / frames, frames * 1000.0 / ms);
    std::printf("  TSC share: SH3=%.1f%% Audio=%.1f%% Blitter=%.1f%%\n",
                totalTsc > 0 ? 100.0 * sumSh3Tsc / totalTsc : 0.0,
                totalTsc > 0 ? 100.0 * sumAudTsc / totalTsc : 0.0,
                totalTsc > 0 ? 100.0 * sumBlitTsc / totalTsc : 0.0);
    std::printf("  Avg dispatches/frame: %.1f (interp=%llu, lastInterpPc=0x%08X) | Avg draws/frame: %.1f | Uploads: %llu (%llu px)\n",
                static_cast<double>(board.cpu.nativeBlocks) / frames,
                (unsigned long long)board.cpu.interpretedBlocks,
                board.cpu.lastInterpretedPc,
                static_cast<double>(totalDraws) / frames,
                (unsigned long long)totalUploads,
                (unsigned long long)totalUploadPx);
    std::printf("  Audio YMZ770: rate=%u Hz | totalSamples=%llu | nonZero=%llu | peakAll=%u | peakPlay=%u\n",
                board.sound.sampleRate,
                (unsigned long long)audioTotalSamples,
                (unsigned long long)audioNonZeroSamples,
                audioPeakAll,
                audioPeakPlay);
    if (!dumpBlocksPath.empty()) {
        board.cpu.SaveRecording(dumpBlocksPath.c_str());
        std::printf("Saved block recording to %s\n", dumpBlocksPath.c_str());
    }
    if (!dumpRamPath.empty()) {
        const std::string endPath = dumpRamPath + ".end";
        FILE *out = std::fopen(endPath.c_str(), "wb");
        if (out) {
            std::fwrite(board.ram.data(), 1, board.ram.size(), out);
            std::fclose(out);
            std::printf("Dumped end RAM to %s\n", endPath.c_str());
        }
    }
#ifdef MUSHISAM_PROFILE_HOT
    {
        std::vector<std::pair<uint64_t, uint32_t>> byCyc;
        byCyc.reserve(spelunk::g_hotPcCycles.size());
        uint64_t totCyc = 0;
        for (const auto &[pc, cyc] : spelunk::g_hotPcCycles) {
            byCyc.push_back({cyc, pc});
            totCyc += cyc;
        }
        std::sort(byCyc.begin(), byCyc.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        FILE *hf = std::fopen("port/nxdk/mushisam/work/hot_pcs.txt", "w");
        std::printf("=== TOP 30 HOT SH-3 ENTRY PCs (total cyc=%llu) ===\n", (unsigned long long)totCyc);
        uint64_t cum = 0;
        for (size_t i = 0; i < byCyc.size(); ++i) {
            const uint32_t pc = byCyc[i].second;
            const uint64_t cyc = byCyc[i].first;
            const uint64_t cnt = spelunk::g_hotPcCounts[pc];
            cum += cyc;
            if (i < 30) {
                std::printf("  %2zu. %08X : %12llu cyc (%5.2f%%, cum %5.2f%%) | %10llu calls\n",
                            i + 1, pc, (unsigned long long)cyc,
                            totCyc ? 100.0 * cyc / totCyc : 0.0,
                            totCyc ? 100.0 * cum / totCyc : 0.0,
                            (unsigned long long)cnt);
            }
            if (hf && (i < 200 || (totCyc && cum * 100 <= totCyc * 98))) {
                std::fprintf(hf, "%08X %llu %llu\n", pc, (unsigned long long)cyc, (unsigned long long)cnt);
            }
        }
        if (hf) std::fclose(hf);
    }
#endif
    return 0;
}
