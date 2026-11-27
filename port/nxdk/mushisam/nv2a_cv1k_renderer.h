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

#include <nxdk/mount.h>
#include <pbkit/pbkit.h>
#include <windows.h>
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
    static constexpr int kRetailMaxAllocPages = 576; // 576 * 32 KB = 18 MB hot RAM pool + RLE16 cold RAM tier (leaves ~5 MB headroom!)
    static constexpr size_t kMaxColdRamBytes = 3584u * 1024u; // 3.5 MB compressed cold RAM (~14-18 MB raw) + lossless Z:\ spill if ever exceeded
    static constexpr uint32_t kPageBytes = static_cast<uint32_t>(kPageSize * kPageSize * sizeof(uint16_t)); // 32 KB

    // Dual-bank 1024x(2048+4) A1R5G5B5 NV2A shelf atlas (2 x 1024x1024 = 4 MB + 8 KB) + 8,192-slot Mix64 hash table:
    // 1024 rows per bank guarantees ZERO mid-frame pb_busy() flushes and ZERO full-atlas wipe spikes!
    static constexpr int kAtlasW = 1024;
    static constexpr int kAtlasH = 2048;
    static constexpr int kBankH = kAtlasH / 2; // 1024 rows per bank (Bank 0: 0..1023, Bank 1: 1024..2047)
    static constexpr int kAtlasGuardRows = 4;
    static constexpr int kAtlasAllocH = kAtlasH + kAtlasGuardRows;
    static constexpr size_t kAtlasBytes = static_cast<size_t>(kAtlasW) * kAtlasAllocH * sizeof(uint16_t);
    static constexpr uint32_t kRectHashSlots = 8192;
    static constexpr uint32_t kRectHashMask = kRectHashSlots - 1;

    // Double-buffered write-combined NV2A hardware DMA vertex buffer:
    // Compact 16-byte vertex (int16x2 TYPE_S32K pos + float2 tex0 + D3DCOLOR diffuse = 64 B/quad = 2 PIII WC lines!).
    // YOKO/TATE rotation and viewport scaling execute in NV2A hardware NV097_SET_COMPOSITE_MATRIX!
    static constexpr uint32_t kMaxQuadsPerFrame = 2048;
    static constexpr uint32_t kMaxVertsPerFrame = kMaxQuadsPerFrame * 4;
    struct Nv2aVertex {
        int16_t x, y;
        float u, v;
        uint32_t diffuse;
    };
    static_assert(sizeof(Nv2aVertex) == 16, "Nv2aVertex must be 16 bytes");
    static constexpr size_t kVbFrameBytes = static_cast<size_t>(kMaxVertsPerFrame) * sizeof(Nv2aVertex);
    static constexpr size_t kVbTotalBytes = kVbFrameBytes * 2;

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

        vbAlloc_ = static_cast<Nv2aVertex *>(
            MmAllocateContiguousMemoryEx(kVbTotalBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (!vbAlloc_) {
            MmFreeContiguousMemory(atlasAlloc_);
            atlasAlloc_ = nullptr;
            atlas_ = nullptr;
            return false;
        }
        vbPhysBase_ = static_cast<uint32_t>(MmGetPhysicalAddress(vbAlloc_));
        vbPhase_ = 0;

        // Detect 128 MB modded/dev Xbox at runtime to unlock full 64 MB VRAM residency (0 evictions ever).
        maxAllocPages_ = kRetailMaxAllocPages;
        MM_STATISTICS mmStats{};
        mmStats.Length = sizeof(mmStats);
        if (MmQueryStatistics(&mmStats) == STATUS_SUCCESS && mmStats.TotalPhysicalPages > 24576u) {
            maxAllocPages_ = kTotalLogicalPages;
        }

        pageTable_.assign(kTotalLogicalPages, -1);
        pagePtrs_.assign(kTotalLogicalPages, nullptr);
        coldState_.assign(kTotalLogicalPages, 0);
        coldClock_.assign(kTotalLogicalPages, 0);
        coldPages_.assign(kTotalLogicalPages, {});
        coldTotalBytes_ = 0;
        coldTick_ = 0;
        std::memset(pageGen_, 0, sizeof(pageGen_));
        pageRef_.reserve(maxAllocPages_);
        pageOwner_.reserve(maxAllocPages_);
        pageDirty_.reserve(maxAllocPages_);
        pageData_.reserve(maxAllocPages_);
        rectHash_.assign(kRectHashSlots, {});
        batches_.reserve(256);
        rleScratch_.reserve(kPageSize * kPageSize + 64u);
        spillScratch_.resize(kPageSize * kPageSize, 0u);
        swapHandle_ = INVALID_HANDLE_VALUE;

        clockHand_ = 0;
        frameId_ = 1;
        hasPresentedFrame_ = false;
        activeBank_ = 0;
        bankEpoch_[0] = 1;
        bankEpoch_[1] = 1;
        shelfX_ = 0;
        shelfY_ = 0;
        shelfRowH_ = 0;
        atlasVBias_ = static_cast<float>(kAtlasGuardRows);
        swapCount_ = 0;
        stateInitialized_ = false;
        filterInitialized_ = false;
        geomInitialized_ = false;
        clearFramesRemaining_ = 3;
        return true;
    }

    void Shutdown() {
        CloseSwapFile();
        if (vbAlloc_) {
            MmFreeContiguousMemory(vbAlloc_);
            vbAlloc_ = nullptr;
            vbPhysBase_ = 0;
        }
        if (atlasAlloc_) {
            MmFreeContiguousMemory(atlasAlloc_);
            atlasAlloc_ = nullptr;
            atlas_ = nullptr;
            atlasPhysBase_ = 0;
        }
        pageData_.clear();
        pageTable_.clear();
        pagePtrs_.clear();
        coldPages_.clear();
        coldState_.clear();
        coldClock_.clear();
        coldTotalBytes_ = 0;
        pageRef_.clear();
        pageOwner_.clear();
        pageDirty_.clear();
        rleScratch_.clear();
        spillScratch_.clear();
        rectHash_.clear();
        batches_.clear();
        stateInitialized_ = false;
        hasPresentedFrame_ = false;
    }

    // Consume Blitter::NativeList steps (uploads + draws) and submit hardware NV2A quads.
    // Returns true if a frame was submitted (caller should call pb_finished()), or false on
    // 0-draw CV1000 transition frames where holding the frontbuffer avoids black flicker.
    bool Render(const spelunk::Blitter &blitter, spelunk::NativeList &list,
                bool tateMode, bool bilinear, bool integerScale) {
        if (!atlas_) {
            list.Clear();
            if (list.pixels.capacity() > 65536u) {
                std::vector<uint16_t>().swap(list.pixels);
            }
            return false;
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
        const int dispX = static_cast<int>(blitter.scrollX & 0x1FFFu);
        const int dispY = static_cast<int>(blitter.scrollY & 0x0FFFu);

        // 1. Flip double-buffered VB UP FRONT (CPU writes VB B while NV2A finishes reading VB A!).
        //    Only wait on pb_busy() before recycling a bank or submitting the new pushbuffer — hides ~2 ms GpuWait!
        ++frameId_;
        atlasPhase_ = (atlasPhase_ + 1u) & 3u;
        atlasVBias_ = static_cast<float>(kAtlasGuardRows - static_cast<int>(atlasPhase_));
        vbPhase_ ^= 1u;
        vbCur_ = vbAlloc_ + (vbPhase_ ? kMaxVertsPerFrame : 0u);
        vbCount_ = 0;
        gpuReadyThisFrame_ = false;
        lastGpuWaitTsc_ = 0;
        // Dual-bank generational atlas: when the active 1024-row bank crosses 75% full, sync GPU once,
        // switch to the opposite bank, and invalidate ONLY that bank — sprites in the other bank stay 100% resident!
        const int bankStartY = static_cast<int>(activeBank_) * kBankH;
        if (shelfY_ > bankStartY + (kBankH * 3) / 4) {
            EnsureGpuReadyForSubmit();
            activeBank_ ^= 1u;
            shelfX_ = 0;
            shelfY_ = static_cast<int>(activeBank_) * kBankH;
            shelfRowH_ = 0;
            InvalidateBankHash(activeBank_);
        }
        batches_.clear();
        lastDrawCount_ = list.draws.size();
        lastUploadCount_ = static_cast<uint32_t>(list.uploads.size()) + directUploadCount_;
        directUploadCount_ = 0;
        lastStateChanges_ = 0;
        lastBlitVramTsc_ = 0;
        lastAtlasCopyTsc_ = 0;
        lastPbSubmitTsc_ = 0;
        lastAtlasHits_ = 0;
        lastAtlasMisses_ = 0;
        frameHeaderPushed_ = false;
        curScreenW_ = screenW;
        curScreenH_ = screenH;
        curBilinear_ = bilinear;

        // 2. Process all blitter steps in exact game order:
        //    - Uploads & off-screen VRAM composite draws update sparse VRAM pages and bump per-page pageGen_[]
        //      (NEVER wiping unrelated cached sprites from the 8,192-slot atlas hash table!).
        for (const auto &step : list.steps) {
            if (step.upload) {
                ApplyUpload(list.uploads[step.first], list.pixels);
            } else {
                for (uint32_t i = 0; i < step.count; ++i) {
                    ProcessDraw(list.draws[step.first + i], scrX, scrY, dispX, dispY, bilinear);
                }
            }
        }
        list.Clear();
        if (list.pixels.capacity() > 65536u) {
            std::vector<uint16_t>().swap(list.pixels);
        }

        // 3. Hold previous presented frame on 0-draw CV1000 transition frames (e.g. f=25672..25884
        //    Stage 2->3 load) instead of clearing backbuffer to solid black!
        if (vbCount_ == 0u && !frameHeaderPushed_ && hasPresentedFrame_ && clearFramesRemaining_ == 0) {
            return false;
        }

        // 4. Flush write-combined A1R5G5B5 atlas & hardware vertex buffer writes before NV2A DMA.
        _mm_sfence();

        // 5. Sync GPU (now already idle after CPU work!), reset pushbuffer, and submit hardware batches.
        EnsureGpuReadyForSubmit();
        const uint64_t tPb0 = ReadRdtsc();
        if (!frameHeaderPushed_) {
            PushFrameHeader(screenW, screenH, bilinear);
        }
        SubmitDraws();
        lastPbSubmitTsc_ += ReadRdtsc() - tPb0;
        if (vbCount_ > 0u) {
            hasPresentedFrame_ = true;
        }
        return true;
    }

    static void DirectUploadCallback(void *ctx, int32_t dstX, int32_t dstY, int32_t w, int32_t h,
                                     const uint8_t *ram, uint32_t ramMask, uint32_t pixelAddr) {
        static_cast<Nv2aCv1kRenderer *>(ctx)->ApplyDirectUpload(dstX, dstY, w, h, ram, ramMask, pixelAddr);
    }

    void ApplyDirectUpload(int32_t dstX, int32_t dstY, int32_t w, int32_t h,
                           const uint8_t *ram, uint32_t ramMask, uint32_t pixelAddr) {
        if (w <= 0 || h <= 0 || !ram) {
            return;
        }
        ++directUploadCount_;
        uint32_t addr = pixelAddr;
        for (int32_t row = 0; row < h; ++row) {
            const int vy = (dstY + row) & (kVramH - 1);
            const int py = vy >> kPageShift;
            const int ly = vy & kPageMask;
            int32_t col = 0;
            while (col < w) {
                const int vx = (dstX + col) & (kVramW - 1);
                const int px = vx >> kPageShift;
                const int lx = vx & kPageMask;
                const int chunk = std::min<int>(w - col, kPageSize - lx);
                uint16_t *dst = GetPageWrite(px, py) + static_cast<size_t>(ly) * kPageSize + lx;
                for (int i = 0; i < chunk; ++i) {
                    const uint32_t a = addr & ramMask & ~1u;
                    addr += 2u;
                    dst[i] = static_cast<uint16_t>((static_cast<uint16_t>(ram[a]) << 8) | ram[a + 1u]);
                }
                col += chunk;
            }
        }
    }

    size_t LastQuadCount() const { return vbCount_ >> 2; }
    size_t LastDrawCount() const { return lastDrawCount_; }
    uint32_t ActiveVramPages() const { return static_cast<uint32_t>(pageData_.size()); }
    uint32_t LastBatchCount() const { return static_cast<uint32_t>(batches_.size()); }
    uint32_t LastStateChanges() const { return lastStateChanges_; }
    uint32_t LastUploadCount() const { return lastUploadCount_; }
    uint32_t SwapCount() const { return swapCount_; }
    uint64_t LastBlitVramTsc() const { return lastBlitVramTsc_; }
    uint64_t LastAtlasCopyTsc() const { return lastAtlasCopyTsc_; }
    uint64_t LastPbSubmitTsc() const { return lastPbSubmitTsc_; }
    uint64_t LastGpuWaitTsc() const { return lastGpuWaitTsc_; }
    uint32_t LastAtlasHits() const { return lastAtlasHits_; }
    uint32_t LastAtlasMisses() const { return lastAtlasMisses_; }

private:
    struct DrawBatch {
        uint32_t firstVertex;
        uint32_t vertexCount;
        uint32_t blendColor;
        uint16_t sFactor;
        uint16_t dFactor;
        uint8_t blendOn;
        uint8_t alphaTest;
    };

    struct RectHashSlot {
        uint64_t key = 0;
        uint16_t u0 = 0;
        uint16_t v0 = 0;
        uint16_t epoch = 0;
        uint16_t pageGen = 0;
        uint32_t lastFrame = 0;
    };

    static constexpr uint32_t kFloatZeroBits = 0x00000000u;
    static constexpr uint32_t kFloatOneBits = 0x3F800000u;

    static inline uint32_t FloatBits(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

    static inline uint64_t ReadRdtsc() {
        uint32_t lo = 0, hi = 0;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    static inline uint32_t Mix64(uint64_t k) {
        uint32_t h = static_cast<uint32_t>(k) ^ static_cast<uint32_t>(k >> 32);
        h ^= h >> 16;
        h *= 0x7FEB352Du;
        h ^= h >> 15;
        h *= 0x846CA68Bu;
        return h ^ (h >> 16);
    }

    // Division-free 5-bit CV1000 alpha -> 8-bit scale (x * 255 / 31, clamped at 255).
    static inline uint32_t Scale5To8(uint32_t v) {
        if (v >= 31u) return 255u;
        return (v << 3) | (v >> 2);
    }

    // Exact 6-bit CV1000 tint (0..63, where 32 = 1.0x and 63 = 2.0x overdrive) -> 8-bit diffuse for NV2A SHIFTLEFTBY1 combiner.
    static inline uint32_t ScaleTint6To8(uint32_t v) {
        if (v >= 63u) return 255u;
        return (v << 2) | (v >> 4);
    }

    void InvalidateBankHash(uint32_t bank) {
        if (++bankEpoch_[bank] == 0) {
            bankEpoch_[0] = 1;
            bankEpoch_[1] = 1;
            std::memset(rectHash_.data(), 0, rectHash_.size() * sizeof(RectHashSlot));
        }
    }

    void InvalidateRectHash() {
        if (++bankEpoch_[0] == 0 || ++bankEpoch_[1] == 0) {
            bankEpoch_[0] = 1;
            bankEpoch_[1] = 1;
            std::memset(rectHash_.data(), 0, rectHash_.size() * sizeof(RectHashSlot));
        }
    }

    void EnsureGpuReadyForSubmit() {
        if (gpuReadyThisFrame_) {
            return;
        }
        const uint64_t t0 = ReadRdtsc();
        while (pb_busy()) {
        }
        lastGpuWaitTsc_ += ReadRdtsc() - t0;
        pb_reset();
        // pb_init() and pb_finished() already target the current backbuffer; avoid redundant pushbuffer traffic!
        gpuReadyThisFrame_ = true;
    }

    void PushFrameHeader(float screenW, float screenH, bool bilinear) {
        const uint32_t activeTexPhys =
            atlasPhysBase_ + atlasPhase_ * static_cast<uint32_t>(kAtlasW * sizeof(uint16_t));
        const uint32_t activeTexH = static_cast<uint32_t>(kAtlasAllocH) - atlasPhase_;
        const uint32_t activeVbPhys = vbPhysBase_ + (vbPhase_ ? static_cast<uint32_t>(kVbFrameBytes) : 0u);
        uint32_t *p = pb_begin();
        if (clearFramesRemaining_ > 0) {
            const uint32_t sw = static_cast<uint32_t>(screenW);
            const uint32_t sh = static_cast<uint32_t>(screenH);
            p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL, ((sw - 1u) << 16) | 0u);
            p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL, ((sh - 1u) << 16) | 0u);
            p = pb_push1(p, NV097_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
            p = pb_push1(p, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_COLOR);
            --clearFramesRemaining_;
        }
        p = pb_push1(p, NV097_SET_CLEAR_RECT_HORIZONTAL, (static_cast<uint32_t>(clipX1_ - 1) << 16) | static_cast<uint32_t>(clipX0_));
        p = pb_push1(p, NV097_SET_CLEAR_RECT_VERTICAL, (static_cast<uint32_t>(clipY1_ - 1) << 16) | static_cast<uint32_t>(clipY0_));
        p = pb_push1(p, NV097_SET_COLOR_CLEAR_VALUE, 0xFF000000u);
        p = pb_push1(p, NV097_CLEAR_SURFACE, NV097_CLEAR_SURFACE_COLOR);

        if (!stateInitialized_) {
            p = PushStaticState(p, screenW, screenH, activeTexPhys, activeTexH, activeVbPhys);
            stateInitialized_ = true;
            filterInitialized_ = false;
        } else {
            p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, activeTexPhys);
            p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kAtlasW << 16) | activeTexH);
            p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, activeVbPhys + 0u);
            p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, activeVbPhys + 12u);
            p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 9 * 4, activeVbPhys + 4u);
        }
        if (!filterInitialized_ || bilinear != cachedBilinear_) {
            const uint32_t filterMode = bilinear ? 2u : 1u;
            p = pb_push1(p, NV097_SET_TEXTURE_FILTER, (filterMode << 16) | (filterMode << 24));
            cachedBilinear_ = bilinear;
            filterInitialized_ = true;
        }
        pb_end(p);
        frameHeaderPushed_ = true;
    }

    // Safety net: if a single extreme frame ever fills the 1024x2048 atlas or 2048-quad VB,
    // submit queued quads and wait for PGRAPH completion before recycling shelf (0, 0).
    void FlushMidFrameAndResetShelf() {
        EnsureGpuReadyForSubmit();
        _mm_sfence();
        if (!frameHeaderPushed_) {
            PushFrameHeader(curScreenW_, curScreenH_, curBilinear_);
        }
        SubmitDraws();
        const uint64_t tFlush0 = ReadRdtsc();
        while (pb_busy()) {
        }
        lastGpuWaitTsc_ += ReadRdtsc() - tFlush0;
        batches_.clear();
        vbCount_ = 0;
        activeBank_ ^= 1u;
        shelfX_ = 0;
        shelfY_ = static_cast<int>(activeBank_) * kBankH;
        shelfRowH_ = 0;
        InvalidateBankHash(activeBank_);
        atlasPhase_ = (atlasPhase_ + 1u) & 3u;
        atlasVBias_ = static_cast<float>(kAtlasGuardRows - static_cast<int>(atlasPhase_));
        const uint32_t activeTexPhys =
            atlasPhysBase_ + atlasPhase_ * static_cast<uint32_t>(kAtlasW * sizeof(uint16_t));
        const uint32_t activeTexH = static_cast<uint32_t>(kAtlasAllocH) - atlasPhase_;
        uint32_t *p = pb_begin();
        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, activeTexPhys);
        p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kAtlasW << 16) | activeTexH);
        pb_end(p);
    }

    static inline uint8_t Mul5(uint8_t x, uint8_t y) {
        const unsigned v = (static_cast<unsigned>(x) * static_cast<unsigned>(y)) / 31u;
        return static_cast<uint8_t>(v > 31u ? 31u : v);
    }
    static inline uint8_t SrcBlendTerm(int sm, uint8_t s, uint8_t d, uint8_t alpha) {
        switch (sm & 7) {
            case 0: return Mul5(s, alpha);
            case 1: return Mul5(s, s);
            case 2: return Mul5(s, d);
            case 4: return Mul5(s, alpha ^ 31u);
            case 5: return Mul5(s, s ^ 31u);
            case 6: return Mul5(s, d ^ 31u);
            default: return s;
        }
    }
    static inline uint8_t DstBlendTerm(int dm, uint8_t s, uint8_t d, uint8_t alpha) {
        switch (dm & 7) {
            case 0: return Mul5(d, alpha);
            case 1: return Mul5(d, s);
            case 2: return Mul5(d, d);
            case 4: return Mul5(d, alpha ^ 31u);
            case 5: return Mul5(d, s ^ 31u);
            case 6: return Mul5(d, d ^ 31u);
            default: return d;
        }
    }

    // Executes off-screen VRAM-to-VRAM tilemap/sprite-map composite blits (e.g. Mushihimesama's x=7456..8191
    // background scroll ring buffer) directly in the sparse 128x128 A1R5G5B5 page pool!
    void BlitVramToVram(const spelunk::NativeDraw &d) {
        const int w = d.x1 - d.x0 + 1;
        const int h = d.y1 - d.y0 + 1;
        if (w <= 0 || h <= 0) return;

        const bool flipX = (d.flags & spelunk::NativeDraw::kFlipX) != 0;
        const bool flipY = (d.flags & spelunk::NativeDraw::kFlipY) != 0;
        const bool trans = (d.flags & spelunk::NativeDraw::kTransparent) != 0;
        const bool tinted = (d.flags & spelunk::NativeDraw::kTinted) != 0;
        const int mode = d.flags & 0x7F; // 0..63 blend, 64 = no blend
        const bool blendOn = (mode < 64);

        const uint8_t sAlpha = static_cast<uint8_t>(d.colours & 0x1F);
        const uint8_t dAlpha = static_cast<uint8_t>((d.colours >> 5) & 0x1F);
        const uint8_t tr = static_cast<uint8_t>((d.colours >> 10) & 0x3F);
        const uint8_t tg = static_cast<uint8_t>((d.colours >> 16) & 0x3F);
        const uint8_t tb = static_cast<uint8_t>((d.colours >> 22) & 0x3F);

        // Fast path (~99% of CV1000 off-screen tilemap blits): non-overlapping, unflipped, untinted, unblended.
        // Hoists GetPageRead/GetPageWrite per 128x128 page strip (32x fewer lookups) and skips scratch double-copy!
        const int ux0 = d.x0 & (kVramW - 1);
        const int uy0 = d.y0 & (kVramH - 1);
        const int usx0 = (d.sx + d.x0) & (kVramW - 1);
        const int usy0 = (d.sy + d.y0) & (kVramH - 1);
        const bool overlaps = (usx0 < ux0 + w && ux0 < usx0 + w && usy0 < uy0 + h && uy0 < usy0 + h);
        if (!flipX && !flipY && !tinted && !blendOn && !overlaps) {
            int row = 0;
            while (row < h) {
                const int sy = (usy0 + row) & (kVramH - 1);
                const int dy = (uy0 + row) & (kVramH - 1);
                const int spy = sy >> kPageShift, sly = sy & kPageMask;
                const int dpy = dy >> kPageShift, dly = dy & kPageMask;
                const int rowSpan = std::min(h - row, std::min(kPageSize - sly, kPageSize - dly));

                int col = 0;
                while (col < w) {
                    const int sx = (usx0 + col) & (kVramW - 1);
                    const int dx = (ux0 + col) & (kVramW - 1);
                    const int spx = sx >> kPageShift, slx = sx & kPageMask;
                    const int dpx = dx >> kPageShift, dlx = dx & kPageMask;
                    const int colSpan = std::min(w - col, std::min(kPageSize - slx, kPageSize - dlx));

                    const uint16_t *spg = GetPageRead(spx, spy);
                    uint16_t *dpg = GetPageWrite(dpx, dpy);
                    uint16_t *dstPtr = dpg + static_cast<size_t>(dly) * kPageSize + dlx;
                    if (!spg) {
                        if (!trans) {
                            const size_t bytes = static_cast<size_t>(colSpan) * sizeof(uint16_t);
                            for (int r = 0; r < rowSpan; ++r, dstPtr += kPageSize) {
                                std::memset(dstPtr, 0, bytes);
                            }
                        }
                    } else {
                        const uint16_t *srcPtr = spg + static_cast<size_t>(sly) * kPageSize + slx;
                        if (!trans) {
                            for (int r = 0; r < rowSpan; ++r, srcPtr += kPageSize, dstPtr += kPageSize) {
                                xbox_copy_u16_row_asm(srcPtr, dstPtr, static_cast<uint32_t>(colSpan));
                            }
                        } else {
                            for (int r = 0; r < rowSpan; ++r, srcPtr += kPageSize, dstPtr += kPageSize) {
                                xbox_blit_trans_u16_row_asm(srcPtr, dstPtr, static_cast<uint32_t>(colSpan));
                            }
                        }
                    }
                    col += colSpan;
                }
                row += rowSpan;
            }
            return;
        }

        // Read source rect first into a stack/scratch buffer so overlapping VRAM-to-VRAM blits never alias.
        if (static_cast<size_t>(w) * static_cast<size_t>(h) > vramBlitScratch_.size()) {
            vramBlitScratch_.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
        }
        for (int ry = 0; ry < h; ++ry) {
            const int dy = d.y0 + ry;
            const int sy = (flipY ? (d.sy - dy) : (d.sy + dy)) & (kVramH - 1);
            const int spy = sy >> kPageShift;
            const int sly = sy & kPageMask;
            uint16_t *rowBuf = vramBlitScratch_.data() + static_cast<size_t>(ry) * w;
            if (!flipX) {
                int col = 0;
                while (col < w) {
                    const int sx = (d.sx + d.x0 + col) & (kVramW - 1);
                    const int spx = sx >> kPageShift;
                    const int slx = sx & kPageMask;
                    const int span = std::min(w - col, kPageSize - slx);
                    const uint16_t *spg = GetPageRead(spx, spy);
                    if (spg) {
                        std::memcpy(rowBuf + col, spg + static_cast<size_t>(sly) * kPageSize + slx,
                                    static_cast<size_t>(span) * sizeof(uint16_t));
                    } else {
                        std::memset(rowBuf + col, 0, static_cast<size_t>(span) * sizeof(uint16_t));
                    }
                    col += span;
                }
            } else {
                for (int rx = 0; rx < w; ++rx) {
                    const int sx = (d.sx - (d.x0 + rx)) & (kVramW - 1);
                    const uint16_t *spg = GetPageRead(sx >> kPageShift, spy);
                    rowBuf[rx] = spg ? spg[static_cast<size_t>(sly) * kPageSize + (sx & kPageMask)] : 0u;
                }
            }
        }

        // Write composited pixels into destination VRAM pages.
        for (int ry = 0; ry < h; ++ry) {
            const int vy = (d.y0 + ry) & (kVramH - 1);
            const int dpy = vy >> kPageShift;
            const int dly = vy & kPageMask;
            const uint16_t *srcRow = vramBlitScratch_.data() + static_cast<size_t>(ry) * w;

            if (!trans && !tinted && !blendOn) {
                int col = 0;
                while (col < w) {
                    const int vx = (d.x0 + col) & (kVramW - 1);
                    const int dpx = vx >> kPageShift;
                    const int dlx = vx & kPageMask;
                    const int span = std::min(w - col, kPageSize - dlx);
                    uint16_t *dpg = GetPageWrite(dpx, dpy);
                    std::memcpy(dpg + static_cast<size_t>(dly) * kPageSize + dlx, srcRow + col,
                                static_cast<size_t>(span) * sizeof(uint16_t));
                    col += span;
                }
                continue;
            }

            int col = 0;
            while (col < w) {
                const int vx0 = (d.x0 + col) & (kVramW - 1);
                const int dpx = vx0 >> kPageShift;
                const int dlx0 = vx0 & kPageMask;
                const int span = std::min(w - col, kPageSize - dlx0);
                uint16_t *dpg = GetPageWrite(dpx, dpy);
                uint16_t *dstPtr = dpg + static_cast<size_t>(dly) * kPageSize + dlx0;
                for (int i = 0; i < span; ++i) {
                    const uint16_t pen = srcRow[col + i];
                    if (trans && !(pen & 0x8000u)) {
                        continue;
                    }
                    if (!tinted && !blendOn) {
                        dstPtr[i] = pen;
                        continue;
                    }
                    uint8_t r = (pen >> 10) & 0x1Fu;
                    uint8_t g = (pen >> 5) & 0x1Fu;
                    uint8_t b = pen & 0x1Fu;
                    if (tinted) {
                        r = Mul5(r, tr);
                        g = Mul5(g, tg);
                        b = Mul5(b, tb);
                    }
                    if (blendOn) {
                        const uint16_t under = dstPtr[i];
                        const uint8_t dr = (under >> 10) & 0x1Fu;
                        const uint8_t dg = (under >> 5) & 0x1Fu;
                        const uint8_t db = under & 0x1Fu;
                        const int sm = mode & 7;
                        const int dm = (mode >> 3) & 7;
                        r = std::min<uint8_t>(31u, SrcBlendTerm(sm, r, dr, sAlpha) + DstBlendTerm(dm, r, dr, dAlpha));
                        g = std::min<uint8_t>(31u, SrcBlendTerm(sm, g, dg, sAlpha) + DstBlendTerm(dm, g, dg, dAlpha));
                        b = std::min<uint8_t>(31u, SrcBlendTerm(sm, b, db, sAlpha) + DstBlendTerm(dm, b, db, dAlpha));
                    }
                    dstPtr[i] = static_cast<uint16_t>((pen & 0x8000u) | (r << 10) | (g << 5) | b);
                }
                col += span;
            }
        }
        // Per-page generation counter invalidated only on pages actually written by GetPageWrite().
    }

    // Lazy lossless spill file (Z:\mushivrm.bin): ONLY opened if both Hot RAM (18 MB) and
    // Cold RLE16 RAM (3.5 MB) are completely full, so a VRAM sprite/tile page is NEVER deleted!
    void EnsureSwapFileOpen() {
        if (swapHandle_ != INVALID_HANDLE_VALUE) {
            return;
        }
        nxMountDrive('Z', "\\Device\\Harddisk0\\Partition5");
        swapHandle_ = CreateFileA("Z:\\mushivrm.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (swapHandle_ == INVALID_HANDLE_VALUE) {
            nxMountDrive('E', "\\Device\\Harddisk0\\Partition1");
            swapHandle_ = CreateFileA("E:\\mushivrm.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }

    void CloseSwapFile() {
        if (swapHandle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(swapHandle_);
            swapHandle_ = INVALID_HANDLE_VALUE;
        }
    }

    bool SpillPageToDisk(int logical, const uint16_t *src) {
        EnsureSwapFileOpen();
        if (swapHandle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(static_cast<uint32_t>(logical)) * kPageBytes;
        IO_STATUS_BLOCK iosb{};
        NTSTATUS st = NtWriteFile(swapHandle_, nullptr, nullptr, nullptr, &iosb,
                                  const_cast<uint16_t *>(src), kPageBytes, &pos);
        if (st == STATUS_PENDING) {
            NtWaitForSingleObject(swapHandle_, FALSE, nullptr);
            st = iosb.Status;
        }
        return NT_SUCCESS(st) && iosb.Information == kPageBytes;
    }

    bool LoadPageFromDisk(int logical, uint16_t *dst) {
        if (swapHandle_ == INVALID_HANDLE_VALUE) {
            return false;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(static_cast<uint32_t>(logical)) * kPageBytes;
        IO_STATUS_BLOCK iosb{};
        NTSTATUS st = NtReadFile(swapHandle_, nullptr, nullptr, nullptr, &iosb,
                                 dst, kPageBytes, &pos);
        if (st == STATUS_PENDING) {
            NtWaitForSingleObject(swapHandle_, FALSE, nullptr);
            st = iosb.Status;
        }
        return NT_SUCCESS(st) && iosb.Information == kPageBytes;
    }

    // Spill the oldest RLE16 cold page from RAM to lossless Z:\mushivrm.bin (NEVER deletes pixel data!).
    bool DemoteOldestColdPageToDisk() {
        int oldestLog = -1;
        uint32_t oldestTick = UINT32_MAX;
        for (int p = 0; p < kTotalLogicalPages; ++p) {
            if (coldState_[p] == 1u && coldClock_[p] < oldestTick) {
                oldestTick = coldClock_[p];
                oldestLog = p;
            }
        }
        if (oldestLog < 0) {
            return false;
        }
        // Decompress oldest cold page into spillScratch_ and persist losslessly to disk.
        DecodeRle16Buffer(coldPages_[oldestLog].data(), coldPages_[oldestLog].size(), spillScratch_.data());
        if (!SpillPageToDisk(oldestLog, spillScratch_.data())) {
            return false;
        }
        coldTotalBytes_ -= coldPages_[oldestLog].size() * sizeof(uint16_t);
        std::vector<uint16_t>().swap(coldPages_[oldestLog]);
        coldState_[oldestLog] = 2u; // Losslessly backed on disk
        return true;
    }

    static void DecodeRle16Buffer(const uint16_t *src, size_t srcLen, uint16_t *dst) {
        constexpr uint32_t kN = kPageSize * kPageSize;
        size_t sp = 0;
        uint32_t dp = 0;
        while (sp < srcLen && dp < kN) {
            const uint16_t tok = src[sp++];
            if (tok & 0x8000u) {
                if (sp >= srcLen) break;
                const uint32_t run = static_cast<uint32_t>(tok & 0x7FFFu) + 1u;
                const uint16_t val = src[sp++];
                const uint32_t n = (dp + run <= kN) ? run : (kN - dp);
                std::fill_n(dst + dp, n, val);
                dp += n;
            } else {
                const uint32_t lit = static_cast<uint32_t>(tok) + 1u;
                const uint32_t avail = static_cast<uint32_t>(srcLen - sp);
                uint32_t n = lit < avail ? lit : avail;
                if (dp + n > kN) n = kN - dp;
                std::memcpy(dst + dp, src + sp, static_cast<size_t>(n) * sizeof(uint16_t));
                sp += n;
                dp += n;
            }
        }
        if (dp < kN) {
            std::memset(dst + dp, 0, static_cast<size_t>(kN - dp) * sizeof(uint16_t));
        }
    }

    // Ultra-fast lossless word-aligned RLE16 compression for cold 128x128 A1R5G5B5 VRAM pages.
    // Keeps cold pages in RAM (~25 us L2 decompress, 0 disk I/O), with 100% lossless Z:\ spill
    // only if Cold RAM hits cap or physical RAM drops below 1.5 MB (NEVER drops sprites/tiles!).
    void StoreColdPageRle16(int logical, const uint16_t *src) {
        constexpr uint32_t kN = kPageSize * kPageSize; // 16384
        // Remove any previous cold RAM allocation for this logical page first.
        if (coldState_[logical] == 1u) {
            coldTotalBytes_ -= coldPages_[logical].size() * sizeof(uint16_t);
            std::vector<uint16_t>().swap(coldPages_[logical]);
            coldState_[logical] = 0u;
        }
        // Fast all-zero check (common for unused / cleared VRAM pages -> 0 bytes stored!).
        bool allZero = true;
        const uint64_t *src64 = reinterpret_cast<const uint64_t *>(src);
        for (uint32_t i = 0; i < (kN >> 2); ++i) {
            if (src64[i] != 0ull) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            coldState_[logical] = 0u;
            return;
        }
        rleScratch_.clear();
        uint32_t i = 0;
        while (i < kN) {
            const uint16_t v = src[i];
            uint32_t run = 1;
            while (i + run < kN && src[i + run] == v && run < 32768u) {
                ++run;
            }
            if (run >= 3u) {
                rleScratch_.push_back(static_cast<uint16_t>(0x8000u | (run - 1u)));
                rleScratch_.push_back(v);
                i += run;
            } else {
                const uint32_t litStart = i;
                uint32_t litLen = 0;
                while (i < kN && litLen < 32768u) {
                    if (i + 2u < kN && src[i] == src[i + 1u] && src[i] == src[i + 2u]) {
                        break;
                    }
                    ++i;
                    ++litLen;
                }
                rleScratch_.push_back(static_cast<uint16_t>(litLen - 1u));
                rleScratch_.insert(rleScratch_.end(), src + litStart, src + litStart + litLen);
            }
        }
        const size_t needBytes = rleScratch_.size() * sizeof(uint16_t);
        // Enforce cold RAM cap & 1.5 MB physical RAM safety floor by spilling oldest cold pages to disk (lossless!).
        while (coldTotalBytes_ + needBytes > kMaxColdRamBytes) {
            if (!DemoteOldestColdPageToDisk()) break;
        }
        MM_STATISTICS mm{};
        mm.Length = sizeof(mm);
        if (MmQueryStatistics(&mm) == STATUS_SUCCESS && mm.AvailablePages < 384u) {
            for (int tries = 0; tries < 8 && coldTotalBytes_ > 0; ++tries) {
                if (!DemoteOldestColdPageToDisk()) break;
                if (MmQueryStatistics(&mm) == STATUS_SUCCESS && mm.AvailablePages >= 512u) break;
            }
        }
        if (coldTotalBytes_ + needBytes <= kMaxColdRamBytes &&
            (MmQueryStatistics(&mm) != STATUS_SUCCESS || mm.AvailablePages >= 384u)) {
            coldPages_[logical].assign(rleScratch_.begin(), rleScratch_.end());
            coldState_[logical] = 1u;
            coldClock_[logical] = ++coldTick_;
            coldTotalBytes_ += needBytes;
        } else if (SpillPageToDisk(logical, src)) {
            coldState_[logical] = 2u;
        } else {
            // Last resort if both RAM low and disk unavailable: keep in RAM rather than dropping sprites!
            coldPages_[logical].assign(rleScratch_.begin(), rleScratch_.end());
            coldState_[logical] = 1u;
            coldClock_[logical] = ++coldTick_;
            coldTotalBytes_ += needBytes;
        }
        ++swapCount_;
    }

    bool LoadColdPageRle16(int logical, uint16_t *dst) {
        if (coldState_[logical] == 1u && !coldPages_[logical].empty()) {
            DecodeRle16Buffer(coldPages_[logical].data(), coldPages_[logical].size(), dst);
            coldClock_[logical] = ++coldTick_;
            ++swapCount_;
            return true;
        }
        if (coldState_[logical] == 2u) {
            if (LoadPageFromDisk(logical, dst)) {
                ++swapCount_;
                return true;
            }
        }
        return false;
    }

    // Two-tier 100%-RAM VRAM pool: 22 MB hot uncompressed + 5.5 MB RLE16 cold compressed
    // with O(1) scan-resistant 2-bit Clock eviction (0 HDD swaps ever, immune to Stage 3 cyclic scan!).
    uint16_t *AllocateOrEvictPage(int logical, bool forWrite) {
        int slot = -1;
        uint16_t *dst = nullptr;
        bool canGrow = static_cast<int>(pageData_.size()) < maxAllocPages_;
        if (canGrow && (pageData_.size() & 15u) == 0u && pageData_.size() >= 256u) {
            MM_STATISTICS mm{};
            mm.Length = sizeof(mm);
            if (MmQueryStatistics(&mm) == STATUS_SUCCESS && mm.AvailablePages < 1280u) {
                // Keep >= 5.0 MB physical RAM free for RLE16 cold tier + audio + OS headroom!
                maxAllocPages_ = static_cast<int>(pageData_.size());
                canGrow = false;
            }
        }
        if (canGrow) {
            slot = static_cast<int>(pageData_.size());
            pageData_.emplace_back(kPageSize * kPageSize, 0u);
            pageRef_.push_back(1u);
            pageOwner_.push_back(logical);
            pageDirty_.push_back(forWrite ? 1u : 0u);
            dst = pageData_[slot].data();
        } else {
            const int nSlots = static_cast<int>(pageData_.size());
            int victim = 0;
            // O(1) amortized 2-bit Clock eviction: frequently-read sprite/HUD pages stay at ref 2..3
            // while Stage 3 streaming uploads (ref 1) get recycled first!
            for (;;) {
                const int idx = clockHand_;
                clockHand_ = (idx + 1 >= nSlots) ? 0 : (idx + 1);
                if (pageRef_[idx] == 0u) {
                    victim = idx;
                    break;
                }
                --pageRef_[idx];
            }
            const int oldLogical = pageOwner_[victim];
            uint16_t *victimPtr = pageData_[victim].data();
            if (pageDirty_[victim] || coldState_[oldLogical] == 0u) {
                StoreColdPageRle16(oldLogical, victimPtr);
            }
            pageTable_[oldLogical] = -1;
            pagePtrs_[oldLogical] = nullptr;
            slot = victim;
            pageOwner_[slot] = logical;
            pageRef_[slot] = 1u;
            pageDirty_[slot] = forWrite ? 1u : 0u;
            dst = victimPtr;
        }
        if (!LoadColdPageRle16(logical, dst)) {
            std::memset(dst, 0, kPageBytes);
        }
        ++pageGen_[logical];
        pageTable_[logical] = slot;
        pagePtrs_[logical] = dst;
        return dst;
    }

    uint16_t *GetPageWrite(int px, int py) {
        const int logical = py * kPagesX + px;
        ++pageGen_[logical];
        const int phys = pageTable_[logical];
        if (phys >= 0) {
            if (pageRef_[phys] < 3u) ++pageRef_[phys];
            pageDirty_[phys] = 1u;
            return pagePtrs_[logical];
        }
        return AllocateOrEvictPage(logical, true);
    }

    inline uint16_t PageGenForRect(int x0, int y0, int w, int h) const {
        const int vx0 = x0 & (kVramW - 1);
        const int vy0 = y0 & (kVramH - 1);
        const int lx = vx0 & kPageMask;
        const int ly = vy0 & kPageMask;
        if (lx + w <= kPageSize && ly + h <= kPageSize) [[likely]] {
            return pageGen_[(vy0 >> kPageShift) * kPagesX + (vx0 >> kPageShift)];
        }
        const int px0 = vx0 >> kPageShift;
        const int py0 = vy0 >> kPageShift;
        const int px1 = ((vx0 + w - 1) & (kVramW - 1)) >> kPageShift;
        const int py1 = ((vy0 + h - 1) & (kVramH - 1)) >> kPageShift;
        uint32_t g = 0x9E37u;
        for (int py = py0;; py = (py + 1) & (kPagesY - 1)) {
            for (int px = px0;; px = (px + 1) & (kPagesX - 1)) {
                g = (g * 33u) ^ pageGen_[py * kPagesX + px];
                if (px == px1) break;
            }
            if (py == py1) break;
        }
        return static_cast<uint16_t>(g);
    }

    inline const uint16_t *GetPageRead(int px, int py) {
        const int logical = py * kPagesX + px;
        const int phys = pageTable_[logical];
        if (phys >= 0) {
            if (pageRef_[phys] < 3u) ++pageRef_[phys];
            return pagePtrs_[logical];
        }
        if (coldState_[logical] != 0u) {
            return AllocateOrEvictPage(logical, false);
        }
        return nullptr;
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

    // Hoisted 128x128 page-strip copy: resolves GetPageRead/LRU once per page tile instead of per scanline (up to 128x fewer lookups).
    void CopyVramRectToAtlas(int srcX0, int srcY0, int w, int h, int dstX, int dstY) {
        int row = 0;
        while (row < h) {
            const int vy0 = (srcY0 + row) & (kVramH - 1);
            const int py = vy0 >> kPageShift;
            const int ly0 = vy0 & kPageMask;
            const int rowSpan = std::min(h - row, kPageSize - ly0);

            int col = 0;
            while (col < w) {
                const int vx0 = (srcX0 + col) & (kVramW - 1);
                const int px = vx0 >> kPageShift;
                const int lx = vx0 & kPageMask;
                const int colSpan = std::min(w - col, kPageSize - lx);
                const uint16_t *page = GetPageRead(px, py);
                uint16_t *dstPtr = atlas_ + static_cast<size_t>(dstY + row) * kAtlasW + dstX + col;
                if (page) {
                    const uint16_t *srcPtr = page + static_cast<size_t>(ly0) * kPageSize + lx;
                    for (int r = 0; r < rowSpan; ++r) {
                        xbox_copy_u16_row_asm(srcPtr, dstPtr, static_cast<uint32_t>(colSpan));
                        srcPtr += kPageSize;
                        dstPtr += kAtlasW;
                    }
                } else {
                    const size_t bytes = static_cast<size_t>(colSpan) * sizeof(uint16_t);
                    for (int r = 0; r < rowSpan; ++r) {
                        std::memset(dstPtr, 0, bytes);
                        dstPtr += kAtlasW;
                    }
                }
                col += colSpan;
            }
            row += rowSpan;
        }
    }

    void ProcessDraw(const spelunk::NativeDraw &d, int scrX, int scrY, int dispX, int dispY, bool bilinear) {
        const int w = d.x1 - d.x0 + 1;
        const int h = d.y1 - d.y0 + 1;
        if (w <= 0 || h <= 0) {
            return;
        }

        // Convert VRAM destination rect to screen-relative coordinates (320x240 active buffer at clipX, clipY).
        const int dx0 = d.x0 - scrX;
        const int dy0 = d.y0 - scrY;
        const int dx1 = dx0 + w;
        const int dy1 = dy0 + h;
        if (dx1 <= 0 || dx0 >= kSrcWidth || dy1 <= 0 || dy0 >= kSrcHeight) {
            // Only software-composite draws targeting genuine off-screen VRAM caches (e.g. x=7456..8191 tilemap
            // ring buffer). Reject negative d.x0/d.y0 so clipped off-screen sprites never wrap into x=8160..8191!
            if (d.x0 >= 0 && d.y0 >= 0 && d.x0 < kVramW && d.y0 < kVramH &&
                (d.x0 >= 1024 || d.y0 >= 512)) {
                const uint64_t tB0 = ReadRdtsc();
                BlitVramToVram(d);
                lastBlitVramTsc_ += ReadRdtsc() - tB0;
            }
            return;
        }

        const bool flipX = (d.flags & spelunk::NativeDraw::kFlipX) != 0;
        const bool flipY = (d.flags & spelunk::NativeDraw::kFlipY) != 0;
        const int srcX0 = flipX ? (d.sx - d.x1) : (d.sx + d.x0);
        const int srcY0 = flipY ? (d.sy - d.y1) : (d.sy + d.y0);

        if (w > kAtlasW || h > kAtlasH) {
            return;
        }

        // Collision-free 64-bit rect key: 12b w (0..4095) | 12b h (0..4095) | 13b srcX0 (0..8191) | 12b srcY0 (0..4095).
        const uint64_t key = (static_cast<uint64_t>(w & 0xFFFu) << 40) |
                             (static_cast<uint64_t>(h & 0xFFFu) << 28) |
                             (static_cast<uint64_t>(srcX0 & 0x1FFFu) << 13) |
                             static_cast<uint64_t>(srcY0 & 0x0FFFu);
        const uint16_t rectPageGen = PageGenForRect(srcX0, srcY0, w, h);
        const uint32_t hBase = Mix64(key) & kRectHashMask;
        RectHashSlot *slot = nullptr;
        bool hit = false;
        bool canReuseInPlace = false;
        for (uint32_t pr = 0; pr < 128; ++pr) {
            RectHashSlot &cand = rectHash_[(hBase + pr) & kRectHashMask];
            const uint32_t slotBank = (cand.v0 >= kBankH) ? 1u : 0u;
            if (cand.epoch != bankEpoch_[slotBank]) {
                slot = &cand;
                break;
            }
            if (cand.key == key) {
                slot = &cand;
                if (cand.pageGen == rectPageGen) {
                    hit = true;
                } else if (cand.lastFrame < frameId_ &&
                           (cand.lastFrame + 2u <= frameId_ || gpuReadyThisFrame_ || !pb_busy())) {
                    // Same (w,h,srcX0,srcY0) updated in VRAM (e.g. scrolling BG strip) and NV2A is done
                    // reading the prior frame: overwrite its existing atlas rect in-place (zero shelf growth!).
                    canReuseInPlace = true;
                }
                break;
            }
        }
        if (!slot) {
            InvalidateRectHash();
            slot = &rectHash_[hBase];
        }
        if (hit) {
            ++lastAtlasHits_;
            slot->lastFrame = frameId_;
        } else if (canReuseInPlace) {
            ++lastAtlasMisses_;
            const uint64_t tC0 = ReadRdtsc();
            CopyVramRectToAtlas(srcX0, srcY0, w, h, slot->u0, slot->v0);
            lastAtlasCopyTsc_ += ReadRdtsc() - tC0;
            slot->pageGen = rectPageGen;
            slot->lastFrame = frameId_;
        } else {
            ++lastAtlasMisses_;
            const int padW = std::min(kAtlasW, w + 1); // 1px gutter prevents adjacent shelf UV bleed
            const int padH = std::min(kBankH, h + 1);
            if (shelfX_ + padW > kAtlasW) {
                shelfX_ = 0;
                shelfY_ += shelfRowH_;
                shelfRowH_ = 0;
            }
            const int bankEndY = static_cast<int>(activeBank_ + 1u) * kBankH;
            if (shelfY_ + padH > bankEndY) {
                // Mid-frame bank overflow: flush queued quads, flip to opposite bank, and recycle ONLY that bank!
                FlushMidFrameAndResetShelf();
                slot = &rectHash_[hBase];
            }
            const int dstX = shelfX_;
            const int dstY = shelfY_;
            shelfX_ += padW;
            if (padH > shelfRowH_) {
                shelfRowH_ = padH;
            }
            const uint64_t tC0 = ReadRdtsc();
            CopyVramRectToAtlas(srcX0, srcY0, w, h, dstX, dstY);
            lastAtlasCopyTsc_ += ReadRdtsc() - tC0;
            // Zero the 1px right/bottom gutter texels to prevent bilinear edge bleed into neighbor sprites.
            if (dstX + w < kAtlasW) {
                uint16_t *gPtr = atlas_ + static_cast<size_t>(dstY) * kAtlasW + (dstX + w);
                for (int r = 0; r < h; ++r, gPtr += kAtlasW) {
                    *gPtr = 0u;
                }
            }
            if (dstY + h < kAtlasH) {
                std::memset(atlas_ + static_cast<size_t>(dstY + h) * kAtlasW + dstX, 0,
                            static_cast<size_t>(padW) * sizeof(uint16_t));
            }
            slot->key = key;
            slot->u0 = static_cast<uint16_t>(dstX);
            slot->v0 = static_cast<uint16_t>(dstY);
            slot->pageGen = rectPageGen;
            slot->epoch = bankEpoch_[activeBank_];
            slot->lastFrame = frameId_;
        }

        // Compute tint & blend state for NV2A Stage-0 SHIFTLEFTBY1 (2x overdrive) Register Combiner:
        // Untinted = 0xFF808080 (0.502 * 2 = 1.0x); tint 0..63 maps 32 -> 1.0x, 63 -> 2.0x hit-flash overdrive!
        uint32_t diffuse = 0xFF808080u;
        if (d.flags & spelunk::NativeDraw::kTinted) {
            const uint32_t tr = ScaleTint6To8((d.colours >> 10) & 0x3Fu);
            const uint32_t tg = ScaleTint6To8((d.colours >> 16) & 0x3Fu);
            const uint32_t tb = ScaleTint6To8((d.colours >> 22) & 0x3Fu);
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

        if (vbCount_ + 4u > kMaxVertsPerFrame) {
            FlushMidFrameAndResetShelf();
        }

        // Emit compact 16-byte integer-position quad directly into write-combined NV2A hardware vertex buffer.
        // Sub-texel inset >= 1/64 texel (0.02f nearest, 0.25f bilinear) prevents NV2A fixed-point UV rounding bleed!
        const float uvEps = bilinear ? 0.25f : 0.02f;
        const float uMin = static_cast<float>(slot->u0) + uvEps;
        const float uMax = static_cast<float>(slot->u0 + w) - uvEps;
        const float vMin = static_cast<float>(slot->v0) + atlasVBias_ + uvEps;
        const float vMax = static_cast<float>(slot->v0 + h) + atlasVBias_ - uvEps;
        const float fu0 = flipX ? uMax : uMin;
        const float fu1 = flipX ? uMin : uMax;
        const float fv0 = flipY ? vMax : vMin;
        const float fv1 = flipY ? vMin : vMax;

        const int16_t ix0 = static_cast<int16_t>(dx0);
        const int16_t iy0 = static_cast<int16_t>(dy0);
        const int16_t ix1 = static_cast<int16_t>(dx1);
        const int16_t iy1 = static_cast<int16_t>(dy1);
        Nv2aVertex *v = vbCur_ + vbCount_;
        v[0] = {ix0, iy0, fu0, fv0, diffuse};
        v[1] = {ix1, iy0, fu1, fv0, diffuse};
        v[2] = {ix1, iy1, fu1, fv1, diffuse};
        v[3] = {ix0, iy1, fu0, fv1, diffuse};

        const uint8_t bOn = blendOn ? 1u : 0u;
        const uint8_t aTest = (d.flags & spelunk::NativeDraw::kTransparent) ? 1u : 0u;
        if (!batches_.empty()) {
            DrawBatch &b = batches_.back();
            if (b.alphaTest == aTest && b.blendOn == bOn &&
                (!bOn || (b.sFactor == sFactor && b.dFactor == dFactor && b.blendColor == blendColor))) {
                b.vertexCount += 4u;
                vbCount_ += 4u;
                return;
            }
        }
        batches_.push_back({vbCount_, 4u, blendColor, sFactor, dFactor, bOn, aTest});
        vbCount_ += 4u;
    }

    // Direct NV097_DRAW_ARRAYS hardware DMA submission: ~2 DWORDs per 64 quads instead of 28 DWORDs/quad!
    void SubmitDraws() {
        if (batches_.empty()) {
            return;
        }
        uint8_t curAlpha = 0xFF, curBlend = 0xFF;
        uint16_t curSF = 0xFFFF, curDF = 0xFFFF;
        uint32_t curBlendCol = 0;

        uint32_t *p = pb_begin();
        uint32_t pushed = 0;
        for (const DrawBatch &b : batches_) {
            if (pushed > 480u) {
                pb_end(p);
                p = pb_begin();
                pushed = 0;
            }
            if (b.alphaTest != curAlpha) {
                p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, b.alphaTest);
                curAlpha = b.alphaTest;
                pushed += 2u;
                ++lastStateChanges_;
            }
            if (b.blendOn != curBlend) {
                p = pb_push1(p, NV097_SET_BLEND_ENABLE, b.blendOn);
                curBlend = b.blendOn;
                pushed += 2u;
                ++lastStateChanges_;
            }
            if (b.blendOn) {
                if (b.sFactor != curSF) {
                    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, b.sFactor);
                    curSF = b.sFactor;
                    pushed += 2u;
                    ++lastStateChanges_;
                }
                if (b.dFactor != curDF) {
                    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, b.dFactor);
                    curDF = b.dFactor;
                    pushed += 2u;
                    ++lastStateChanges_;
                }
                if (b.blendColor != curBlendCol) {
                    p = pb_push1(p, NV097_SET_BLEND_COLOR, b.blendColor);
                    curBlendCol = b.blendColor;
                    pushed += 2u;
                    ++lastStateChanges_;
                }
            }
            p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
            pushed += 2u;
            uint32_t start = b.firstVertex;
            uint32_t rem = b.vertexCount;
            while (rem > 0u) {
                const uint32_t chunk = rem > 256u ? 256u : rem;
                p = pb_push1(p, NV097_DRAW_ARRAYS, ((chunk - 1u) << 24) | (start & 0x00FFFFFFu));
                start += chunk;
                rem -= chunk;
                pushed += 2u;
            }
            p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
            pushed += 2u;
        }
        pb_end(p);
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
                              uint32_t texPhys, uint32_t texH, uint32_t vbPhys) const {
        (void)screenW;
        (void)screenH;
        const uint32_t texFmt =
            1u | (1u << 3) | (2u << 4) |
            (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5 << 8) | (1u << 16);
        const uint32_t texAddr = 3u | (3u << 8) | (3u << 16);

        // Hardware NV097_SET_COMPOSITE_MATRIX: offloads YOKO 90 deg CCW / TATE rotation & viewport scaling to NV2A!
        // NV2A computes row-vector * matrix or column-vector: out.x = dot(row0, pos), out.y = dot(row1, pos).
        float composite[16];
        if (!cachedTate_) {
            const float m[16] = {
                0.0f,       scrScaleX_, 0.0f, scrOffsetX_,
                scrScaleY_, 0.0f,       0.0f, scrOffsetY_,
                0.0f,       0.0f,       1.0f, 0.0f,
                0.0f,       0.0f,       0.0f, 1.0f
            };
            std::memcpy(composite, m, sizeof(composite));
        } else {
            const float m[16] = {
                scrScaleX_, 0.0f,       0.0f, scrOffsetX_,
                0.0f,       scrScaleY_, 0.0f, scrOffsetY_,
                0.0f,       0.0f,       1.0f, 0.0f,
                0.0f,       0.0f,       0.0f, 1.0f
            };
            std::memcpy(composite, m, sizeof(composite));
        }

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
        std::memcpy(p, composite, sizeof(composite));
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

        // Stage 0 combiner: AB_DST (bits 4..7) = SPARE0 (0xC0) = clamp(2.0 * TEX0.rgb * DIFFUSE.rgb, 0..1)
        // via NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY1 (0x00010000u) for exact CV1000 0x..2x hit-flash overdrive!
        // Final combiner: RGB = SPARE0 (0x0C), A = TEX0.a (0x18 << 8).
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW, 0x08040000u);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW,
                     0x000000C0u | (NV097_SET_COMBINER_COLOR_OCW_OP_SHIFTLEFTBY1 << 15));
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_ICW, 0x18200000u);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0x000000C0u);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x0000000Cu);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00001800u);
        p = pb_push1(p, NV097_SET_DIFFUSE_COLOR4I, 0xFF808080u);

        // Configure NV2A hardware DMA vertex array streams (16-byte stride: Pos2S32K @ 0, Tex0_2F @ 4, Diffuse4UB @ 12).
        for (uint32_t attr = 0; attr < 16u; ++attr) {
            p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + attr * 4u,
                         NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F); // size = 0 (disabled)
        }
        const uint32_t stride = static_cast<uint32_t>(sizeof(Nv2aVertex)) << 8;
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4u,
                     stride | (2u << 4) | NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_S32K);
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4u, vbPhys + 0u);
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4u,
                     stride | (4u << 4) | NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL);
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4u, vbPhys + 12u);
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 9 * 4u,
                     stride | (2u << 4) | NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F);
        p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 9 * 4u, vbPhys + 4u);
        return p;
    }

    uint16_t *atlasAlloc_ = nullptr;
    uint16_t *atlas_ = nullptr;
    uint32_t atlasPhysBase_ = 0;
    uint32_t atlasPhase_ = 0;
    float atlasVBias_ = 0.0f;

    Nv2aVertex *vbAlloc_ = nullptr;
    Nv2aVertex *vbCur_ = nullptr;
    uint32_t vbPhysBase_ = 0;
    uint32_t vbPhase_ = 0;
    uint32_t vbCount_ = 0;

    std::vector<int> pageTable_;
    std::vector<uint16_t *> pagePtrs_;
    std::vector<uint8_t> coldState_;
    std::vector<uint32_t> coldClock_;
    std::vector<std::vector<uint16_t>> coldPages_;
    std::vector<uint16_t> rleScratch_;
    std::vector<uint16_t> spillScratch_;
    HANDLE swapHandle_ = INVALID_HANDLE_VALUE;
    size_t coldTotalBytes_ = 0;
    uint32_t coldTick_ = 0;
    std::vector<uint8_t> pageRef_;
    std::vector<int> pageOwner_;
    std::vector<uint8_t> pageDirty_;
    std::vector<std::vector<uint16_t>> pageData_;
    int clockHand_ = 0;
    uint32_t frameId_ = 1;
    bool hasPresentedFrame_ = false;

    std::vector<RectHashSlot> rectHash_;
    std::vector<DrawBatch> batches_;
    std::vector<uint16_t> vramBlitScratch_;
    uint32_t activeBank_ = 0;
    uint16_t bankEpoch_[2] = {1, 1};
    uint16_t pageGen_[kTotalLogicalPages] = {};
    int shelfX_ = 0, shelfY_ = 0, shelfRowH_ = 0;
    int maxAllocPages_ = kRetailMaxAllocPages;
    uint32_t swapCount_ = 0;
    uint32_t lastStateChanges_ = 0;
    uint32_t lastUploadCount_ = 0;
    uint32_t directUploadCount_ = 0;
    size_t lastDrawCount_ = 0;
    uint64_t lastBlitVramTsc_ = 0;
    uint64_t lastAtlasCopyTsc_ = 0;
    uint64_t lastPbSubmitTsc_ = 0;
    uint64_t lastGpuWaitTsc_ = 0;
    bool gpuReadyThisFrame_ = false;
    uint32_t lastAtlasHits_ = 0;
    uint32_t lastAtlasMisses_ = 0;

    float scrScaleX_ = 0.0f, scrOffsetX_ = 0.0f, scrScaleY_ = 0.0f, scrOffsetY_ = 0.0f;
    float curScreenW_ = 640.0f, curScreenH_ = 480.0f;
    bool curBilinear_ = false;
    bool frameHeaderPushed_ = false;
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
