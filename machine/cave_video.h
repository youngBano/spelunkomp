// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The video of Cave's first boards: a sprite chip (013) that draws up to 1,024 sprites of any size in 16-pixel steps
// into a buffer of its own once a frame, and up to four tile layer chips (038), each a 512 x 512 layer of 8 x 8 or
// 16 x 16 tiles with its own scroll, a scroll for each line and a choice of the layer's row for each line. Tiles and
// sprites each carry a priority from 0 to 3; a sprite goes below the tiles of its own priority.
//
// What is in which register and memory, and the order things are drawn in, are as MAME's cave_v.cpp, sprite013.cpp
// and tmap038.cpp describe them (BSD-3-Clause; Luca Elia, Paul Priest, David Haywood): see THIRD_PARTY_NOTICES.md.
#pragma once

#include <cstdint>
#include <vector>

namespace spelunk {

class Archive;

// A frame as a native renderer takes it (port/gpu68k.h), instead of the software model's picture: the sprites the chip
// drew at its latch, read from its list as the chip reads it (the list's order, its time limit, the screen's flips),
// and the layers' memory and registers and the palette as they are when the picture is made
struct NativeSprite {
    int32_t x, y, width, height;   // where its first pixel drawn lands (256ths of a pixel; flipped: its last), the size in pixels
    uint32_t code;                 // the first of its pixels in spritePixels (a multiple of 256)
    uint32_t attributes;           // its colour and priority as the sprite buffer holds them (bits 8-15), flips (16, 17)
    int32_t zoomX, zoomY;          // 256ths of a pixel from one of its pixels to the next (0x100: none)
};
struct NativeFrame68k {
    std::vector<NativeSprite> sprites;
    uint32_t spritesLatched = 0; // (one more each latch)
    std::vector<uint8_t> layerRam[4], palette;
    uint16_t layerRegs[4][3] = {}, spriteRegs[2] = {};
    bool tileBank = false;
    uint32_t pictures = 0;       // (one more each picture made)
};

struct CaveVideo {
    // (Mazinger Z's board shows 384 across: its game.cfg's `screen_width`, which the build takes)
#ifdef SPELUNK_SCREEN_WIDTH
    static constexpr int kWidth = SPELUNK_SCREEN_WIDTH, kHeight = 240;
#else
    static constexpr int kWidth = 320, kHeight = 240;
#endif
    static constexpr int kLayers = 4;

    // --- what the board is (set once, from the game's settings) ---
    int layers = 3;
    // The sprites' pixels, one a byte (unpacked from the ROMs when the set is loaded); the size is a power of two
    std::vector<uint8_t> spritePixels;
    bool spritesZoom = false;     // (the sprite list's other layout, with a zoom for each sprite)
    uint32_t spriteColourStep = 256; // (palette entries from one of a sprite's colours to the next)
    // Each layer's tiles, a byte a pixel, 64 bytes a tile; and how its colours sit in the palette
    struct LayerKind {
        std::vector<uint8_t> pixels;
        uint32_t colourBase = 0x4000, colourStep = 256;
        bool only8 = false; // (the board gives the layer only the memory of its 8 x 8 tiles)
        uint32_t colourMask = 0x3F; // (which of a tile's 6 colour bits count: 6-bit tiles have 16 colours of 64 entries)
        bool banked = false; // (its 8 x 8 tiles below 0x10000 are those 0x40000 further on while `tileBank` is set)
    };
    LayerKind kinds[kLayers];
    uint32_t backgroundPen = 0x7F00;
    // The screen shows the chips' picture from this pixel across (Air Gallet's board: 1)
    int left = 0;
    // The sprite chip shows the list the game chose a frame before (Mazinger Z's board)
    bool spritesDelay = false;

    // --- what the game writes ---
    std::vector<uint8_t> spriteRam;   // 64 KB: four lists, the registers say which is shown
    std::vector<uint8_t> layerRam[kLayers]; // 32 KB each: 16 x 16 tiles at 0, lines at 0x1000, 8 x 8 tiles at 0x4000
    std::vector<uint8_t> paletteRam;  // 64 KB: 32,768 colours, 5 bits each of green, red, blue
    uint16_t spriteRegs[0x40] = {};
    uint16_t layerRegs[kLayers][3] = {};
    bool tileBank = false;   // (Air Gallet's board: written with the EEPROM's lines)
    uint16_t listBefore = 0; // (which sprite list the registers named at the last frame's latch; kept by the board)

    CaveVideo();
    // The sprite chip draws the list on show into its buffer (at the vertical blank's interrupt)
    void LatchSprites();
    // The picture: 320 x 240, 32 bits a pixel, red in the low byte
    void Draw(uint32_t *out) const;
    void Serialize(Archive &ar);

    // What LatchSprites drew: for each pixel 0, or the sprite's pixel, colour and priority
    std::vector<uint16_t> spriteBuffer;

    // A native renderer's frame: set, and LatchSprites and Draw fill it as well; the software picture (spriteBuffer,
    // Draw's pixels) is made only while `rasterize` is set (the reference: --software, and --check-video beside the GPU)
    NativeFrame68k *native = nullptr;
    bool rasterize = true;

private:
    // Per-instance scratch keeps Draw reentrant across boards without requiring platform TLS destructors.
    mutable std::vector<uint32_t> paletteColours;

    uint16_t Word(const std::vector<uint8_t> &memory, uint32_t at) const {
        return static_cast<uint16_t>((memory[at] << 8) | memory[at + 1]);
    }
    void DrawSprite(uint32_t code, uint16_t colourPriority, int x, int y, bool flipX, bool flipY, int width, int height, int zoomX, int zoomY);
};

} // namespace spelunk
