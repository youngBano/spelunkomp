#pragma once

// Native Original Xbox AC'97 DMA audio backend via hal/audio.h (replaces SDL2 Audio).
// Uses a lock-free SPSC ring buffer and integer 16.16 fixed-point resampling (44.1 kHz mono -> 48 kHz stereo)
// so the DPC interrupt callback never touches x87/SSE floating-point state.
#include "xbox_asm.h"
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

    // Runs in AC'97 DPC context: pure i386 integer assembly (zero FPU/SSE touched).
    void FillAndSubmit() {
        int16_t *dst = dmaBuf_[curDma_];
        xbox_resample_mono_to_stereo_asm(ring_, kRingMask, &ringRead_, ringWrite_,
                                         &phase16_, &lastSample_, dst, kDmaFrames, kResampleStep16);
        XAudioProvideSamples(reinterpret_cast<unsigned char *>(dst), kDmaBytes, 0);
        curDma_ = (curDma_ + 1) % kNumDmaBuffers;
    }

    int16_t *dmaBuf_[kNumDmaBuffers] = {};
    // 32-byte Pentium III cache-line separation prevents false sharing between DPC consumer and game producer.
    alignas(32) int16_t ring_[kRingSize] = {};
    alignas(32) volatile uint32_t ringRead_ = 0;
    uint32_t phase16_ = 0;
    int16_t lastSample_ = 0;
    int curDma_ = 0;
    alignas(32) volatile uint32_t ringWrite_ = 0;
    bool initialized_ = false;
};

} // namespace spelunk_xbox
