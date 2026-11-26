#pragma once

// Hardware-backed Cave 68000 vs Original Xbox CPU/NV2A timing lag profiler.
// Tracks per-stage microsecond budgets against Cave's 17,408 us frame period (57.44485 Hz),
// separates Xbox host CPU/render lag from authentic Cave 68000 arcade slowdown, and exposes
// a fixed-layout global struct (magic = "PERF") inspectable live via GDB without serial I/O.
#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

struct alignas(32) XboxPerfStats {
    static constexpr uint32_t kMagic = 0x50455246u; // "PERF"
    static constexpr uint32_t kCaveBudgetUs = 17408u; // 272 lines @ 15,625 Hz (~57.44485 Hz)
    static constexpr int kMaxStageSlots = 12;         // Loop 1 (0..5) + Loop 2 (6..11)

    uint32_t magic = kMagic;
    uint32_t frames = 0;
    uint32_t lagFrames = 0;          // totalUs > 17408 (Xbox fell behind Cave timing)
    uint32_t cpuLagFrames = 0;       // cpuUs > 17408
    uint32_t renderLagFrames = 0;    // renderUs > 17408
    uint32_t caveSlowdownFrames = 0; // authentic Cave 68000 frames with no new sprite list

    uint32_t currentMode = 0;        // ddonpach RAM 0x1017A6 (7 = play, 8 = continue)
    uint32_t currentStage = 0;       // ddonpach RAM 0x101978 (0..5)
    uint32_t currentLoop = 0;        // ddonpach RAM 0x10197A (0..1)
    uint32_t activeSprites = 0;
    uint32_t submittedQuads = 0;

    uint32_t lastCpuUs = 0;
    uint32_t lastRenderUs = 0;
    uint32_t lastGpuWaitUs = 0;
    uint32_t lastTotalUs = 0;

    uint32_t maxCpuUs = 0;
    uint32_t maxRenderUs = 0;
    uint32_t maxGpuWaitUs = 0;
    uint32_t maxTotalUs = 0;

    uint32_t worstFrame = 0;
    uint32_t worstStage = 0;
    uint32_t worstSprites = 0;
    uint32_t worstQuads = 0;

    uint64_t sumCpuUs = 0;
    uint64_t sumRenderUs = 0;
    uint64_t sumGpuWaitUs = 0;
    uint64_t sumTotalUs = 0;

    uint32_t stageFrames[kMaxStageSlots] = {};
    uint32_t stageLagFrames[kMaxStageSlots] = {};
    uint32_t stageMaxTotalUs[kMaxStageSlots] = {};
    uint32_t stageMaxCpuUs[kMaxStageSlots] = {};
    uint32_t stageMaxRenderUs[kMaxStageSlots] = {};

    void Reset() {
        std::memset(this, 0, sizeof(*this));
        magic = kMagic;
    }

    void RecordFrame(uint32_t cpuUs, uint32_t renderUs, uint32_t gpuWaitUs,
                     uint32_t mode, uint32_t stage, uint32_t loop,
                     uint32_t sprites, uint32_t quads, bool newSpriteList) {
        const uint32_t totalUs = cpuUs + renderUs + gpuWaitUs;
        ++frames;
        currentMode = mode;
        currentStage = stage;
        currentLoop = loop;
        activeSprites = sprites;
        submittedQuads = quads;

        lastCpuUs = cpuUs;
        lastRenderUs = renderUs;
        lastGpuWaitUs = gpuWaitUs;
        lastTotalUs = totalUs;

        sumCpuUs += cpuUs;
        sumRenderUs += renderUs;
        sumGpuWaitUs += gpuWaitUs;
        sumTotalUs += totalUs;

        if (!newSpriteList) {
            ++caveSlowdownFrames;
        }
        if (cpuUs > maxCpuUs) maxCpuUs = cpuUs;
        if (renderUs > maxRenderUs) maxRenderUs = renderUs;
        if (gpuWaitUs > maxGpuWaitUs) maxGpuWaitUs = gpuWaitUs;
        if (totalUs > maxTotalUs) {
            maxTotalUs = totalUs;
            worstFrame = frames;
            worstStage = stage + loop * 6u;
            worstSprites = sprites;
            worstQuads = quads;
        }

        if (cpuUs > kCaveBudgetUs) ++cpuLagFrames;
        if (renderUs > kCaveBudgetUs) ++renderLagFrames;
        if (totalUs > kCaveBudgetUs) ++lagFrames;

        const uint32_t slot = (stage < 6u ? stage : 5u) + (loop ? 6u : 0u);
        ++stageFrames[slot];
        if (totalUs > kCaveBudgetUs) ++stageLagFrames[slot];
        if (totalUs > stageMaxTotalUs[slot]) stageMaxTotalUs[slot] = totalUs;
        if (cpuUs > stageMaxCpuUs[slot]) stageMaxCpuUs[slot] = cpuUs;
        if (renderUs > stageMaxRenderUs[slot]) stageMaxRenderUs[slot] = renderUs;
    }
};

// Exported global instance for zero-overhead live GDB inspection on Original Xbox / Xemu.
inline XboxPerfStats g_xboxPerf;

} // namespace spelunk_xbox
