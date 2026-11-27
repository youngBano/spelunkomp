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
#include "machine/amm_decoder.h"
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
    std::string dumpAssetsPath;
    std::string dumpSfxPath;

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
        } else if (a == "--dump-assets" && i + 1 < argc) {
            dumpAssetsPath = argv[++i];
        } else if (a == "--dump-sfx" && i + 1 < argc) {
            dumpSfxPath = argv[++i];
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
    if (!dumpSfxPath.empty()) {
        // Offline pre-bake all short YMZ770 SFX phrases (<= 32,768 samples) into flat 4-bit IMA-ADPCM sfx_adpcm.bin.
        const uint8_t *rom = board.soundRom.data();
        const uint32_t romSize = static_cast<uint32_t>(board.soundRom.size());
        std::vector<spelunk::ImaAdpcmCache::DiskEntry> entries;
        std::vector<uint8_t> payload;
        spelunk::ImaAdpcmCache enc;
        std::vector<int16_t> pcm;
        std::vector<int16_t> block(0x1000, 0);
        for (uint32_t p = 0; p < 256u; ++p) {
            const uint32_t off = (static_cast<uint32_t>(rom[4u * p + 1u]) << 16) |
                                 (static_cast<uint32_t>(rom[4u * p + 2u]) << 8) |
                                 static_cast<uint32_t>(rom[4u * p + 3u]);
            if (off == 0u || off >= romSize) continue;
            spelunk::AmmDecoder dec(rom);
            int bitPos = static_cast<int>(off * 8u);
            const int endBit = static_cast<int>(romSize * 8u);
            int tableIdx = 0;
            uint32_t rate = 16000u;
            pcm.clear();
            bool tooLong = false;
            for (int iter = 0; iter < 64; ++iter) {
                const int made = dec.DecodeFrame(bitPos, endBit, block.data(), tableIdx);
                if (dec.Rate() > 0) rate = static_cast<uint32_t>(dec.Rate());
                if (made <= 0) break;
                if (pcm.size() + static_cast<size_t>(made) > spelunk::ImaAdpcmCache::kMaxSfxSamples) {
                    tooLong = true;
                    break;
                }
                pcm.insert(pcm.end(), block.data(), block.data() + made);
                if (made < 1152) break;
            }
            if (!tooLong && !pcm.empty()) {
                enc.Reset();
                if (enc.StorePcm(static_cast<uint8_t>(p), rate, pcm.data(), static_cast<uint32_t>(pcm.size()))) {
                    const auto *pe = enc.Get(static_cast<uint8_t>(p));
                    if (pe && !pe->adpcm.empty()) {
                        spelunk::ImaAdpcmCache::DiskEntry de{};
                        de.phrase = p;
                        de.sampleRate = pe->sampleRate;
                        de.totalSamples = pe->totalSamples;
                        de.dataOff = static_cast<uint32_t>(payload.size());
                        de.dataBytes = static_cast<uint32_t>(pe->adpcm.size());
                        entries.push_back(de);
                        payload.insert(payload.end(), pe->adpcm.begin(), pe->adpcm.end());
                    }
                }
            }
        }
        spelunk::ImaAdpcmCache::DiskHeader hdr{};
        hdr.magic = spelunk::ImaAdpcmCache::kPackMagic;
        hdr.count = static_cast<uint32_t>(entries.size());
        hdr.totalBytes = static_cast<uint32_t>(payload.size());
        FILE *sf = std::fopen(dumpSfxPath.c_str(), "wb");
        if (sf) {
            std::fwrite(&hdr, sizeof(hdr), 1, sf);
            std::fwrite(entries.data(), sizeof(spelunk::ImaAdpcmCache::DiskEntry), entries.size(), sf);
            std::fwrite(payload.data(), 1, payload.size(), sf);
            std::fclose(sf);
            std::printf("Wrote %s: %u SFX phrases (%.2f KB flat ADPCM)\n",
                        dumpSfxPath.c_str(), hdr.count,
                        (sizeof(hdr) + entries.size() * sizeof(spelunk::ImaAdpcmCache::DiskEntry) + payload.size()) / 1024.0);
        }
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

    // Asset & VRAM page profiler per bucket: 0=Boot/Menu, 1=Stage1, 2=Stage2, 3=Stage3, 4=Stage4, 5=Stage5
    struct AssetProf {
        int curBucket = 0;
        std::vector<uint8_t> nandSeen[6];
        std::vector<uint8_t> vramRead[6];
        std::vector<uint8_t> vramWrite[6];
        uint64_t blitVramDraws = 0, blitVramOpaque = 0, blitVramOverlap = 0, blitVramPixels = 0;
        AssetProf() {
            for (int i = 0; i < 6; ++i) {
                nandSeen[i].assign(65536, 0);
                vramRead[i].assign(2048, 0);
                vramWrite[i].assign(2048, 0);
            }
        }
    } aprof;
    if (!dumpAssetsPath.empty()) {
        board.nand.onPageReadCtx = &aprof;
        board.nand.onPageRead = [](void *ctx, uint32_t page) {
            auto *ap = static_cast<AssetProf *>(ctx);
            if (page < 65536u) ap->nandSeen[ap->curBucket][page] = 1;
        };
    }

    for (int f = 1; f <= frames; ++f) {
        uint32_t p1 = 0, sys = 0;
        fp.Update(board, p1, sys);
        board.inputs[0] = p1;
        board.system = sys;
        if (!dumpAssetsPath.empty()) {
            const bool ip = spelunk_xbox::MushisamFakePlay::InPlay(board);
            const uint32_t st = spelunk_xbox::MushisamFakePlay::Stage(board);
            aprof.curBucket = ip ? static_cast<int>(st < 5u ? (st + 1u) : 5u) : 0;
        }
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
                    if (!dumpAssetsPath.empty()) {
                        const auto &up = nl.uploads[st.first];
                        for (int y = up.y; y < up.y + up.height; y += 64) {
                            const int py = (y & 4095) >> 7;
                            for (int x = up.x; x < up.x + up.width; x += 64) {
                                const int px = (x & 8191) >> 7;
                                aprof.vramWrite[aprof.curBucket][py * 64 + px] = 1;
                            }
                        }
                    }
                } else {
                    seenDraw = true;
                    for (uint32_t i = 0; i < st.count; ++i) {
                        const auto &d = nl.draws[st.first + i];
                        const int dx0 = d.x0 - scrX, dy0 = d.y0 - scrY;
                        const int w = d.x1 - d.x0 + 1, h = d.y1 - d.y0 + 1;
                        if (!dumpAssetsPath.empty() && w > 0 && h > 0) {
                            const bool flipX = (d.flags & spelunk::NativeDraw::kFlipX) != 0;
                            const bool flipY = (d.flags & spelunk::NativeDraw::kFlipY) != 0;
                            const int sx0 = flipX ? (d.sx - d.x1) : (d.sx + d.x0);
                            const int sy0 = flipY ? (d.sy - d.y1) : (d.sy + d.y0);
                            const bool offscr = (dx0 + w <= 0 || dx0 >= 320 || dy0 + h <= 0 || dy0 >= 240);
                            if (offscr) {
                                const int ux = d.x0 & 8191, uy = d.y0 & 4095;
                                if (ux >= 1024 || uy >= 512) {
                                    ++aprof.blitVramDraws;
                                    aprof.blitVramPixels += static_cast<uint64_t>(w) * h;
                                    const bool trans = (d.flags & spelunk::NativeDraw::kTransparent) != 0;
                                    const bool tinted = (d.flags & spelunk::NativeDraw::kTinted) != 0;
                                    const bool blendOn = (d.flags & 64) == 0;
                                    if (!trans && !tinted && !blendOn) ++aprof.blitVramOpaque;
                                    const int usx = sx0 & 8191, usy = sy0 & 4095;
                                    if (usx < ux + w && ux < usx + w && usy < uy + h && uy < usy + h) ++aprof.blitVramOverlap;
                                    aprof.vramWrite[aprof.curBucket][((uy) >> 7) * 64 + ((ux) >> 7)] = 1;
                                    aprof.vramRead[aprof.curBucket][((usy) >> 7) * 64 + ((usx) >> 7)] = 1;
                                }
                            } else {
                                const int usx = sx0 & 8191, usy = sy0 & 4095;
                                aprof.vramRead[aprof.curBucket][(usy >> 7) * 64 + (usx >> 7)] = 1;
                                aprof.vramRead[aprof.curBucket][(((usy + h - 1) & 4095) >> 7) * 64 + (((usx + w - 1) & 8191) >> 7)] = 1;
                            }
                        }
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
    if (!dumpAssetsPath.empty()) {
        FILE *af = std::fopen(dumpAssetsPath.c_str(), "w");
        const char *bNames[6] = {"BootMenu", "Stage1", "Stage2", "Stage3", "Stage4", "Stage5"};
        std::printf("=== ASSET & VRAM WORKING-SET PROFILE (%d frames) ===\n", frames);
        std::printf("BlitVramToVram: draws=%llu (%.1f/frame), opaque=%.1f%%, overlap=%.2f%%, avgPx=%.1f\n",
                    (unsigned long long)aprof.blitVramDraws,
                    static_cast<double>(aprof.blitVramDraws) / frames,
                    aprof.blitVramDraws ? 100.0 * aprof.blitVramOpaque / aprof.blitVramDraws : 0.0,
                    aprof.blitVramDraws ? 100.0 * aprof.blitVramOverlap / aprof.blitVramDraws : 0.0,
                    aprof.blitVramDraws ? static_cast<double>(aprof.blitVramPixels) / aprof.blitVramDraws : 0.0);
        for (int b = 0; b < 6; ++b) {
            int nCnt = 0, vrCnt = 0, vwCnt = 0, vuCnt = 0;
            for (int p = 0; p < 65536; ++p) if (aprof.nandSeen[b][p]) ++nCnt;
            for (int p = 0; p < 2048; ++p) {
                if (aprof.vramRead[b][p]) ++vrCnt;
                if (aprof.vramWrite[b][p]) ++vwCnt;
                if (aprof.vramRead[b][p] || aprof.vramWrite[b][p]) ++vuCnt;
            }
            if (nCnt || vuCnt) {
                std::printf("  %-8s: NAND=%5d pages (%.2f MB) | VRAM read=%4d (%.1f MB) write=%4d (%.1f MB) union=%4d (%.1f MB)\n",
                            bNames[b], nCnt, nCnt * 2112.0 / (1024.0 * 1024.0),
                            vrCnt, vrCnt * 32.0 / 1024.0, vwCnt, vwCnt * 32.0 / 1024.0,
                            vuCnt, vuCnt * 32.0 / 1024.0);
                if (af) {
                    std::fprintf(af, "BUCKET %d %s %d\n", b, bNames[b], nCnt);
                    for (int p = 0; p < 65536; ++p) {
                        if (aprof.nandSeen[b][p]) std::fprintf(af, "%u\n", static_cast<unsigned>(p));
                    }
                }
            }
        }
        if (af) std::fclose(af);
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
            if (hf && (i < 600 || (totCyc && cum * 1000 <= totCyc * 992))) {
                std::fprintf(hf, "%08X %llu %llu\n", pc, (unsigned long long)cyc, (unsigned long long)cnt);
            }
        }
        if (hf) std::fclose(hf);
    }
#endif
    return 0;
}
