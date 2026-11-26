#pragma once

// Native Original Xbox hardware timer & Cave 68000 exact arcade frame pacing (57.44485 Hz).
// Uses KeQueryPerformanceCounter / KeQueryPerformanceFrequency (3.579545 MHz MCPX hardware timer)
// and Pentium III RDTSC directly instead of host/SDL/hw_cave timers.
#include <xboxkrnl/xboxkrnl.h>

#include <cstdint>

namespace spelunk_xbox {

class XboxTimer {
public:
    // Cave 68000 arcade frame period: 272 lines @ 15,625 Hz = 17,408,000 ns (~57.44485 Hz).
    static constexpr uint64_t kCaveFrameNs = 17408000ULL;

    void Init() {
        freq_ = static_cast<uint64_t>(KeQueryPerformanceFrequency());
        if (freq_ == 0) {
            freq_ = 3579545ULL;
        }
        ticksPerCaveFrame_ = (freq_ * kCaveFrameNs + 500000000ULL) / 1000000000ULL;
        maxAccumTicks_ = ticksPerCaveFrame_ * 2ULL;
        lastTick_ = NowTicks();
        accumTicks_ = ticksPerCaveFrame_; // Run first frame immediately.
    }

    static inline uint64_t NowTicks() {
        return static_cast<uint64_t>(KeQueryPerformanceCounter());
    }

    static inline uint64_t ReadRdtsc() {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }

    // Advances hardware timer accumulator and returns true if a Cave 68000 frame is due
    // (paces 60 Hz Xbox VSync smoothly to exact 57.44485 Hz arcade cadence).
    bool ConsumeArcadeFrame() {
        const uint64_t now = NowTicks();
        const uint64_t delta = (now >= lastTick_) ? (now - lastTick_) : 0ULL;
        lastTick_ = now;
        accumTicks_ += delta;
        if (accumTicks_ > maxAccumTicks_) {
            accumTicks_ = maxAccumTicks_;
        }
        if (accumTicks_ >= ticksPerCaveFrame_) {
            accumTicks_ -= ticksPerCaveFrame_;
            return true;
        }
        return false;
    }

    uint64_t TicksToMicros(uint64_t ticks) const {
        return (ticks * 1000000ULL) / freq_;
    }

    uint64_t Frequency() const { return freq_; }
    uint64_t TicksPerCaveFrame() const { return ticksPerCaveFrame_; }

private:
    uint64_t freq_ = 3579545ULL;
    uint64_t ticksPerCaveFrame_ = 62313ULL;
    uint64_t maxAccumTicks_ = 124626ULL;
    uint64_t lastTick_ = 0;
    uint64_t accumTicks_ = 0;
};

} // namespace spelunk_xbox
