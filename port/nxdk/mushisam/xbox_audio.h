#pragma once

// Zero-DPC direct AC'97 hardware ring audio driver for Original Xbox (Cave CV1000 YMZ770C).
// Bypasses nxdk hal/audio.c's single-DPC coalescing + analogDrained permanent latchup by
// initializing XAudioInit(16, 2, nullptr, nullptr) and polling hardware AC'97 CIV/LVI/SR
// (0xFEC00114..0xFEC0011B) directly from the main loop with automatic DCH halt recovery.
#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>
#include <cstring>

extern "C" {
extern AC97_DEVICE ac97Device;
}

namespace spelunk_xbox {

class XboxAudio {
public:
    static constexpr uint32_t kNumBuffers = 8;        // 8 x 1024 stereo frames @ 48 kHz = 170.7 ms cushion
    static constexpr uint32_t kTargetInFlight = 6;    // Keep 6 buffers queued (~128 ms ahead of hardware CIV)
    static constexpr uint32_t kBufFrames48k = 1024;   // 21.33 ms per descriptor
    static constexpr uint32_t kBufBytes = kBufFrames48k * 2 * sizeof(int16_t); // 4096 bytes
    static constexpr uint32_t kRingSize = 32768;
    static constexpr uint32_t kRingMask = kRingSize - 1;
    static constexpr uint32_t kMaxQueued = 16384;

    bool Init(uint32_t srcRate = 16000u) {
        SetSampleRate(srcRate);
        ringRead_ = 0;
        ringWrite_ = 0;
        phase16_ = 0;
        lastSample_ = 0;
        nextBuf_ = 0;
        started_ = false;
        underruns_ = 0;
        bufsSubmitted_ = 0;
        peakAmp_ = 0;
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

        // NULL callback: main loop polls AC'97 hardware CIV/LVI/SR directly via Pump().
        XAudioInit(16, 2, nullptr, nullptr);
        initialized_ = true;

        // Pre-queue kTargetInFlight silent buffers and start AC'97 immediately so CIV begins advancing.
        for (uint32_t i = 0; i < kTargetInFlight; ++i) {
            int16_t *dst = dmaBufs_[nextBuf_];
            nextBuf_ = (nextBuf_ + 1u) % kNumBuffers;
            std::memset(dst, 0, kBufBytes);
            XAudioProvideSamples(reinterpret_cast<unsigned char *>(dst), kBufBytes, 0);
            ++bufsSubmitted_;
        }
        XAudioPlay();
        started_ = true;
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
            started_ = false;
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
        uint32_t peak = peakAmp_;
        for (size_t i = 0; i < count; ++i) {
            const int16_t s = samples[i];
            const uint32_t a = static_cast<uint32_t>(s < 0 ? -static_cast<int32_t>(s) : static_cast<int32_t>(s));
            if (a > peak) peak = a;
            ring_[w & kRingMask] = s;
            ++w;
        }
        peakAmp_ = peak;
        ringWrite_ = w;
    }

    // Poll hardware AC'97 PCM-Out busmaster registers (0xFEC00114..0xFEC0011B), replenish all
    // consumed descriptors up to kTargetInFlight, and auto-recover if DCH (DMA Halted) tripped.
    void Pump() {
        if (!initialized_) {
            return;
        }
        volatile uint8_t *pb = reinterpret_cast<volatile uint8_t *>(ac97Device.mmio);
        if (!pb) {
            return;
        }

        const uint8_t civ = pb[0x114];
        const uint8_t lvi = pb[0x115];
        const uint16_t sr = *reinterpret_cast<volatile uint16_t *>(pb + 0x116);
        const uint8_t cr = pb[0x11B];

        // Clear any pending status flags (BCIS / LVBCI / FIFOE) so hardware never stalls on uncleared IRQ bits.
        if (sr & 0x1Cu) {
            pb[0x116] = 0x1Cu;
            pb[0x176] = 0x1Cu;
        }

        const bool halted = ((sr & 0x01u) != 0u) || ((cr & 0x01u) == 0u);
        uint32_t inFlight = halted ? 0u : (((static_cast<uint32_t>(lvi) - static_cast<uint32_t>(civ)) & 31u) + 1u);
        if (inFlight > 16u) {
            // Wraparound guard if CIV raced just past LVI before DCH latched.
            inFlight = 0u;
        }

        // Submit enough buffers to keep kTargetInFlight descriptors queued ahead of CIV.
        uint32_t pushed = 0;
        while (inFlight < kTargetInFlight && pushed < kNumBuffers) {
            int16_t *dst = dmaBufs_[nextBuf_];
            nextBuf_ = (nextBuf_ + 1u) % kNumBuffers;
            FillStereo48k(dst, kBufFrames48k);
            XAudioProvideSamples(reinterpret_cast<unsigned char *>(dst), kBufBytes, 0);
            ++inFlight;
            ++pushed;
            ++bufsSubmitted_;
        }

        // Auto-recover from DCH halt (e.g. after a long boot or stage-load NAND stall).
        if (halted && started_) {
            ++underruns_;
            pb[0x116] = 0xFFu;
            pb[0x176] = 0xFFu;
            XAudioPause();
            XAudioPlay();
        }
    }

    uint32_t QueuedSamples() const { return ringWrite_ - ringRead_; }
    uint32_t PeakAmp() const { return peakAmp_; }
    uint32_t Underruns() const { return underruns_; }
    uint32_t BuffersSubmitted() const { return bufsSubmitted_; }

    uint32_t Ac97CivLviSr() const {
        if (!initialized_ || !ac97Device.mmio) return 0u;
        volatile uint8_t *pb = reinterpret_cast<volatile uint8_t *>(ac97Device.mmio);
        const uint32_t civ = pb[0x114];
        const uint32_t lvi = pb[0x115];
        const uint32_t sr = *reinterpret_cast<volatile uint16_t *>(pb + 0x116);
        return (civ << 24) | (lvi << 16) | (sr & 0xFFFFu);
    }

private:
    void FillStereo48k(int16_t *dstStereo, uint32_t frames) {
        uint32_t r = ringRead_;
        const uint32_t w = ringWrite_;
        uint32_t phase = phase16_;
        const uint32_t step = step16_;
        int16_t last = lastSample_;

        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t avail = w - r;
            int32_t s = 0;
            if (avail >= 2u) {
                const int32_t s0 = ring_[r & kRingMask];
                const int32_t s1 = ring_[(r + 1u) & kRingMask];
                const int32_t frac = static_cast<int32_t>(phase & 0xFFFFu);
                s = s0 + (((s1 - s0) * frac) >> 16);
                phase += step;
                r += (phase >> 16);
                phase &= 0xFFFFu;
                last = static_cast<int16_t>(s);
            } else if (avail == 1u) {
                s = ring_[r & kRingMask];
                phase += step;
                if (phase >= 0x10000u) {
                    ++r;
                    phase &= 0xFFFFu;
                }
                last = static_cast<int16_t>(s);
            } else {
                // Smooth exponential decay to 0 on underrun (zero DC pop/click).
                last = static_cast<int16_t>(last >> 1);
                s = last;
            }
            dstStereo[i * 2u + 0u] = static_cast<int16_t>(s);
            dstStereo[i * 2u + 1u] = static_cast<int16_t>(s);
        }

        ringRead_ = r;
        phase16_ = phase;
        lastSample_ = last;
    }

    uint32_t ringRead_ = 0;
    uint32_t ringWrite_ = 0;
    uint32_t phase16_ = 0;
    uint32_t step16_ = 21845u; // 16000 / 48000 in 16.16
    int16_t lastSample_ = 0;
    uint32_t nextBuf_ = 0;
    uint32_t underruns_ = 0;
    uint32_t bufsSubmitted_ = 0;
    uint32_t peakAmp_ = 0;
    bool initialized_ = false;
    bool started_ = false;
    int16_t *dmaBufs_[kNumBuffers] = {};
    int16_t ring_[kRingSize] = {};
};

} // namespace spelunk_xbox
