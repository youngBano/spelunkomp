// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The chip's behaviour follows MAME's ymz770.cpp (BSD-3-Clause; R. Belmont and MetalliC): see THIRD_PARTY_NOTICES.md.
#include "ymz770.h"

#include <cstdio>

#include "amm_decoder.h"

#include <algorithm>

namespace spelunk {

Ymz770::Ymz770() {
    for (auto &block : m_block) {
        block.assign(0x1000, 0);
    }
}

Ymz770::~Ymz770() = default;

void Ymz770::Reset() {
    for (int i = 0; i < 16; i++) {
        m_channels[i] = Channel{};
        m_decoders[i] = std::make_unique<AmmDecoder>(rom);
    }
    for (Sequence &s : m_sequences) {
        s = Sequence{};
    }
    m_mute = m_volume = m_boost = m_clip = 0;
    m_made = m_madeFrom = 0;
    sampleRate = 16000;
    out.clear();
}

void Ymz770::RunTo(uint64_t cycles, uint64_t cpuHz) {
    if (!rom || held || cycles <= m_madeFrom) {
        return;
    }
    const uint64_t due = (cycles - m_madeFrom) * sampleRate / cpuHz;
    while (m_made < due) {
        const uint32_t rate = sampleRate;
        out.push_back(Sample());
        m_made++;
        if (sampleRate != rate) {
            // (a phrase at another rate has begun: time is counted afresh from here at the new rate)
            m_madeFrom += m_made * cpuHz / rate;
            m_made = 0;
            rateChanged = true;
            RunTo(cycles, cpuHz);
            return;
        }
    }
}

// The sequences: a register and a value at a time from the ROM; 0x0E waits (the sequence's timer, in 32 samples),
// 0x0F ends or loops
void Ymz770::RunSequencer() {
    for (Sequence &s : m_sequences) {
        if (!s.playing) {
            continue;
        }
        if (s.delay > 0) {
            s.delay--;
            continue;
        }
        const uint8_t reg = Rom(s.offset++);
        const uint8_t value = Rom(s.offset++);
        switch (reg) {
        case 0x0F:
            for (int ch = 0; ch < 8; ch++) {
                if (s.stopChannels & (1 << ch)) {
                    m_channels[ch].playing = false;
                }
            }
            if (s.loop) {
                s.offset = SequenceOffset(s.number);
            } else {
                s.playing = false;
            }
            break;
        case 0x0E:
            s.delay = s.timer * 32 + 32 - 1;
            break;
        default:
            WriteRegister(reg, value);
            break;
        }
    }
}

int16_t Ymz770::Sample() {
    RunSequencer();
    int32_t mix = 0;
    for (int i = 0; i < 16; i++) {
        Channel &c = m_channels[i];
        if (c.remaining == 0 && c.playing) {
            for (;;) {
                if (c.lastBlock) {
                    if (c.loop) {
                        c.pending = true; // (the phrase again)
                    } else {
                        c.playing = false;
                        m_decoders[i]->Forget();
                    }
                }
                if (!c.playing) {
                    break;
                }
                if (c.pending) {
                    c.tableIndex = (Rom(4 * c.phrase) >> 4) & 7;
                    c.position = static_cast<int>(8 * PhraseOffset(c.phrase));
                    if (!c.lastBlock && c.latchVolume2 != c.volume2) {
                        m_decoders[i]->Forget();
                    }
                    c.pending = false;
                }
                const int made = m_decoders[i]->DecodeFrame(c.position, static_cast<int>(romSize * 8), m_block[i].data(), c.tableIndex);
                if (made <= 0) {
                    c.playing = !c.lastBlock; // (a phrase that gives nothing twice running is over)
                    c.lastBlock = true;
                    c.remaining = 0;
                    continue;
                }
                c.remaining = made;
                const int rate = m_decoders[i]->Rate();
                if (rate > 0 && static_cast<uint32_t>(rate) != sampleRate) {
                    sampleRate = static_cast<uint32_t>(rate);
                }
                c.lastBlock = c.remaining < 1152;
                c.at = 0;
                c.volume2 = c.latchVolume2;
                c.pan = c.latchPan;
                break;
            }
        }
        if (c.remaining > 0) {
            int32_t sample = m_block[i][c.at++];
            sample = (sample * c.volume2) >> 7; // (0 - 128: all of it)
            mix += (sample * c.pan) >> 7;       // (the right output: 64 is the middle)
            c.remaining--;
            if (c.remaining == 0 && !c.playing) {
                m_decoders[i]->Forget();
            }
        }
    }
    if (m_mute) {
        return 0;
    }
    mix *= m_volume; // (128: as it is)
    mix >>= 7 - m_boost;
    switch (m_clip) {
    case 3: mix = std::clamp(mix, -32768 * 75 / 100, 32768 * 75 / 100); break;
    case 2: mix = std::clamp(mix, -32768 * 875 / 1000, 32768 * 875 / 1000); break;
    default: break;
    }
    return static_cast<int16_t>(std::clamp(mix, -32768, 32767));
}

void Ymz770::WriteRegister(uint8_t reg, uint8_t value) {
    if (reg < 0x40) {
        switch (reg) {
        case 0x00: m_mute = value & 1; break;
        case 0x01: m_volume = value; break;
        case 0x02:
            m_boost = value & 7;
            m_clip = (value >> 4) & 7;
            break;
        default: break;
        }
    } else if (reg < 0x60) {
        Channel &c = m_channels[(reg >> 2) & 7];
        switch (reg & 3) {
        case 0: c.phrase = value; break;
        case 1: c.latchVolume2 = value; break;
        case 2: c.latchPan = static_cast<uint8_t>(std::min(value << 3, 128)); break;
        case 3:
            // (both key-on bits: keep playing if it is, start if it is not)
            if ((value & 6) == 2 || ((value & 6) == 6 && !c.playing)) {
                c.pending = true;
                c.playing = true;
                c.lastBlock = false;
                if (traceFrame && (value & 1)) {
                    fprintf(stderr, "sound: frame %llu: channel %d plays phrase %d, looped\n", static_cast<unsigned long long>(*traceFrame), (reg >> 2) & 7,
                            c.phrase);
                }
            } else if ((value & 6) == 0) {
                c.playing = false;
            }
            c.loop = (value & 1) ? 255 : 0;
            break;
        }
    } else if (reg >= 0x80) {
        Sequence &s = m_sequences[(reg >> 4) & 7];
        switch (reg & 0x0F) {
        case 0: s.number = value; break;
        case 1:
            if ((value & 6) == 2 || ((value & 6) == 6 && !s.playing)) {
                s.offset = SequenceOffset(s.number);
                s.delay = 0;
                s.playing = true;
                if (traceFrame) {
                    fprintf(stderr, "sound: frame %llu: slot %d plays sequence %d\n", static_cast<unsigned long long>(*traceFrame), (reg >> 4) & 7, s.number);
                }
            } else if ((value & 6) == 0 && s.playing) {
                s.playing = false;
                for (int i = 0; i < 8; i++) {
                    if (s.stopChannels & (1 << i)) {
                        m_channels[i].playing = false;
                    }
                }
            }
            s.loop = value & 1;
            break;
        case 2: s.timer = static_cast<uint16_t>((s.timer & 0x00FF) | (value << 8)); break;
        case 3: s.timer = static_cast<uint16_t>((s.timer & 0xFF00) | value); break;
        case 6: s.stopChannels = value; break;
        default: break;
        }
    }
}

void Ymz770::Serialize(Archive &ar) {
    ar.Mark(0x594D5A37);
    ar.Pod(m_channels);
    ar.Pod(m_sequences);
    ar.Pod(m_mute);
    ar.Pod(m_volume);
    ar.Pod(m_boost);
    ar.Pod(m_clip);
    ar.Pod(sampleRate);
    ar.Pod(m_made);
    ar.Pod(m_madeFrom);
    for (auto &block : m_block) {
        ar.Bytes(block.data(), block.size() * sizeof(int16_t));
    }
    if (!ar.Saving()) {
        // (the decoders' filter history is not kept: a click's worth at most, where a phrase was cut)
        for (auto &d : m_decoders) {
            if (d) {
                d->Forget();
            }
        }
        rateChanged = true;
    }
}

void Ymz770::KeepDecoders(int which) {
    for (int i = 0; i < 16; i++) {
        m_keptHas[which][i] = m_decoders[i] != nullptr;
        if (m_decoders[i]) {
            if (m_keptDecoders[which][i]) {
                *m_keptDecoders[which][i] = *m_decoders[i];
            } else {
                m_keptDecoders[which][i] = std::make_unique<AmmDecoder>(*m_decoders[i]);
            }
        }
    }
}

void Ymz770::PutBackDecoders(int which) {
    for (int i = 0; i < 16; i++) {
        if (!m_keptHas[which][i]) {
            m_decoders[i].reset();
        } else if (m_decoders[i]) {
            *m_decoders[i] = *m_keptDecoders[which][i];
        } else {
            m_decoders[i] = std::make_unique<AmmDecoder>(*m_keptDecoders[which][i]);
        }
    }
}

} // namespace spelunk
