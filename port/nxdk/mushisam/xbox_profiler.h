#pragma once

// Hardware-backed Cave CV1000 (SH-3) vs Original Xbox CPU/NV2A timing lag profiler.
// Tracks per-stage microsecond budgets against CV1000's 16,662 us frame period (60.0184 Hz)
// and exposes a fixed-layout global struct (magic = "PERF") inspectable live via GDB.
#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

struct alignas(32) XboxPerfStats {
    static constexpr uint32_t kMagic = 0x50455246u; // "PERF"
    static constexpr uint32_t kCv1kBudgetUs = 16662u;
    static constexpr int kMaxStageSlots = 6; // Stages 1..5 + extra

    uint32_t magic = kMagic;
    uint32_t frames = 0;
    uint32_t lagFrames = 0;
    uint32_t cpuLagFrames = 0;
    uint32_t renderLagFrames = 0;
    uint32_t activeVramPages = 0;

    uint32_t inPlay = 0;
    uint32_t currentStage = 0;
    uint32_t nandReads = 0;
    uint32_t activeDraws = 0;
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
    uint32_t worstDraws = 0;
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
                     uint32_t playing, uint32_t stage, uint32_t pages,
                     uint32_t nReads, uint32_t draws, uint32_t quads) {
        const uint32_t totalUs = cpuUs + renderUs + gpuWaitUs;
        ++frames;
        inPlay = playing;
        currentStage = stage;
        activeVramPages = pages;
        nandReads = nReads;
        activeDraws = draws;
        submittedQuads = quads;

        lastCpuUs = cpuUs;
        lastRenderUs = renderUs;
        lastGpuWaitUs = gpuWaitUs;
        lastTotalUs = totalUs;

        sumCpuUs += cpuUs;
        sumRenderUs += renderUs;
        sumGpuWaitUs += gpuWaitUs;
        sumTotalUs += totalUs;

        if (cpuUs > maxCpuUs) maxCpuUs = cpuUs;
        if (renderUs > maxRenderUs) maxRenderUs = renderUs;
        if (gpuWaitUs > maxGpuWaitUs) maxGpuWaitUs = gpuWaitUs;
        if (totalUs > maxTotalUs) {
            maxTotalUs = totalUs;
            worstFrame = frames;
            worstStage = stage;
            worstDraws = draws;
            worstQuads = quads;
        }

        if (cpuUs > kCv1kBudgetUs) ++cpuLagFrames;
        if (renderUs > kCv1kBudgetUs) ++renderLagFrames;
        if (totalUs > kCv1kBudgetUs) ++lagFrames;

        const uint32_t slot = stage < static_cast<uint32_t>(kMaxStageSlots) ? stage : 5u;
        ++stageFrames[slot];
        if (totalUs > kCv1kBudgetUs) ++stageLagFrames[slot];
        if (totalUs > stageMaxTotalUs[slot]) stageMaxTotalUs[slot] = totalUs;
        if (cpuUs > stageMaxCpuUs[slot]) stageMaxCpuUs[slot] = cpuUs;
        if (renderUs > stageMaxRenderUs[slot]) stageMaxRenderUs[slot] = renderUs;
    }
};

inline XboxPerfStats g_xboxPerf;

} // namespace spelunk_xbox
