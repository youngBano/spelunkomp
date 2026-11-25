// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The Yamaha YMZ280B (PCMD8): eight voices of 4-bit ADPCM or 8- or 16-bit PCM from ROM, each with its own pitch,
// level and place between the speakers, loops, and an interrupt when a voice comes to its end. Cave's first boards
// with one (DoDonPachi, ESP Ra.De., Guwange and others) have it as their whole sound hardware.
//
// The registers, the decoding and the mixing are as MAME's ymz280b.cpp has them (BSD-3-Clause, Aaron Giles): see
// THIRD_PARTY_NOTICES.md.
#pragma once

#include "wide_math.h"

#include <cstdint>
#include <vector>

namespace spelunk {

class Archive;

class Ymz280b {
public:
    // The sample ROMs as the chip sees them, set before Reset
    const uint8_t *rom = nullptr;
    uint32_t romSize = 0;
    void Reset();

    // The chip's two addresses (the board has already brought the chip up to the CPU's time)
    void Write(unsigned offset, uint8_t value);
    uint8_t Read(unsigned offset, bool changeNothing);
    // The interrupt line (a voice has ended, the game has not yet read the status)
    bool Irq() const {
        return m_irqState;
    }
    // Makes samples up to `cycles` (CPU cycles of `cpuHz`) into `out`: 16 bits, one channel (the boards mix the two
    // into one), at `sampleRate`
    void RunTo(uint64_t cycles, uint64_t cpuHz);
    // (measurement: from a run timed as the core's to the chip's own crystal, mid-run: the ticks counted at its rate)
    void UseCrystal(uint64_t cycles, uint64_t cpuHz) {
        tickCycles = 0;
        endPast = 0;
        m_made = MulDivFloor(cycles, sampleRate * 2ull, cpuHz);
    }
    // When (in CPU cycles) a voice will next end with its interrupt let through, if before `limit` and nothing is
    // written to the chip meanwhile; ~0 if none (or the line is already up)
    uint64_t NextIrq(uint64_t limit, uint64_t cpuHz) const;
    // Run-ahead: while set, the chip stands still (no samples, writes dropped)
    bool held = false;
    std::vector<int16_t> out;
    static constexpr uint32_t sampleRate = 44100; // (the chip's clock, 16.9344 MHz, / 384)
    // (measurement: the chip's ticks as `tickCycles` CPU cycles each, from `tickOffset`, in place of its own crystal's
    // rate: Arcade-Cave_MiSTer runs it from the CPU's clock, a tick every 181.5 cycles)
    double tickCycles = 0, tickOffset = 0;
    double irqDelay = 0, irqDelayPerVoice = 0, slotIdle = 0, slotPlay = 0, slotFetch = 0;
    // (the core's scheduler comes to each voice in turn, keyOnAt cycles into a tick plus the slots of the voices before
    // it: a key-on written after that starts the voice a tick later; 0: as soon as written)
    double keyOnAt = 0;
    uint32_t endPast = 0; // (nibbles a voice plays past its end address: the core's rule ends a byte later than MAME's)
    // What a voice plays (its start address in the ROM), or -1: the tunes are the looped ones
    int Playing(int voice) const {
        return m_voices[voice & 7].playing ? static_cast<int>(m_voices[voice & 7].start / 2) : -1;
    }
    bool Looping(int voice) const {
        return m_voices[voice & 7].playing && m_voices[voice & 7].looping;
    }
    // What is started, written to stderr with the frame it points at (finding which tune is which)
    const uint64_t *traceFrame = nullptr;

    void Serialize(Archive &ar);

private:
    struct Voice {
        uint8_t playing = 0, keyon = 0, looping = 0, mode = 0, level = 0, pan = 0;
        bool ended = false;
        uint16_t fnum = 0;
        uint32_t start = 0, stop = 0, loopStart = 0, loopEnd = 0, position = 0; // (in nibbles)
        int32_t signal = 0, step = 0x7F, loopSignal = 0, loopStep = 0x7F;
        uint32_t loopCount = 0;
        int32_t left = 0, right = 0, outputStep = 1, outputPosition = 512;
        int16_t last = 0, current = 0;
    };
    Voice m_voices[8];
    uint8_t m_register = 0, m_status = 0, m_irqMask = 0;
    bool m_irqEnable = false, m_keyonEnable = false, m_extEnable = false, m_irqState = false;
    uint8_t m_extLatch = 0;
    uint32_t m_extHigh = 0, m_extMiddle = 0, m_extAddress = 0;
    uint64_t m_made = 0; // (ticks of the chip's own rate, twice sampleRate, made since reset)
    int32_t m_pending = 0;
    bool m_half = false;
    // (measurement, keyOnLatency: where the last RunTo came to, and voices whose start waits a tick; not saved)
    uint64_t m_lastCycles = 0;
    uint8_t m_hold[8] = {};
    char m_tickState[8] = {}; // (the voices' states in the last tick made: - idle, p playing, f fetching)
    bool m_early = false;     // (SPELUNK_TRACE_SOUND: the voice just keyed on took the last tick's step)

    uint8_t Rom(uint32_t at) const {
        return at < romSize ? rom[at] : 0;
    }
    void WriteRegister(uint8_t value);
    void UpdateIrq();
    static void UpdateVolumes(Voice &v);
    int16_t Next(Voice &v, int number);
    void Tick();
};

} // namespace spelunk
