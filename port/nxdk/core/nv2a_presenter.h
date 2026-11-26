#pragma once

#include <pbkit/pbkit.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

class Nv2aPresenter {
public:
    static constexpr int kSrcWidth = 320;
    static constexpr int kSrcHeight = 240;
    static constexpr size_t kTexBytes = kSrcWidth * kSrcHeight * sizeof(uint32_t);

    bool Init() {
        tex_ = static_cast<uint32_t *>(
            MmAllocateContiguousMemoryEx(kTexBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
        if (!tex_) {
            return false;
        }
        std::memset(tex_, 0, kTexBytes);
        return true;
    }

    void Shutdown() {
        if (tex_) {
            MmFreeContiguousMemory(tex_);
            tex_ = nullptr;
        }
    }

    uint32_t *TexBuffer() { return tex_; }

    void Present(bool tateMode, bool bilinear, bool integerScale) {
        if (!tex_) {
            return;
        }

        const float screenW = static_cast<float>(pb_back_buffer_width());
        const float screenH = static_cast<float>(pb_back_buffer_height());

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

        // Convert screen-space pixel coordinates to NDC [-1, 1].
        const float ndcX0 = (x0 / screenW) * 2.0f - 1.0f;
        const float ndcX1 = (x1 / screenW) * 2.0f - 1.0f;
        const float ndcY0 = 1.0f - (y0 / screenH) * 2.0f;
        const float ndcY1 = 1.0f - (y1 / screenH) * 2.0f;

        // Linear NV2A textures use unnormalized texel coordinates.
        float u0, v0, u1, v1, u2, v2, u3, v3;
        if (!tateMode) {
            // 90 degrees CCW rotation for upright vertical shmup on horizontal display.
            u0 = 320.0f; v0 = 0.0f;
            u1 = 320.0f; v1 = 240.0f;
            u2 = 0.0f;   v2 = 240.0f;
            u3 = 0.0f;   v3 = 0.0f;
        } else {
            // Native unrotated orientation for TATE display.
            u0 = 0.0f;   v0 = 0.0f;
            u1 = 320.0f; v1 = 0.0f;
            u2 = 320.0f; v2 = 240.0f;
            u3 = 0.0f;   v3 = 240.0f;
        }

        const uint32_t texPhys = static_cast<uint32_t>(MmGetPhysicalAddress(tex_));
        const uint32_t texFmt =
            1u |                                                              // DMA context A
            (1u << 3) |                                                       // Border source color
            (2u << 4) |                                                       // 2D texture
            (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8B8G8R8 << 8) |         // Direct ABGR8888 (no CPU swap)
            (1u << 16);                                                       // 1 mipmap level
        const uint32_t texAddr = 3u | (3u << 8) | (3u << 16);                 // Clamp to edge U/V/P
        const uint32_t filterMode = bilinear ? 2u : 1u;                       // 1=nearest, 2=linear
        const uint32_t texFilter = (filterMode << 16) | (filterMode << 24);

        // NV2A fixed-function applies COMPOSITE_MATRIX and adds VIEWPORT_OFFSET (does not multiply VIEWPORT_SCALE).
        const float kComposite[16] = {
            screenW * 0.5f, 0.0f,            0.0f, 0.0f,
            0.0f,           -screenH * 0.5f, 0.0f, 0.0f,
            0.0f,           0.0f,            1.0f, 0.0f,
            0.0f,           0.0f,            0.0f, 1.0f
        };

        // Flush write-combined CPU writes to tex_ before NV2A DMA read.
        __asm__ volatile("sfence" ::: "memory");

        uint32_t *p = pb_begin();
        p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
        p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
        p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
        p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);

        // Fixed-function transform state with identity composite matrix.
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
                     FloatBits(0.0f), FloatBits(0.0f));
        p = pb_push4(p, NV097_SET_VIEWPORT_SCALE,
                     FloatBits(screenW * 0.5f), FloatBits(-screenH * 0.5f),
                     FloatBits(1.0f), FloatBits(1.0f));

        // Bind linear ABGR8888 texture to stage 0 and disable stages 1-3.
        p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, texPhys);
        p = pb_push1(p, NV097_SET_TEXTURE_FORMAT, texFmt);
        p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, texAddr);
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, NV097_SET_TEXTURE_CONTROL0_ENABLE | (0xFFFu << 6));
        p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1, (kSrcWidth * 4u) << 16);
        p = pb_push1(p, NV097_SET_TEXTURE_FILTER, texFilter);
        p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (kSrcWidth << 16) | kSrcHeight);
        for (uint32_t stage = 1; stage < 4; ++stage) {
            p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0 + stage * 0x40u, 0);
        }

        // Stage 0 = 2D projective texture, stages 1-3 disabled.
        p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE);
        p = pb_push1(p, NV097_SET_SHADER_OTHER_STAGE_INPUT, 0);

        // Stage 0 combiner discards (leaves TEX0 untouched); final combiner outputs RGB = tex0, Alpha = 1.0.
        p = pb_push1(p, NV097_SET_COMBINER_CONTROL, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
        p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_ALPHA_OCW, 0);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000008u);
        p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00002000u);

        // Emit NDC textured quad.
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
        p = EmitVertex(p, ndcX0, ndcY0, u0, v0);
        p = EmitVertex(p, ndcX1, ndcY0, u1, v1);
        p = EmitVertex(p, ndcX1, ndcY1, u2, v2);
        p = EmitVertex(p, ndcX0, ndcY1, u3, v3);
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
        pb_end(p);
    }

private:
    static uint32_t FloatBits(float v) {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        return bits;
    }

    static uint32_t *EmitVertex(uint32_t *p, float x, float y, float u, float v) {
        // Texcoord0 is vertex attribute 9 (stride 8 bytes per 2F attribute).
        p = pb_push2(p, NV097_SET_VERTEX_DATA2F_M + 9 * 8, FloatBits(u), FloatBits(v));
        p = pb_push4(p, NV097_SET_VERTEX4F, FloatBits(x), FloatBits(y), FloatBits(0.0f), FloatBits(1.0f));
        return p;
    }

    uint32_t *tex_ = nullptr;
};

} // namespace spelunk_xbox
