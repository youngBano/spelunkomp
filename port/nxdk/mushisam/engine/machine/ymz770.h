// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The CV1000's sound chip: a Yamaha YMZ770C. Eight playback channels of compressed samples ("phrases", in Yamaha's
// AMM format) from the two sound ROMs, with a volume and a pan each, and a sequencer of eight sequences: lists in the
// ROM of register writes and waits, which is how the games' music is played. One output (the board wires only the
// right channel).
//
// The chip's behaviour follows MAME's ymz770.cpp (BSD-3-Clause; R. Belmont and MetalliC): see THIRD_PARTY_NOTICES.md.
// The phrases are decoded by machine/amm_decoder.*.
#pragma once

#include "../runtime/state.h"
#ifdef XBOX_FAST_CPU
#include "../native_cv1k/adpcm_cache.h"
#endif

#include <cstdint>
#include <memory>
#include <vector>

namespace spelunk {

class AmmDecoder;

class Ymz770 {
public:
    Ymz770();
    ~Ymz770();

    // The sound ROMs as the chip sees them (U23 then U24), set before Reset
    const uint8_t *rom = nullptr;
    uint32_t romSize = 0;
    void Reset();

    // A register written by the game (the board has already brought the chip up to the CPU's time)
    void WriteRegister(uint8_t reg, uint8_t value);
    // Makes samples up to `time` (in CPU cycles of `cpuHz`), into `out` (16-bit, one channel, at `sampleRate`)
    void RunTo(uint64_t cycles, uint64_t cpuHz);
    // Run-ahead: while set, the chip stands still (no samples, writes dropped): the frames run ahead are run again
    // for real, and heard then
    bool held = false;
    std::vector<int16_t> out;
    uint32_t sampleRate = 16000; // (the phrases say: it changes when one of another rate starts)
    bool rateChanged = false;

    void Serialize(Archive &ar);
    // The decoders' filter history, which Serialize does not keep (a state file stays small, and a click is all it
    // costs there), kept and put back with the board's exact moments (Cv1000::KeepNow, PutBack): online play's rollback
    // and run-ahead's kept frames then make the same sound as the frames run once
    void KeepDecoders(int which);
    void PutBackDecoders(int which);

private:
    struct Channel {
        uint16_t phrase = 0;
        uint8_t volume2 = 0, latchVolume2 = 0, pan = 64, latchPan = 64, loop = 0;
        bool pending = false, playing = false, lastBlock = false;
        int remaining = 0, at = 0, tableIndex = 0, position = 0;
    };
public:
    // What is started, written to stderr with the frame it points at (finding which tune is which)
    const uint64_t *traceFrame = nullptr;
    // The number of the sequence a slot plays, or -1; and the phrase a channel plays (looped ones are the tunes), or -1
    int SequencePlaying(int slot) const {
        return m_sequences[slot & 7].playing ? m_sequences[slot & 7].number : -1;
    }
    int PhrasePlaying(int channel) const {
        return m_channels[channel & 15].playing ? m_channels[channel & 15].phrase : -1;
    }

private:
    struct Sequence {
        uint32_t delay = 0, offset = 0;
        uint16_t number = 0, timer = 0, stopChannels = 0;
        uint8_t loop = 0;
        bool playing = false;
    };
    Channel m_channels[16];
    std::unique_ptr<AmmDecoder> m_decoders[16];
    std::unique_ptr<AmmDecoder> m_keptDecoders[2][16];
    bool m_keptHas[2][16] = {};
    std::vector<int16_t> m_block[16];
    Sequence m_sequences[8];
    uint8_t m_mute = 0, m_volume = 0, m_boost = 0, m_clip = 0;
    uint64_t m_made = 0;     // samples made since m_madeFrom
    uint64_t m_madeFrom = 0; // the CPU cycle the count began at (moved when the rate changes)
#ifdef XBOX_FAST_CPU
    ImaAdpcmCache m_adpcmCache;
    ImaAdpcmCache::DecoderState m_adpcmState[16];
    bool m_useAdpcm[16] = {};
    // Zero-spike incremental capture: records first natural playback of short SFX phrases, then commits to ADPCM.
    bool m_capturing[16] = {};
    uint8_t m_capturePhrase[16] = {};
    uint32_t m_captureRate[16] = {};
    std::vector<int16_t> m_captureBuf[16];
#endif

    uint8_t Rom(uint32_t offset) const {
        return rom[offset & (romSize - 1u)];
    }
    uint32_t PhraseOffset(unsigned phrase) const {
        return (Rom(4 * phrase + 1) << 16) | (Rom(4 * phrase + 2) << 8) | Rom(4 * phrase + 3);
    }
    uint32_t SequenceOffset(unsigned number) const {
        return (Rom(4 * number + 0x401) << 16) | (Rom(4 * number + 0x402) << 8) | Rom(4 * number + 0x403);
    }
    void RunSequencer();
    int16_t Sample();
};

} // namespace spelunk
