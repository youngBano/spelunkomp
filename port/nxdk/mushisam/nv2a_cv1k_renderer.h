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
    static constexpr int kRetailMaxAllocPages = 768; // 768 * 32 KB = 24 MB hot RAM pool on 64 MB retail Xbox (eliminates Z:\ swaps!)
    static constexpr uint32_t kPageBytes = static_cast<uint32_t>(kPageSize * kPageSize * sizeof(uint16_t)); // 32 KB

    // 1024x(1024+4) A1R5G5B5 dynamic per-frame NV2A atlas (2 MB + 8 KB).
    static constexpr int kAtlasW = 1024;
    static constexpr int kAtlasH = 1024;
    static constexpr int kAtlasGuardRows = 4;
    static constexpr int kAtlasAllocH = kAtlasH + kAtlasGuardRows;
    static constexpr size_t kAtlasBytes = static_cast<size_t>(kAtlasW) * kAtlasAllocH * sizeof(uint16_t);
    static constexpr uint32_t kRectHashSlots = 4096;
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
        pageOnDisk_.assign(kTotalLogicalPages, 0);
        pageLru_.reserve(maxAllocPages_);
        pageOwner_.reserve(maxAllocPages_);
        pageDirty_.reserve(maxAllocPages_);
        pageData_.reserve(maxAllocPages_);
        rectHash_.assign(kRectHashSlots, {});
        batches_.reserve(256);

        // Open lossless 64 MB VRAM swap backing file on Xbox utility cache partition (Z: -> fallback E:).
        OpenSwapFile();

        lruClock_ = 0;
        rectEpoch_ = 1;
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
        pageOnDisk_.clear();
        pageLru_.clear();
        pageOwner_.clear();
        pageDirty_.clear();
        rectHash_.clear();
        batches_.clear();
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

        // 1. Flip double-buffered hardware VB; keep persistent multi-frame A1R5G5B5 atlas warm across frames!
        vbPhase_ ^= 1u;
        vbCur_ = vbAlloc_ + (vbPhase_ ? kMaxVertsPerFrame : 0u);
        vbCount_ = 0;
        batches_.clear();
        lastDrawCount_ = list.draws.size();
        lastUploadCount_ = static_cast<uint32_t>(list.uploads.size());
        lastStateChanges_ = 0;
        bool atlasMutated = false;

        // 2. Process all blitter steps in exact game order (writes vertices straight to WC hardware VB).
        for (const auto &step : list.steps) {
            if (step.upload) {
                ApplyUpload(list.uploads[step.first], list.pixels);
                // Any upload may mutate source VRAM regions; reset persistent shelf atlas safely.
                ResetAtlasCache();
                atlasMutated = true;
            } else {
                for (uint32_t i = 0; i < step.count; ++i) {
                    if (ProcessDraw(list.draws[step.first + i], scrX, scrY)) {
                        atlasMutated = true;
                    }
                }
            }
        }
        list.Clear();

        // Only cycle 4-row texture offset when atlas pixels actually changed (preserves NV2A L1 tex cache on static frames!).
        if (atlasMutated) {
            atlasPhase_ = (atlasPhase_ + 1u) & 3u;
            const float newBias = static_cast<float>(kAtlasGuardRows - static_cast<int>(atlasPhase_));
            const float deltaBias = newBias - atlasVBias_;
            atlasVBias_ = newBias;
            if (deltaBias != 0.0f && vbCount_ > 0u) {
                for (uint32_t i = 0; i < vbCount_; ++i) {
                    vbCur_[i].v += deltaBias;
                }
            }
        }

        // 3. Flush write-combined A1R5G5B5 atlas & hardware vertex buffer writes before NV2A DMA.
        _mm_sfence();

        // 4. Single fused pushbuffer frame header: NV097 hardware clear + static/dynamic texture & VB DMA pointers.
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

        // 5. Submit hardware NV2A DMA vertex-array batches (NV097_DRAW_ARRAYS).
        SubmitDraws();
    }

    size_t LastQuadCount() const { return vbCount_ >> 2; }
    size_t LastDrawCount() const { return lastDrawCount_; }
    uint32_t ActiveVramPages() const { return static_cast<uint32_t>(pageData_.size()); }
    uint32_t LastBatchCount() const { return static_cast<uint32_t>(batches_.size()); }
    uint32_t LastStateChanges() const { return lastStateChanges_; }
    uint32_t LastUploadCount() const { return lastUploadCount_; }
    uint32_t SwapCount() const { return swapCount_; }

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

    void ResetAtlasCache() {
        shelfX_ = 0;
        shelfY_ = 0;
        shelfRowH_ = 0;
        if (++rectEpoch_ == 0) {
            rectEpoch_ = 1;
            std::memset(rectHash_.data(), 0, rectHash_.size() * sizeof(RectHashSlot));
        }
    }

    void OpenSwapFile() {
        CloseSwapFile();
        if (!nxIsDriveMounted('Z')) {
            nxMountDrive('Z', "\\Device\\Harddisk0\\Partition5\\");
        }
        swapHandle_ = CreateFileA("Z:\\mushivrm.bin", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (swapHandle_ == INVALID_HANDLE_VALUE) {
            if (!nxIsDriveMounted('E')) {
                nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
            }
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

    void SwapOutPage(int logical, const uint16_t *src) {
        if (swapHandle_ == INVALID_HANDLE_VALUE) {
            return;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(logical) * static_cast<LONGLONG>(kPageBytes);
        if (SetFilePointerEx(swapHandle_, pos, nullptr, FILE_BEGIN)) {
            DWORD written = 0;
            if (WriteFile(swapHandle_, src, kPageBytes, &written, nullptr) && written == kPageBytes) {
                pageOnDisk_[logical] = 1;
                ++swapCount_;
            }
        }
    }

    bool SwapInPage(int logical, uint16_t *dst) {
        if (swapHandle_ == INVALID_HANDLE_VALUE || !pageOnDisk_[logical]) {
            return false;
        }
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(logical) * static_cast<LONGLONG>(kPageBytes);
        if (SetFilePointerEx(swapHandle_, pos, nullptr, FILE_BEGIN)) {
            DWORD got = 0;
            if (ReadFile(swapHandle_, dst, kPageBytes, &got, nullptr) && got == kPageBytes) {
                ++swapCount_;
                return true;
            }
        }
        return false;
    }

    uint16_t *AllocateOrEvictPage(int logical, bool forWrite) {
        int slot = -1;
        uint16_t *dst = nullptr;
        if (static_cast<int>(pageData_.size()) < maxAllocPages_) {
            slot = static_cast<int>(pageData_.size());
            pageData_.emplace_back(kPageSize * kPageSize, 0u);
            pageLru_.push_back(++lruClock_);
            pageOwner_.push_back(logical);
            pageDirty_.push_back(forWrite ? 1u : 0u);
            dst = pageData_[slot].data();
        } else {
            int victim = 0;
            uint32_t oldest = UINT32_MAX;
            for (int i = 0; i < maxAllocPages_; ++i) {
                if (pageLru_[i] < oldest) {
                    oldest = pageLru_[i];
                    victim = i;
                }
            }
            const int oldLogical = pageOwner_[victim];
            uint16_t *victimPtr = pageData_[victim].data();
            if (pageDirty_[victim] || !pageOnDisk_[oldLogical]) {
                SwapOutPage(oldLogical, victimPtr);
            }
            pageTable_[oldLogical] = -1;
            pagePtrs_[oldLogical] = nullptr;
            slot = victim;
            pageOwner_[slot] = logical;
            pageLru_[slot] = ++lruClock_;
            pageDirty_[slot] = forWrite ? 1u : 0u;
            dst = victimPtr;
        }
        if (!SwapInPage(logical, dst)) {
            std::memset(dst, 0, kPageBytes);
        }
        pageTable_[logical] = slot;
        pagePtrs_[logical] = dst;
        return dst;
    }

    uint16_t *GetPageWrite(int px, int py) {
        const int logical = py * kPagesX + px;
        const int phys = pageTable_[logical];
        if (phys >= 0) {
            pageLru_[phys] = ++lruClock_;
            pageDirty_[phys] = 1;
            return pagePtrs_[logical];
        }
        return AllocateOrEvictPage(logical, true);
    }

    inline const uint16_t *GetPageRead(int px, int py) {
        const int logical = py * kPagesX + px;
        const int phys = pageTable_[logical];
        if (phys >= 0) {
            pageLru_[phys] = ++lruClock_;
            return pagePtrs_[logical];
        }
        if (pageOnDisk_[logical]) {
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

    bool ProcessDraw(const spelunk::NativeDraw &d, int scrX, int scrY) {
        const int w = d.x1 - d.x0 + 1;
        const int h = d.y1 - d.y0 + 1;
        if (w <= 0 || h <= 0) {
            return false;
        }

        // Convert VRAM destination rect to screen-relative coordinates (320x240 window at scrollX, scrollY).
        const int dx0 = d.x0 - scrX;
        const int dy0 = d.y0 - scrY;
        const int dx1 = dx0 + w;
        const int dy1 = dy0 + h;
        if (dx1 <= 0 || dx0 >= kSrcWidth || dy1 <= 0 || dy0 >= kSrcHeight) {
            return false;
        }

        const bool flipX = (d.flags & spelunk::NativeDraw::kFlipX) != 0;
        const bool flipY = (d.flags & spelunk::NativeDraw::kFlipY) != 0;
        const int srcX0 = flipX ? (d.sx - d.x1) : (d.sx + d.x0);
        const int srcY0 = flipY ? (d.sy - d.y1) : (d.sy + d.y0);

        if (w > kAtlasW || h > kAtlasH) {
            return false;
        }

        // O(1) persistent multi-frame rect dedup in 2 MB A1R5G5B5 atlas.
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
            ResetAtlasCache();
            slot = &rectHash_[hBase];
        }
        bool copiedToAtlas = false;
        if (!hit) {
            if (shelfX_ + w > kAtlasW) {
                shelfX_ = 0;
                shelfY_ += shelfRowH_;
                shelfRowH_ = 0;
            }
            if (shelfY_ + h > kAtlasH) {
                // Shelf full across frames: reset persistent cache cleanly and place at (0, 0).
                ResetAtlasCache();
                slot = &rectHash_[hBase];
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
            copiedToAtlas = true;
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
            return copiedToAtlas;
        }

        // Emit compact 16-byte integer-position quad directly into write-combined NV2A hardware vertex buffer.
        // NV2A hardware NV097_SET_COMPOSITE_MATRIX performs YOKO 90 deg CCW / TATE transform & viewport scaling!
        constexpr float kUvEps = 0.001f;
        const float uMin = static_cast<float>(slot->u0) + kUvEps;
        const float uMax = static_cast<float>(slot->u0 + w) - kUvEps;
        const float vMin = static_cast<float>(slot->v0) + atlasVBias_ + kUvEps;
        const float vMax = static_cast<float>(slot->v0 + h) + atlasVBias_ - kUvEps;
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
                return copiedToAtlas;
            }
        }
        batches_.push_back({vbCount_, 4u, blendColor, sFactor, dFactor, bOn, aTest});
        vbCount_ += 4u;
        return copiedToAtlas;
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
    std::vector<uint8_t> pageOnDisk_;
    std::vector<uint32_t> pageLru_;
    std::vector<int> pageOwner_;
    std::vector<uint8_t> pageDirty_;
    std::vector<std::vector<uint16_t>> pageData_;
    HANDLE swapHandle_ = INVALID_HANDLE_VALUE;
    uint32_t lruClock_ = 0;

    std::vector<RectHashSlot> rectHash_;
    std::vector<DrawBatch> batches_;
    uint16_t rectEpoch_ = 1;
    int shelfX_ = 0, shelfY_ = 0, shelfRowH_ = 0;
    int maxAllocPages_ = kRetailMaxAllocPages;
    uint32_t swapCount_ = 0;
    uint32_t lastStateChanges_ = 0;
    uint32_t lastUploadCount_ = 0;
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
