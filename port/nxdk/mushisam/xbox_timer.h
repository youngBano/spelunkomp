#pragma once

// Native Original Xbox hardware timer & Cave CV1000 exact arcade frame pacing (60.0184 Hz).
// Uses KeQueryPerformanceCounter / KeQueryPerformanceFrequency (3.579545 MHz MCPX hardware timer)
// and Pentium III RDTSC directly.
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>

namespace spelunk_xbox {

class XboxTimer {
public:
    // Cave CV1000 arcade frame period: 262 lines * 407 dots @ 6.4 MHz = 16,661,562 ns (~60.0184 Hz).
    static constexpr uint64_t kCv1kFrameNs = 16661562ULL;

    void Init() {
        freq_ = static_cast<uint64_t>(KeQueryPerformanceFrequency());
        if (freq_ == 0) {
            freq_ = 3579545ULL;
        }
        ticksPerArcadeFrame_ = (freq_ * kCv1kFrameNs + 500000000ULL) / 1000000000ULL;
        maxAccumTicks_ = ticksPerArcadeFrame_ * 2ULL;
        lastTick_ = NowTicks();
        accumTicks_ = ticksPerArcadeFrame_;
    }

    static inline uint64_t NowTicks() {
        return static_cast<uint64_t>(KeQueryPerformanceCounter());
    }

    static inline uint64_t ReadRdtsc() {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    bool ConsumeArcadeFrame() {
        const uint64_t now = NowTicks();
        const uint64_t delta = (now >= lastTick_) ? (now - lastTick_) : 0ULL;
        lastTick_ = now;
        accumTicks_ += delta;
        if (accumTicks_ > maxAccumTicks_) {
            accumTicks_ = maxAccumTicks_;
        }
        if (accumTicks_ >= ticksPerArcadeFrame_) {
            accumTicks_ -= ticksPerArcadeFrame_;
            return true;
        }
        return false;
    }

    uint64_t TicksToMicros(uint64_t ticks) const {
        return (ticks * 1000000ULL) / freq_;
    }

    uint64_t Frequency() const { return freq_; }
    uint64_t TicksPerArcadeFrame() const { return ticksPerArcadeFrame_; }

private:
    uint64_t freq_ = 3579545ULL;
    uint64_t ticksPerArcadeFrame_ = 59641ULL;
    uint64_t maxAccumTicks_ = 119282ULL;
    uint64_t lastTick_ = 0;
    uint64_t accumTicks_ = 0;
};

} // namespace spelunk_xbox
