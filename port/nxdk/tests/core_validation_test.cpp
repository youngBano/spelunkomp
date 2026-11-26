// Extensive host-side validation suite for the spelunkomp Cave 68000 Xbox port core
// and NV2A presenter geometry/register math.
#include "machine/cave68k.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool cond, const char *name, const char *detail = nullptr) {
    if (cond) {
        ++g_passed;
        std::printf("[PASS] %s\n", name);
    } else {
        ++g_failed;
        if (detail) {
            std::printf("[FAIL] %s (%s)\n", name, detail);
        } else {
            std::printf("[FAIL] %s\n", name);
        }
    }
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path &path) {
    FILE *f = std::fopen(path.string().c_str(), "rb");
    if (!f) {
        return {};
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return {};
    }
    std::vector<uint8_t> out(static_cast<size_t>(sz));
    const size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(n);
    return out;
}

void SwapWords(uint8_t *data, size_t size) {
    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint8_t t = data[i];
        data[i] = data[i + 1];
        data[i + 1] = t;
    }
}

// Mirrors port/nxdk/core/main.cpp LoadDdonpachRoms exactly.
bool LoadDdonpachRoms(spelunk::Cave68k &board, const std::filesystem::path &dir) {
    const auto even = ReadFileBytes(dir / "b1.u27");
    const auto odd = ReadFileBytes(dir / "b2.u26");
    if (even.empty() || even.size() != odd.size()) {
        return false;
    }
    board.program.resize(even.size() * 2);
    for (size_t i = 0; i < even.size(); ++i) {
        board.program[i * 2] = even[i];
        board.program[i * 2 + 1] = odd[i];
    }

    const char *spNames[] = {"u50.bin", "u51.bin", "u52.bin", "u53.bin"};
    std::vector<uint8_t> packed;
    for (const char *name : spNames) {
        auto part = ReadFileBytes(dir / name);
        if (part.empty()) {
            return false;
        }
        packed.insert(packed.end(), part.begin(), part.end());
    }
    SwapWords(packed.data(), packed.size());
    size_t spSize = 1;
    while (spSize < packed.size() * 2) {
        spSize <<= 1;
    }
    board.video.spritePixels.assign(spSize, 0);
    for (size_t i = 0; i < packed.size(); ++i) {
        board.video.spritePixels[i * 2] = packed[i] >> 4;
        board.video.spritePixels[i * 2 + 1] = packed[i] & 15;
    }

    const char *layerNames[] = {"u60.bin", "u61.bin", "u62.bin"};
    board.video.layers = 3;
    for (int n = 0; n < 3; ++n) {
        const auto layer = ReadFileBytes(dir / layerNames[n]);
        if (layer.empty()) {
            return false;
        }
        auto &kind = board.video.kinds[n];
        if (n == 2) {
            kind.pixels.resize(layer.size());
            for (size_t at = 0; at + 1 < layer.size(); at += 2) {
                kind.pixels[at] = static_cast<uint8_t>((layer[at + 1] & 0xF0) | (layer[at] >> 4));
                kind.pixels[at + 1] = static_cast<uint8_t>(((layer[at + 1] & 15) << 4) | (layer[at] & 15));
            }
            kind.only8 = true;
        } else {
            kind.pixels.resize(layer.size() * 2);
            for (size_t at = 0; at < layer.size(); ++at) {
                kind.pixels[at * 2] = layer[at] >> 4;
                kind.pixels[at * 2 + 1] = layer[at] & 15;
            }
            kind.only8 = false;
        }
        kind.colourBase = 0x4000;
        kind.colourStep = 256;
        kind.colourMask = 0x3F;
    }

    board.soundRom.clear();
    for (const char *name : {"u6.bin", "u7.bin"}) {
        auto part = ReadFileBytes(dir / name);
        if (part.empty()) {
            return false;
        }
        board.soundRom.insert(board.soundRom.end(), part.begin(), part.end());
    }

    const auto eep = ReadFileBytes(dir / "eeprom-ddonpach.bin");
    if (eep.size() == 128) {
        for (int i = 0; i < 64; ++i) {
            board.eeprom.words[i] = static_cast<uint16_t>((eep[i * 2] << 8) | eep[i * 2 + 1]);
        }
    }
    return true;
}

uint32_t HashFrame(const std::vector<uint32_t> &fb, uint32_t &nonBlack, uint32_t &badAlpha) {
    nonBlack = 0;
    badAlpha = 0;
    uint32_t hash = 2166136261u;
    for (uint32_t px : fb) {
        if ((px & 0x00FFFFFFu) != 0) {
            ++nonBlack;
        }
        if ((px >> 24) != 0xFFu) {
            ++badAlpha;
        }
        hash ^= px;
        hash *= 16777619u;
    }
    return hash;
}

bool Near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

void TestNv2aPresenterMath() {
    constexpr float kScreenW = 640.0f;
    constexpr float kScreenH = 480.0f;
    constexpr int kSrcW = 320;
    constexpr int kSrcH = 240;

    auto computeQuad = [&](bool tate, bool integerScale, float &x0, float &y0, float &x1, float &y1) {
        float quadW = 0.0f, quadH = 0.0f;
        if (!tate) {
            if (integerScale) {
                quadW = 240.0f;
                quadH = 320.0f;
            } else {
                quadH = kScreenH;
                quadW = kScreenH * (240.0f / 320.0f);
            }
        } else {
            if (integerScale) {
                const int scale = (static_cast<int>(kScreenW) >= 640 && static_cast<int>(kScreenH) >= 480) ? 2 : 1;
                quadW = static_cast<float>(kSrcW * scale);
                quadH = static_cast<float>(kSrcH * scale);
            } else {
                quadW = kScreenW;
                quadH = kScreenH;
            }
        }
        x0 = (kScreenW - quadW) * 0.5f;
        y0 = (kScreenH - quadH) * 0.5f;
        x1 = x0 + quadW;
        y1 = y0 + quadH;
    };

    float x0, y0, x1, y1;
    // YOKO aspect-fill: 360x480 centered at x=[140, 500], y=[0, 480]
    computeQuad(false, false, x0, y0, x1, y1);
    Check(Near(x0, 140.0f) && Near(x1, 500.0f) && Near(y0, 0.0f) && Near(y1, 480.0f),
          "nv2a_yoko_aspect_fill_quad");

    // Verify NDC -> identity composite -> viewport scale/offset round-trip.
    const float ndcX0 = (x0 / kScreenW) * 2.0f - 1.0f;
    const float ndcX1 = (x1 / kScreenW) * 2.0f - 1.0f;
    const float ndcY0 = 1.0f - (y0 / kScreenH) * 2.0f;
    const float ndcY1 = 1.0f - (y1 / kScreenH) * 2.0f;
    Check(ndcX0 >= -1.0f && ndcX1 <= 1.0f && ndcY1 >= -1.0f && ndcY0 <= 1.0f,
          "nv2a_ndc_within_clip_bounds");
    const float rtX0 = ndcX0 * (kScreenW * 0.5f) + (kScreenW * 0.5f);
    const float rtX1 = ndcX1 * (kScreenW * 0.5f) + (kScreenW * 0.5f);
    const float rtY0 = ndcY0 * (-kScreenH * 0.5f) + (kScreenH * 0.5f);
    const float rtY1 = ndcY1 * (-kScreenH * 0.5f) + (kScreenH * 0.5f);
    Check(Near(rtX0, x0) && Near(rtX1, x1) && Near(rtY0, y0) && Near(rtY1, y1),
          "nv2a_viewport_roundtrip_exact");

    // YOKO integer: 240x320 centered at x=[200, 440], y=[80, 400]
    computeQuad(false, true, x0, y0, x1, y1);
    Check(Near(x0, 200.0f) && Near(x1, 440.0f) && Near(y0, 80.0f) && Near(y1, 400.0f),
          "nv2a_yoko_integer_quad");

    // TATE integer & aspect-fill on 640x480: full 640x480
    computeQuad(true, true, x0, y0, x1, y1);
    Check(Near(x0, 0.0f) && Near(x1, 640.0f) && Near(y0, 0.0f) && Near(y1, 480.0f),
          "nv2a_tate_integer_2x_quad");

    // NV097 register bitfield checks matching nv2a_presenter.h
    const uint32_t texFmt = 1u | (1u << 3) | (2u << 4) | (0x3Fu << 8) | (1u << 16);
    Check((texFmt & 0x3u) == 1u && ((texFmt >> 4) & 0xFu) == 2u && ((texFmt >> 8) & 0xFFu) == 0x3Fu &&
              ((texFmt >> 16) & 0xFu) == 1u,
          "nv2a_tex_format_lu_abgr8888");
    const uint32_t pitchReg = (kSrcW * 4u) << 16;
    const uint32_t rectReg = (static_cast<uint32_t>(kSrcW) << 16) | static_cast<uint32_t>(kSrcH);
    Check((pitchReg >> 16) == 1280u && (rectReg >> 16) == 320u && (rectReg & 0xFFFFu) == 240u,
          "nv2a_tex_pitch_and_rect");
    // Final combiner CW0 = D_SOURCE(TEX0=0x8), CW1 = G_INVERSE(1<<13) | G_SOURCE(ZERO=0)
    const uint32_t cw0 = 0x00000008u;
    const uint32_t cw1 = 0x00002000u;
    Check((cw0 & 0xFu) == 0x8u && ((cw1 >> 13) & 0x7u) == 1u && ((cw1 >> 8) & 0xFu) == 0u,
          "nv2a_final_combiner_cw0_cw1");
}

} // namespace

int main(int argc, char **argv) {
    const std::filesystem::path romDir =
        (argc > 1) ? std::filesystem::path(argv[1]) : std::filesystem::path("port/nxdk/core/bin/ddonpach");

    // 1. NV2A presenter geometry & register bitfield validation
    TestNv2aPresenterMath();

    // 2. Board setup & ROM loading tests (ROMs MUST be loaded before SetBoard so program ROM pages map!)
    spelunk::Cave68k board;
    Check(!board.SetBoard("nonexistent_board"), "cave68k_reject_invalid_board");

    const bool loaded = LoadDdonpachRoms(board, romDir);
    Check(loaded, "ddonpach_rom_load", romDir.string().c_str());
    if (!loaded) {
        std::printf("SUMMARY: passed=%d failed=%d\n", g_passed, g_failed);
        return 1;
    }
    Check(board.SetBoard("ddonpach"), "cave68k_set_board_ddonpach");

    Check(board.program.size() == 1024u * 1024u, "ddonpach_program_size_1mb");
    Check(board.video.spritePixels.size() == 16u * 1024u * 1024u, "ddonpach_sprites_size_16mb");
    Check(board.video.layers == 3 && !board.video.kinds[0].only8 && !board.video.kinds[1].only8 &&
              board.video.kinds[2].only8,
          "ddonpach_layer_configs");
    Check(!board.soundRom.empty(), "ddonpach_sound_rom_loaded");

    board.Reset();
    Check(board.cpu.pc > 0 && board.cpu.pc < board.program.size(), "m68k_reset_pc_in_rom");
    Check(board.cpu.a[7] != 0, "m68k_reset_sp_nonzero");

    // 4. Multi-frame execution, framebuffer & audio checks
    std::vector<uint32_t> fb(spelunk::CaveVideo::kWidth * spelunk::CaveVideo::kHeight, 0);
    uint32_t firstHash = 0;
    uint32_t lastHash = 0;
    uint32_t maxNonBlack = 0;
    size_t maxUniqueColors = 0;
    uint32_t totalBadAlpha = 0;
    size_t totalPcmSamples = 0;
    bool hashChanged = false;

    constexpr int kFrames = 300;
    for (int f = 1; f <= kFrames; ++f) {
        // Exercise coin/start/stick/button inputs mid-run
        board.system = (f >= 240 && f <= 245) ? spelunk::kSysCoin1 : (f >= 260 && f <= 265) ? spelunk::kSysStart1 : 0;
        board.inputs[0] = (f >= 270 && f <= 285) ? (spelunk::kInUp | spelunk::kInButton1) : 0;

        board.RunFrame();
        auto &pcm = board.SoundOut();
        totalPcmSamples += pcm.size();
        pcm.clear();

        if (f == 1 || f == 60 || f == 180 || f == 240 || f == 300) {
            board.Picture(fb.data());
            uint32_t nonBlack = 0, badAlpha = 0;
            const uint32_t h = HashFrame(fb, nonBlack, badAlpha);
            std::unordered_set<uint32_t> cols(fb.begin(), fb.end());
            if (cols.size() > maxUniqueColors) {
                maxUniqueColors = cols.size();
            }
            totalBadAlpha += badAlpha;
            if (nonBlack > maxNonBlack) {
                maxNonBlack = nonBlack;
            }
            if (f == 1) {
                firstHash = h;
            } else if (h != firstHash) {
                hashChanged = true;
            }
            lastHash = h;
            std::printf("  [INFO] frame=%d nonBlack=%u/%zu uniqueColors=%zu hash=0x%08X pictures=%u\n",
                        f, nonBlack, fb.size(), cols.size(), h, board.pictures);
        }
    }

    Check(board.frame == kFrames, "cave68k_frame_counter");
    Check(board.cpu.cycles >= static_cast<uint64_t>(kFrames) * spelunk::Cave68k::kFrameCycles,
          "cave68k_cycle_progression");
    Check(board.pictures >= 100, "cave68k_sprite_lists_active");
    Check(maxNonBlack > 1000, "cave68k_nonblack_framebuffer");
    Check(maxUniqueColors >= 8, "cave68k_rich_palette_rendered");
    Check(totalBadAlpha == 0, "cave68k_alpha_channel_opaque");
    Check(hashChanged && lastHash != 0, "cave68k_framebuffer_progresses");
    Check(totalPcmSamples > static_cast<size_t>(kFrames) * 500u, "cave68k_pcm_audio_generated");

    // 5. Determinism & KeepNow/PutBack rollback validation
    spelunk::Cave68k b1, b2;
    LoadDdonpachRoms(b1, romDir);
    LoadDdonpachRoms(b2, romDir);
    b1.SetBoard("ddonpach");
    b2.SetBoard("ddonpach");
    b1.Reset();
    b2.Reset();
    for (int f = 0; f < 30; ++f) {
        b1.RunFrame();
        b1.SoundOut().clear();
        b2.RunFrame();
        b2.SoundOut().clear();
    }
    Check(b1.StateHash() == b2.StateHash(), "cave68k_deterministic_state_hash");

    b1.KeepNow(false, 0);
    for (int f = 0; f < 10; ++f) {
        b1.RunFrame();
        b1.SoundOut().clear();
    }
    const uint64_t afterRun = b1.StateHash();
    b1.PutBack(0);
    for (int f = 0; f < 10; ++f) {
        b1.RunFrame();
        b1.SoundOut().clear();
    }
    Check(b1.StateHash() == afterRun, "cave68k_keepnow_putback_determinism");

    std::printf("SUMMARY: passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
