#pragma once

// Native Original Xbox AC'97 DMA audio driver (hal/audio.h, zero SDL2) for CV1000 YMZ770C.
// Supports dynamic YMZ770C sample rates (16 kHz default -> 48 kHz stereo) via pure-integer ASM DPC resampler.
#include "xbox_asm.h"

#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>
#include <cstring>

namespace spelunk_xbox {

class XboxAudio {
public:
    static constexpr uint32_t kNumBuffers = 4;
    // 2048 stereo frames @ 48 kHz = 42.67 ms per DMA buffer (170.7 ms total cushion):
    // prevents nxdk hal/audio.c analogDrained latchup during boot/stage-load spikes!
    static constexpr uint32_t kBufFrames48k = 2048;
    static constexpr uint32_t kBufBytes = kBufFrames48k * 2 * sizeof(int16_t);
    static constexpr uint32_t kRingSize = 32768;
    static constexpr uint32_t kRingMask = kRingSize - 1;
    static constexpr uint32_t kMaxQueued = 16384;

    bool Init(uint32_t srcRate = 16000u) {
        SetSampleRate(srcRate);
        ringRead_ = 0;
        ringWrite_ = 0;
        phase16_ = 0;
        lastSample_ = 0;
        playing_ = false;
        std::memset(ring_, 0, sizeof(ring_));

        for (uint32_t i = 0; i < kNumBuffers; ++i) {
            dmaBufs_[i] = static_cast<int16_t *>(
                MmAllocateContiguousMemoryEx(kBufBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_NOCACHE));
            if (!dmaBufs_[i]) {
                Shutdown();
                return false;
            }
            std::memset(dmaBufs_[i], 0, kBufBytes);
        }

        XAudioInit(16, 2, &XboxAudio::AudioCallback, this);
        for (uint32_t i = 0; i < kNumBuffers; ++i) {
            XAudioProvideSamples(reinterpret_cast<unsigned char *>(dmaBufs_[i]), kBufBytes, 0);
        }
        initialized_ = true;
        return true;
    }

    void SetSampleRate(uint32_t hz) {
        if (hz < 8000u || hz > 48000u) {
            hz = 16000u;
        }
        step16_ = static_cast<uint32_t>((static_cast<uint64_t>(hz) << 16) / 48000ULL);
    }

    void Shutdown() {
        if (initialized_) {
            XAudioPause();
            initialized_ = false;
        }
        for (uint32_t i = 0; i < kNumBuffers; ++i) {
            if (dmaBufs_[i]) {
                MmFreeContiguousMemory(dmaBufs_[i]);
                dmaBufs_[i] = nullptr;
            }
        }
    }

    void PushMono(const int16_t *samples, size_t count) {
        if (!initialized_ || !samples || count == 0) {
            return;
        }
        uint32_t w = ringWrite_;
        const uint32_t r = ringRead_;
        const uint32_t used = w - r;
        if (used >= kMaxQueued) {
            return;
        }
        uint32_t space = kMaxQueued - used;
        if (count > space) {
            count = space;
        }
        for (size_t i = 0; i < count; ++i) {
            ring_[w & kRingMask] = samples[i];
            ++w;
        }
        __asm__ volatile("" ::: "memory");
        ringWrite_ = w;
        // Start AC'97 DMA once we have ~25 ms of audio buffered so startup never starves hal/audio.c.
        if (!playing_ && (w - r) >= 400u) {
            playing_ = true;
            XAudioPlay();
        }
    }

private:
    static void AudioCallback(void *pac97Device, void *data) {
        (void)pac97Device;
        auto *self = static_cast<XboxAudio *>(data);
        if (!self || !self->initialized_) {
            return;
        }
        int16_t *dst = self->dmaBufs_[self->nextBuf_];
        self->nextBuf_ = (self->nextBuf_ + 1) % kNumBuffers;
        self->FillStereo48k(dst, kBufFrames48k);
        XAudioProvideSamples(reinterpret_cast<unsigned char *>(dst), kBufBytes, 0);
    }

    void FillStereo48k(int16_t *dstStereo, uint32_t frames) {
        xbox_resample_mono_to_stereo_asm(
            ring_, kRingMask, &ringRead_, ringWrite_,
            &phase16_, &lastSample_, dstStereo, frames, step16_);
    }

    volatile uint32_t ringRead_ = 0;
    uint32_t phase16_ = 0;
    int16_t lastSample_ = 0;
    uint32_t nextBuf_ = 0;
    volatile uint32_t step16_ = 21845u; // 16000 / 48000 in 16.16
    uint8_t padLine1_[16] = {};

    volatile uint32_t ringWrite_ = 0;
    bool initialized_ = false;
    bool playing_ = false;
    int16_t *dmaBufs_[kNumBuffers] = {};
    uint8_t padLine2_[12] = {};
    int16_t ring_[kRingSize] = {};
};

} // namespace spelunk_xbox
