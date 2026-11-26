#pragma once

// Native Original Xbox NV2A hardware primitive renderer for Cave CV1000 (Mushihimesama).
// Engineered for 64 MB retail Original Xbox RAM + Pentium III Coppermine-128 + NV2A GPU:
// - Zero 64 MB software VRAM: uses a sparse on-demand 128x128 A1R5G5B5 page pool (capped at 18 MB).
// - Native NV2A A1R5G5B5 textures (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5): zero 16->32-bit conversion!
// - 4-row cycling texture offset (8 KB guard) guarantees NV2A/Xemu texture cache freshness every frame.
// - Decoupled 2,048-slot Mix64 hash table + shelf atlas (2 MB) for O(1) sprite/rect dedup per frame.
// - Bit-exact screen-space quad coordinates (kComposite = Identity, 0.001 texel UV inset).
// - Hardware alpha-test (A1 > 0), hardware diffuse tint modulation, and hardware alpha blending.
#include "machine/blitter.h"
#include "xbox_asm.h"

#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include <xmmintrin.h>

namespace spelunk_xbox {

class Nv2aCv1kRenderer {
public:
    static constexpr int kSrcWidth = spelunk::Blitter::kScreenWidth;   // 320
    static constexpr int kSrcHeight = spelunk::Blitter::kScreenHeight; // 240
    static constexpr int kVramW = spelunk::Blitter::kVramWidth;        // 8192
    static constexpr int kVramH = spelunk::Blitter::kVramHeight;       // 4096

    // Sparse on-demand 128x128 A1R5G5B5 VRAM pages (32 KB each).
    static constexpr int kPageShift = 7;
    static constexpr int kPageSize = 1 << kPageShift; // 128
    static constexpr int kPageMask = kPageSize - 1;
    static constexpr int kPagesX = kVramW / kPageSize; // 64
    static constexpr int kPagesY = kVramH / kPageSize; // 32
    static constexpr int kTotalLogicalPages = kPagesX * kPagesY; // 2,048
    static constexpr int kMaxAllocPages = 576; // 576 * 32 KB = 18 MB hard RAM cap (safe on 64 MB Xbox)

    // 1024x(1024+4) A1R5G5B5 dynamic per-frame NV2A atlas (2 MB + 8 KB).
    static constexpr int kAtlasW = 1024;
    static constexpr int kAtlasH = 1024;
    static constexpr int kAtlasGuardRows = 4;
    static constexpr int kAtlasAllocH = kAtlasH + kAtlasGuardRows;
    static constexpr size_t kAtlasBytes = static_cast<size_t>(kAtlasW) * kAtlasAllocH * sizeof(uint16_t);
    static constexpr uint32_t kRectHashSlots = 4096;
    static constexpr uint32_t kRectHashMask = kRectHashSlots - 1;

    bool Init() {
        atlasAlloc_ = static_cast<uint16_t *>(
            MmAllocateContiguousMemoryEx(kAtlasBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (!atlasAlloc_) {
            return false;
        }
        std::memset(atlasAlloc_, 0, kAtlasBytes);
        atlas_ = atlasAlloc_ + static_cast<size_t>(kAtlasW) * kAtlasGuardRows;
        atlasPhysBase_ = static_cast<uint32_t>(MmGetPhysicalAddress(atlasAlloc_));
        atlasPhase_ = 0;

        pageTable_.assign(kTotalLogicalPages, -1);
        pagePtrs_.assign(kTotalLogicalPages, nullptr);
        pageLru_.reserve(kMaxAllocPages);
        pageOwner_.reserve(kMaxAllocPages);
        pageData_.reserve(kMaxAllocPages);
        rectHash_.assign(kRectHashSlots, {});
        cmds_.reserve(2048);

        lruClock_ = 0;
        rectEpoch_ = 1;
        stateInitialized_ = false;
        filterInitialized_ = false;
        geomInitialized_ = false;
        clearFramesRemaining_ = 3;
        return true;
    }

    void Shutdown() {
        if (atlasAlloc_) {
            MmFreeContiguousMemory(atlasAlloc_);
            atlasAlloc_ = nullptr;
            atlas_ = nullptr;
            atlasPhysBase_ = 0;
        }
        pageData_.clear();
        pageTable_.clear();
        pagePtrs_.clear();
        pageLru_.clear();
        pageOwner_.clear();
        rectHash_.clear();
        cmds_.clear();
        stateInitialized_ = false;
    }

    // Consume Blitter::NativeList steps (uploads + draws) and submit hardware NV2A quads.
    void Render(const spelunk::Blitter &blitter, spelunk::NativeList &list,
                bool tateMode, bool bilinear, bool integerScale) {
        if (!atlas_) {
            list.Clear();
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

        // CV1000 double-buffers in VRAM: draws target the active clip window (clipX, clipY)
        // while the display scans out the previous buffer at (scrollX, scrollY).
        const int scrX = static_cast<int>(blitter.clipX & 0x1FFFu);
        const int scrY = static_cast<int>(blitter.clipY & 0x0FFFu);

        // 1. Cycle 4-row texture offset & reset per-frame shelf atlas.
        atlasPhase_ = (atlasPhase_ + 1u) & 3u;
        atlasVBias_ = static_cast<float>(kAtlasGuardRows - static_cast<int>(atlasPhase_));
        shelfX_ = 0;
        shelfY_ = 0;
        shelfRowH_ = 0;
        if (++rectEpoch_ == 0) {
            rectEpoch_ = 1;
            std::memset(rectHash_.data(), 0, rectHash_.size() * sizeof(RectHashSlot));
        }
        cmds_.clear();
        lastDrawCount_ = list.draws.size();

        // 2. Process all blitter steps in exact game order.
        for (const auto &step : list.steps) {
            if (step.upload) {
                ApplyUpload(list.uploads[step.first], list.pixels);
                // Any upload may mutate source VRAM regions; bump rect dedup epoch safely.
                if (++rectEpoch_ == 0) {
                    rectEpoch_ = 1;
                    std::memset(rectHash_.data(), 0, rectHash_.size() * sizeof(RectHashSlot));
                }
            } else {
                for (uint32_t i = 0; i < step.count; ++i) {
                    ProcessDraw(list.draws[step.first + i], scrX, scrY);
                }
            }
        }
        list.Clear();

        // 3. Flush write-combined A1R5G5B5 atlas writes before NV2A DMA.
        _mm_sfence();

        // 4. Clear letterbox on layout change, then clear game viewport black.
        if (clearFramesRemaining_ > 0) {
            pb_fill(0, 0, static_cast<int>(screenW), static_cast<int>(screenH), 0xFF000000);
            --clearFramesRemaining_;
        }
        pb_fill(clipX0_, clipY0_, clipX1_ - clipX0_, clipY1_ - clipY0_, 0xFF000000);

        // 5. Push invariant NV2A state once + cycle 4-row texture offset every frame.
        const uint32_t activeTexPhys =
            atlasPhysBase_ + atlasPhase_ * static_cast<uint32_t>(kAtlasW * sizeof(uint16_t));
        const uint32_t activeTexH = static_cast<uint32_t>(kAtlasAllocH) - atlasPhase_;
        uint32_t *p = pb_begin();
        if (!stateInitialized_) {
            p = PushStaticState(p, screenW, screenH, activeTexPhys, activeTexH);
            stateInitialized_ = true;
            filterInitialized_ = false;
        } else {
            p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, activeTexPhys);
            p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kAtlasW << 16) | activeTexH);
        }
        if (!filterInitialized_ || bilinear != cachedBilinear_) {
            const uint32_t filterMode = bilinear ? 2u : 1u;
            p = pb_push1(p, NV097_SET_TEXTURE_FILTER, (filterMode << 16) | (filterMode << 24));
            cachedBilinear_ = bilinear;
            filterInitialized_ = true;
        }
        pb_end(p);

        // 6. Submit hardware NV2A quads with state coalescing (alpha-test, blend, tint).
        SubmitDraws();
    }

    size_t LastQuadCount() const { return cmds_.size(); }
    size_t LastDrawCount() const { return lastDrawCount_; }
    uint32_t ActiveVramPages() const { return static_cast<uint32_t>(pageData_.size()); }

private:
    struct alignas(32) DrawCmd {
        uint32_t x[4], y[4], u[4], v[4];
        uint32_t diffuse;
        uint32_t blendColor;
        uint16_t sFactor;
        uint16_t dFactor;
        uint8_t blendOn;
        uint8_t alphaTest;
        uint8_t pad[2];
    };

    struct RectHashSlot {
        uint64_t key = 0;
        uint16_t u0 = 0;
        uint16_t v0 = 0;
        uint16_t epoch = 0;
        uint16_t pad = 0;
    };

    static constexpr uint32_t kFloatZeroBits = 0x00000000u;
    static constexpr uint32_t kFloatOneBits = 0x3F800000u;

    static inline uint32_t FloatBits(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

    static inline uint32_t Mix64(uint64_t k) {
        uint32_t h = static_cast<uint32_t>(k) ^ static_cast<uint32_t>(k >> 32);
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        return h ^ (h >> 16);
    }

    // Division-free 5/6-bit CV1000 channel -> 8-bit scale (x * 255 / 31, clamped at 255).
    static inline uint32_t Scale5To8(uint32_t v) {
        if (v >= 31u) return 255u;
        return (v << 3) | (v >> 2);
    }

    uint16_t *GetPageWrite(int px, int py) {
        const int logical = py * kPagesX + px;
        if (uint16_t *ptr = pagePtrs_[logical]) {
            return ptr;
        }
        if (static_cast<int>(pageData_.size()) < kMaxAllocPages) {
            const int phys = static_cast<int>(pageData_.size());
            pageData_.emplace_back(kPageSize * kPageSize, 0u);
            pageLru_.push_back(++lruClock_);
            pageOwner_.push_back(logical);
            pageTable_[logical] = phys;
            pagePtrs_[logical] = pageData_[phys].data();
            return pagePtrs_[logical];
        }
        // Evict least-recently-used VRAM page (keeps RAM strictly <= 18 MB on 64 MB Xbox).
        int victim = 0;
        uint32_t oldest = UINT32_MAX;
        for (int i = 0; i < kMaxAllocPages; ++i) {
            if (pageLru_[i] < oldest) {
                oldest = pageLru_[i];
                victim = i;
            }
        }
        const int oldLogical = pageOwner_[victim];
        pageTable_[oldLogical] = -1;
        pagePtrs_[oldLogical] = nullptr;
        pageOwner_[victim] = logical;
        pageLru_[victim] = ++lruClock_;
        pageTable_[logical] = victim;
        uint16_t *dst = pageData_[victim].data();
        pagePtrs_[logical] = dst;
        std::memset(dst, 0, kPageSize * kPageSize * sizeof(uint16_t));
        return dst;
    }

    inline const uint16_t *GetPageRead(int px, int py) const {
        return pagePtrs_[py * kPagesX + px];
    }

    void ApplyUpload(const spelunk::NativeUpload &up, const std::vector<uint16_t> &pixels) {
        if (up.width <= 0 || up.height <= 0 || up.pixels >= pixels.size()) {
            return;
        }
        const uint16_t *srcBase = pixels.data() + up.pixels;
        for (int row = 0; row < up.height; ++row) {
            const int vy = (up.y + row) & (kVramH - 1);
            const int py = vy >> kPageShift;
            const int ly = vy & kPageMask;
            const uint16_t *srcRow = srcBase + static_cast<size_t>(row) * up.width;
            int col = 0;
            while (col < up.width) {
                const int vx = (up.x + col) & (kVramW - 1);
                const int px = vx >> kPageShift;
                const int lx = vx & kPageMask;
                const int chunk = std::min(up.width - col, kPageSize - lx);
                uint16_t *page = GetPageWrite(px, py);
                std::memcpy(page + static_cast<size_t>(ly) * kPageSize + lx, srcRow + col,
                            static_cast<size_t>(chunk) * sizeof(uint16_t));
                col += chunk;
            }
        }
    }

    void CopyVramRectToAtlas(int srcX0, int srcY0, int w, int h, int dstX, int dstY) {
        for (int row = 0; row < h; ++row) {
            const int vy = (srcY0 + row) & (kVramH - 1);
            const int py = vy >> kPageShift;
            const int ly = vy & kPageMask;
            uint16_t *dstRow = atlas_ + static_cast<size_t>(dstY + row) * kAtlasW + dstX;
            int col = 0;
            while (col < w) {
                const int vx = (srcX0 + col) & (kVramW - 1);
                const int px = vx >> kPageShift;
                const int lx = vx & kPageMask;
                const int chunk = std::min(w - col, kPageSize - lx);
                const uint16_t *page = GetPageRead(px, py);
                if (page) {
                    xbox_copy_u16_row_asm(page + static_cast<size_t>(ly) * kPageSize + lx,
                                          dstRow + col, static_cast<uint32_t>(chunk));
                } else {
                    std::memset(dstRow + col, 0, static_cast<size_t>(chunk) * sizeof(uint16_t));
                }
                col += chunk;
            }
        }
    }

    void ProcessDraw(const spelunk::NativeDraw &d, int scrX, int scrY) {
        const int w = d.x1 - d.x0 + 1;
        const int h = d.y1 - d.y0 + 1;
        if (w <= 0 || h <= 0) {
            return;
        }

        // Convert VRAM destination rect to screen-relative coordinates (320x240 window at scrollX, scrollY).
        const int dx0 = d.x0 - scrX;
        const int dy0 = d.y0 - scrY;
        const int dx1 = dx0 + w;
        const int dy1 = dy0 + h;
        if (dx1 <= 0 || dx0 >= kSrcWidth || dy1 <= 0 || dy0 >= kSrcHeight) {
            return;
        }

        const bool flipX = (d.flags & spelunk::NativeDraw::kFlipX) != 0;
        const bool flipY = (d.flags & spelunk::NativeDraw::kFlipY) != 0;
        const int srcX0 = flipX ? (d.sx - d.x1) : (d.sx + d.x0);
        const int srcY0 = flipY ? (d.sy - d.y1) : (d.sy + d.y0);

        if (w > kAtlasW || h > kAtlasH) {
            return;
        }

        // O(1) per-frame rect dedup in 2 MB A1R5G5B5 atlas.
        const uint64_t key = (static_cast<uint64_t>(w & 0x3FF) << 44) |
                             (static_cast<uint64_t>(h & 0x3FF) << 34) |
                             (static_cast<uint64_t>(srcX0 & 0x1FFF) << 16) |
                             static_cast<uint64_t>(srcY0 & 0x0FFF);
        const uint32_t hBase = Mix64(key) & kRectHashMask;
        RectHashSlot *slot = nullptr;
        bool hit = false;
        for (uint32_t pr = 0; pr < 64; ++pr) {
            RectHashSlot &cand = rectHash_[(hBase + pr) & kRectHashMask];
            if (cand.epoch != rectEpoch_) {
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
            return;
        }
        if (!hit) {
            if (shelfX_ + w > kAtlasW) {
                shelfX_ = 0;
                shelfY_ += shelfRowH_;
                shelfRowH_ = 0;
            }
            if (shelfY_ + h > kAtlasH) {
                return;
            }
            const int dstX = shelfX_;
            const int dstY = shelfY_;
            shelfX_ += w;
            if (h > shelfRowH_) {
                shelfRowH_ = h;
            }
            CopyVramRectToAtlas(srcX0, srcY0, w, h, dstX, dstY);
            slot->key = key;
            slot->u0 = static_cast<uint16_t>(dstX);
            slot->v0 = static_cast<uint16_t>(dstY);
            slot->epoch = rectEpoch_;
        }

        // Compute tint & blend state (division-free Scale5To8).
        uint32_t diffuse = 0xFFFFFFFFu;
        if (d.flags & spelunk::NativeDraw::kTinted) {
            const uint32_t tr = Scale5To8((d.colours >> 10) & 0x3Fu);
            const uint32_t tg = Scale5To8((d.colours >> 16) & 0x3Fu);
            const uint32_t tb = Scale5To8((d.colours >> 22) & 0x3Fu);
            diffuse = 0xFF000000u | (tb << 16) | (tg << 8) | tr;
        }

        const bool blendOn = (d.flags & 64) == 0;
        uint16_t sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ONE;
        uint16_t dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ZERO;
        uint32_t blendColor = 0xFFFFFFFFu;
        if (blendOn) {
            const uint32_t sa = Scale5To8(d.colours & 0x1Fu);
            const uint32_t da = Scale5To8((d.colours >> 5) & 0x1Fu);
            const int sm = d.flags & 7;
            const int dm = (d.flags >> 3) & 7;
            blendColor = (sa << 24) | (da << 16) | (da << 8) | da;
            switch (sm) {
                case 0: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_CONSTANT_ALPHA; break;
                case 1: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_COLOR; break;
                case 2: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_DST_COLOR; break;
                case 4: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_CONSTANT_ALPHA; break;
                case 5: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_SRC_COLOR; break;
                case 6: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ONE_MINUS_DST_COLOR; break;
                default: sFactor = NV097_SET_BLEND_FUNC_SFACTOR_V_ONE; break;
            }
            switch (dm) {
                case 0: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_CONSTANT_COLOR; break;
                case 1: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_SRC_COLOR; break;
                case 2: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_DST_COLOR; break;
                case 4: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_CONSTANT_COLOR; break;
                case 5: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_COLOR; break;
                case 6: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_DST_COLOR; break;
                default: dFactor = NV097_SET_BLEND_FUNC_DFACTOR_V_ONE; break;
            }
        }

        // Emit screen-space quad with 0.001-texel UV inset.
        constexpr float kUvEps = 0.001f;
        const float uMin = static_cast<float>(slot->u0) + kUvEps;
        const float uMax = static_cast<float>(slot->u0 + w) - kUvEps;
        const float vMin = static_cast<float>(slot->v0) + atlasVBias_ + kUvEps;
        const float vMax = static_cast<float>(slot->v0 + h) + atlasVBias_ - kUvEps;
        const uint32_t fu0 = FloatBits(flipX ? uMax : uMin);
        const uint32_t fu1 = FloatBits(flipX ? uMin : uMax);
        const uint32_t fv0 = FloatBits(flipY ? vMax : vMin);
        const uint32_t fv1 = FloatBits(flipY ? vMin : vMax);

        DrawCmd cmd{};
        cmd.diffuse = diffuse;
        cmd.blendColor = blendColor;
        cmd.sFactor = sFactor;
        cmd.dFactor = dFactor;
        cmd.blendOn = blendOn ? 1u : 0u;
        cmd.alphaTest = (d.flags & spelunk::NativeDraw::kTransparent) ? 1u : 0u;

        const float ax0 = static_cast<float>(dx0), ay0 = static_cast<float>(dy0);
        const float ax1 = static_cast<float>(dx1), ay1 = static_cast<float>(dy1);
        if (!cachedTate_) {
            const uint32_t xA = FloatBits(ay0 * scrScaleX_ + scrOffsetX_);
            const uint32_t xB = FloatBits(ay1 * scrScaleX_ + scrOffsetX_);
            const uint32_t yA = FloatBits(ax0 * scrScaleY_ + scrOffsetY_);
            const uint32_t yB = FloatBits(ax1 * scrScaleY_ + scrOffsetY_);
            cmd.x[0] = xA; cmd.y[0] = yA; cmd.u[0] = fu0; cmd.v[0] = fv0;
            cmd.x[1] = xA; cmd.y[1] = yB; cmd.u[1] = fu1; cmd.v[1] = fv0;
            cmd.x[2] = xB; cmd.y[2] = yB; cmd.u[2] = fu1; cmd.v[2] = fv1;
            cmd.x[3] = xB; cmd.y[3] = yA; cmd.u[3] = fu0; cmd.v[3] = fv1;
        } else {
            const uint32_t xA = FloatBits(ax0 * scrScaleX_ + scrOffsetX_);
            const uint32_t xB = FloatBits(ax1 * scrScaleX_ + scrOffsetX_);
            const uint32_t yA = FloatBits(ay0 * scrScaleY_ + scrOffsetY_);
            const uint32_t yB = FloatBits(ay1 * scrScaleY_ + scrOffsetY_);
            cmd.x[0] = xA; cmd.y[0] = yA; cmd.u[0] = fu0; cmd.v[0] = fv0;
            cmd.x[1] = xB; cmd.y[1] = yA; cmd.u[1] = fu1; cmd.v[1] = fv0;
            cmd.x[2] = xB; cmd.y[2] = yB; cmd.u[2] = fu1; cmd.v[2] = fv1;
            cmd.x[3] = xA; cmd.y[3] = yB; cmd.u[3] = fu0; cmd.v[3] = fv1;
        }
        cmds_.push_back(cmd);
    }

    void SubmitDraws() {
        if (cmds_.empty()) {
            return;
        }
        uint8_t curAlpha = 0xFF, curBlend = 0xFF;
        uint16_t curSF = 0xFFFF, curDF = 0xFFFF;
        uint32_t curBlendCol = 0, curDiff = 0;

        size_t i = 0;
        const size_t total = cmds_.size();
        while (i < total) {
            uint32_t *p = pb_begin();
            size_t emitted = 0;
            bool inBegin = false;

            while (i < total && emitted < 256) {
                const DrawCmd &c = cmds_[i];
                const bool stateDiff = (c.alphaTest != curAlpha) || (c.blendOn != curBlend) ||
                                       (c.blendOn && (c.sFactor != curSF || c.dFactor != curDF || c.blendColor != curBlendCol));
                if (stateDiff) {
                    if (inBegin) {
                        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
                        inBegin = false;
                    }
                    if (c.alphaTest != curAlpha) {
                        p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, c.alphaTest);
                        curAlpha = c.alphaTest;
                    }
                    if (c.blendOn != curBlend) {
                        p = pb_push1(p, NV097_SET_BLEND_ENABLE, c.blendOn);
                        curBlend = c.blendOn;
                    }
                    if (c.blendOn) {
                        if (c.sFactor != curSF) {
                            p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, c.sFactor);
                            curSF = c.sFactor;
                        }
                        if (c.dFactor != curDF) {
                            p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, c.dFactor);
                            curDF = c.dFactor;
                        }
                        if (c.blendColor != curBlendCol) {
                            p = pb_push1(p, NV097_SET_BLEND_COLOR, c.blendColor);
                            curBlendCol = c.blendColor;
                        }
                    }
                }
                if (!inBegin) {
                    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
                    inBegin = true;
                }
                if (c.diffuse != curDiff) {
                    p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, c.diffuse);
                    curDiff = c.diffuse;
                }
                for (int v = 0; v < 4; ++v) {
                    p = pb_push2(p, NV097_SET_VERTEX_DATA2F_M + 9 * 8, c.u[v], c.v[v]);
                    p = pb_push3(p, NV097_SET_VERTEX3F, c.x[v], c.y[v], kFloatZeroBits);
                }
                ++i;
                ++emitted;
            }
            if (inBegin) {
                p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
            }
            pb_end(p);
        }
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

        if (!tateMode) {
            scrScaleX_ = quadW / static_cast<float>(kSrcHeight);
            scrOffsetX_ = vpX0;
            scrScaleY_ = -(quadH / static_cast<float>(kSrcWidth));
            scrOffsetY_ = vpY0 + quadH;
        } else {
            scrScaleX_ = quadW / static_cast<float>(kSrcWidth);
            scrOffsetX_ = vpX0;
            scrScaleY_ = quadH / static_cast<float>(kSrcHeight);
            scrOffsetY_ = vpY0;
        }

        cachedScreenW_ = screenW;
        cachedScreenH_ = screenH;
        cachedTate_ = tateMode;
        cachedInteger_ = integerScale;
        geomInitialized_ = true;
    }

    uint32_t *PushStaticState(uint32_t *p, float screenW, float screenH,
                              uint32_t texPhys, uint32_t texH) const {
        (void)screenW;
        (void)screenH;
        const uint32_t texFmt =
            1u | (1u << 3) | (2u << 4) |
            (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5 << 8) | (1u << 16);
        const uint32_t texAddr = 3u | (3u << 8) | (3u << 16);

        const float kComposite[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };

        p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
        p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
        p = pb_push1(p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD);

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

        p = pb_push4(p, NV097_SET_VIEWPORT_OFFSET, kFloatZeroBits, kFloatZeroBits, kFloatZeroBits, kFloatZeroBits);
        p = pb_push4(p, NV097_SET_VIEWPORT_SCALE, kFloatOneBits, kFloatOneBits, kFloatOneBits, kFloatOneBits);

        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, texPhys);
        p = pb_push1(p, NV097_SET_TEXTURE_FORMAT, texFmt);
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, texAddr);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE | (0xFFFu << 6));
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1, (kAtlasW * 2u) << 16);
        p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kAtlasW << 16) | texH);
        for (uint32_t stage = 1; stage < 4; ++stage) {
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + stage * 0x40u, 0);
        }

        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE);
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);

        // Stage 0 combiner: AB_DST (bits 4..7) = SPARE0 (0xC0) = TEX0.rgb * DIFFUSE.rgb;
        // Final combiner: RGB = SPARE0 (0x0C), A = TEX0.a (0x18 << 8).
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0x08040000u);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0x000000C0u);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0x18200000u);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0x000000C0u);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x0000000Cu);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001800u);
        p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, 0xFFFFFFFFu);
        return p;
    }

    uint16_t *atlasAlloc_ = nullptr;
    uint16_t *atlas_ = nullptr;
    uint32_t atlasPhysBase_ = 0;
    uint32_t atlasPhase_ = 0;
    float atlasVBias_ = 0.0f;

    std::vector<int> pageTable_;
    std::vector<uint16_t *> pagePtrs_;
    std::vector<uint32_t> pageLru_;
    std::vector<int> pageOwner_;
    std::vector<std::vector<uint16_t>> pageData_;
    uint32_t lruClock_ = 0;

    std::vector<RectHashSlot> rectHash_;
    std::vector<DrawCmd> cmds_;
    uint16_t rectEpoch_ = 1;
    int shelfX_ = 0, shelfY_ = 0, shelfRowH_ = 0;
    size_t lastDrawCount_ = 0;

    float scrScaleX_ = 0.0f, scrOffsetX_ = 0.0f, scrScaleY_ = 0.0f, scrOffsetY_ = 0.0f;
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
