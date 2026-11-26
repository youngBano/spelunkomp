#pragma once

// Native Original Xbox NV2A hardware primitive renderer for Cave 68000 (DoDonPachi).
// Replaces CPU software CaveVideo::Draw() and desktop SDL_GPU/gpu68k.cpp with direct
// NV097 hardware textured quads, hardware alpha-test transparency, hardware YOKO/TATE
// rotation & scaling, hardware scissor clipping, and a contiguous VRAM tile/sprite atlas.
#include "machine/cave_video.h"

#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <xmmintrin.h>

namespace spelunk_xbox {

class Nv2aCaveRenderer {
public:
    static constexpr int kSrcWidth = spelunk::CaveVideo::kWidth;   // 320
    static constexpr int kSrcHeight = spelunk::CaveVideo::kHeight; // 240
    static constexpr int kAtlasW = 1024;
    static constexpr int kAtlasH = 1024;
    static constexpr size_t kAtlasBytes = static_cast<size_t>(kAtlasW) * kAtlasH * sizeof(uint32_t); // 4 MB

    // Top half (1024x512): 128x64 = 8,192 cached 8x8 tile slots.
    static constexpr int kTileCols = kAtlasW / 8; // 128
    static constexpr int kTileRows = 512 / 8;     // 64
    static constexpr int kTileSlots = kTileCols * kTileRows; // 8192 (power of 2)
    static constexpr int kTileMask = kTileSlots - 1;

    // Bottom half (1024x512): per-frame shelf-allocated sprite atlas (y = 512..1023).
    static constexpr int kSpriteAreaY = 512;
    static constexpr int kMaxQuadsPerBatch = 48; // Safe well within pbkit single-block limit.

    bool Init() {
        atlas_ = static_cast<uint32_t *>(
            MmAllocateContiguousMemoryEx(kAtlasBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (!atlas_) {
            return false;
        }
        std::memset(atlas_, 0, kAtlasBytes);
        atlasPhys_ = static_cast<uint32_t>(MmGetPhysicalAddress(atlas_));

        tileCache_.assign(kTileSlots, {});
        paletteLut_.assign(0x8000, 0xFF000000u);
        paletteEpoch_ = 1;
        lastPaletteHash_ = 0;
        stateInitialized_ = false;
        filterInitialized_ = false;
        clearFramesRemaining_ = 3;
        return true;
    }

    void Shutdown() {
        if (atlas_) {
            MmFreeContiguousMemory(atlas_);
            atlas_ = nullptr;
            atlasPhys_ = 0;
        }
        tileCache_.clear();
        paletteLut_.clear();
        stateInitialized_ = false;
    }

    void Render(const spelunk::CaveVideo &video, const spelunk::NativeFrame68k &frame,
                bool tateMode, bool bilinear, bool integerScale) {
        if (!atlas_ || frame.palette.size() < 0x10000) {
            return;
        }

        const float screenW = static_cast<float>(pb_back_buffer_width());
        const float screenH = static_cast<float>(pb_back_buffer_height());

        if (!geomInitialized_ || tateMode != cachedTate_ || integerScale != cachedInteger_ ||
            screenW != cachedScreenW_ || screenH != cachedScreenH_) {
            ComputeViewport(screenW, screenH, tateMode, integerScale);
            clearFramesRemaining_ = 3;
            stateInitialized_ = false; // Re-apply scissor rect for new viewport.
        }

        // 1. Update 15-bit Cave palette -> 32-bit A8B8G8R8 LUT (detect changes to invalidate tile cache).
        UpdatePalette(frame.palette);

        // 2. Clear full screen black when layout changes; clear game viewport with Cave background color.
        if (clearFramesRemaining_ > 0) {
            pb_fill(0, 0, static_cast<int>(screenW), static_cast<int>(screenH), 0xFF000000);
            --clearFramesRemaining_;
        }
        const uint32_t bgAbgr = paletteLut_[video.backgroundPen & 0x7FFF];
        const uint32_t bgArgb = (bgAbgr & 0xFF00FF00u) | ((bgAbgr & 0xFFu) << 16) | ((bgAbgr >> 16) & 0xFFu);
        pb_fill(clipX0_, clipY0_, clipX1_ - clipX0_, clipY1_ - clipY0_, bgArgb);

        // 3. Build priority-sorted NV2A hardware quad list (0..3 back-to-front) and populate VRAM atlas.
        quads_.clear();
        spriteShelfX_ = 0;
        spriteShelfY_ = kSpriteAreaY;
        spriteShelfRowH_ = 0;
        spriteCache_.clear();

        // Prepare layer descriptors matching CaveVideo::Draw().
        struct LayerDesc {
            bool on = false, big = false, flipX = false, flipY = false, rowScroll = false, rowSelect = false;
            int scrollX = 0, scrollY = 0, priority = 0;
            const std::vector<uint8_t> *ram = nullptr;
            const spelunk::CaveVideo::LayerKind *kind = nullptr;
        } layers[spelunk::CaveVideo::kLayers];

        for (int n = 0; n < video.layers; ++n) {
            auto &l = layers[n];
            const uint16_t *regs = frame.layerRegs[n];
            l.kind = &video.kinds[n];
            l.ram = &frame.layerRam[n];
            l.on = !(regs[2] & 0x10) && !l.kind->pixels.empty() && !l.ram->empty();
            l.big = !l.kind->only8 && (regs[1] & 0x2000);
            l.flipX = !(regs[0] & 0x8000);
            l.flipY = !(regs[1] & 0x8000);
            l.rowScroll = !l.kind->only8 && (regs[0] & 0x4000);
            l.rowSelect = !l.kind->only8 && (regs[1] & 0x4000);
            l.priority = regs[2] & 3;
            const int offsetX = 0x13 - (n + 1) - (l.big ? 0 : 8);
            const int offsetY = -0x12;
            l.scrollX = (regs[0] & 0x1FF) - frame.spriteRegs[0] + (l.flipX ? offsetX + 2 : -offsetX);
            l.scrollY = (regs[1] & 0x1FF) - frame.spriteRegs[1] + (l.flipY ? offsetY + 2 : -offsetY);
        }

        // Exact Cave 68000 priority order: for p = 0..3, sprites of priority p then tiles of priority p.
        for (int prio = 0; prio < 4; ++prio) {
            EmitSpritesForPriority(video, frame, prio);
            // Order layers by (layer.priority, layerIndex)
            for (int lp = 0; lp < 4; ++lp) {
                for (int n = 0; n < video.layers; ++n) {
                    if (layers[n].on && layers[n].priority == lp) {
                        EmitLayerTilesForPriority(video, frame, n, layers[n].big, layers[n].flipX, layers[n].flipY,
                                                  layers[n].scrollX, layers[n].scrollY, prio);
                    }
                }
            }
        }

        // 4. Flush all write-combined VRAM atlas writes before NV2A DMA reads.
        _mm_sfence();

        // 5. Push static NV2A state (once) + filter (on change) + batched hardware QUADS.
        uint32_t *p = pb_begin();
        if (!stateInitialized_) {
            p = PushStaticState(p, screenW, screenH);
            stateInitialized_ = true;
            filterInitialized_ = false;
        }
        if (!filterInitialized_ || bilinear != cachedBilinear_) {
            const uint32_t filterMode = bilinear ? 2u : 1u;
            p = pb_push1(p, NV097_SET_TEXTURE_FILTER, (filterMode << 16) | (filterMode << 24));
            cachedBilinear_ = bilinear;
            filterInitialized_ = true;
        }
        pb_end(p);

        const size_t total = quads_.size();
        for (size_t base = 0; base < total; base += kMaxQuadsPerBatch) {
            const size_t count = std::min<size_t>(kMaxQuadsPerBatch, total - base);
            p = pb_begin();
            p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
            for (size_t i = 0; i < count; ++i) {
                const Quad &q = quads_[base + i];
                for (int v = 0; v < 4; ++v) {
                    p = pb_push2(p, NV097_SET_VERTEX_DATA2F_M + 9 * 8, q.u[v], q.v[v]);
                    p = pb_push4(p, NV097_SET_VERTEX4F, q.x[v], q.y[v], kFloatZeroBits, kFloatOneBits);
                }
            }
            p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
            pb_end(p);
        }
    }

    size_t LastQuadCount() const { return quads_.size(); }

private:
    struct Quad {
        uint32_t x[4], y[4], u[4], v[4];
    };

    struct TileSlot {
        uint64_t key = 0;
        bool empty = true;
    };

    struct SpriteEntry {
        uint64_t key = 0;
        uint16_t u0 = 0, v0 = 0;
        bool empty = true;
    };

    static constexpr uint32_t kFloatZeroBits = 0x00000000u;
    static constexpr uint32_t kFloatOneBits = 0x3F800000u;

    static inline uint32_t FloatBits(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

    static inline uint16_t ReadWordBE(const std::vector<uint8_t> &mem, uint32_t at) {
        return static_cast<uint16_t>((mem[at] << 8) | mem[at + 1]);
    }

    void ComputeViewport(float screenW, float screenH, bool tateMode, bool integerScale) {
        float quadW = 0.0f, quadH = 0.0f;
        if (!tateMode) {
            if (integerScale) {
                quadW = 240.0f;
                quadH = 320.0f;
            } else {
                quadH = screenH;
                quadW = screenH * (240.0f / 320.0f);
            }
        } else {
            if (integerScale) {
                const int scale = (static_cast<int>(screenW) >= 640 && static_cast<int>(screenH) >= 480) ? 2 : 1;
                quadW = static_cast<float>(kSrcWidth * scale);
                quadH = static_cast<float>(kSrcHeight * scale);
            } else {
                quadW = screenW;
                quadH = screenH;
            }
        }
        vpX0_ = (screenW - quadW) * 0.5f;
        vpY0_ = (screenH - quadH) * 0.5f;
        vpW_ = quadW;
        vpH_ = quadH;
        clipX0_ = std::max(0, static_cast<int>(vpX0_));
        clipY0_ = std::max(0, static_cast<int>(vpY0_));
        clipX1_ = std::min(static_cast<int>(screenW), static_cast<int>(vpX0_ + vpW_ + 0.5f));
        clipY1_ = std::min(static_cast<int>(screenH), static_cast<int>(vpY0_ + vpH_ + 0.5f));
        cachedScreenW_ = screenW;
        cachedScreenH_ = screenH;
        cachedTate_ = tateMode;
        cachedInteger_ = integerScale;
        geomInitialized_ = true;
    }

    void UpdatePalette(const std::vector<uint8_t> &palRam) {
        // Fast 32-bit FNV-1a over 64 KB palette RAM to skip LUT rebuild & tile cache invalidation when unchanged.
        uint32_t hash = 2166136261u;
        const uint32_t *words = reinterpret_cast<const uint32_t *>(palRam.data());
        for (size_t i = 0; i < 0x4000; ++i) {
            hash ^= words[i];
            hash *= 16777619u;
        }
        if (hash == lastPaletteHash_) {
            return;
        }
        lastPaletteHash_ = hash;
        ++paletteEpoch_;
        if (paletteEpoch_ == 0) {
            paletteEpoch_ = 1;
            for (auto &s : tileCache_) {
                s.key = 0;
            }
        }
        for (uint32_t n = 0; n < 0x8000; ++n) {
            const uint16_t c = ReadWordBE(palRam, n * 2);
            const uint32_t g = (c >> 10) & 31u;
            const uint32_t r = (c >> 5) & 31u;
            const uint32_t b = c & 31u;
            paletteLut_[n] = 0xFF000000u | (((b << 3) | (b >> 2)) << 16) |
                             (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
        }
    }

    // Push a hardware quad for arcade rectangle [ax0..ax1, ay0..ay1] sampling atlas [u0..u1, v0..v1].
    void PushArcadeQuad(float ax0, float ay0, float ax1, float ay1, float u0, float v0, float u1, float v1) {
        if (ax1 <= 0.0f || ax0 >= static_cast<float>(kSrcWidth) ||
            ay1 <= 0.0f || ay0 >= static_cast<float>(kSrcHeight)) {
            return;
        }
        auto mapPt = [&](float ax, float ay, uint32_t &outX, uint32_t &outY) {
            float sx, sy;
            if (!cachedTate_) {
                // 90 degrees CCW YOKO rotation
                sx = vpX0_ + (ay * (1.0f / static_cast<float>(kSrcHeight))) * vpW_;
                sy = vpY0_ + ((static_cast<float>(kSrcWidth) - ax) * (1.0f / static_cast<float>(kSrcWidth))) * vpH_;
            } else {
                sx = vpX0_ + (ax * (1.0f / static_cast<float>(kSrcWidth))) * vpW_;
                sy = vpY0_ + (ay * (1.0f / static_cast<float>(kSrcHeight))) * vpH_;
            }
            outX = FloatBits((sx / cachedScreenW_) * 2.0f - 1.0f);
            outY = FloatBits(1.0f - (sy / cachedScreenH_) * 2.0f);
        };

        Quad q;
        const uint32_t fu0 = FloatBits(u0), fv0 = FloatBits(v0);
        const uint32_t fu1 = FloatBits(u1), fv1 = FloatBits(v1);
        mapPt(ax0, ay0, q.x[0], q.y[0]); q.u[0] = fu0; q.v[0] = fv0;
        mapPt(ax1, ay0, q.x[1], q.y[1]); q.u[1] = fu1; q.v[1] = fv0;
        mapPt(ax1, ay1, q.x[2], q.y[2]); q.u[2] = fu1; q.v[2] = fv1;
        mapPt(ax0, ay1, q.x[3], q.y[3]); q.u[3] = fu0; q.v[3] = fv1;
        quads_.push_back(q);
    }

    void EmitSpritesForPriority(const spelunk::CaveVideo &video, const spelunk::NativeFrame68k &frame, int prio) {
        if (video.spritePixels.empty()) {
            return;
        }
        const uint32_t spMask = static_cast<uint32_t>(video.spritePixels.size() - 1);
        for (const auto &s : frame.sprites) {
            const int spPrio = static_cast<int>((s.attributes >> 14) & 3u);
            if (spPrio != prio || s.width <= 0 || s.height <= 0) {
                continue;
            }
            const bool flipX = (s.attributes & (1u << 16)) != 0;
            const bool flipY = (s.attributes & (1u << 17)) != 0;
            const uint32_t colourBank = (s.attributes >> 8) & 0x3Fu;
            const uint32_t palBase = (colourBank * video.spriteColourStep) & 0x7FFFu;

            // Compute screen-space bounding box in arcade pixels (matches CaveVideo::DrawSprite).
            const float dx = static_cast<float>(s.width * s.zoomX) * (1.0f / 256.0f);
            const float dy = static_cast<float>(s.height * s.zoomY) * (1.0f / 256.0f);
            const float x0 = static_cast<float>(s.x) * (1.0f / 256.0f);
            const float y0 = static_cast<float>(s.y) * (1.0f / 256.0f);
            if (x0 + dx <= 0.0f || x0 >= static_cast<float>(kSrcWidth) ||
                y0 + dy <= 0.0f || y0 >= static_cast<float>(kSrcHeight)) {
                continue;
            }

            // Look up or unpack sprite into per-frame shelf atlas (y = 512..1023).
            const uint64_t key = (static_cast<uint64_t>(colourBank) << 48) |
                                 (static_cast<uint64_t>(s.width & 0x3FF) << 38) |
                                 (static_cast<uint64_t>(s.height & 0x3FF) << 28) |
                                 (s.code & 0x0FFFFFFFu);
            const SpriteEntry *entry = nullptr;
            for (const auto &e : spriteCache_) {
                if (e.key == key) {
                    entry = &e;
                    break;
                }
            }
            if (!entry) {
                if (spriteShelfX_ + s.width > kAtlasW) {
                    spriteShelfX_ = 0;
                    spriteShelfY_ += spriteShelfRowH_;
                    spriteShelfRowH_ = 0;
                }
                if (spriteShelfY_ + s.height > kAtlasH) {
                    continue; // Atlas full for this frame.
                }
                const int dstX = spriteShelfX_;
                const int dstY = spriteShelfY_;
                spriteShelfX_ += s.width;
                if (s.height > spriteShelfRowH_) {
                    spriteShelfRowH_ = s.height;
                }

                bool anyOpaque = false;
                for (int sy = 0; sy < s.height; ++sy) {
                    uint32_t *dstRow = atlas_ + static_cast<size_t>(dstY + sy) * kAtlasW + dstX;
                    const uint32_t srcOff = s.code + static_cast<uint32_t>(sy * s.width);
                    for (int sx = 0; sx < s.width; ++sx) {
                        const uint8_t px = video.spritePixels[(srcOff + static_cast<uint32_t>(sx)) & spMask];
                        if (px) {
                            dstRow[sx] = paletteLut_[(palBase + px) & 0x7FFFu];
                            anyOpaque = true;
                        } else {
                            dstRow[sx] = 0x00000000u; // Alpha = 0 -> hardware alpha-test discard
                        }
                    }
                }
                spriteCache_.push_back({key, static_cast<uint16_t>(dstX), static_cast<uint16_t>(dstY), !anyOpaque});
                entry = &spriteCache_.back();
            }

            if (entry->empty) {
                continue;
            }
            const float u0 = flipX ? static_cast<float>(entry->u0 + s.width) : static_cast<float>(entry->u0);
            const float u1 = flipX ? static_cast<float>(entry->u0) : static_cast<float>(entry->u0 + s.width);
            const float v0 = flipY ? static_cast<float>(entry->v0 + s.height) : static_cast<float>(entry->v0);
            const float v1 = flipY ? static_cast<float>(entry->v0) : static_cast<float>(entry->v0 + s.height);
            PushArcadeQuad(x0, y0, x0 + dx, y0 + dy, u0, v0, u1, v1);
        }
    }

    void EmitLayerTilesForPriority(const spelunk::CaveVideo &video, const spelunk::NativeFrame68k &frame,
                                   int layerIdx, bool big, bool flipX, bool flipY, int scrollX, int scrollY, int prio) {
        const auto &kind = video.kinds[layerIdx];
        const auto &ram = frame.layerRam[layerIdx];
        const size_t tileCount = kind.pixels.size() / 64;
        if (tileCount == 0) {
            return;
        }

        // Emit visible 8x8 tiles (16x16 big tiles are decomposed into their 4 constituent 8x8 sub-tiles).
        const int startTx = 0;
        const int endTx = (kSrcWidth / 8) + 1;
        const int startTy = 0;
        const int endTy = (kSrcHeight / 8) + 1;

        const int offX = (flipX ? (kSrcWidth - 1) : video.left) + scrollX;
        const int offY = (flipY ? (kSrcHeight - 1) : 0) + scrollY;
        const int subOffX = flipX ? (7 - (offX & 7)) : (offX & 7);
        const int subOffY = flipY ? (7 - (offY & 7)) : (offY & 7);

        for (int ty = startTy; ty <= endTy; ++ty) {
            const int screenY = ty * 8 - subOffY;
            const int sampleY = std::clamp(screenY + 4, 0, kSrcHeight - 1);
            const int line = flipY ? (kSrcHeight - 1 - sampleY) : sampleY;
            const int py = (line + scrollY) & 0x1FF;

            for (int tx = startTx; tx <= endTx; ++tx) {
                const int screenX = tx * 8 - subOffX;
                const int sampleX = std::clamp(screenX + 4, 0, kSrcWidth - 1);
                const int px = ((flipX ? (kSrcWidth - 1 - sampleX) : (sampleX + video.left)) + scrollX) & 0x1FF;

                uint32_t tileWord, code;
                if (big) {
                    const uint32_t at = static_cast<uint32_t>((py >> 4) * 32 + (px >> 4)) * 4;
                    tileWord = (static_cast<uint32_t>(ReadWordBE(ram, at)) << 16) | ReadWordBE(ram, at + 2);
                    code = ((tileWord & 0xFFFFu) * 4u) ^ static_cast<uint32_t>((px >> 3) & 1) ^
                           static_cast<uint32_t>(((py >> 3) & 1) * 2);
                } else {
                    const uint32_t at = 0x4000u + static_cast<uint32_t>((py >> 3) * 64 + (px >> 3)) * 4;
                    tileWord = (static_cast<uint32_t>(ReadWordBE(ram, at)) << 16) | ReadWordBE(ram, at + 2);
                    code = tileWord & 0x3FFFFu;
                    if (kind.banked && frame.tileBank && code < 0x10000u) {
                        code += 0x40000u;
                    }
                }

                const int tilePrio = static_cast<int>(tileWord >> 30);
                if (tilePrio != prio) {
                    continue;
                }

                const uint32_t tileIdx = static_cast<uint32_t>(code % tileCount);
                const uint32_t colourIdx = (tileWord >> 24) & kind.colourMask;
                const uint64_t key = (static_cast<uint64_t>(paletteEpoch_) << 36) |
                                     (static_cast<uint64_t>(layerIdx) << 32) |
                                     (static_cast<uint64_t>(colourIdx) << 20) |
                                     tileIdx;
                const uint32_t slotIdx = static_cast<uint32_t>((key ^ (key >> 13) ^ (key >> 27)) & kTileMask);
                TileSlot &slot = tileCache_[slotIdx];
                const int slotX = static_cast<int>(slotIdx % kTileCols) * 8;
                const int slotY = static_cast<int>(slotIdx / kTileCols) * 8;

                if (slot.key != key) {
                    const uint32_t palBase = (kind.colourBase + colourIdx * kind.colourStep) & 0x7FFFu;
                    const uint8_t *srcTile = &kind.pixels[static_cast<size_t>(tileIdx) * 64];
                    bool anyOpaque = false;
                    for (int row = 0; row < 8; ++row) {
                        uint32_t *dstRow = atlas_ + static_cast<size_t>(slotY + row) * kAtlasW + slotX;
                        const uint8_t *srcRow = srcTile + row * 8;
                        for (int col = 0; col < 8; ++col) {
                            const uint8_t pxVal = srcRow[col];
                            if (pxVal) {
                                dstRow[col] = paletteLut_[(palBase + pxVal) & 0x7FFFu];
                                anyOpaque = true;
                            } else {
                                dstRow[col] = 0x00000000u;
                            }
                        }
                    }
                    slot.key = key;
                    slot.empty = !anyOpaque;
                }

                if (slot.empty) {
                    continue;
                }

                const float u0 = flipX ? static_cast<float>(slotX + 8) : static_cast<float>(slotX);
                const float u1 = flipX ? static_cast<float>(slotX) : static_cast<float>(slotX + 8);
                const float v0 = flipY ? static_cast<float>(slotY + 8) : static_cast<float>(slotY);
                const float v1 = flipY ? static_cast<float>(slotY) : static_cast<float>(slotY + 8);
                PushArcadeQuad(static_cast<float>(screenX), static_cast<float>(screenY),
                               static_cast<float>(screenX + 8), static_cast<float>(screenY + 8),
                               u0, v0, u1, v1);
            }
        }
    }

    uint32_t *PushStaticState(uint32_t *p, float screenW, float screenH) const {
        const uint32_t texFmt =
            1u |                                                      // DMA context A
            (1u << 3) |                                               // Border source color
            (2u << 4) |                                               // 2D texture
            (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 << 8) | // Direct ABGR8888
            (1u << 16);                                               // 1 mipmap level
        const uint32_t texAddr = 3u | (3u << 8) | (3u << 16);         // Clamp to edge U/V/P

        const float kComposite[16] = {
            screenW * 0.5f, 0.0f,            0.0f, 0.0f,
            0.0f,           -screenH * 0.5f, 0.0f, 0.0f,
            0.0f,           0.0f,            1.0f, 0.0f,
            0.0f,           0.0f,            0.0f, 1.0f
        };

        p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
        p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);

        // Hardware alpha-test: discard transparent pixel-0 texels (alpha == 0).
        p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 1);
        p = pb_push1(p, NV097_SET_ALPHA_FUNC, 0x204); // GREATER
        p = pb_push1(p, NV097_SET_ALPHA_REF, 0);

        // Hardware scissor clip to exact game viewport rect so overhanging tiles/sprites stay inside pillarboxes.
        p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_MODE, 0);
        const uint32_t clipH = static_cast<uint32_t>(clipX0_) | (static_cast<uint32_t>(clipX1_ - 1) << 16);
        const uint32_t clipV = static_cast<uint32_t>(clipY0_) | (static_cast<uint32_t>(clipY1_ - 1) << 16);
        for (int i = 0; i < 8; ++i) {
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_HORIZ(i), clipH);
            p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_VERT(i), clipV);
        }

        p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE, 0);
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = pb_push1(p, NV097_SET_TEXTURE_MATRIX_ENABLE, 0);
        p = pb_push1(p, NV097_SET_COLOR_MASK, 0x01010101u);

        pb_push(p, NV097_SET_COMPOSITE_MATRIX, 16);
        p++;
        std::memcpy(p, kComposite, sizeof(kComposite));
        p += 16;

        p = pb_push4(p, NV097_SET_VIEWPORT_OFFSET,
                     FloatBits(screenW * 0.5f), FloatBits(screenH * 0.5f),
                     kFloatZeroBits, kFloatZeroBits);
        p = pb_push4(p, NV097_SET_VIEWPORT_SCALE,
                     FloatBits(screenW * 0.5f), FloatBits(-screenH * 0.5f),
                     kFloatOneBits, kFloatOneBits);

        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, atlasPhys_);
        p = pb_push1(p, NV097_SET_TEXTURE_FORMAT, texFmt);
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, texAddr);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE | (0xFFFu << 6));
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1, (kAtlasW * 4u) << 16);
        p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kAtlasW << 16) | kAtlasH);
        for (uint32_t stage = 1; stage < 4; ++stage) {
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + stage * 0x40u, 0);
        }

        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE);
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);

        // Final combiner: RGB = TEX0.rgb (D=0x8), Alpha = TEX0.a (G_ALPHA=1, G_SOURCE=0x8 -> 0x1800) for alpha-test!
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000008u);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001800u);
        return p;
    }

    uint32_t *atlas_ = nullptr;
    uint32_t atlasPhys_ = 0;

    std::vector<TileSlot> tileCache_;
    std::vector<uint32_t> paletteLut_;
    std::vector<SpriteEntry> spriteCache_;
    std::vector<Quad> quads_;

    uint32_t paletteEpoch_ = 1;
    uint32_t lastPaletteHash_ = 0;
    int spriteShelfX_ = 0;
    int spriteShelfY_ = kSpriteAreaY;
    int spriteShelfRowH_ = 0;

    float vpX0_ = 0.0f, vpY0_ = 0.0f, vpW_ = 360.0f, vpH_ = 480.0f;
    int clipX0_ = 140, clipY0_ = 0, clipX1_ = 500, clipY1_ = 480;
    float cachedScreenW_ = 0.0f, cachedScreenH_ = 0.0f;
    bool stateInitialized_ = false;
    bool geomInitialized_ = false;
    bool filterInitialized_ = false;
    bool cachedTate_ = false;
    bool cachedBilinear_ = false;
    bool cachedInteger_ = false;
    int clearFramesRemaining_ = 3;
};

} // namespace spelunk_xbox
