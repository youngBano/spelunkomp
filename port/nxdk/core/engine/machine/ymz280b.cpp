// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The registers, the decoding and the mixing are as MAME's ymz280b.cpp has them (BSD-3-Clause, Aaron Giles).
#include "ymz280b.h"

#include "../runtime/state.h"

#include <algorithm>
#include <cstdio>

namespace spelunk {

namespace {
    constexpr int kFracBits = 9, kFracOne = 1 << kFracBits;
    constexpr int kIndexScale[8] = {0x0E6, 0x0E6, 0x0E6, 0x0E6, 0x133, 0x199, 0x200, 0x266};
} // namespace

void Ymz280b::Reset() {
    for (Voice &v : m_voices) {
        v = Voice();
    }
    m_register = m_status = m_irqMask = 0;
    m_irqEnable = m_keyonEnable = m_extEnable = m_irqState = false;
    m_extLatch = 0;
    m_extHigh = m_extMiddle = m_extAddress = 0;
    m_made = 0;
    m_pending = 0;
    m_half = false;
    m_lastCycles = 0;
    std::fill(std::begin(m_hold), std::end(m_hold), uint8_t{0});
    std::fill(std::begin(m_tickState), std::end(m_tickState), char{0});
    out.clear();
}

void Ymz280b::UpdateIrq() {
    m_irqState = m_irqEnable && (m_status & m_irqMask);
}

void Ymz280b::UpdateVolumes(Voice &v) {
    if (v.pan == 8) {
        v.left = v.right = v.level;
    } else if (v.pan < 8) {
        v.left = v.level;
        v.right = v.pan == 0 ? 0 : v.level * (v.pan - 1) / 7;
    } else {
        v.left = v.level * (15 - v.pan) / 7;
        v.right = v.level;
    }
}

void Ymz280b::WriteRegister(uint8_t data) {
    if (m_register < 0x80) {
        const int number = (m_register >> 2) & 7;
        Voice &v = m_voices[number];
        auto part = [&](uint32_t &address, uint32_t keep, int shift) { address = (address & (keep << 1)) | (static_cast<uint32_t>(data) << shift); };
        switch (m_register & 0xE3) {
        case 0x00: v.fnum = (v.fnum & 0x100) | data; break;
        case 0x01:
            v.fnum = static_cast<uint16_t>((v.fnum & 0xFF) | ((data & 1) << 8));
            v.looping = (data & 0x10) >> 4;
            if ((data & 0x60) == 0) {
                data &= 0x7F; // (no mode: as the key off)
            } else {
                v.mode = (data & 0x60) >> 5;
            }
            if (!v.keyon && (data & 0x80) && m_keyonEnable) {
                v.playing = 1;
                v.position = v.start;
                v.signal = v.loopSignal = 0;
                v.step = v.loopStep = 0x7F;
                v.loopCount = 0;
                v.ended = false;
                // (its first sample is fetched at its first tick, whatever the voice played before: a key-on
                // while it still sounded had kept the old sound's place between two samples, and ended up to
                // 512 / (fnum + 1) ticks off)
                v.outputPosition = kFracOne;
                v.last = v.current = 0;
                if (tickCycles > 0 && keyOnAt > 0) {
                    // (the core's scheduler comes to the voice keyOnAt cycles into a tick and the slots of the voices
                    // before it, which can run past the tick's end: a key-on written before the last tick's visit
                    // starts the voice with that tick, one after this tick's with the next, otherwise with this one)
                    double before = keyOnAt - tickCycles, visit = keyOnAt;
                    for (int m = 0; m < number; m++) {
                        const Voice &w = m_voices[m];
                        before += m_tickState[m] == 'p' ? slotPlay : m_tickState[m] == 'f' ? slotFetch : slotIdle;
                        visit += m_hold[m] || !w.playing || !w.mode ? slotIdle : w.outputPosition >= kFracOne ? slotFetch : slotPlay;
                    }
                    const double into = static_cast<double>(m_lastCycles) - tickOffset - static_cast<double>(m_made) * tickCycles;
                    if (into >= visit) {
                        m_hold[number] = 1; // (the scheduler has passed the voice in this tick: it starts with the next)
                    } else if (m_made > 0 && into < before) {
                        // (the last tick's visit is still to come: the voice makes that tick's step now)
                        v.outputPosition -= kFracOne;
                        v.last = v.current;
                        v.current = Next(v, number);
                        v.outputPosition += v.outputStep;
                        m_early = true;
                    }
                }
                if (traceFrame) {
                    char before[9] = {};
                    for (int m = 0; m < number; m++) {
                        before[m] = m_tickState[m] ? m_tickState[m] : '-';
                    }
                    fprintf(stderr, "sound: frame %llu: voice %d plays %06X%s stop %06X fnum %X mode %d tick %llu at %llu [%s]%s%s\n",
                            static_cast<unsigned long long>(*traceFrame), number, v.start / 2, v.looping ? " (looped)" : "", v.stop / 2, v.fnum,
                            v.mode, static_cast<unsigned long long>(m_made), static_cast<unsigned long long>(m_lastCycles), before,
                            m_hold[number] ? " held" : "", m_early ? " early" : "");
                }
                m_early = false;
            } else if (v.keyon && !(data & 0x80)) {
                v.playing = 0;
            }
            v.keyon = (data & 0x80) >> 7;
            break;
        case 0x02:
            v.level = data;
            UpdateVolumes(v);
            break;
        case 0x03:
            v.pan = data & 0x0F;
            UpdateVolumes(v);
            break;
        case 0x20: part(v.start, 0x00FFFF, 17); break;
        case 0x21: part(v.loopStart, 0x00FFFF, 17); break;
        case 0x22: part(v.loopEnd, 0x00FFFF, 17); break;
        case 0x23: part(v.stop, 0x00FFFF, 17); break;
        case 0x40: part(v.start, 0xFF00FF, 9); break;
        case 0x41: part(v.loopStart, 0xFF00FF, 9); break;
        case 0x42: part(v.loopEnd, 0xFF00FF, 9); break;
        case 0x43: part(v.stop, 0xFF00FF, 9); break;
        case 0x60: part(v.start, 0xFFFF00, 1); break;
        case 0x61: part(v.loopStart, 0xFFFF00, 1); break;
        case 0x62: part(v.loopEnd, 0xFFFF00, 1); break;
        case 0x63: part(v.stop, 0xFFFF00, 1); break;
        default: break;
        }
        v.outputStep = (v.mode == 1 ? (v.fnum & 0xFF) : (v.fnum & 0x1FF)) + 1;
        return;
    }
    switch (m_register) {
    case 0x84: m_extHigh = static_cast<uint32_t>(data) << 16; break;
    case 0x85: m_extMiddle = static_cast<uint32_t>(data) << 8; break;
    case 0x86:
        m_extAddress = m_extHigh | m_extMiddle | data;
        if (m_extEnable) {
            m_extLatch = Rom(m_extAddress);
        }
        break;
    case 0x87:
        if (m_extEnable) {
            m_extAddress = (m_extAddress + 1) & 0xFFFFFF; // (a write to ROM: only the address moves)
        }
        break;
    case 0xFE:
        m_irqMask = data;
        UpdateIrq();
        break;
    case 0xFF:
        m_extEnable = (data & 0x40) != 0;
        m_irqEnable = (data & 0x10) != 0;
        UpdateIrq();
        if (m_keyonEnable && !(data & 0x80)) {
            for (Voice &v : m_voices) {
                v.playing = 0;
            }
        } else if (!m_keyonEnable && (data & 0x80)) {
            for (Voice &v : m_voices) {
                if (v.keyon && v.looping) {
                    v.playing = 1;
                }
            }
        }
        m_keyonEnable = (data & 0x80) != 0;
        break;
    default: break;
    }
}

void Ymz280b::Write(unsigned offset, uint8_t value) {
    if (held) {
        return;
    }
    if (!(offset & 1)) {
        m_register = value;
    } else {
        WriteRegister(value);
    }
}

uint8_t Ymz280b::Read(unsigned offset, bool changeNothing) {
    if (!(offset & 1)) {
        if (!m_extEnable) {
            return 0xFF;
        }
        const uint8_t value = m_extLatch;
        if (!changeNothing && !held) {
            m_extLatch = Rom(m_extAddress);
            m_extAddress = (m_extAddress + 1) & 0xFFFFFF;
        }
        return value;
    }
    const uint8_t value = m_status;
    if (!changeNothing && !held) {
        m_status = 0;
        UpdateIrq();
    }
    return value;
}

// The voice's next sample at its own rate
int16_t Ymz280b::Next(Voice &v, int number) {
    if (!v.playing) {
        // (after the end: back to nothing by sixteenths)
        const int t = v.current;
        return static_cast<int16_t>(t < 0 ? -((-t * 15) >> 4) : (t * 15) >> 4);
    }
    int sample = 0;
    switch (v.mode) {
    case 1: {
        const int nibble = Rom(v.position / 2) >> ((~v.position & 1) << 2);
        const int value = (nibble & 7) * 2 + 1;
        v.signal += (v.step * ((nibble & 8) ? -value : value)) / 8;
        v.signal = std::clamp(v.signal, -32768, 32767);
        v.step = std::clamp((v.step * kIndexScale[nibble & 7]) >> 8, 0x7F, 0x6000);
        sample = v.signal;
        v.position++;
        if (v.looping) {
            if (v.position == v.loopStart && v.loopCount == 0) {
                v.loopSignal = v.signal;
                v.loopStep = v.step;
            }
            if (v.position >= v.loopEnd && v.keyon) {
                v.position = v.loopStart;
                v.signal = v.loopSignal;
                v.step = v.loopStep;
                v.loopCount++;
            }
        }
        break;
    }
    case 2:
        sample = static_cast<int8_t>(Rom(v.position / 2)) * 256;
        v.position += 2;
        if (v.looping && v.position >= v.loopEnd && v.keyon) {
            v.position = v.loopStart;
        }
        break;
    case 3:
        sample = static_cast<int16_t>((Rom(v.position / 2 + 1) << 8) + Rom(v.position / 2));
        v.position += 4;
        if (v.looping && v.position >= v.loopEnd && v.keyon) {
            v.position = v.loopStart;
        }
        break;
    default: break;
    }
    if (v.mode && v.position >= v.stop + endPast) {
        if (traceFrame) {
            // (and what the voices before it did in this tick: - idle, p playing, f fetching; measurement)
            char before[9] = {};
            for (int m = 0; m < number; m++) {
                before[m] = m_tickState[m];
            }
            fprintf(stderr, "sound: frame %llu: voice %d ends (tick %llu) [%s]\n", static_cast<unsigned long long>(*traceFrame), number,
                    static_cast<unsigned long long>(m_made), before);
        }
        v.playing = 0;
        m_status |= static_cast<uint8_t>(1 << number);
        UpdateIrq();
    }
    return static_cast<int16_t>(sample);
}

// One tick of the chip's own rate (88,200 a second): every voice, each brought from its own rate by a straight line
// between two of its samples
void Ymz280b::Tick() {
    int32_t left = 0, right = 0;
    for (int n = 0; n < 8; n++) {
        Voice &v = m_voices[n];
        m_tickState[n] = '-';
        if (m_hold[n]) [[unlikely]] {
            m_hold[n] = 0;
            continue;
        }
        if (!v.playing && v.current == 0 && v.last == 0) {
            v.outputPosition = kFracOne; // (the next sound starts at once)
            continue;
        }
        m_tickState[n] = v.playing ? (v.outputPosition >= kFracOne ? 'f' : 'p') : '-';
        if (v.outputPosition >= kFracOne) {
            v.outputPosition -= kFracOne;
            const int16_t next = Next(v, n);
            v.last = v.current;
            v.current = next;
        }
        const int32_t between = (static_cast<int32_t>(v.last) * (kFracOne - v.outputPosition) + static_cast<int32_t>(v.current) * v.outputPosition) >> kFracBits;
        left += between * v.left / 2;
        right += between * v.right / 2;
        v.outputPosition += v.outputStep;
    }
    // (each side is a 16-bit sample times 256; the two into one channel, two ticks into one sample)
    const int32_t one = (left + right) / 512;
    if (m_half) {
        out.push_back(static_cast<int16_t>(std::clamp((m_pending + one) / 2, -32768, 32767)));
    } else {
        m_pending = one;
    }
    m_half = !m_half;
}

void Ymz280b::RunTo(uint64_t cycles, uint64_t cpuHz) {
    if (held) {
        return;
    }
    m_lastCycles = cycles;
    const uint64_t want = tickCycles > 0 ? (cycles < tickOffset ? 0 : static_cast<uint64_t>((cycles - tickOffset) / tickCycles))
                                         : MulDivFloor(cycles, sampleRate * 2ull, cpuHz);
    while (m_made < want) {
        Tick();
        m_made++;
    }
}

uint64_t Ymz280b::NextIrq(uint64_t limit, uint64_t cpuHz) const {
    constexpr uint64_t kNever = ~0ull;
    if (held || !m_irqEnable || !m_irqMask) { // (with the line already up too: the run stops there all the same)
        return kNever;
    }
    const uint64_t rate = sampleRate * 2ull;
    const uint64_t last = tickCycles > 0 ? (limit < tickOffset ? 0 : static_cast<uint64_t>((limit - tickOffset) / tickCycles))
                                         : MulDivFloor(limit, rate, cpuHz);
    struct V {
        bool play, on, hold;
        uint8_t mode, looping, keyon;
        uint32_t position, stop, loopStart, loopEnd;
        int32_t outputPosition, outputStep;
    } v[8];
    bool any = false;
    for (int n = 0; n < 8; n++) {
        const Voice &x = m_voices[n];
        v[n] = {x.playing && x.mode, x.playing && x.mode && (m_irqMask & (1 << n)) && !(m_status & (1 << n)), m_hold[n] != 0, x.mode, x.looping,
                x.keyon, x.position, x.stop, x.loopStart, x.loopEnd, x.outputPosition, x.outputStep};
        any = any || v[n].on;
    }
    if (!any) {
        return kNever;
    }
    for (uint64_t k = m_made; k < last; k++) {
        double slot = 0; // (measurement timing: where in the tick the core's scheduler comes to the voice)
        for (int n = 0; n < 8; n++) {
            V &y = v[n];
            if (y.hold) {
                y.hold = false;
                slot += slotIdle;
                continue;
            }
            if (!y.play) {
                slot += slotIdle;
                continue;
            }
            const bool fetch = y.outputPosition >= kFracOne;
            if (fetch) {
                y.outputPosition -= kFracOne;
                y.position += y.mode == 1 ? 1 : y.mode == 2 ? 2 : 4;
                if (y.looping && y.position >= y.loopEnd && y.keyon) {
                    y.position = y.loopStart;
                }
                if (y.position >= y.stop + endPast) {
                    y.play = false;
                    if (y.on) {
                        // (made by the tick numbered k: when RunTo's count reaches k + 1)
                        if (tickCycles > 0) {
                            // (the core raises it when its scheduler next comes to the voice, in the next tick: the
                            // voices before it take their time as they stand in that tick, idle, playing or fetching)
                            slot = 0;
                            for (int m = 0; m < n; m++) {
                                slot += !v[m].play ? slotIdle : v[m].outputPosition >= kFracOne ? slotFetch : slotPlay;
                            }
                            const double at = tickOffset + (k + 1) * tickCycles + irqDelay + irqDelayPerVoice * n + slot;
                            return static_cast<uint64_t>(at) + (at > static_cast<uint64_t>(at) ? 1 : 0);
                        }
                        return MulDivCeil(k + 1, cpuHz, rate);
                    }
                }
            }
            slot += fetch ? slotFetch : slotPlay;
            y.outputPosition += y.outputStep;
        }
    }
    return kNever;
}

void Ymz280b::Serialize(Archive &ar) {
    ar.Mark(0x595A3238);
    ar.Pod(m_voices);
    ar.Pod(m_register);
    ar.Pod(m_status);
    ar.Pod(m_irqMask);
    ar.Pod(m_irqEnable);
    ar.Pod(m_keyonEnable);
    ar.Pod(m_extEnable);
    ar.Pod(m_irqState);
    ar.Pod(m_extLatch);
    ar.Pod(m_extHigh);
    ar.Pod(m_extMiddle);
    ar.Pod(m_extAddress);
    ar.Pod(m_made);
    ar.Pod(m_pending);
    ar.Pod(m_half);
}

} // namespace spelunk
