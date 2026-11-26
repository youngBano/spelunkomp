// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// Yamaha's AMM, the compressed sound the YMZ770 plays (machine/ymz770.*): MPEG-1 audio Layer II (ISO/IEC 11172-3)
// with a header of Yamaha's own. What is MPEG's: the sync word and the ID and layer bits, then after the header the
// Layer II frame as the standard has it (bit allocations, scalefactor selection, scalefactors, the samples in twelve
// granules of three, joint stereo above a bound), its allocation tables (11172-3's four, and 13818-3's for the half
// rates), its requantisation and its synthesis filterbank. What is AMM's:
//
// - the header after the layer bits: 1 bit "older CBR" form, 4 bits whole packets, 2 bits rate, 2 bits slices in
//   the last packet, 2 bits mode, 2 bits mode extension, 3 bits allocation table, 1 bit zero (no bit rate, no
//   padding, no CRC);
// - a frame may hold fewer than the 36 slices of 32 samples: 3 for each whole packet and the last packet's (the older
//   form always 36). A frame of none ends the phrase; one of fewer than 36 is the phrase's last;
// - the allocation table is named in the header, and in the older form by the phrase table instead (the caller's
//   `cbrTable`), one of 11172-3's four (0-3) or 13818-3's (4);
// - frames are a pure bitstream: one starts where the last ended, found by its sync word;
// - the ID bit 0 (MPEG-2's) halves the three rates.
//
// Written for this project from the standards and from what MAME's ymz770.cpp and mpeg_audio.cpp do with AMM (the
// format's details above): see THIRD_PARTY_NOTICES.md.
#pragma once

#include <cstdint>

namespace spelunk {

class AmmDecoder {
public:
    static constexpr int kMaxSamples = 1152; // (a frame's, each channel's)

    explicit AmmDecoder(const uint8_t *data);

    // Decodes the frame at bit `bit` of the data, or the first after it (by its sync word), reading no bit at or past
    // `end`, into `pcm` (16-bit, the channels interleaved). Returns the samples made for each channel (0 for a frame
    // of none) and moves `bit` past the frame; or -1, `bit` unmoved, when no whole frame is left before `end`.
    // `cbrTable`: the allocation table of the older form's frames.
    int DecodeFrame(int &bit, int end, int16_t *pcm, int cbrTable);
    // The last frame's sample rate (0: none the standards name) and channels
    int Rate() const {
        return m_rate;
    }
    int Channels() const {
        return m_channels;
    }

    // The filterbank's history let go (silence before the next frame)
    void Forget();

private:
    // The bits, most significant first; a read past the end is an overrun, which ends the frame unmade
    struct Bits {
        const uint8_t *data;
        int at, end;
        bool overrun;
        uint32_t Take(int count);
        uint32_t Unchecked(int count);
    };

    struct Quantiser; // (a number of levels: how its codes are read and requantised)

    // The last 16 matrixed vectors of each channel (the standard's V, 32 values of the 64 that differ; the newest at
    // m_newest[channel])
    double m_history[2][16][32];
    int m_newest[2];

    const uint8_t *m_data;
    int m_rate = 0, m_channels = 1;

    bool FindSync(Bits &in) const;
    void Synthesise(int channel, const double *subbands, int bandsUsed, int16_t *pcm, int stride);
};

} // namespace spelunk
