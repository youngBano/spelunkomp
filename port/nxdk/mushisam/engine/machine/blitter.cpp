// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The operations, the blending and the timing follow MAME's cv1k_v.cpp (BSD-3-Clause, David Haywood, Luca Elia and
// MetalliC; timing from buffi's "CV1000 Blitter Research"): see THIRD_PARTY_NOTICES.md.
#include "blitter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace spelunk {

namespace {
    // Colour arithmetic on 5-bit channels, clamped: x * y / 31 (y up to 63: a tint of 32 is "unchanged"),
    // (31 - x) * y / 31, and x + y
    uint8_t g_mul[0x20][0x40], g_rev[0x20][0x40], g_add[0x20][0x20];
    bool g_tables = [] {
        for (int y = 0; y < 0x40; y++) {
            for (int x = 0; x < 0x20; x++) {
                g_mul[x][y] = static_cast<uint8_t>(std::min((x * y) / 0x1F, 0x1F));
                g_rev[x ^ 0x1F][y] = g_mul[x][y];
            }
        }
        for (int y = 0; y < 0x20; y++) {
            for (int x = 0; x < 0x20; x++) {
                g_add[x][y] = static_cast<uint8_t>(std::min(x + y, 0x1F));
            }
        }
        return true;
    }();

    // Time, as measured on the board (buffi's research). The draws are counted in VRAM clocks: 76.8 MHz (12.8 MHz x 6,
    // 13.0208 ns; MAME rounds it to 13). The screen's line fetch comes every line (63.594 us: 407 dots of 6.4 MHz,
    // 60.0184 Hz) and takes 166 VRAM clocks (2.161 us; MAME: 2.16 us every 63.6 us of the list's own time, which
    // leaves out the fetches that fall in the time the fetches add). Both as ikacore_CV1k's notes on the board have
    // them; together they bring buffi's blitter-only tests from 0.4% fast to the board's numbers (docs/SLOWDOWN.md)
    constexpr int kVramClockHz = 76800000, kMainRamClockNs = 20, kUploadChunkGapNs = 1130;
    constexpr int kLinePeriodNs = 63594, kLineFetchNs = 2161;
    constexpr int kChunkBytes = 64, kChunkReadNs = 700;
    constexpr int kDrawBytes = 20, kClipBytes = 2;
    // Draws reach this far outside the screen's area of VRAM
    constexpr int kClipMargin = 32;

    struct BlitArgs {
        uint16_t *vram;
        int srcX, srcY, dstX, dstY, startX, startY, dimX, dimY, stepX, stepY;
        uint8_t sAlpha, dAlpha, tint[3];
    };
    using BlitFn = void (*)(const BlitArgs &);

    // The source's part of a blend (mode 0-7): the source times the source alpha, itself, the destination, one, and
    // the same four with the factor taken from one
    template <int SM>
    inline uint8_t SourceTerm(uint8_t s, uint8_t d, uint8_t alpha) {
        switch (SM) {
        case 0: return g_mul[alpha][s];
        case 1: return g_mul[s][s];
        case 2: return g_mul[d][s];
        case 4: return g_rev[alpha][s];
        case 5: return g_rev[s][s];
        case 6: return g_rev[d][s];
        default: return s;
        }
    }
    // The destination's part: the destination times the destination alpha, the source, itself, one, and the same four
    // with the factor taken from one
    template <int DM>
    inline uint8_t DestTerm(uint8_t s, uint8_t d, uint8_t alpha) {
        switch (DM) {
        case 0: return g_mul[d][alpha];
        case 1: return g_mul[s][d];
        case 2: return g_mul[d][d];
        case 4: return g_rev[alpha][d];
        case 5: return g_rev[s][d];
        case 6: return g_rev[d][d];
        default: return d;
        }
    }

    // MODE: source mode | destination mode << 3, or 64 for no blending
    template <int MODE, bool TINT, bool TRANS>
    void Blit(const BlitArgs &a) {
        for (int y = a.startY; y < a.dimY; y++) {
            uint16_t *dst = a.vram + static_cast<size_t>(a.dstY + y) * Blitter::kVramWidth + a.dstX + a.startX;
            const uint16_t *src = a.vram + static_cast<size_t>((a.srcY + a.stepY * y) & 0x0FFF) * Blitter::kVramWidth + a.srcX +
                                  a.stepX * a.startX;
            const uint16_t *const end = dst + (a.dimX - a.startX);
            for (; dst < end; dst++, src += a.stepX) {
                const uint16_t pen = *src;
                if (TRANS && !(pen & 0x8000)) {
                    continue;
                }
                if (MODE == 64 && !TINT) {
                    *dst = pen;
                    continue;
                }
                uint8_t r = (pen >> 10) & 0x1F, g = (pen >> 5) & 0x1F, b = pen & 0x1F;
                if (TINT) {
                    r = g_mul[r][a.tint[0]];
                    g = g_mul[g][a.tint[1]];
                    b = g_mul[b][a.tint[2]];
                }
                if (MODE != 64) {
                    const uint16_t under = *dst;
                    const uint8_t dr = (under >> 10) & 0x1F, dg = (under >> 5) & 0x1F, db = under & 0x1F;
                    const uint8_t nr = g_add[SourceTerm<MODE & 7>(r, dr, a.sAlpha)][DestTerm<(MODE >> 3) & 7>(r, dr, a.dAlpha)];
                    const uint8_t ng = g_add[SourceTerm<MODE & 7>(g, dg, a.sAlpha)][DestTerm<(MODE >> 3) & 7>(g, dg, a.dAlpha)];
                    const uint8_t nb = g_add[SourceTerm<MODE & 7>(b, db, a.sAlpha)][DestTerm<(MODE >> 3) & 7>(b, db, a.dAlpha)];
                    r = nr;
                    g = ng;
                    b = nb;
                }
                *dst = static_cast<uint16_t>((pen & 0x8000) | (r << 10) | (g << 5) | b);
            }
        }
    }

    template <size_t... I>
    constexpr std::array<BlitFn, sizeof...(I)> MakeBlits(std::index_sequence<I...>) {
        return {&Blit<I / 4, (I & 2) != 0, (I & 1) != 0>...};
    }
    // (by mode * 4 + tinted * 2 + transparent)
    const std::array<BlitFn, 65 * 4> kBlits = MakeBlits(std::make_index_sequence<65 * 4>{});

    // The 32 x 32 pixel rows of VRAM a rectangle touches (each costs the blitter a fixed time)
    int VramRows(int startX, int startY, int dimX, int dimY) {
        int across = 0, rows = 0;
        for (int left = dimX; left > 0; left -= 32) {
            across++;
            if ((startX & 31) + std::min(32, left) > 32) {
                across++;
            }
        }
        for (int left = dimY; left > 0; left -= 32) {
            rows += across;
            if ((startY & 31) + std::min(32, left) > 32) {
                rows += across;
            }
        }
        return rows;
    }
} // namespace

Blitter::Blitter()
#ifdef XBOX_NO_SW_VRAM
    : vram()
#else
    : vram(static_cast<size_t>(kVramWidth) * kVramHeight, 0)
#endif
{
    (void)g_tables;
#ifdef XBOX_NO_SW_VRAM
    rasterize = false;
#endif
}

Blitter::Clip Blitter::ScreenClip() const {
    Clip c;
    c.left = std::max(0, static_cast<int>(clipX) - kClipMargin);
    c.right = std::min(kVramWidth - 1, static_cast<int>(clipX) + kScreenWidth - 1 + kClipMargin);
    c.top = std::max(0, static_cast<int>(clipY) - kClipMargin);
    c.bottom = std::min(kVramHeight - 1, static_cast<int>(clipY) + kScreenHeight - 1 + kClipMargin);
    return c;
}

void Blitter::Idle(unsigned bytes) {
    m_idleBytes += bytes;
    if (m_idleBytes >= kChunkBytes) {
        m_idleBytes -= kChunkBytes;
        m_delayNs += kChunkReadNs;
        stats.idleNs += keepStats ? kChunkReadNs : 0;
    }
}

// Upload: 0x2000, 0, 0x9999, 0x9999, destination x, y, width - 1, height - 1, then the pixels
void Blitter::Upload(uint32_t &address) {
    const uint32_t chunkOffset = (address - (listAddress & 0x1FFFFFFFu)) & (kChunkBytes - 1);
    Word(address);
    Word(address);
    Word(address);
    Word(address);
    const uint32_t dstX = Word(address) & 0x1FFF;
    const uint32_t dstY = Word(address) & 0x0FFF;
    const uint32_t dimX = (Word(address) & 0x1FFF) + 1;
    const uint32_t dimY = (Word(address) & 0x0FFF) + 1;
    if (m_journal && rasterize) {
        Touch(static_cast<int>(dstY), static_cast<int>(dstY + dimY)); // (a row more: the last row's pixels can run into it)
    }
    // (the native list takes what lies inside VRAM; a picture that runs off its right edge is cut there)
    const uint32_t nativeW = std::min(dimX, static_cast<uint32_t>(kVramWidth) - dstX);
    const uint32_t nativeH = std::min(dimY, static_cast<uint32_t>(kVramHeight) - dstY);
    const bool useDirectUpload = (!rasterize && native && onDirectUpload && native->draws.empty() &&
                                  nativeW == dimX && nativeH == dimY);
    size_t nativeAt = 0;
    if (useDirectUpload) {
        // Zero-copy fast path: stream straight from SH-3 RAM into sparse VRAM pages (0 B NativeList::pixels!).
        onDirectUpload(onDirectUploadCtx, static_cast<int32_t>(dstX), static_cast<int32_t>(dstY),
                       static_cast<int32_t>(nativeW), static_cast<int32_t>(nativeH),
                       ram, ramMask, address);
        address += dimX * dimY * 2u;
    } else {
        if (native) {
            nativeAt = native->pixels.size();
            native->pixels.resize(nativeAt + static_cast<size_t>(nativeW) * nativeH);
            native->uploads.push_back({static_cast<int32_t>(dstX), static_cast<int32_t>(dstY), static_cast<int32_t>(nativeW), static_cast<int32_t>(nativeH), nativeAt});
            native->steps.push_back({true, static_cast<uint32_t>(native->uploads.size() - 1), 1});
        }
        if (!rasterize && native && nativeW == dimX && nativeH == dimY) {
            // Fast path for native Xbox NV2A mode (zero per-pixel branches).
            uint16_t *dst = native->pixels.data() + nativeAt;
            const uint32_t count = dimX * dimY;
            for (uint32_t i = 0; i < count; ++i) {
                dst[i] = Word(address);
            }
        } else {
            for (uint32_t y = 0; y < dimY; y++) {
                size_t at = static_cast<size_t>(dstY + y) * kVramWidth + dstX;
                for (uint32_t x = 0; x < dimX; x++, at++) {
                    const uint16_t pen = Word(address);
                    if (rasterize && at < vram.size()) {
                        vram[at] = pen;
                    }
                    if (native && x < nativeW && y < nativeH) {
                        native->pixels[nativeAt + static_cast<size_t>(y) * nativeW + x] = pen;
                    }
                }
            }
        }
    }
    // The time is main RAM's: the blitter takes the bus from the CPU for each 64 bytes it reads, 32 bits a clock
    const uint32_t bytes = 16 + dimX * dimY * 2;
    const uint32_t clocks = bytes / 4;
    lastStolenCpuCycles += clocks * 2;
    m_delayNs += static_cast<uint64_t>(clocks) * kMainRamClockNs;
    m_delayNs += static_cast<uint64_t>((chunkOffset + bytes) / kChunkBytes) * kUploadChunkGapNs;
    if (keepStats) {
        stats.uploadNs += static_cast<uint64_t>(clocks) * kMainRamClockNs + static_cast<uint64_t>((chunkOffset + bytes) / kChunkBytes) * kUploadChunkGapNs;
    }
    m_idleBytes = 0;
    uploads++;
    uploadedPixels += dimX * dimY;
}

// Draw: attributes (0x1000, flip x 0x800, flip y 0x400, blend 0x200, transparent 0x100, source mode 0x70, destination
// mode 0x07), source alpha << 8 | destination alpha, source x, y, destination x, y, width - 1, height - 1, tint red,
// tint green << 8 | blue
void Blitter::DrawOp(uint32_t &address) {
    Draw d;
    d.attr = Word(address);
    d.alpha = Word(address);
    d.srcX = Word(address);
    d.srcY = Word(address);
    d.dstX = static_cast<int16_t>(Word(address));
    d.dstY = static_cast<int16_t>(Word(address));
    d.width = Word(address);
    d.height = Word(address);
    d.tintR = Word(address);
    d.tintGB = Word(address);
    if (record) {
        draws.push_back(d);
    }
    stats.draws += keepStats;

    BlitArgs a;
    a.vram = vram.data();
    const unsigned dMode = d.attr & 7, sMode = (d.attr >> 4) & 7;
    const bool trans = d.attr & 0x100, flipY = d.attr & 0x400, flipX = d.attr & 0x800;
    bool blend = d.attr & 0x200;
    a.sAlpha = static_cast<uint8_t>((d.alpha >> 8) >> 3);
    a.dAlpha = static_cast<uint8_t>((d.alpha & 0xFF) >> 3);
    a.tint[0] = static_cast<uint8_t>((d.tintR & 0xFF) >> 2);
    a.tint[1] = static_cast<uint8_t>((d.tintGB >> 8) >> 2);
    a.tint[2] = static_cast<uint8_t>((d.tintGB & 0xFF) >> 2);
    const bool tinted = a.tint[0] != 0x20 || a.tint[1] != 0x20 || a.tint[2] != 0x20;
    // (all of the source and none of the destination: no blend)
    if (sMode == 0 && a.sAlpha == 0x1F && dMode == 4 && a.dAlpha == 0x1F) {
        blend = false;
    }
    a.srcX = d.srcX & 0x1FFF;
    a.srcY = d.srcY & 0x0FFF;
    a.dstX = d.dstX;
    a.dstY = d.dstY;
    a.dimX = (d.width & 0x1FFF) + 1;
    a.dimY = (d.height & 0x0FFF) + 1;

    // --- the time it takes (before the draw's own clipping changes the sizes) ---
#if !defined(XBOX_NO_SW_VRAM) && !defined(XBOX_FAST_CPU)
    // The destination is read as unsigned 16-bit here, as MAME's cv1k_v.cpp does: a draw that starts left of the clip
    // or above it costs a list entry's read alone, although the draw below paints its pixels inside. Timing such draws
    // by what they paint (2026-11-22, #56) made Mushihimesama's stage 3 late about three times as often as two boards
    // filmed at 60 pictures a second (docs/BOARD-VIDEO.md, "Mushihimesama's stage 3"); read this way it is a little
    // under both boards, which played a busier route.
    {
        int x0 = static_cast<uint16_t>(d.dstX), y0 = static_cast<uint16_t>(d.dstY);
        int x1 = static_cast<uint16_t>(x0 + a.dimX - 1), y1 = static_cast<uint16_t>(y0 + a.dimY - 1);
        const bool overlaps = !(x0 > m_clip.right || x1 < m_clip.left || y0 > m_clip.bottom || y1 < m_clip.top);
        if (!overlaps) {
            Idle(kDrawBytes);
        } else {
            x0 = std::max(x0, m_clip.left);
            y0 = std::max(y0, m_clip.top);
            x1 = std::min(x1, m_clip.right);
            y1 = std::min(y1, m_clip.bottom);
            const int w = x1 - x0 + 1, h = y1 - y0 + 1;
            m_idleBytes = 0;
            const int srcRows = VramRows(a.srcX, a.srcY, w, h);
            const int dstRows = VramRows(x0, y0, w, h);
            // (draws go four pixels at a time)
            x0 -= x0 & 3;
            x1 += (4 - ((x1 + 1) & 3)) & 3;
            const int dw = x1 - x0 + 1, dh = y1 - y0 + 1;
            // (clocks: the source read four pixels a clock, the destination read and written, 6 a source row, 20 + 11
            // a destination row, 12 a draw)
            const int64_t clocks = static_cast<int64_t>(w) * h / 4 + static_cast<int64_t>(dw) * dh / 2 + srcRows * 6 + dstRows * (20 + 11) + 12;
            m_vramClocks += static_cast<uint64_t>(std::max<int64_t>(clocks, 0));
            if (keepStats) {
                const uint64_t px = static_cast<uint64_t>(w) * h / 4 + static_cast<uint64_t>(dw) * dh / 2;
                stats.drawn++;
                stats.pixels += static_cast<uint64_t>(w) * h;
                stats.pixelClocks += px;
                stats.rowClocks += srcRows * 6 + dstRows * 31;
                stats.fixedClocks += 12;
                if (blend) {
                    stats.blended++;
                    stats.blendClocks += clocks;
                }
                if (d.dstX < m_clip.left || d.dstY < m_clip.top) {
                    stats.overLeftTop++;
                    stats.overLeftTopClocks += clocks;
                }
                if (d.dstX + a.dimX - 1 > m_clip.right || d.dstY + a.dimY - 1 > m_clip.bottom) {
                    stats.overRightBottom++;
                    stats.overRightBottomClocks += clocks;
                }
                if (static_cast<int64_t>(w) * h >= 64 * 64) {
                    stats.big++;
                    stats.bigClocks += clocks;
                }
                const int sx0 = std::max(x0, static_cast<int>(clipX)), sy0 = std::max(y0, static_cast<int>(clipY));
                const int sx1 = std::min(x0 + dw - 1, static_cast<int>(clipX) + kScreenWidth - 1), sy1 = std::min(y1, static_cast<int>(clipY) + kScreenHeight - 1);
                const int64_t on = (sx1 >= sx0 && sy1 >= sy0) ? static_cast<int64_t>(sx1 - sx0 + 1) * (sy1 - sy0 + 1) : 0;
                stats.offscreenPixels += static_cast<uint64_t>(std::max<int64_t>(static_cast<int64_t>(dw) * dh - on, 0));
            }
        }
    }
#endif

    // --- the draw ---
    if (flipX) {
        a.srcX += a.dimX - 1;
    }
    if (flipY) {
        a.srcY += a.dimY - 1;
    }
    a.stepX = flipX ? -1 : 1;
    a.stepY = flipY ? -1 : 1;
    a.startX = a.startY = 0;
    if (a.dstY < m_clip.top) {
        a.startY = m_clip.top - a.dstY;
    }
    if (a.dstY + a.dimY > m_clip.bottom) {
        a.dimY -= (a.dstY + a.dimY - 1) - m_clip.bottom;
    }
    // (a source that would wrap round the side of VRAM is not drawn)
    if (flipX ? (a.srcX & 0x1FFF) < ((a.srcX - (a.dimX - 1)) & 0x1FFF) : (a.srcX & 0x1FFF) > ((a.srcX + (a.dimX - 1)) & 0x1FFF)) {
        return;
    }
    if (a.dstX < m_clip.left) {
        a.startX = m_clip.left - a.dstX;
    }
    if (a.dstX + a.dimX > m_clip.right) {
        a.dimX -= (a.dstX + a.dimX - 1) - m_clip.right;
    }
    if (a.startX >= a.dimX || a.startY >= a.dimY) {
        return;
    }
    if (m_journal && rasterize) {
        Touch(a.dstY + a.startY, a.dstY + a.dimY - 1);
    }
    const unsigned mode = blend ? (sMode | (dMode << 3)) : 64;
    if (native) {
        NativeDraw n;
        n.x0 = a.dstX + a.startX;
        n.y0 = a.dstY + a.startY;
        n.x1 = a.dstX + a.dimX - 1;
        n.y1 = a.dstY + a.dimY - 1;
        n.sx = flipX ? a.srcX + a.dstX : a.srcX - a.dstX;
        n.sy = flipY ? a.srcY + a.dstY : a.srcY - a.dstY;
        n.flags = static_cast<int32_t>(mode) | (tinted ? NativeDraw::kTinted : 0) | (trans ? NativeDraw::kTransparent : 0) |
                  (flipX ? NativeDraw::kFlipX : 0) | (flipY ? NativeDraw::kFlipY : 0);
        n.colours = a.sAlpha | (a.dAlpha << 5) | (a.tint[0] << 10) | (a.tint[1] << 16) | (a.tint[2] << 22);
        // (draws that follow one another are one step)
        if (!native->steps.empty() && !native->steps.back().upload) {
            native->steps.back().count++;
        } else {
            native->steps.push_back({false, static_cast<uint32_t>(native->draws.size()), 1});
        }
        native->draws.push_back(n);
    }
    if (rasterize) {
        kBlits[mode * 4 + (tinted ? 2 : 0) + (trans ? 1 : 0)](a);
    }
}

void Blitter::Run() {
    uint32_t address = listAddress & 0x1FFFFFFFu;
    m_clip = ScreenClip();
    lists++;
    // (a list that never ends would be a game's bug; RAM is finite either way)
    for (uint32_t guard = 0; guard < 0x400000; guard++) {
        uint32_t peek = address;
        const uint16_t op = Word(peek);
        switch (op & 0xF000) {
        case 0x0000:
        case 0xF000:
            return;
        case 0xC000:
            address += 2;
            m_clip = Word(address) ? ScreenClip() : Clip{0, kVramWidth - 1, 0, kVramHeight - 1};
            Idle(kClipBytes);
            break;
        case 0x2000:
            Upload(address);
            break;
        case 0x1000:
            DrawOp(address);
            break;
        default:
            fprintf(stderr, "blitter: unknown operation %04X at %08X\n", op, address);
            return;
        }
    }
}

uint32_t Blitter::Read(uint32_t offset) {
    switch (offset) {
    case 0x10: return busy ? 0 : 0x10; // ready
    case 0x24: return 0xFFFFFFFFu;
    case 0x28: return 0xFFFFFFFFu;
    case 0x50: return dipSwitches;
    default: return 0;
    }
}

void Blitter::Write(uint32_t offset, uint32_t value) {
    switch (offset) {
    case 0x04:
        if (value & 1) {
            m_delayNs = 0;
            m_vramClocks = 0;
            lastStolenCpuCycles = 0;
            stats = ListStats{};
            Run();
#if !defined(XBOX_NO_SW_VRAM) && !defined(XBOX_FAST_CPU)
            m_delayNs += m_vramClocks * 1000000000ull / kVramClockHz;
            // (every line period the blitter stops to fetch a line of the picture for the screen, also in the time
            // those stops add)
            stats.lineFetchNs = m_delayNs / (kLinePeriodNs - kLineFetchNs) * kLineFetchNs;
            m_delayNs += stats.lineFetchNs;
#endif
            lastBusyNs = m_delayNs;
            busy = true;
            started = true;
        }
        break;
    case 0x08: listAddress = value; break;
    case 0x14: scrollX = value; break;
    case 0x18: scrollY = value; break;
    case 0x40: clipX = value; break;
    case 0x44: clipY = value; break;
    default: break; // (0x24: a handshake at each interrupt; 0x28: coins; 0x30-0x3C: the test menu's screen adjustments)
    }
}

void Blitter::Picture(uint32_t *out) const {
    const uint32_t sx = scrollX, sy = scrollY;
    for (int y = 0; y < kScreenHeight; y++) {
        const uint16_t *row = &vram[static_cast<size_t>((sy + y) & (kVramHeight - 1)) * kVramWidth];
        for (int x = 0; x < kScreenWidth; x++) {
            const uint16_t p = row[(sx + x) & (kVramWidth - 1)];
            const uint32_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
            *out++ = 0xFF000000u | ((b << 3 | b >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (r << 3 | r >> 2);
        }
    }
}

void Blitter::Touch(int firstRow, int lastRow) {
    firstRow = std::max(firstRow, 0);
    lastRow = std::min(lastRow, kVramHeight - 1);
    for (Journal &j : m_journals) {
        if (!j.on) {
            continue;
        }
        for (int row = firstRow; row <= lastRow; row++) {
            if (!j.rowKept[row]) {
                j.rowKept[row] = 1;
                j.keptRows.push_back(static_cast<uint32_t>(row));
                std::memcpy(&j.backup[static_cast<size_t>(row) * kVramWidth], &vram[static_cast<size_t>(row) * kVramWidth], kVramWidth * sizeof(uint16_t));
            }
        }
    }
}

void Blitter::BeginJournal(int which) {
    Journal &j = m_journals[which];
    if (j.backup.empty()) {
        j.backup.resize(vram.size());
        j.rowKept.assign(kVramHeight, 0);
    }
    // (begun again while on: VRAM as it is now is what is kept from here, online play's anchor moved on)
    EndJournal(which);
    j.on = m_journal = true;
}

void Blitter::EndJournal(int which) {
    Journal &j = m_journals[which];
    for (uint32_t row : j.keptRows) {
        j.rowKept[row] = 0;
    }
    j.keptRows.clear();
    j.on = false;
    m_journal = m_journals[0].on || m_journals[1].on;
}

void Blitter::Rollback(int which) {
    Journal &j = m_journals[which];
    for (uint32_t row : j.keptRows) {
        std::memcpy(&vram[static_cast<size_t>(row) * kVramWidth], &j.backup[static_cast<size_t>(row) * kVramWidth], kVramWidth * sizeof(uint16_t));
    }
    EndJournal(which);
}

void Blitter::Serialize(Archive &ar) {
    ar.Mark(0x424C4954);
    ar.Vec(vram);
    SerializeRegisters(ar);
}

void Blitter::SerializeRegisters(Archive &ar) {
    ar.Pod(scrollX);
    ar.Pod(scrollY);
    ar.Pod(clipX);
    ar.Pod(clipY);
    ar.Pod(listAddress);
    ar.Pod(busy);
    ar.Pod(dipSwitches);
    ar.Pod(m_idleBytes);
}

} // namespace spelunk
