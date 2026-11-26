// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The CV1000's video: a blitter in the board's FPGA with a VRAM of 8192 x 4096 pixels (15-bit colour and one
// transparency bit). The game writes a list of operations into main RAM (upload a picture into VRAM, draw a rectangle
// of VRAM to another place in VRAM with flips, a tint and blending, set the clip) and tells the blitter to run it; the
// screen shows a 320 x 240 window of VRAM at a scroll position, so a game draws a frame beside the one on show and
// then moves the window.
//
// The operations, the blending and the time the blitter takes follow MAME's cv1k_v.cpp (BSD-3-Clause, David Haywood,
// Luca Elia and MetalliC), whose timing is from buffi's "CV1000 Blitter Research": see THIRD_PARTY_NOTICES.md.
#pragma once

#include "../runtime/state.h"

#include <cstdint>
#include <vector>

namespace spelunk {

// The lists the game ran, as the native renderer takes them (port/gpu.h, docs/NATIVE.md): each draw with its clipping
// done and its source worked out, and each upload with its pixels, in the game's order
struct NativeDraw {
    int32_t x0, y0, x1, y1; // the destination's pixels in VRAM (inclusive), after the clip
    int32_t sx, sy;         // the source of destination (x, y) is (sx + x, sy + y), or (sx - x, sy - y) where flipped
                            // (y taken modulo VRAM's height)
    int32_t flags;          // the blend (0-63: source mode | destination mode << 3; 64: none), then the bits below
    int32_t colours;        // source alpha | destination alpha << 5 | tint red << 10 | green << 16 | blue << 22
    enum : int32_t { kTinted = 1 << 7, kTransparent = 1 << 8, kFlipX = 1 << 9, kFlipY = 1 << 10 };
};
struct NativeUpload {
    int32_t x, y, width, height;
    size_t pixels; // (where its pixels begin in NativeList::pixels: width * height of them)
};
struct NativeList {
    struct Step {
        bool upload;
        uint32_t first, count; // (draws[first .. first + count), or uploads[first])
    };
    std::vector<Step> steps;
    std::vector<NativeDraw> draws;
    std::vector<NativeUpload> uploads;
    std::vector<uint16_t> pixels;
    void Clear() {
        steps.clear();
        draws.clear();
        uploads.clear();
        pixels.clear();
    }
};

// A part of VRAM
struct VramArea {
    int x, y, w, h;
};

class Blitter {
public:
    static constexpr int kVramWidth = 0x2000, kVramHeight = 0x1000;
    static constexpr int kScreenWidth = 320, kScreenHeight = 240;

    Blitter();

    // Main RAM as the board holds it (big-endian), and its size less one
    const uint8_t *ram = nullptr;
    uint32_t ramMask = 0;

    // The registers (offsets from 0x18000000; all 32 bits wide)
    uint32_t Read(uint32_t offset);
    void Write(uint32_t offset, uint32_t value);
    // The time the list just started will keep the blitter busy, and the time its uploads take from the CPU (set by a
    // write that starts a list; the board turns them into CPU cycles)
    uint64_t lastBusyNs = 0, lastStolenCpuCycles = 0;
    // A list's start, with "Slowdown as the board" on: the blitter reads the list's first 64 bytes before its first
    // operation (a chunk's read, as buffi measured it), which the time above leaves out. Seen on the board as the
    // benchmark's BLIT LATENCY (docs/SLOWDOWN.md); with the feature off the games keep MAME's count
    static constexpr uint64_t kListStartNs = 1130;
    bool started = false; // (a list was started by the last Write)
    bool busy = false;    // (the board clears it when the time is up)

    uint8_t dipSwitches = 0; // S2 (read at 0x50)

    // The 320 x 240 picture on show (the window of VRAM at the scroll position), as 32-bit pixels with red in the low
    // byte
    void Picture(uint32_t *out) const;

    // One draw operation as the list gave it (the frame's draws, for the native renderer and the checks)
    struct Draw {
        uint16_t attr, alpha, srcX, srcY;
        int16_t dstX, dstY;
        uint16_t width, height, tintR, tintGB;
    };
    // What the lists run since the last ClearRecord did, when `record` is set
    bool record = false;
    std::vector<Draw> draws;
    uint32_t uploads = 0, uploadedPixels = 0, lists = 0;
    void ClearRecord() {
        draws.clear();
        uploads = uploadedPixels = lists = 0;
    }
    // What the last list's time was made of (measurement only: SPELUNK_BLIT_LOG, machine/cv1000.cpp; not state)
    struct ListStats {
        uint32_t draws, drawn, blended, overLeftTop, overRightBottom, big;
        uint64_t pixels, pixelClocks, rowClocks, fixedClocks, blendClocks, overLeftTopClocks, overRightBottomClocks, bigClocks;
        uint64_t offscreenPixels; // (painted outside the screen, in the clip's margin)
        uint64_t uploadNs, idleNs, lineFetchNs;
    };
    ListStats stats{};
    bool keepStats = false;

    // The native renderer's part: with `native` set, what the lists do is noted there in order; with `rasterize` off
    // the software blitter draws nothing (VRAM is then the GPU's, and `vram` is brought up to date only when asked)
    NativeList *native = nullptr;
    bool rasterize = true;

    std::vector<uint16_t> vram; // kVramWidth x kVramHeight: bit 15 opaque, then red, green, blue (5 bits each)
    uint32_t scrollX = 0, scrollY = 0, clipX = 0, clipY = 0, listAddress = 0;

    void Serialize(Archive &ar);
    // Everything but VRAM's pixels (run-ahead keeps those by the rows written: below)
    void SerializeRegisters(Archive &ar);
    // Run-ahead (port/main.cpp): from BeginJournal, each row of VRAM is kept as it was before it is first written;
    // Rollback puts those rows back and ends the journal. Two journals can be kept at once, each from its own moment
    // (online play's rollback: port/netplay.h); EndJournal ends one without putting anything back.
    static constexpr int kJournals = 2;
    void BeginJournal(int which = 0);
    void Rollback(int which = 0);
    void EndJournal(int which);

private:
    struct Clip {
        int left, right, top, bottom; // (inclusive)
    };
    Clip m_clip{0, kVramWidth - 1, 0, kVramHeight - 1};
    Clip ScreenClip() const;
    uint16_t Word(uint32_t &address) const {
        const uint32_t a = address & ramMask & ~1u;
        address += 2;
        return static_cast<uint16_t>((ram[a] << 8) | ram[a + 1]);
    }
    void Run();
    void Upload(uint32_t &address);
    void DrawOp(uint32_t &address);
    void Idle(unsigned bytes);
    struct Journal {
        bool on = false;
        std::vector<uint16_t> backup; // (VRAM's size once used: only the rows noted hold anything)
        std::vector<uint8_t> rowKept;
        std::vector<uint32_t> keptRows;
    };
    Journal m_journals[kJournals];
    bool m_journal = false; // (either is on)
    void Touch(int firstRow, int lastRow);
    uint64_t m_delayNs = 0;
    uint64_t m_vramClocks = 0; // (the list's draws, while it is run: not state)
    unsigned m_idleBytes = 0;
};

} // namespace spelunk
