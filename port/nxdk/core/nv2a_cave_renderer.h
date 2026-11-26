#pragma once

// Cache-oriented native Original Xbox NV2A hardware primitive renderer for Cave 68000 (DoDonPachi).
// Engineered for the Original Xbox Pentium III Coppermine-128 cache hierarchy (16 KB L1D, 128 KB L2, 32-byte lines):
// - Single-pass bucketed emission: scans frame.sprites and each active layerRam ONCE (75% less DRAM/L2 traffic).
// - Packed 8-byte tile cache entries (64 KB total, 4 slots per 32-byte cache line) with epoch invalidation.
// - O(1) 2,048-slot epoch-tagged open-addressed sprite hash table (16 bytes/slot, zero O(N^2) linear scans).
// - Zero per-vertex float divisions (precomputed affine NDC transform).
// - Hand-tuned Pentium III i686/MMX/SSE1 assembly kernels (xbox_asm.S) for palette hash/MMX LUT and branchless VRAM unpack.
#include "machine/cave_video.h"
#include "xbox_asm.h"

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
    // Each slot is a single packed uint64_t (8 bytes): bit 63 = empty, bits 62..0 = key -> 64 KB total (half of L2).
    static constexpr int kTileCols = kAtlasW / 8;            // 128
    static constexpr int kTileRows = 512 / 8;                // 64
    static constexpr int kTileSlots = kTileCols * kTileRows; // 8,192 (power of 2)
    static constexpr uint32_t kTileMask = kTileSlots - 1;
    static constexpr uint64_t kTileEmptyBit = 1ULL << 63;
    static constexpr uint64_t kTileKeyMask = ~kTileEmptyBit;

    // Bottom half (1024x512): per-frame shelf-allocated sprite atlas with O(1) 2,048-slot hash table (32 KB).
    static constexpr int kSpriteAreaY = 512;
    static constexpr uint32_t kSpriteHashSlots = 2048; // power of 2 (>= 2x max 1024 sprites/frame)
    static constexpr uint32_t kSpriteHashMask = kSpriteHashSlots - 1;
    static constexpr int kMaxQuadsPerBatch = 48;

    bool Init() {
        atlas_ = static_cast<uint32_t *>(
            MmAllocateContiguousMemoryEx(kAtlasBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (!atlas_) {
            return false;
        }
        std::memset(atlas_, 0, kAtlasBytes);
        atlasPhys_ = static_cast<uint32_t>(MmGetPhysicalAddress(atlas_));

        tileCache_.assign(kTileSlots, 0ULL);
        spriteHash_.assign(kSpriteHashSlots, {});
        // +256 mirrored tail allows bounds-safe unmasked palBase[px] indexing in assembly.
        paletteLut_.assign(0x8000 + 256, 0xFF000000u);

        // Pre-reserve all 8 priority buckets once so steady-state frames do zero heap allocations.
        for (int p = 0; p < 4; ++p) {
            spriteQuads_[p].reserve(256);
            tileQuads_[p].reserve(1024);
        }

        paletteEpoch_ = 1;
        spriteEpoch_ = 1;
        lastPaletteHash_ = 0;
        stateInitialized_ = false;
        filterInitialized_ = false;
        geomInitialized_ = false;
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
        spriteHash_.clear();
        paletteLut_.clear();
        for (int p = 0; p < 4; ++p) {
            spriteQuads_[p].clear();
            tileQuads_[p].clear();
        }
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
            stateInitialized_ = false;
        }

        // 1. Dirty-hash & MMX-convert 15-bit Cave palette only when changed.
        UpdatePalette(frame.palette);

        // 2. Clear full screen black only on layout change; fill game viewport with Cave background color.
        if (clearFramesRemaining_ > 0) {
            pb_fill(0, 0, static_cast<int>(screenW), static_cast<int>(screenH), 0xFF000000);
            --clearFramesRemaining_;
        }
        const uint32_t bgAbgr = paletteLut_[video.backgroundPen & 0x7FFF];
        const uint32_t bgArgb = (bgAbgr & 0xFF00FF00u) | ((bgAbgr & 0xFFu) << 16) | ((bgAbgr >> 16) & 0xFFu);
        pb_fill(clipX0_, clipY0_, clipX1_ - clipX0_, clipY1_ - clipY0_, bgArgb);

        // 3. Reset priority buckets & O(1) sprite cache epoch (zero memset, zero heap allocs).
        for (int p = 0; p < 4; ++p) {
            spriteQuads_[p].clear();
            tileQuads_[p].clear();
        }
        spriteShelfX_ = 0;
        spriteShelfY_ = kSpriteAreaY;
        spriteShelfRowH_ = 0;
        if (++spriteEpoch_ == 0) {
            spriteEpoch_ = 1;
            std::memset(spriteHash_.data(), 0, spriteHash_.size() * sizeof(SpriteHashSlot));
        }

        // 4. Single linear pass over frame.sprites -> distributes into spriteQuads_[0..3].
        EmitAllSpritesSinglePass(video, frame);

        // 5. Single linear pass over each active layer (ordered by layer.priority, layerIndex)
        //    -> distributes into tileQuads_[0..3] with 75% less layerRam traffic.
        struct LayerDesc {
            bool on = false, big = false, flipX = false, flipY = false;
            int scrollX = 0, scrollY = 0, priority = 0;
        } layers[spelunk::CaveVideo::kLayers];

        for (int n = 0; n < video.layers; ++n) {
            auto &l = layers[n];
            const uint16_t *regs = frame.layerRegs[n];
            l.on = !(regs[2] & 0x10) && !video.kinds[n].pixels.empty() && !frame.layerRam[n].empty();
            if (!l.on) {
                continue;
            }
            l.big = !video.kinds[n].only8 && (regs[1] & 0x2000);
            l.flipX = !(regs[0] & 0x8000);
            l.flipY = !(regs[1] & 0x8000);
            l.priority = regs[2] & 3;
            const int offsetX = 0x13 - (n + 1) - (l.big ? 0 : 8);
            const int offsetY = -0x12;
            l.scrollX = (regs[0] & 0x1FF) - frame.spriteRegs[0] + (l.flipX ? offsetX + 2 : -offsetX);
            l.scrollY = (regs[1] & 0x1FF) - frame.spriteRegs[1] + (l.flipY ? offsetY + 2 : -offsetY);
        }

        for (int lp = 0; lp < 4; ++lp) {
            for (int n = 0; n < video.layers; ++n) {
                if (layers[n].on && layers[n].priority == lp) {
                    EmitLayerTilesSinglePass(video, frame, n, layers[n].big, layers[n].flipX, layers[n].flipY,
                                             layers[n].scrollX, layers[n].scrollY);
                }
            }
        }

        // 6. Flush write-combined VRAM atlas writes before NV2A DMA read.
        _mm_sfence();

        // 7. Push invariant NV2A state once + filter on change.
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

        // 8. Submit priority-ordered NV2A QUADS (exact Cave order: for p=0..3, sprites[p] then tiles[p]).
        lastQuadCount_ = 0;
        for (int prio = 0; prio < 4; ++prio) {
            SubmitQuadBucket(spriteQuads_[prio]);
            SubmitQuadBucket(tileQuads_[prio]);
        }
    }

    size_t LastQuadCount() const { return lastQuadCount_; }

private:
    // Exactly 64 bytes = 2 Pentium III cache lines (no split-line loads during pushbuffer submission).
    struct alignas(32) Quad {
        uint32_t x[4], y[4], u[4], v[4];
    };

    // Exactly 16 bytes = 2 slots per 32-byte Pentium III cache line.
    struct SpriteHashSlot {
        uint64_t key = 0;
        uint16_t u0 = 0;
        uint16_t v0 = 0;
        uint16_t epoch = 0;
        uint8_t empty = 1;
        uint8_t pad = 0;
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
        const float vpX0 = (screenW - quadW) * 0.5f;
        const float vpY0 = (screenH - quadH) * 0.5f;

        clipX0_ = std::max(0, static_cast<int>(vpX0));
        clipY0_ = std::max(0, static_cast<int>(vpY0));
        clipX1_ = std::min(static_cast<int>(screenW), static_cast<int>(vpX0 + quadW + 0.5f));
        clipY1_ = std::min(static_cast<int>(screenH), static_cast<int>(vpY0 + quadH + 0.5f));

        // Precompute affine Arcade -> NDC transform (zero float divisions per vertex!).
        const float invScreenW2 = 2.0f / screenW;
        const float invScreenH2 = 2.0f / screenH;
        if (!tateMode) {
            // YOKO 90 deg CCW:
            // ndcX = (vpX0 + ay * (quadW / 240)) * (2 / screenW) - 1
            // ndcY = 1 - (vpY0 + (320 - ax) * (quadH / 320)) * (2 / screenH)
            ndcScaleX_ = (quadW / static_cast<float>(kSrcHeight)) * invScreenW2;
            ndcOffsetX_ = vpX0 * invScreenW2 - 1.0f;
            ndcScaleY_ = (quadH / static_cast<float>(kSrcWidth)) * invScreenH2;
            ndcOffsetY_ = 1.0f - (vpY0 + quadH) * invScreenH2;
        } else {
            // TATE native:
            // ndcX = (vpX0 + ax * (quadW / 320)) * (2 / screenW) - 1
            // ndcY = 1 - (vpY0 + ay * (quadH / 240)) * (2 / screenH)
            ndcScaleX_ = (quadW / static_cast<float>(kSrcWidth)) * invScreenW2;
            ndcOffsetX_ = vpX0 * invScreenW2 - 1.0f;
            ndcScaleY_ = -(quadH / static_cast<float>(kSrcHeight)) * invScreenH2;
            ndcOffsetY_ = 1.0f - vpY0 * invScreenH2;
        }

        cachedScreenW_ = screenW;
        cachedScreenH_ = screenH;
        cachedTate_ = tateMode;
        cachedInteger_ = integerScale;
        geomInitialized_ = true;
    }

    void UpdatePalette(const std::vector<uint8_t> &palRam) {
        const uint32_t hash = xbox_hash_palette_64k_asm(
            reinterpret_cast<const uint32_t *>(palRam.data()), 0x4000u);
        if (hash == lastPaletteHash_) {
            return;
        }
        lastPaletteHash_ = hash;
        paletteEpoch_ = (paletteEpoch_ + 1u) & 0x7FFFu; // 15-bit epoch fits in packed 64-bit tileCache_ key.
        if (paletteEpoch_ == 0) {
            paletteEpoch_ = 1;
            std::memset(tileCache_.data(), 0, tileCache_.size() * sizeof(uint64_t));
        }
        xbox_convert_palette_32k_mmx(reinterpret_cast<const uint16_t *>(palRam.data()), paletteLut_.data());
        std::memcpy(paletteLut_.data() + 0x8000, paletteLut_.data(), 256 * sizeof(uint32_t));
    }

    inline void PushArcadeQuad(std::vector<Quad> &bucket,
                               float ax0, float ay0, float ax1, float ay1,
                               float u0, float v0, float u1, float v1) const {
        if (ax1 <= 0.0f || ax0 >= static_cast<float>(kSrcWidth) ||
            ay1 <= 0.0f || ay0 >= static_cast<float>(kSrcHeight)) {
            return;
        }

        Quad q;
        const uint32_t fu0 = FloatBits(u0), fv0 = FloatBits(v0);
        const uint32_t fu1 = FloatBits(u1), fv1 = FloatBits(v1);

        if (!cachedTate_) {
            // YOKO: ndcX depends only on ay, ndcY depends only on ax -> only 4 float ops total!
            const uint32_t xA = FloatBits(ay0 * ndcScaleX_ + ndcOffsetX_);
            const uint32_t xB = FloatBits(ay1 * ndcScaleX_ + ndcOffsetX_);
            const uint32_t yA = FloatBits(ax0 * ndcScaleY_ + ndcOffsetY_);
            const uint32_t yB = FloatBits(ax1 * ndcScaleY_ + ndcOffsetY_);
            q.x[0] = xA; q.y[0] = yA; q.u[0] = fu0; q.v[0] = fv0;
            q.x[1] = xA; q.y[1] = yB; q.u[1] = fu1; q.v[1] = fv0;
            q.x[2] = xB; q.y[2] = yB; q.u[2] = fu1; q.v[2] = fv1;
            q.x[3] = xB; q.y[3] = yA; q.u[3] = fu0; q.v[3] = fv1;
        } else {
            const uint32_t xA = FloatBits(ax0 * ndcScaleX_ + ndcOffsetX_);
            const uint32_t xB = FloatBits(ax1 * ndcScaleX_ + ndcOffsetX_);
            const uint32_t yA = FloatBits(ay0 * ndcScaleY_ + ndcOffsetY_);
            const uint32_t yB = FloatBits(ay1 * ndcScaleY_ + ndcOffsetY_);
            q.x[0] = xA; q.y[0] = yA; q.u[0] = fu0; q.v[0] = fv0;
            q.x[1] = xB; q.y[1] = yA; q.u[1] = fu1; q.v[1] = fv0;
            q.x[2] = xB; q.y[2] = yB; q.u[2] = fu1; q.v[2] = fv1;
            q.x[3] = xA; q.y[3] = yB; q.u[3] = fu0; q.v[3] = fv1;
        }
        bucket.push_back(q);
    }

    void EmitAllSpritesSinglePass(const spelunk::CaveVideo &video, const spelunk::NativeFrame68k &frame) {
        if (video.spritePixels.empty()) {
            return;
        }
        const uint8_t *spData = video.spritePixels.data();
        const uint32_t spMask = static_cast<uint32_t>(video.spritePixels.size() - 1);
        const uint32_t *lutBase = paletteLut_.data();

        for (const auto &s : frame.sprites) {
            if (s.width <= 0 || s.height <= 0) {
                continue;
            }
            const float dx = static_cast<float>(s.width * s.zoomX) * (1.0f / 256.0f);
            const float dy = static_cast<float>(s.height * s.zoomY) * (1.0f / 256.0f);
            const float x0 = static_cast<float>(s.x) * (1.0f / 256.0f);
            const float y0 = static_cast<float>(s.y) * (1.0f / 256.0f);
            if (x0 + dx <= 0.0f || x0 >= static_cast<float>(kSrcWidth) ||
                y0 + dy <= 0.0f || y0 >= static_cast<float>(kSrcHeight)) {
                continue;
            }

            const int spPrio = static_cast<int>((s.attributes >> 14) & 3u);
            const bool flipX = (s.attributes & (1u << 16)) != 0;
            const bool flipY = (s.attributes & (1u << 17)) != 0;
            const uint32_t colourBank = (s.attributes >> 8) & 0x3Fu;
            const uint32_t palBase = (colourBank * video.spriteColourStep) & 0x7FFFu;

            const uint64_t key = (static_cast<uint64_t>(colourBank) << 48) |
                                 (static_cast<uint64_t>(s.width & 0x3FF) << 38) |
                                 (static_cast<uint64_t>(s.height & 0x3FF) << 28) |
                                 (s.code & 0x0FFFFFFFu);

            // O(1) open-addressed hash lookup (epoch-tagged, 2 slots per 32-byte cache line).
            uint32_t idx = static_cast<uint32_t>((key ^ (key >> 17) ^ (key >> 33)) & kSpriteHashMask);
            SpriteHashSlot *slot = nullptr;
            bool hit = false;
            for (uint32_t probe = 0; probe < 16; ++probe) {
                SpriteHashSlot &cand = spriteHash_[(idx + probe) & kSpriteHashMask];
                if (cand.epoch != spriteEpoch_) {
                    slot = &cand;
                    break;
                }
                if (cand.key == key) {
                    slot = &cand;
                    hit = true;
                    break;
                }
            }
            if (!slot) {
                continue;
            }

            if (!hit) {
                if (spriteShelfX_ + s.width > kAtlasW) {
                    spriteShelfX_ = 0;
                    spriteShelfY_ += spriteShelfRowH_;
                    spriteShelfRowH_ = 0;
                }
                if (spriteShelfY_ + s.height > kAtlasH) {
                    continue;
                }
                const int dstX = spriteShelfX_;
                const int dstY = spriteShelfY_;
                spriteShelfX_ += s.width;
                if (s.height > spriteShelfRowH_) {
                    spriteShelfRowH_ = s.height;
                }

                uint32_t anyOpaque = 0;
                const uint32_t *palPtr = lutBase + palBase;
                for (int sy = 0; sy < s.height; ++sy) {
                    uint32_t *dstRow = atlas_ + static_cast<size_t>(dstY + sy) * kAtlasW + dstX;
                    const uint32_t srcOff = s.code + static_cast<uint32_t>(sy * s.width);
                    anyOpaque |= xbox_unpack_sprite_row_asm(
                        spData, srcOff, spMask, static_cast<uint32_t>(s.width), dstRow, palPtr);
                }
                slot->key = key;
                slot->u0 = static_cast<uint16_t>(dstX);
                slot->v0 = static_cast<uint16_t>(dstY);
                slot->epoch = spriteEpoch_;
                slot->empty = (anyOpaque == 0) ? 1 : 0;
            }

            if (slot->empty) {
                continue;
            }
            const float u0 = flipX ? static_cast<float>(slot->u0 + s.width) : static_cast<float>(slot->u0);
            const float u1 = flipX ? static_cast<float>(slot->u0) : static_cast<float>(slot->u0 + s.width);
            const float v0 = flipY ? static_cast<float>(slot->v0 + s.height) : static_cast<float>(slot->v0);
            const float v1 = flipY ? static_cast<float>(slot->v0) : static_cast<float>(slot->v0 + s.height);
            PushArcadeQuad(spriteQuads_[spPrio], x0, y0, x0 + dx, y0 + dy, u0, v0, u1, v1);
        }
    }

    void EmitLayerTilesSinglePass(const spelunk::CaveVideo &video, const spelunk::NativeFrame68k &frame,
                                  int layerIdx, bool big, bool flipX, bool flipY, int scrollX, int scrollY) {
        const auto &kind = video.kinds[layerIdx];
        const auto &ram = frame.layerRam[layerIdx];
        const size_t tileCount = kind.pixels.size() / 64;
        if (tileCount == 0) {
            return;
        }
        const uint8_t *tilePixels = kind.pixels.data();
        const uint32_t *lutBase = paletteLut_.data();
        const uint64_t layerEpochPrefix = (static_cast<uint64_t>(paletteEpoch_) << 36) |
                                          (static_cast<uint64_t>(layerIdx) << 32);

        const int endTx = (kSrcWidth / 8) + 1;
        const int endTy = (kSrcHeight / 8) + 1;

        const int offX = (flipX ? (kSrcWidth - 1) : video.left) + scrollX;
        const int offY = (flipY ? (kSrcHeight - 1) : 0) + scrollY;
        const int subOffX = flipX ? (7 - (offX & 7)) : (offX & 7);
        const int subOffY = flipY ? (7 - (offY & 7)) : (offY & 7);

        for (int ty = 0; ty <= endTy; ++ty) {
            const int screenY = ty * 8 - subOffY;
            const int sampleY = std::clamp(screenY + 4, 0, kSrcHeight - 1);
            const int line = flipY ? (kSrcHeight - 1 - sampleY) : sampleY;
            const int py = (line + scrollY) & 0x1FF;
            const uint32_t rowBaseBig = static_cast<uint32_t>((py >> 4) * 32) * 4u;
            const uint32_t rowBaseSmall = 0x4000u + static_cast<uint32_t>((py >> 3) * 64) * 4u;
            const uint32_t subYBit = static_cast<uint32_t>(((py >> 3) & 1) * 2);

            for (int tx = 0; tx <= endTx; ++tx) {
                const int screenX = tx * 8 - subOffX;
                const int sampleX = std::clamp(screenX + 4, 0, kSrcWidth - 1);
                const int px = ((flipX ? (kSrcWidth - 1 - sampleX) : (sampleX + video.left)) + scrollX) & 0x1FF;

                uint32_t tileWord, code;
                if (big) {
                    const uint32_t at = rowBaseBig + static_cast<uint32_t>(px >> 4) * 4u;
                    tileWord = (static_cast<uint32_t>(ReadWordBE(ram, at)) << 16) | ReadWordBE(ram, at + 2);
                    code = ((tileWord & 0xFFFFu) * 4u) ^ static_cast<uint32_t>((px >> 3) & 1) ^ subYBit;
                } else {
                    const uint32_t at = rowBaseSmall + static_cast<uint32_t>(px >> 3) * 4u;
                    tileWord = (static_cast<uint32_t>(ReadWordBE(ram, at)) << 16) | ReadWordBE(ram, at + 2);
                    code = tileWord & 0x3FFFFu;
                    if (kind.banked && frame.tileBank && code < 0x10000u) {
                        code += 0x40000u;
                    }
                }

                const int tilePrio = static_cast<int>(tileWord >> 30);
                const uint32_t tileIdx = static_cast<uint32_t>(code % tileCount);
                const uint32_t colourIdx = (tileWord >> 24) & kind.colourMask;
                const uint64_t key = layerEpochPrefix | (static_cast<uint64_t>(colourIdx) << 20) | tileIdx;
                const uint32_t slotIdx = static_cast<uint32_t>((key ^ (key >> 13) ^ (key >> 27)) & kTileMask);
                uint64_t entry = tileCache_[slotIdx];

                const int slotX = static_cast<int>(slotIdx & (kTileCols - 1)) * 8;
                const int slotY = static_cast<int>(slotIdx >> 7) * 8;

                if ((entry & kTileKeyMask) != key) {
                    const uint32_t palBase = (kind.colourBase + colourIdx * kind.colourStep) & 0x7FFFu;
                    const uint8_t *srcTile = tilePixels + static_cast<size_t>(tileIdx) * 64;
                    uint32_t *dstTile = atlas_ + static_cast<size_t>(slotY) * kAtlasW + slotX;
                    const uint32_t anyOpaque = xbox_unpack_tile8x8_asm(
                        srcTile, dstTile, static_cast<uint32_t>(kAtlasW * sizeof(uint32_t)), lutBase + palBase);
                    entry = key | (anyOpaque == 0 ? kTileEmptyBit : 0ULL);
                    tileCache_[slotIdx] = entry;
                }

                if ((entry & kTileEmptyBit) != 0) {
                    continue;
                }

                const float u0 = flipX ? static_cast<float>(slotX + 8) : static_cast<float>(slotX);
                const float u1 = flipX ? static_cast<float>(slotX) : static_cast<float>(slotX + 8);
                const float v0 = flipY ? static_cast<float>(slotY + 8) : static_cast<float>(slotY);
                const float v1 = flipY ? static_cast<float>(slotY) : static_cast<float>(slotY + 8);
                PushArcadeQuad(tileQuads_[tilePrio],
                               static_cast<float>(screenX), static_cast<float>(screenY),
                               static_cast<float>(screenX + 8), static_cast<float>(screenY + 8),
                               u0, v0, u1, v1);
            }
        }
    }

    void SubmitQuadBucket(const std::vector<Quad> &bucket) {
        const size_t total = bucket.size();
        lastQuadCount_ += total;
        for (size_t base = 0; base < total; base += kMaxQuadsPerBatch) {
            const size_t count = std::min<size_t>(kMaxQuadsPerBatch, total - base);
            uint32_t *p = pb_begin();
            p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
            const Quad *qPtr = bucket.data() + base;
            for (size_t i = 0; i < count; ++i) {
                const Quad &q = qPtr[i];
                for (int v = 0; v < 4; ++v) {
                    p = pb_push2(p, NV097_SET_VERTEX_DATA2F_M + 9 * 8, q.u[v], q.v[v]);
                    p = pb_push4(p, NV097_SET_VERTEX4F, q.x[v], q.y[v], kFloatZeroBits, kFloatOneBits);
                }
            }
            p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
            pb_end(p);
        }
    }

    uint32_t *PushStaticState(uint32_t *p, float screenW, float screenH) const {
        const uint32_t texFmt =
            1u | (1u << 3) | (2u << 4) |
            (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 << 8) | (1u << 16);
        const uint32_t texAddr = 3u | (3u << 8) | (3u << 16);

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

        p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 1);
        p = pb_push1(p, NV097_SET_ALPHA_FUNC, NV097_SET_ALPHA_FUNC_V_GREATER);
        p = pb_push1(p, NV097_SET_ALPHA_REF, 0);

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

        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000008u);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001800u);
        return p;
    }

    uint32_t *atlas_ = nullptr;
    uint32_t atlasPhys_ = 0;

    std::vector<uint64_t> tileCache_;
    std::vector<SpriteHashSlot> spriteHash_;
    std::vector<uint32_t> paletteLut_;
    std::vector<Quad> spriteQuads_[4];
    std::vector<Quad> tileQuads_[4];

    uint32_t paletteEpoch_ = 1;
    uint16_t spriteEpoch_ = 1;
    uint32_t lastPaletteHash_ = 0;
    size_t lastQuadCount_ = 0;
    int spriteShelfX_ = 0;
    int spriteShelfY_ = kSpriteAreaY;
    int spriteShelfRowH_ = 0;

    float ndcScaleX_ = 0.0f, ndcOffsetX_ = 0.0f, ndcScaleY_ = 0.0f, ndcOffsetY_ = 0.0f;
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
