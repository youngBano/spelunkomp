// Standalone host benchmark for Mushihimesama (Cave CV1000-B / SH-3 native static recomp):
// Verifies streaming 64 KB NAND (132 MB file), 8 MB CV1000-B RAM, zero-VRAM blitter,
// native SH-3 blocks, and MushisamFakePlay timing against CV1000's 16,662 us budget.
#include "machine/cv1000.h"
#include "mushisam_fake_play.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

void SwapWords(uint8_t *d, size_t n) {
    for (size_t i = 0; i + 1 < n; i += 2) {
        std::swap(d[i], d[i + 1]);
    }
}

bool LoadFileSwapped(const char *path, std::vector<uint8_t> &dst, size_t offset = 0) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        std::fprintf(stderr, "Missing %s\n", path);
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
    SwapWords(dst.data() + offset, got);
    return true;
}

} // namespace

int main() {
    spelunk::Cv1000 board;
    board.boardD = false;
    if (!LoadFileSwapped("../mushisam/bin/mushisam/mushisam_u4", board.program, 0)) return 1;
    if (!LoadFileSwapped("../mushisam/bin/mushisam/u23", board.soundRom, 0)) return 1;
    if (!LoadFileSwapped("../mushisam/bin/mushisam/u24", board.soundRom, 0x400000)) return 1;
    if (!board.nand.AttachFile("../mushisam/bin/mushisam/mushisam_u2")) return 1;

    spelunk::NativeList nativeList;
    board.blitter.rasterize = false;
    board.blitter.native = &nativeList;
    board.cpu.mode = spelunk::SH3::Mode::Native;
    board.Reset();

    spelunk_xbox::MushisamFakePlay fp;
    fp.Reset();
    fp.enabled = true;

    constexpr int kTotalFrames = 3600;
    constexpr uint64_t kBudgetUs = 16662;
    uint64_t sumUs = 0, maxUs = 0;
    uint32_t lagFrames = 0, playFrames = 0, maxDraws = 0;

    for (int f = 0; f < kTotalFrames; ++f) {
        uint32_t p1 = 0, sys = 0;
        fp.Update(board, p1, sys);
        board.inputs[0] = p1;
        board.system = sys;

        const auto t0 = std::chrono::steady_clock::now();
        board.RunFrame();
        board.sound.out.clear();
        const uint64_t us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());

        sumUs += us;
        if (us > maxUs) maxUs = us;
        if (us > kBudgetUs) ++lagFrames;
        if (fp.InPlay(board)) ++playFrames;
        if (nativeList.draws.size() > maxDraws) maxDraws = static_cast<uint32_t>(nativeList.draws.size());
        nativeList.Clear();
    }

    std::printf("Mushihimesama Host Native Bench (%d frames):\n", kTotalFrames);
    std::printf("  Avg CPU: %.1f us | Max CPU: %llu us | Host Lag (>16662us): %u\n",
                static_cast<double>(sumUs) / kTotalFrames,
                static_cast<unsigned long long>(maxUs), lagFrames);
    std::printf("  Active Play Frames: %u | Max Draws/Frame: %u | NAND Page Reads: %llu\n",
                playFrames, maxDraws, static_cast<unsigned long long>(board.nand.reads));
    return (playFrames > 500) ? 0 : 1;
}
