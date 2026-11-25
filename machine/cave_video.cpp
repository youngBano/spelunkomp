// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// What is in which register and memory, and the order things are drawn in, are as MAME's cave_v.cpp, sprite013.cpp
// and tmap038.cpp describe them (BSD-3-Clause; Luca Elia, Paul Priest, David Haywood).
#include "cave_video.h"

#include "../runtime/state.h"

#include <algorithm>
#include <cstring>

namespace spelunk {

namespace {
    constexpr int kBufferSize = 512; // (the sprite chip's buffer: 512 x 512)
    // The sprite chip's time for a frame's sprites: 32 clocks a sprite and one a pixel of its size (more on a board
    // that shows more than 360 across)
    constexpr uint32_t kSpriteClocks = (CaveVideo::kWidth > 360 ? 512 : 448) * 272 * 2;
    int Signed10(int v) {
        v &= 0x3FF;
        return v & 0x200 ? v - 0x400 : v;
    }
} // namespace

CaveVideo::CaveVideo()
    : spriteRam(0x10000, 0)
    , paletteRam(0x10000, 0)
    , spriteBuffer(kBufferSize * kBufferSize, 0)
    , paletteColours(0x8000) {
    for (auto &ram : layerRam) {
        ram.assign(0x8000, 0);
    }
}

void CaveVideo::DrawSprite(uint32_t code, uint16_t colourPriority, int x, int y, bool flipX, bool flipY, int width, int height, int zoomX, int zoomY) {
    if (spritePixels.empty()) {
        return;
    }
    const uint32_t mask = static_cast<uint32_t>(spritePixels.size() - 1);
    // (positions in 256ths of a pixel, x and y the first drawn pixel's: a zoom below 0x100 leaves source pixels out,
    // one above leaves gaps; each pixel lands on the screen pixel its position falls in.) The source is written in its
    // own order, row by row from the first, as the sprite chip writes it (SpriteBlitter.sv: flipping moves where a
    // pixel lands, not when it is written), so where a zoom below 0x100 puts two of its pixels on one screen pixel the
    // later of them stays, flipped or not
    code <<= 8;
    for (int sy = 0; sy < height; sy++) {
        const int py = (y + (flipY ? height - 1 - sy : sy) * zoomY) >> 8;
        if (py < 0 || py >= kHeight) {
            continue;
        }
        uint16_t *row = &spriteBuffer[static_cast<size_t>(py) * kBufferSize];
        const uint32_t from = code + static_cast<uint32_t>(sy * width);
        for (int sx = 0; sx < width; sx++) {
            const int px = (x + (flipX ? width - 1 - sx : sx) * zoomX) >> 8;
            if (px < 0 || px >= kWidth) {
                continue;
            }
            const uint8_t pixel = spritePixels[(from + static_cast<uint32_t>(sx)) & mask];
            if (pixel) {
                row[px] = pixel | colourPriority;
            }
        }
    }
}

void CaveVideo::LatchSprites() {
    if (rasterize) {
        for (int y = 0; y < kHeight; y++) {
            std::memset(&spriteBuffer[static_cast<size_t>(y) * kBufferSize], 0, kWidth * sizeof(uint16_t));
        }
    }
    if (native) {
        native->sprites.clear();
        native->spritesLatched++;
    }
    // (each sprite the chip draws: into the software buffer, and handed to the native renderer; the screen shows the
    // chip's picture from `left` across)
    // (x and y in 256ths of a pixel: where the first pixel drawn lands, the last of its row or column if flipped)
    auto sprite = [this](uint32_t code, uint16_t colourPriority, int x, int y, bool flipX, bool flipY, int width, int height, int zoomX, int zoomY) {
        x -= left << 8;
        if (native) {
            native->sprites.push_back({x, y, width, height, code << 8,
                                       static_cast<uint32_t>(colourPriority) | (flipX ? 1u << 16 : 0) | (flipY ? 1u << 17 : 0), zoomX, zoomY});
        }
        if (rasterize) {
            DrawSprite(code, colourPriority, x, y, flipX, flipY, width, height, zoomX, zoomY);
        }
    };
    const bool allFlipX = spriteRegs[0] & 0x8000, allFlipY = spriteRegs[1] & 0x8000;
    const uint32_t list = ((spritesDelay ? listBefore : spriteRegs[4]) & 3u) * 0x4000;
    const int across = kWidth + left; // (the chip's picture: the screen's, and what is left of it)
    uint32_t clocks = 0;
    for (uint32_t at = list; at < list + 0x4000; at += 16) {
        clocks += 32;
        if (clocks > kSpriteClocks) {
            break;
        }
        int x, y, zoomX = 0x100, zoomY = 0x100, width, height;
        uint16_t attributes;
        uint32_t code;
        if (!spritesZoom) {
            attributes = Word(spriteRam, at);
            code = Word(spriteRam, at + 2) + ((attributes & 3u) << 16);
            const uint16_t size = Word(spriteRam, at + 8);
            width = ((size >> 8) & 0x1F) * 16;
            height = (size & 0x1F) * 16;
            x = Signed10(Word(spriteRam, at + 4));
            y = Signed10(Word(spriteRam, at + 6));
            if (!width || !height || x + width <= 0 || x >= across || y + height <= 0 || y >= kHeight) {
                continue;
            }
            clocks += static_cast<uint32_t>(width * height);
            if (clocks > kSpriteClocks) {
                break;
            }
        } else {
            const bool whole = (spriteRegs[5] & 0x3000) == 0; // (positions in whole pixels, or with 6 bits of fraction)
            x = whole ? (Word(spriteRam, at) & 0x3FF) << 8 : Word(spriteRam, at) << 2;
            y = whole ? (Word(spriteRam, at + 2) & 0x3FF) << 8 : Word(spriteRam, at + 2) << 2;
            attributes = Word(spriteRam, at + 4);
            code = Word(spriteRam, at + 6) + ((attributes & 3u) << 16);
            zoomX = Word(spriteRam, at + 8);
            zoomY = Word(spriteRam, at + 10);
            const uint16_t size = Word(spriteRam, at + 12);
            width = ((size >> 8) & 0x1F) * 16;
            height = (size & 0x1F) * 16;
            if (!width || !height) {
                continue;
            }
            clocks += static_cast<uint32_t>(width * height);
            if (clocks > kSpriteClocks) {
                break;
            }
            // As the sprite chip places them (Arcade-Cave_MiSTer's SpriteBlitter.sv, made from a board): the position
            // keeps its fraction (18 bits, signed, 8 of them below the pixel), source pixel n lands at the position
            // plus n x the zoom, or flipped at the position plus the width less one pixel less n x the zoom, and on
            // the screen pixel that falls in. (MAME's cave.cpp drops the fraction first and moves flipped and
            // one-pixel sprites half a pixel: often a pixel off.) Here the start is the first pixel drawn: source
            // pixel width - 1 if flipped.
            const auto start = [](int at, bool flip, int size, int zoom) {
                at = (at & 0x20000) ? at - 0x40000 : at;
                return flip ? at + (size << 8) - 0x100 - (size - 1) * zoom : at;
            };
            const bool fx = attributes & 8, fy = attributes & 4;
            x = start(x & 0x3FFFF, fx, width, zoomX);
            y = start(y & 0x3FFFF, fy, height, zoomY);
            // (the screen pixels it reaches)
            const int x0 = std::min(x, x + (width - 1) * zoomX) >> 8, x1 = std::max(x, x + (width - 1) * zoomX) >> 8;
            const int y0 = std::min(y, y + (height - 1) * zoomY) >> 8, y1 = std::max(y, y + (height - 1) * zoomY) >> 8;
            if (x1 < 0 || x0 >= across || y1 < 0 || y0 >= kHeight) {
                continue;
            }
            // (the screen turned round: pixel p lands on across - 1 - p, so the first drawn is the last was)
            if (allFlipX) x = (across << 8) - 1 - x - (width - 1) * zoomX;
            if (allFlipY) y = (kHeight << 8) - 1 - y - (height - 1) * zoomY;
            const uint16_t colourPriority = static_cast<uint16_t>(((attributes & 0x30) << 10) | (attributes & 0x3F00));
            sprite(code, colourPriority, x, y, fx != allFlipX, fy != allFlipY, width, height, zoomX, zoomY);
            continue;
        }
        bool flipX = attributes & 8, flipY = attributes & 4;
        if (allFlipX) {
            x = across - x - width;
            flipX = !flipX;
        }
        if (allFlipY) {
            y = kHeight - y - height;
            flipY = !flipY;
        }
        const uint16_t colourPriority = static_cast<uint16_t>(((attributes & 0x30) << 10) | (attributes & 0x3F00));
        sprite(code, colourPriority, x << 8, y << 8, flipX, flipY, width, height, zoomX, zoomY);
    }
}

void CaveVideo::Draw(uint32_t *out) const {
    if (native) {
        for (int n = 0; n < kLayers; n++) {
            native->layerRam[n] = layerRam[n];
        }
        native->palette = paletteRam;
        std::memcpy(native->layerRegs, layerRegs, sizeof layerRegs);
        native->spriteRegs[0] = spriteRegs[0];
        native->spriteRegs[1] = spriteRegs[1];
        native->tileBank = tileBank;
        native->pictures++;
    }
    if (!rasterize) {
        return;
    }
    // The palette as it is now
    auto &colours = paletteColours;
    for (uint32_t n = 0; n < 0x8000; n++) {
        const uint16_t c = Word(paletteRam, n * 2);
        const uint32_t g = (c >> 10) & 31, r = (c >> 5) & 31, b = c & 31;
        colours[n] = 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
    }
    struct Layer {
        bool on = false, big = false, flipX = false, flipY = false, rowScroll = false, rowSelect = false;
        int scrollX = 0, scrollY = 0, priority = 0;
        const std::vector<uint8_t> *ram = nullptr;
        const LayerKind *kind = nullptr;
    };
    Layer set[kLayers];
    for (int n = 0; n < layers; n++) {
        Layer &l = set[n];
        const uint16_t *regs = layerRegs[n];
        l.kind = &kinds[n];
        l.ram = &layerRam[n];
        l.on = !(regs[2] & 0x10) && !l.kind->pixels.empty();
        l.big = !l.kind->only8 && (regs[1] & 0x2000);
        l.flipX = !(regs[0] & 0x8000);
        l.flipY = !(regs[1] & 0x8000);
        l.rowScroll = !l.kind->only8 && (regs[0] & 0x4000);
        l.rowSelect = !l.kind->only8 && (regs[1] & 0x4000);
        l.priority = regs[2] & 3;
        // (each layer sits a pixel to the side of the one before, and 8 x 8 tiles 8 more)
        const int offsetX = 0x13 - (n + 1) - (l.big ? 0 : 8), offsetY = -0x12;
        l.scrollX = (regs[0] & 0x1FF) - spriteRegs[0] + (l.flipX ? offsetX + 2 : -offsetX);
        l.scrollY = (regs[1] & 0x1FF) - spriteRegs[1] + (l.flipY ? offsetY + 2 : -offsetY);
    }
    const uint32_t background = colours[backgroundPen & 0x7FFF];
    for (int y = 0; y < kHeight; y++) {
        // Each layer's row of the tile map for this line, and its scroll
        int mapY[kLayers], lineX[kLayers];
        for (int n = 0; n < layers; n++) {
            const Layer &l = set[n];
            if (!l.on) {
                continue;
            }
            // (a layer turned over is the same layer seen from the screen's other end)
            const int line = l.flipY ? kHeight - 1 - y : y;
            const int row = (l.scrollY + (l.flipY ? 1 : -1) + line) & 0x1FF;
            mapY[n] = l.rowSelect ? Word(*l.ram, 0x1000 + row * 4 + 2) & 0x1FF : (line + l.scrollY) & 0x1FF;
            lineX[n] = l.scrollX + (l.rowScroll ? Word(*l.ram, 0x1000 + row * 4) : 0);
        }
        const uint16_t *sprites = &spriteBuffer[static_cast<size_t>(y) * kBufferSize];
        for (int x = 0; x < kWidth; x++) {
            // The frontmost tile pixel: by the tile's priority, then its layer's, then the layer's number
            int best = -1;
            uint32_t colour = background;
            for (int n = 0; n < layers; n++) {
                const Layer &l = set[n];
                if (!l.on) {
                    continue;
                }
                const int px = ((l.flipX ? kWidth - 1 - x : x + left) + lineX[n]) & 0x1FF, py = mapY[n];
                uint32_t tile, code;
                if (l.big) {
                    const uint32_t at = static_cast<uint32_t>((py >> 4) * 32 + (px >> 4)) * 4;
                    tile = (static_cast<uint32_t>(Word(*l.ram, at)) << 16) | Word(*l.ram, at + 2);
                    code = ((tile & 0xFFFF) * 4) ^ ((px >> 3) & 1) ^ (((py >> 3) & 1) * 2);
                } else {
                    const uint32_t at = 0x4000 + static_cast<uint32_t>((py >> 3) * 64 + (px >> 3)) * 4;
                    tile = (static_cast<uint32_t>(Word(*l.ram, at)) << 16) | Word(*l.ram, at + 2);
                    code = tile & 0x3FFFF;
                    if (l.kind->banked && tileBank && code < 0x10000) {
                        code += 0x40000;
                    }
                }
                const size_t count = l.kind->pixels.size() / 64;
                const uint8_t pixel = l.kind->pixels[(code % count) * 64 + static_cast<size_t>(py & 7) * 8 + (px & 7)];
                if (!pixel) {
                    continue;
                }
                const int rank = static_cast<int>((tile >> 30) << 4) | (l.priority << 2) | n;
                if (rank > best) {
                    best = rank;
                    colour = colours[(l.kind->colourBase + ((tile >> 24) & l.kind->colourMask) * l.kind->colourStep + pixel) & 0x7FFF];
                }
            }
            if (const uint16_t sprite = sprites[x]) {
                const int spritePriority = sprite >> 14, tilePriority = best < 0 ? 0 : (best >> 4) + 1;
                if (tilePriority <= spritePriority) {
                    colour = colours[(((sprite >> 8) & 0x3F) * spriteColourStep + (sprite & 0xFF)) & 0x7FFF];
                }
            }
            out[y * kWidth + x] = colour;
        }
    }
}

void CaveVideo::Serialize(Archive &ar) {
    ar.Mark(0x43564944);
    ar.Vec(spriteRam);
    for (auto &ram : layerRam) {
        ar.Vec(ram);
    }
    ar.Vec(paletteRam);
    ar.Pod(spriteRegs);
    ar.Pod(layerRegs);
}

} // namespace spelunk
