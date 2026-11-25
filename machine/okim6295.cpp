// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The commands, the decoding, the volumes and the paging are as MAME's okim6295.cpp, okiadpcm.cpp and nmk112.cpp have
// them (BSD-3-Clause; Mirko Buffoni, Aaron Giles, Alex W. Jackson).
#include "okim6295.h"
#include "wide_math.h"

#include "../runtime/state.h"

#include <algorithm>
#include <cmath>

namespace spelunk {

namespace {
    // A voice's volume for the four bits of its command, in 32nds (0 dB, -3.2, -6, ...; nine on and silence)
    constexpr int32_t kVolumes[16] = {0x20, 0x16, 0x10, 0x0B, 0x08, 0x06, 0x04, 0x03, 0x02, 0, 0, 0, 0, 0, 0, 0};
    constexpr int8_t kStepShift[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
    // The chips' sample rates: the first 4.22 MHz / 4 / 132, the second twice that
    constexpr uint64_t kClock = 4220000, kDivider = 4 * 132;

    struct Steps {
        int32_t diff[49 * 16];
        Steps() {
            for (int step = 0; step <= 48; step++) {
                const int value = static_cast<int>(std::floor(16.0 * std::pow(11.0 / 10.0, step)));
                for (int nibble = 0; nibble < 16; nibble++) {
                    const int sign = nibble & 8 ? -1 : 1;
                    diff[step * 16 + nibble] =
                        sign * (value * ((nibble >> 2) & 1) + value / 2 * ((nibble >> 1) & 1) + value / 4 * (nibble & 1) + value / 8);
                }
            }
        }
    };
    const Steps kSteps;
} // namespace

void OkiPair::Reset() {
    for (Chip &chip : m_chips) {
        chip = Chip();
    }
    std::fill(std::begin(m_banks), std::end(m_banks), 0);
    m_made = 0;
}

uint8_t OkiPair::Byte(int chip, uint32_t at) const {
    if (!romSize[chip]) {
        return 0;
    }
    at &= 0x3FFFF;
    if (halves) {
        return rom[chip][((static_cast<uint32_t>(m_banks[chip * 4 + (at >> 17)]) << 17) | (at & 0x1FFFF)) % romSize[chip]];
    }
    // (the second chip's phrase table is paged a quarter at a time with the banks; the rest a 64 KB bank at a time)
    const unsigned bank = chip == 1 && at < 0x400 ? at >> 8 : at >> 16;
    const uint32_t pages = romSize[chip] >> 16;
    const uint32_t page = m_banks[chip * 4 + bank] % pages;
    const uint32_t from = (page << 16) | (chip == 1 && at < 0x400 ? at : at & 0xFFFF);
    return rom[chip][from];
}

void OkiPair::SetBank(int which, uint8_t page) {
    m_banks[which & 7] = page;
}

void OkiPair::SetHalves(int chip, uint8_t low, uint8_t high) {
    m_banks[chip * 4] = low;
    m_banks[chip * 4 + 1] = high;
}

int32_t OkiPair::Step(int chip) {
    Tick(chip);
    return m_chips[chip].last;
}

uint8_t OkiPair::Read(int chip, uint64_t at) const {
    uint8_t status = 0xF0;
    for (int n = 0; n < 4; n++) {
        const bool busy = slotOrigin >= 0 ? at < m_busyFrom[chip][n] ? at < m_busyBefore[chip][n] : at < m_busyUntil[chip][n]
                                          : m_chips[chip].voices[n].playing;
        status |= busy ? 1 << n : 0;
    }
    return status;
}

// (measurement) jt6295's clock enables come where an accumulator of `kSlotStep` a 32 MHz clock overflows; a sample is
// 132 of them, and the four voices' turns begin at its 1st, 33rd, 65th and 97th (voices 1, 2, 3, 0; the last 36 long)
namespace {
    constexpr uint64_t kSlotStep[2] = {0x873, 0x10E5};
    constexpr uint64_t kTurnStart[4] = {96, 0, 32, 64};
}
uint64_t OkiPair::Enables(int chip, uint64_t at) const {
    return (static_cast<uint64_t>(slotOrigin) + 2 * at) * kSlotStep[chip] >> 16;
}
uint64_t OkiPair::CycleOf(int chip, uint64_t enables) const {
    const uint64_t clock = (enables * 65536 + kSlotStep[chip] - 1) / kSlotStep[chip];
    return (clock - static_cast<uint64_t>(slotOrigin) + 1) / 2;
}
// The enable that began `voice`'s turn under way at `at`, or the next one's
uint64_t OkiPair::Turn(int chip, int voice, uint64_t at) const {
    const uint64_t done = Enables(chip, at);
    if (done == 0) {
        return voice == 0 ? 0 : kTurnStart[voice] + 1; // (voice 0's turn until the first enable)
    }
    const uint64_t base = (done - 1) / 132 * 132, q = (done - 1) % 132;
    const int current = q < 32 ? 1 : q < 64 ? 2 : q < 96 ? 3 : 0;
    const uint64_t turn = base + kTurnStart[voice] + 1;
    return current == voice || turn > done ? turn : turn + 132;
}

void OkiPair::Write(int which, uint8_t value, uint64_t at) {
    if (held) {
        return;
    }
    Chip &chip = m_chips[which];
    if (chip.command != -1) {
        // The second byte of a phrase's start: the voices to play it on and its volume
        unsigned voices = value >> 4;
        for (int n = 0; n < 4; n++, voices >>= 1) {
            Voice &v = chip.voices[n];
            const uint32_t base = static_cast<uint32_t>(chip.command) * 8;
            const uint32_t start = ((Byte(which, base) << 16) | (Byte(which, base + 1) << 8) | Byte(which, base + 2)) & 0x3FFFF;
            const uint32_t stop = ((Byte(which, base + 3) << 16) | (Byte(which, base + 4) << 8) | Byte(which, base + 5)) & 0x3FFFF;
            if ((voices & 1) && slotOrigin >= 0) {
                // (jt6295: busy from the command, loaded at the voice's turn, a nibble each sample, and not busy from
                // the turn that finds its byte at the stop address: 2 x (stop - start) nibbles played)
                // (the bit rises when the phrase's addresses have been read: a command again while they are read
                // does not put that off)
                if (at >= m_busyFrom[which][n]) {
                    m_busyBefore[which][n] = m_busyUntil[which][n];
                    m_busyFrom[which][n] = at + slotStartDelay;
                }
                const uint64_t first = Turn(which, n, at);
                const uint64_t nibbles = stop > start ? 2ull * (stop - start) : 0;
                m_busyUntil[which][n] = CycleOf(which, first + 132 * (nibbles + 1));
            }
            if (!(voices & 1) || v.playing) {
                continue;
            }
            if (start < stop) {
                v.playing = true;
                v.base = start;
                v.sample = 0;
                v.count = 2 * (stop - start + 1);
                v.signal = 0;
                v.step = 0;
                v.volume = kVolumes[value & 15];
            }
        }
        chip.command = -1;
    } else if (value & 0x80) {
        chip.command = value & 0x7F;
    } else {
        // Voices stopped
        unsigned voices = value >> 3;
        for (int n = 0; n < 4; n++, voices >>= 1) {
            if (voices & 1) {
                if (slotOrigin >= 0 && m_busyUntil[which][n] > at) {
                    // (jt6295: the stop is taken at the voice's turn)
                    const uint64_t turn = Turn(which, n, at);
                    m_busyUntil[which][n] = std::min(m_busyUntil[which][n], turn <= Enables(which, at) ? at : CycleOf(which, turn));
                }
                chip.voices[n].playing = false;
            }
        }
    }
}

void OkiPair::Tick(int which) {
    Chip &chip = m_chips[which];
    int32_t sum = 0;
    for (Voice &v : chip.voices) {
        if (!v.playing) {
            continue;
        }
        const uint8_t nibble = static_cast<uint8_t>(Byte(which, v.base + v.sample / 2) >> (((v.sample & 1) << 2) ^ 4)) & 15;
        v.signal = std::clamp(v.signal + kSteps.diff[v.step * 16 + nibble], -2048, 2047);
        v.step = std::clamp(v.step + kStepShift[nibble & 7], 0, 48);
        sum += v.signal * v.volume / 32;
        if (++v.sample >= v.count) {
            v.playing = false;
        }
    }
    chip.last = sum;
}

void OkiPair::RunTo(uint64_t cycles, uint64_t cpuHz) {
    if (held) {
        return;
    }
    const uint64_t want = MulDivFloor(cycles, sampleRate, cpuHz);
    while (m_made < want) {
        m_made++;
        // (each chip brought up to the time of this sample)
        for (int which = 0; which < 2; which++) {
            const uint64_t due = MulDivFloor(m_made, kClock * (which + 1), kDivider * sampleRate);
            while (m_chips[which].made < due) {
                Tick(which);
                m_chips[which].made++;
            }
        }
        // (the board mixes the music at 1.6 times the effects; a voice at full volume is the whole range)
        const int32_t mixed = (m_chips[0].last * 128 + m_chips[1].last * 80) / 5;
        out.push_back(static_cast<int16_t>(std::clamp(mixed, -32768, 32767)));
    }
}

void OkiPair::Serialize(Archive &ar) {
    ar.Mark(0x4F4B4932);
    ar.Pod(m_chips);
    ar.Pod(m_banks);
    ar.Pod(m_made);
}

} // namespace spelunk
