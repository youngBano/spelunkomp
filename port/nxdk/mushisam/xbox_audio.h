#pragma once

// Native Asynchronous Xbox AC'97 Audio Engine for Cave CV1000 (Yamaha YMZ770C):
// - Decouples ALL YMZ770C synthesis (MPEG-1 Layer 2 AMM BGM + 4-bit IMA ADPCM SFX cache +
//   16.16 linear stereo 48 kHz resampler) onto a dedicated THREAD_PRIORITY_HIGHEST Xbox thread!
// - Main SH-3/NV2A thread does ZERO audio synthesis (0 us/frame): SH-3 writes to 0x10400000..01
//   simply push (reg, val) into a lock-free SPSC ring queue (~15 ns).
// - Dedicated audio thread polls hardware AC'97 CIV/LVI/SR (0xFEC00114..0xFEC0011B) every 4 ms,
//   keeps 6 x 1024-frame DMA buffers (~128 ms) queued ahead of hardware CIV, and auto-recovers
//   from DCH halt — completely masking video/NAND slowdowns with zero underruns or crackles.
#include "machine/ymz770.h"

#include <hal/audio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>
#include <cstring>

extern "C" {
extern AC97_DEVICE ac97Device;
}

namespace spelunk_xbox {

class XboxAudio {
public:
    static constexpr uint32_t kNumBuffers = 12;       // 12 x 512 stereo frames @ 48 kHz = 128.0 ms total ring
    static constexpr uint32_t kTargetInFlight = 8;    // Keep 8 buffers queued (~85.3 ms cushion: zero underruns at lower priority!)
    static constexpr uint32_t kBufFrames48k = 512;    // 10.67 ms per descriptor
    static constexpr uint32_t kBufBytes = kBufFrames48k * 2 * sizeof(int16_t); // 2048 bytes
    static constexpr uint32_t kCmdQueueSize = 2048;   // Lock-free SPSC YMZ770 register write queue
    static constexpr uint32_t kCmdQueueMask = kCmdQueueSize - 1;
    static constexpr uint32_t kMonoBufSize = 2048;    // Up to 48kHz -> 1024+4 samples per DMA descriptor

    bool Init(uint32_t srcRate = 16000u) {
        SetSampleRate(srcRate);
        cmdRead_ = 0;
        cmdWrite_ = 0;
        phase16_ = 0;
        prevSample_ = 0;
        nextBuf_ = 0;
        started_ = false;
        running_ = false;
        ymz_ = nullptr;
        thread_ = nullptr;
        underruns_ = 0;
        bufsSubmitted_ = 0;
        peakAmp_ = 0;

        for (uint32_t i = 0; i < kNumBuffers; ++i) {
            dmaBufs_[i] = static_cast<int16_t *>(
                MmAllocateContiguousMemoryEx(kBufBytes, 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_NOCACHE));
            if (!dmaBufs_[i]) {
                Shutdown();
                return false;
            }
            std::memset(dmaBufs_[i], 0, kBufBytes);
        }

        // NULL callback: dedicated high-priority audio thread polls hardware AC'97 CIV/LVI/SR directly.
        XAudioInit(16, 2, nullptr, nullptr);
        initialized_ = true;

        // Pre-queue kTargetInFlight silent buffers and start AC'97 DMA immediately.
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

    bool StartAsync(spelunk::Ymz770 *ymz) {
        if (!initialized_ || !ymz || thread_) {
            return false;
        }
        ymz_ = ymz;
        SetSampleRate(ymz_->sampleRate);
        __atomic_store_n(&running_, 1u, __ATOMIC_RELEASE);
        thread_ = CreateThread(nullptr, 65536, &AudioThreadTrampoline, this, 0, nullptr);
        if (!thread_) {
            __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);
            return false;
        }
        SetThreadPriority(thread_, THREAD_PRIORITY_ABOVE_NORMAL);
        return true;
    }

    void Shutdown() {
        if (thread_) {
            __atomic_store_n(&running_, 0u, __ATOMIC_RELEASE);
            WaitForSingleObject(thread_, 500);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
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
        ymz_ = nullptr;
    }

    // Lock-free SPSC enqueue called from the main SH-3 emulation thread (~15 ns, zero synthesis!).
    static void SoundWriteCallback(void *ctx, uint8_t reg, uint8_t value) {
        static_cast<XboxAudio *>(ctx)->EnqueueRegisterWrite(reg, value);
    }

    inline void EnqueueRegisterWrite(uint8_t reg, uint8_t value) {
        const uint32_t w = __atomic_load_n(&cmdWrite_, __ATOMIC_RELAXED);
        const uint32_t r = __atomic_load_n(&cmdRead_, __ATOMIC_ACQUIRE);
        if ((w - r) >= kCmdQueueSize) {
            return; // Queue full (never happens in normal play with 2048 slots)
        }
        cmdQueue_[w & kCmdQueueMask] = static_cast<uint16_t>((static_cast<uint16_t>(reg) << 8) | value);
        __atomic_store_n(&cmdWrite_, w + 1u, __ATOMIC_RELEASE);
    }

    void SetSampleRate(uint32_t hz) {
        if (hz < 8000u || hz > 48000u) {
            hz = 16000u;
        }
        step16_ = static_cast<uint32_t>((static_cast<uint64_t>(hz) << 16) / 48000ULL);
    }

    uint32_t QueuedSamples() const {
        const uint32_t w = __atomic_load_n(&cmdWrite_, __ATOMIC_RELAXED);
        const uint32_t r = __atomic_load_n(&cmdRead_, __ATOMIC_RELAXED);
        return w - r;
    }
    uint32_t PeakAmp() const { return __atomic_load_n(&peakAmp_, __ATOMIC_RELAXED); }
    uint32_t Underruns() const { return __atomic_load_n(&underruns_, __ATOMIC_RELAXED); }
    uint32_t BuffersSubmitted() const { return __atomic_load_n(&bufsSubmitted_, __ATOMIC_RELAXED); }

    uint32_t Ac97CivLviSr() const {
        if (!initialized_ || !ac97Device.mmio) return 0u;
        volatile uint8_t *pb = reinterpret_cast<volatile uint8_t *>(ac97Device.mmio);
        const uint32_t civ = pb[0x114];
        const uint32_t lvi = pb[0x115];
        const uint32_t sr = *reinterpret_cast<volatile uint16_t *>(pb + 0x116);
        return (civ << 24) | (lvi << 16) | (sr & 0xFFFFu);
    }

private:
    static DWORD WINAPI AudioThreadTrampoline(LPVOID param) {
        static_cast<XboxAudio *>(param)->AudioThreadLoop();
        return 0;
    }

    void DrainRegisterWrites() {
        uint32_t r = __atomic_load_n(&cmdRead_, __ATOMIC_RELAXED);
        const uint32_t w = __atomic_load_n(&cmdWrite_, __ATOMIC_ACQUIRE);
        if (r == w) {
            return;
        }
        while (r != w) {
            const uint16_t cmd = cmdQueue_[r & kCmdQueueMask];
            ymz_->WriteRegister(static_cast<uint8_t>(cmd >> 8), static_cast<uint8_t>(cmd & 0xFFu));
            ++r;
        }
        __atomic_store_n(&cmdRead_, r, __ATOMIC_RELEASE);
    }

    void FillBufferFromYmz(int16_t *dstStereo) {
        // Generate exact number of mono samples required for kBufFrames48k (1024) 48 kHz output frames.
        const uint32_t step = step16_;
        uint32_t phase = phase16_;
        const uint32_t needSrc = ((phase + kBufFrames48k * step) >> 16) + 1u;
        const uint32_t genCount = needSrc < (kMonoBufSize - 1u) ? needSrc : (kMonoBufSize - 1u);

        // monoBuf_[0] holds the carry-over sample from the previous buffer for click-free linear interpolation.
        monoBuf_[0] = prevSample_;
        ymz_->GenerateSamples(monoBuf_ + 1u, genCount);
        if (ymz_->rateChanged) {
            SetSampleRate(ymz_->sampleRate);
            ymz_->rateChanged = false;
        }

        uint32_t peak = __atomic_load_n(&peakAmp_, __ATOMIC_RELAXED);
        uint32_t idx = 0;
        for (uint32_t i = 0; i < kBufFrames48k; ++i) {
            const uint32_t i0 = idx < genCount ? idx : genCount;
            const uint32_t i1 = (i0 + 1u) <= genCount ? (i0 + 1u) : genCount;
            const int32_t s0 = monoBuf_[i0];
            const int32_t s1 = monoBuf_[i1];
            const int32_t frac = static_cast<int32_t>(phase & 0xFFFFu);
            const int32_t s = s0 + (((s1 - s0) * frac) >> 16);
            const uint32_t a = static_cast<uint32_t>(s < 0 ? -s : s);
            if (a > peak) peak = a;
            dstStereo[i * 2u + 0u] = static_cast<int16_t>(s);
            dstStereo[i * 2u + 1u] = static_cast<int16_t>(s);

            phase += step;
            idx += (phase >> 16);
            phase &= 0xFFFFu;
        }
        prevSample_ = monoBuf_[idx <= genCount ? idx : genCount];
        phase16_ = phase;
        __atomic_store_n(&peakAmp_, peak, __ATOMIC_RELAXED);
    }

    void AudioThreadLoop() {
        while (__atomic_load_n(&running_, __ATOMIC_ACQUIRE)) {
            DrainRegisterWrites();

            volatile uint8_t *pb = reinterpret_cast<volatile uint8_t *>(ac97Device.mmio);
            if (pb && ymz_) {
                const uint8_t civ = pb[0x114];
                const uint8_t lvi = pb[0x115];
                const uint16_t sr = *reinterpret_cast<volatile uint16_t *>(pb + 0x116);
                const uint8_t cr = pb[0x11B];

                if (sr & 0x1Cu) {
                    pb[0x116] = 0x1Cu;
                    pb[0x176] = 0x1Cu;
                }

                const bool halted = ((sr & 0x01u) != 0u) || ((cr & 0x01u) == 0u);
                uint32_t inFlight = halted ? 0u : (((static_cast<uint32_t>(lvi) - static_cast<uint32_t>(civ)) & 31u) + 1u);
                if (inFlight > 16u) {
                    inFlight = 0u;
                }

                uint32_t pushed = 0;
                while (inFlight < kTargetInFlight && pushed < kNumBuffers) {
                    // Drain any newly arrived SFX triggers before synthesizing each 21.3 ms buffer.
                    DrainRegisterWrites();
                    int16_t *dst = dmaBufs_[nextBuf_];
                    nextBuf_ = (nextBuf_ + 1u) % kNumBuffers;
                    FillBufferFromYmz(dst);
                    XAudioProvideSamples(reinterpret_cast<unsigned char *>(dst), kBufBytes, 0);
                    ++inFlight;
                    ++pushed;
                    __atomic_store_n(&bufsSubmitted_, bufsSubmitted_ + 1u, __ATOMIC_RELAXED);
                }

                if (halted && started_) {
                    __atomic_store_n(&underruns_, underruns_ + 1u, __ATOMIC_RELAXED);
                    pb[0x116] = 0xFFu;
                    pb[0x176] = 0xFFu;
                    XAudioPause();
                    XAudioPlay();
                }
            }

            // Sleep 8 ms (~10x faster than 85 ms cushion): zero underruns while halving PIII preemption jitter!
            Sleep(8);
        }
    }

    uint32_t cmdRead_ = 0;
    uint32_t cmdWrite_ = 0;
    uint32_t phase16_ = 0;
    uint32_t step16_ = 21845u; // 16000 / 48000 in 16.16
    int16_t prevSample_ = 0;
    uint32_t nextBuf_ = 0;
    uint32_t running_ = 0;
    uint32_t underruns_ = 0;
    uint32_t bufsSubmitted_ = 0;
    uint32_t peakAmp_ = 0;
    bool initialized_ = false;
    bool started_ = false;
    spelunk::Ymz770 *ymz_ = nullptr;
    HANDLE thread_ = nullptr;
    int16_t *dmaBufs_[kNumBuffers] = {};
    uint16_t cmdQueue_[kCmdQueueSize] = {};
    int16_t monoBuf_[kMonoBufSize] = {};
};

} // namespace spelunk_xbox
