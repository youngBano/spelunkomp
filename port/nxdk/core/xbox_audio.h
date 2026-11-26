#pragma once

// Native Original Xbox AC'97 DMA audio backend via hal/audio.h (replaces SDL2 Audio).
// Uses a lock-free SPSC ring buffer and integer 16.16 fixed-point resampling (44.1 kHz mono -> 48 kHz stereo)
// so the DPC interrupt callback never touches x87/SSE floating-point state.
#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

class XboxAudio {
public:
    static constexpr int kNumDmaBuffers = 2;
    static constexpr uint16_t kDmaFrames = 1024; // ~21.3 ms @ 48 kHz stereo
    static constexpr uint16_t kDmaBytes = kDmaFrames * 2 * sizeof(int16_t);
    static constexpr uint32_t kRingSize = 16384; // power of 2 (~371 ms @ 44.1 kHz)
    static constexpr uint32_t kRingMask = kRingSize - 1;
    static constexpr uint32_t kMaxQueued = 11025; // 250 ms cap @ 44.1 kHz
    static constexpr uint32_t kResampleStep16 = (44100u << 16) / 48000u; // 60211

    bool Init() {
        for (int i = 0; i < kNumDmaBuffers; ++i) {
            dmaBuf_[i] = static_cast<int16_t *>(
                MmAllocateContiguousMemoryEx(kDmaBytes, 0, 0x3FFAFFF, 0, PAGE_READWRITE | PAGE_WRITECOMBINE));
            if (!dmaBuf_[i]) {
                Shutdown();
                return false;
            }
            std::memset(dmaBuf_[i], 0, kDmaBytes);
        }
        ringRead_ = 0;
        ringWrite_ = 0;
        phase16_ = 0;
        lastSample_ = 0;
        curDma_ = 0;

        XAudioInit(16, 2, &DpcCallback, this);
        for (int i = 0; i < kNumDmaBuffers; ++i) {
            FillAndSubmit();
        }
        XAudioPlay();
        initialized_ = true;
        return true;
    }

    void Shutdown() {
        if (initialized_) {
            XAudioPause();
            initialized_ = false;
        }
        for (int i = 0; i < kNumDmaBuffers; ++i) {
            if (dmaBuf_[i]) {
                MmFreeContiguousMemory(dmaBuf_[i]);
                dmaBuf_[i] = nullptr;
            }
        }
    }

    // Push 44.1 kHz 16-bit mono PCM samples from the game thread.
    void PushMono44k(const int16_t *samples, size_t count) {
        if (!initialized_ || !samples || count == 0) {
            return;
        }
        uint32_t w = ringWrite_;
        const uint32_t r = ringRead_;
        const uint32_t queued = w - r;
        if (queued >= kMaxQueued) {
            return;
        }
        const uint32_t space = kMaxQueued - queued;
        const uint32_t toWrite = (count < space) ? static_cast<uint32_t>(count) : space;
        for (uint32_t i = 0; i < toWrite; ++i) {
            ring_[w & kRingMask] = samples[i];
            ++w;
        }
        __asm__ volatile("" ::: "memory");
        ringWrite_ = w;
    }

private:
    static void DpcCallback(void * /*pac97Device*/, void *data) {
        if (data) {
            static_cast<XboxAudio *>(data)->FillAndSubmit();
        }
    }

    // Runs in AC'97 DPC context: integer-only (no FPU/SSE).
    void FillAndSubmit() {
        int16_t *dst = dmaBuf_[curDma_];
        uint32_t r = ringRead_;
        const uint32_t w = ringWrite_;
        uint32_t phase = phase16_;
        int16_t cur = lastSample_;

        for (uint16_t i = 0; i < kDmaFrames; ++i) {
            const uint32_t avail = w - r;
            if (avail > 1) {
                const int32_t s0 = ring_[r & kRingMask];
                const int32_t s1 = ring_[(r + 1) & kRingMask];
                const int32_t frac = static_cast<int32_t>(phase & 0xFFFFu);
                cur = static_cast<int16_t>(s0 + (((s1 - s0) * frac) >> 16));
                phase += kResampleStep16;
                r += (phase >> 16);
                phase &= 0xFFFFu;
            } else if (avail == 1) {
                cur = ring_[r & kRingMask];
                phase += kResampleStep16;
                if (phase >= 0x10000u) {
                    ++r;
                    phase &= 0xFFFFu;
                }
            } else {
                // Underrun: decay smoothly to zero to avoid pops.
                cur = static_cast<int16_t>(cur >> 1);
            }
            dst[i * 2 + 0] = cur;
            dst[i * 2 + 1] = cur;
        }

        ringRead_ = r;
        phase16_ = phase;
        lastSample_ = cur;

        XAudioProvideSamples(reinterpret_cast<unsigned char *>(dst), kDmaBytes, 0);
        curDma_ = (curDma_ + 1) % kNumDmaBuffers;
    }

    int16_t *dmaBuf_[kNumDmaBuffers] = {};
    int16_t ring_[kRingSize] = {};
    volatile uint32_t ringRead_ = 0;
    volatile uint32_t ringWrite_ = 0;
    uint32_t phase16_ = 0;
    int16_t lastSample_ = 0;
    int curDma_ = 0;
    bool initialized_ = false;
};

} // namespace spelunk_xbox
