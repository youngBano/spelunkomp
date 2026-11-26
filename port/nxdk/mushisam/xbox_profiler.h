#pragma once

// Hardware-backed Cave CV1000 (SH-3) vs Original Xbox CPU/NV2A/APU telemetry profiler (v2).
// Tracks per-stage microsecond budgets against CV1000's 16,662 us frame period (60.0184 Hz),
// Ring-0 Pentium III hardware PMC counters (optional on real Coppermine silicon),
// MmQueryStatistics() physical RAM watermarks, NV2A batch/upload stats, and SH-3 dispatch rates.
// Exposes a fixed-layout global struct (magic = "PERF") inspectable live via GDB.
#include <cstdint>
#include <cstring>

#ifdef NXDK
#include <xboxkrnl/xboxkrnl.h>
#endif

namespace spelunk_xbox {

// Optional Ring-0 Pentium III Coppermine Hardware Performance Monitoring Counters (MSRs 0x186/0x187, PMC0/1).
// Enable with -DXBOX_ENABLE_PMC when profiling on real Original Xbox silicon (Xemu may stub/ignore MSRs).
struct P3Pmc {
    static constexpr uint32_t kEvtIfuIfetchMiss   = 0x00410081u; // L1i cache misses (OS+USR+EN)
    static constexpr uint32_t kEvtDcuLinesIn      = 0x00410045u; // L1d cache line fills
    static constexpr uint32_t kEvtL2LinesIn       = 0x00410024u; // L2 cache misses -> DDR RAM
    static constexpr uint32_t kEvtBrMissPred      = 0x004100C5u; // Branch mispredictions retired
    static constexpr uint32_t kEvtInstRetired     = 0x004100C0u; // Instructions retired (for IPC)

    static inline void Configure(uint32_t evtSel0, uint32_t evtSel1) {
#if defined(NXDK) && defined(XBOX_ENABLE_PMC) && defined(__i386__)
        // PerfEvtSel0 (0x186) has global enable bit 22 (0x00400000).
        const uint32_t lo0 = evtSel0 | 0x00400000u;
        const uint32_t lo1 = evtSel1;
        __asm__ volatile("wrmsr" :: "c"(0x186u), "a"(lo0), "d"(0u));
        __asm__ volatile("wrmsr" :: "c"(0x187u), "a"(lo1), "d"(0u));
        __asm__ volatile("wrmsr" :: "c"(0x0C1u), "a"(0u), "d"(0u));
        __asm__ volatile("wrmsr" :: "c"(0x0C2u), "a"(0u), "d"(0u));
#else
        (void)evtSel0;
        (void)evtSel1;
#endif
    }

    static inline uint64_t Read(uint32_t counterIndex) {
#if defined(NXDK) && defined(XBOX_ENABLE_PMC) && defined(__i386__)
        uint32_t lo = 0, hi = 0;
        __asm__ volatile("rdpmc" : "=a"(lo), "=d"(hi) : "c"(counterIndex));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        (void)counterIndex;
        return 0;
#endif
    }
};

struct alignas(32) XboxPerfStats {
    static constexpr uint32_t kMagic = 0x50455246u; // "PERF"
    static constexpr uint32_t kVersion = 2u;
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

    // 4-way RDTSC subsystem breakdown: SH-3 CPU, YMZ770/AMM Audio, Blitter, NV2A Render
    uint32_t lastSh3Us = 0;
    uint32_t lastAudioUs = 0;
    uint32_t lastBlitterUs = 0;
    uint32_t maxSh3Us = 0;
    uint32_t maxAudioUs = 0;
    uint32_t maxBlitterUs = 0;
    uint64_t sumSh3Us = 0;
    uint64_t sumAudioUs = 0;
    uint64_t sumBlitterUs = 0;

    uint64_t nativeBlocks = 0;
    uint64_t interpretedBlocks = 0;
    uint32_t nandCacheMisses = 0;
    uint32_t rdtscPerUs = 0;

    // v2 Telemetry: NV2A pipeline stats, RAM watermarks, dispatch deltas, and Ring-0 PIII PMCs
    uint32_t version = kVersion;
    uint32_t lastDispatches = 0;
    uint32_t maxDispatches = 0;
    uint32_t lastNv2aBatches = 0;
    uint32_t lastNv2aStateChanges = 0;
    uint32_t lastNv2aUploads = 0;
    uint32_t freeRamPages = 0;
    uint32_t minFreeRamPages = 0xFFFFFFFFu;
    uint32_t totalRamPages = 0;
    uint32_t vramSwapCount = 0;
    uint64_t lastPmc0 = 0;
    uint64_t lastPmc1 = 0;

    void Reset() {
        std::memset(this, 0, sizeof(*this));
        magic = kMagic;
        version = kVersion;
        minFreeRamPages = 0xFFFFFFFFu;
    }

    void SampleRamStats() {
#ifdef NXDK
        MM_STATISTICS stats{};
        stats.Length = sizeof(stats);
        if (MmQueryStatistics(&stats) == STATUS_SUCCESS) {
            totalRamPages = stats.TotalPhysicalPages;
            freeRamPages = stats.AvailablePages;
            if (freeRamPages < minFreeRamPages) {
                minFreeRamPages = freeRamPages;
            }
        }
#endif
    }

    void RecordFrame(uint32_t cpuUs, uint32_t renderUs, uint32_t gpuWaitUs,
                     uint32_t sh3Us, uint32_t audioUs, uint32_t blitterUs,
                     uint64_t natBlocks, uint64_t interpBlocks,
                     uint32_t playing, uint32_t stage, uint32_t pages,
                     uint32_t nReads, uint32_t nMisses, uint32_t draws, uint32_t quads,
                     uint32_t nv2aBatches = 0, uint32_t nv2aStateChanges = 0,
                     uint32_t nv2aUploads = 0, uint32_t swaps = 0,
                     uint64_t pmc0 = 0, uint64_t pmc1 = 0) {
        const uint32_t totalUs = cpuUs + renderUs + gpuWaitUs;
        const uint32_t dispDelta = (natBlocks >= nativeBlocks)
                                       ? static_cast<uint32_t>(natBlocks - nativeBlocks)
                                       : 0u;
        ++frames;
        inPlay = playing;
        currentStage = stage;
        activeVramPages = pages;
        nandReads = nReads;
        nandCacheMisses = nMisses;
        activeDraws = draws;
        submittedQuads = quads;
        nativeBlocks = natBlocks;
        interpretedBlocks = interpBlocks;
        lastDispatches = dispDelta;
        if (dispDelta > maxDispatches) maxDispatches = dispDelta;
        lastNv2aBatches = nv2aBatches;
        lastNv2aStateChanges = nv2aStateChanges;
        lastNv2aUploads = nv2aUploads;
        vramSwapCount = swaps;
        lastPmc0 = pmc0;
        lastPmc1 = pmc1;

        if ((frames & 63u) == 1u) {
            SampleRamStats();
        }

        lastCpuUs = cpuUs;
        lastRenderUs = renderUs;
        lastGpuWaitUs = gpuWaitUs;
        lastTotalUs = totalUs;
        lastSh3Us = sh3Us;
        lastAudioUs = audioUs;
        lastBlitterUs = blitterUs;

        sumCpuUs += cpuUs;
        sumRenderUs += renderUs;
        sumGpuWaitUs += gpuWaitUs;
        sumTotalUs += totalUs;
        sumSh3Us += sh3Us;
        sumAudioUs += audioUs;
        sumBlitterUs += blitterUs;

        if (cpuUs > maxCpuUs) maxCpuUs = cpuUs;
        if (renderUs > maxRenderUs) maxRenderUs = renderUs;
        if (gpuWaitUs > maxGpuWaitUs) maxGpuWaitUs = gpuWaitUs;
        if (sh3Us > maxSh3Us) maxSh3Us = sh3Us;
        if (audioUs > maxAudioUs) maxAudioUs = audioUs;
        if (blitterUs > maxBlitterUs) maxBlitterUs = blitterUs;
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
