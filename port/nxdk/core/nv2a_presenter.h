#pragma once

#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

namespace spelunk_xbox {

class Nv2aPresenter {
public:
    static constexpr int kSrcWidth = 320;
    static constexpr int kSrcHeight = 240;
    static constexpr size_t kTexPixels = static_cast<size_t>(kSrcWidth) * kSrcHeight;
    static constexpr size_t kTexBytes = kTexPixels * sizeof(uint32_t);
    static constexpr int kTexBuffers = 2; // Ping-pong buffers avoid CPU/GPU texture hazards.

    bool Init() {
        for (int i = 0; i < kTexBuffers; ++i) {
            tex_[i] = static_cast<uint32_t *>(
                MmAllocateContiguousMemoryEx(kTexBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
            if (!tex_[i]) {
                Shutdown();
                return false;
            }
            std::memset(tex_[i], 0, kTexBytes);
            texPhys_[i] = static_cast<uint32_t>(MmGetPhysicalAddress(tex_[i]));
        }
        activeTex_ = 0;
        stateInitialized_ = false;
        geomInitialized_ = false;
        filterInitialized_ = false;
        clearFramesRemaining_ = 3;
        return true;
    }

    void Shutdown() {
        for (int i = 0; i < kTexBuffers; ++i) {
            if (tex_[i]) {
                MmFreeContiguousMemory(tex_[i]);
                tex_[i] = nullptr;
                texPhys_[i] = 0;
            }
        }
        stateInitialized_ = false;
    }

    // Active write-combined texture buffer for the current frame.
    uint32_t *TexBuffer() { return tex_[activeTex_]; }

    // Non-temporal SSE1 streaming upload (movntps + prefetchnta) from cached RAM to
    // PAGE_WRITECOMBINE VRAM without polluting the Xbox Pentium III's 16 KB L1 / 128 KB L2 cache.
    void UploadFrameSSE(const uint32_t *src) {
        uint32_t *dst = tex_[activeTex_];
        if (!dst || !src) {
            return;
        }
        const float *s = reinterpret_cast<const float *>(src);
        float *d = reinterpret_cast<float *>(dst);
        // kTexPixels (76,800) is divisible by 16 (64 bytes = 2 Pentium III cache lines per iter).
        for (size_t i = 0; i < kTexPixels; i += 16) {
            _mm_prefetch(reinterpret_cast<const char *>(s + i + 64), _MM_HINT_NTA);
            const __m128 v0 = _mm_loadu_ps(s + i + 0);
            const __m128 v1 = _mm_loadu_ps(s + i + 4);
            const __m128 v2 = _mm_loadu_ps(s + i + 8);
            const __m128 v3 = _mm_loadu_ps(s + i + 12);
            _mm_stream_ps(d + i + 0, v0);
            _mm_stream_ps(d + i + 4, v1);
            _mm_stream_ps(d + i + 8, v2);
            _mm_stream_ps(d + i + 12, v3);
        }
    }

    void Present(bool tateMode, bool bilinear, bool integerScale) {
        if (!tex_[activeTex_]) {
            return;
        }

        const float screenW = static_cast<float>(pb_back_buffer_width());
        const float screenH = static_cast<float>(pb_back_buffer_height());

        if (!geomInitialized_ || tateMode != cachedTate_ || integerScale != cachedInteger_ ||
            screenW != cachedScreenW_ || screenH != cachedScreenH_) {
            UpdateGeometry(screenW, screenH, tateMode, integerScale);
            // Clear all 3 pbkit framebuffers once when layout/pillarbox region changes.
            clearFramesRemaining_ = 3;
        }

        if (clearFramesRemaining_ > 0) {
            pb_fill(0, 0, static_cast<int>(screenW), static_cast<int>(screenH), 0xFF000000);
            --clearFramesRemaining_;
        }

        // Flush Pentium III write-combining buffers to tex_[activeTex_] before NV2A DMA read.
        _mm_sfence();

        uint32_t *p = pb_begin();

        if (!stateInitialized_) {
            p = PushStaticState(p, screenW, screenH);
            stateInitialized_ = true;
            filterInitialized_ = false;
        }

        if (!filterInitialized_ || bilinear != cachedBilinear_) {
            const uint32_t filterMode = bilinear ? 2u : 1u; // 1=nearest, 2=linear
            const uint32_t texFilter = (filterMode << 16) | (filterMode << 24);
            p = pb_push1(p, NV097_SET_TEXTURE_FILTER, texFilter);
            cachedBilinear_ = bilinear;
            filterInitialized_ = true;
        }

        // Bind active ping-pong texture offset and emit precomputed NDC quad.
        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, texPhys_[activeTex_]);
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
        for (int i = 0; i < 4; ++i) {
            p = pb_push2(p, NV097_SET_VERTEX_DATA2F_M + 9 * 8, verts_[i].u, verts_[i].v);
            p = pb_push4(p, NV097_SET_VERTEX4F, verts_[i].x, verts_[i].y, kFloatZeroBits, kFloatOneBits);
        }
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
        pb_end(p);

        activeTex_ ^= 1;
    }

private:
    struct EncodedVertex {
        uint32_t x, y, u, v;
    };

    static constexpr uint32_t kFloatZeroBits = 0x00000000u;
    static constexpr uint32_t kFloatOneBits = 0x3F800000u;

    static uint32_t FloatBits(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

    void UpdateGeometry(float screenW, float screenH, bool tateMode, bool integerScale) {
        float quadW = 0.0f;
        float quadH = 0.0f;
        if (!tateMode) {
            // YOKO: 240x320 upright (3:4 aspect)
            if (integerScale) {
                quadW = 240.0f;
                quadH = 320.0f;
            } else {
                quadH = screenH;
                quadW = screenH * (240.0f / 320.0f);
            }
        } else {
            // TATE: 320x240 native (4:3 aspect)
            if (integerScale) {
                const int scale = (static_cast<int>(screenW) >= 640 && static_cast<int>(screenH) >= 480) ? 2 : 1;
                quadW = static_cast<float>(kSrcWidth * scale);
                quadH = static_cast<float>(kSrcHeight * scale);
            } else {
                quadW = screenW;
                quadH = screenH;
            }
        }

        const float x0 = (screenW - quadW) * 0.5f;
        const float y0 = (screenH - quadH) * 0.5f;
        const float x1 = x0 + quadW;
        const float y1 = y0 + quadH;

        const uint32_t ndcX0 = FloatBits((x0 / screenW) * 2.0f - 1.0f);
        const uint32_t ndcX1 = FloatBits((x1 / screenW) * 2.0f - 1.0f);
        const uint32_t ndcY0 = FloatBits(1.0f - (y0 / screenH) * 2.0f);
        const uint32_t ndcY1 = FloatBits(1.0f - (y1 / screenH) * 2.0f);

        const uint32_t uZero = FloatBits(0.0f);
        const uint32_t uWidth = FloatBits(static_cast<float>(kSrcWidth));
        const uint32_t vHeight = FloatBits(static_cast<float>(kSrcHeight));

        if (!tateMode) {
            // 90 degrees CCW rotation for upright vertical shmup on horizontal display.
            verts_[0] = {ndcX0, ndcY0, uWidth, uZero};
            verts_[1] = {ndcX1, ndcY0, uWidth, vHeight};
            verts_[2] = {ndcX1, ndcY1, uZero,  vHeight};
            verts_[3] = {ndcX0, ndcY1, uZero,  uZero};
        } else {
            // Native unrotated orientation for TATE display.
            verts_[0] = {ndcX0, ndcY0, uZero,  uZero};
            verts_[1] = {ndcX1, ndcY0, uWidth, uZero};
            verts_[2] = {ndcX1, ndcY1, uWidth, vHeight};
            verts_[3] = {ndcX0, ndcY1, uZero,  vHeight};
        }

        cachedScreenW_ = screenW;
        cachedScreenH_ = screenH;
        cachedTate_ = tateMode;
        cachedInteger_ = integerScale;
        geomInitialized_ = true;
    }

    uint32_t *PushStaticState(uint32_t *p, float screenW, float screenH) const {
        const uint32_t texFmt =
            1u |                                                      // DMA context A
            (1u << 3) |                                               // Border source color
            (2u << 4) |                                               // 2D texture
            (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 << 8) | // Direct ABGR8888 (no CPU swap)
            (1u << 16);                                               // 1 mipmap level
        const uint32_t texAddr = 3u | (3u << 8) | (3u << 16);         // Clamp to edge U/V/P

        // NV2A fixed-function applies COMPOSITE_MATRIX and adds VIEWPORT_OFFSET (does not multiply VIEWPORT_SCALE).
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

        p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE, 0);
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
        p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
        p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
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

        p = pb_push1(p, NV097_SET_TEXTURE_FORMAT, texFmt);
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, texAddr);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE | (0xFFFu << 6));
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1, (kSrcWidth * 4u) << 16);
        p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kSrcWidth << 16) | kSrcHeight);
        for (uint32_t stage = 1; stage < 4; ++stage) {
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + stage * 0x40u, 0);
        }

        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE);
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);

        // Stage 0 combiner discards (leaves TEX0 untouched); final combiner outputs RGB = tex0, Alpha = 1.0.
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000008u);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00002000u);
        return p;
    }

    uint32_t *tex_[kTexBuffers] = {};
    uint32_t texPhys_[kTexBuffers] = {};
    int activeTex_ = 0;

    EncodedVertex verts_[4] = {};
    float cachedScreenW_ = 0.0f;
    float cachedScreenH_ = 0.0f;
    bool stateInitialized_ = false;
    bool geomInitialized_ = false;
    bool filterInitialized_ = false;
    bool cachedTate_ = false;
    bool cachedBilinear_ = false;
    bool cachedInteger_ = false;
    int clearFramesRemaining_ = 3;
};

} // namespace spelunk_xbox
