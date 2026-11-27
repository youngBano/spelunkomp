#pragma once

// Fast 4-bit IMA ADPCM phrase cache for CV1000 YMZ770C short SFX phrases on Original Xbox.
// Eliminates repeated MPEG Layer-II polyphase synthesis spikes (3.6 ms -> <0.1 ms) during
// rapid shot/hit/explosion SFX while staying inside a strict 2 MB RAM cap.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace spelunk {

class ImaAdpcmCache {
public:
    static constexpr size_t kMaxCacheBytes = 2u * 1024u * 1024u; // 2 MB strict cap
    static constexpr uint32_t kMaxSfxSamples = 32768u;           // <= ~2.0s @ 16 kHz -> <= 16 KB ADPCM

    struct PhraseEntry {
        uint8_t state = 0; // 0 = unknown, 1 = cached ADPCM, 2 = too long (stream via AMM)
        uint32_t sampleRate = 16000u;
        uint32_t totalSamples = 0;
        const uint8_t *flatPtr = nullptr;
        uint32_t flatBytes = 0;
        std::vector<uint8_t> adpcm;
    };

    struct DiskHeader {
        uint32_t magic;      // 0x4D435041 ('APCM')
        uint32_t count;      // <= 256
        uint32_t totalBytes; // flat ADPCM payload bytes
    };
    struct DiskEntry {
        uint32_t phrase;
        uint32_t sampleRate;
        uint32_t totalSamples;
        uint32_t dataOff;
        uint32_t dataBytes;
    };
    static constexpr uint32_t kPackMagic = 0x4D435041u; // 'APCM'

    struct DecoderState {
        int32_t pred = 0;
        int32_t stepIdx = 0;
        uint32_t samplePos = 0;
        void Reset() {
            pred = 0;
            stepIdx = 0;
            samplePos = 0;
        }
    };

    void Reset() {
        if (m_prebaked) {
            return; // Keep pre-baked flat sfx_adpcm.bin resident across Ymz770::Reset()!
        }
        for (auto &e : m_entries) {
            e.state = 0;
            e.sampleRate = 16000u;
            e.totalSamples = 0;
            e.flatPtr = nullptr;
            e.flatBytes = 0;
            e.adpcm.clear();
        }
        m_usedBytes = 0;
    }

    bool IsPrebaked() const { return m_prebaked; }

    bool LoadPackData(std::vector<uint8_t> &&raw) {
        if (raw.size() < sizeof(DiskHeader)) {
            return false;
        }
        DiskHeader hdr{};
        std::memcpy(&hdr, raw.data(), sizeof(hdr));
        if (hdr.magic != kPackMagic || hdr.count == 0 || hdr.count > 256u) {
            return false;
        }
        const size_t dataBase = sizeof(DiskHeader) + static_cast<size_t>(hdr.count) * sizeof(DiskEntry);
        if (dataBase + hdr.totalBytes > raw.size()) {
            return false;
        }
        m_prebaked = false;
        Reset();
        m_flatPack = std::move(raw);
        for (auto &e : m_entries) {
            e.state = 2; // Any phrase not in pre-baked pack is streamed (BGM / long voice)
        }
        const auto *entries = reinterpret_cast<const DiskEntry *>(m_flatPack.data() + sizeof(DiskHeader));
        const uint8_t *payload = m_flatPack.data() + dataBase;
        for (uint32_t i = 0; i < hdr.count; ++i) {
            const DiskEntry &de = entries[i];
            if (de.phrase < 256u && de.dataOff + de.dataBytes <= hdr.totalBytes) {
                PhraseEntry &e = m_entries[de.phrase];
                e.state = 1;
                e.sampleRate = de.sampleRate;
                e.totalSamples = de.totalSamples;
                e.flatPtr = payload + de.dataOff;
                e.flatBytes = de.dataBytes;
            }
        }
        m_usedBytes = m_flatPack.size();
        m_prebaked = true;
        return true;
    }

    const PhraseEntry *Get(uint8_t phrase) const {
        return (m_entries[phrase].state == 1) ? &m_entries[phrase] : nullptr;
    }

    bool IsRejected(uint8_t phrase) const {
        return m_entries[phrase].state == 2;
    }

    void MarkRejected(uint8_t phrase) {
        m_entries[phrase].state = 2;
    }

    bool StorePcm(uint8_t phrase, uint32_t rate, const int16_t *pcm, uint32_t count) {
        if (count == 0 || count > kMaxSfxSamples) {
            m_entries[phrase].state = 2;
            return false;
        }
        const size_t needBytes = (count + 1u) >> 1;
        if (m_usedBytes + needBytes > kMaxCacheBytes) {
            m_entries[phrase].state = 2;
            return false;
        }
        PhraseEntry &e = m_entries[phrase];
        e.sampleRate = rate;
        e.totalSamples = count;
        e.adpcm.resize(needBytes);

        int32_t pred = 0;
        int32_t stepIdx = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t nib = EncodeNibble(pcm[i], pred, stepIdx);
            if ((i & 1u) == 0u) {
                e.adpcm[i >> 1] = nib;
            } else {
                e.adpcm[i >> 1] |= static_cast<uint8_t>(nib << 4);
            }
        }
        e.state = 1;
        m_usedBytes += needBytes;
        return true;
    }

    static int DecodeBlock(const PhraseEntry &e, DecoderState &st, int16_t *out, uint32_t maxSamples) {
        if (st.samplePos >= e.totalSamples) {
            return 0;
        }
        const uint32_t rem = e.totalSamples - st.samplePos;
        const uint32_t n = rem < maxSamples ? rem : maxSamples;
        int32_t pred = st.pred;
        int32_t stepIdx = st.stepIdx;
        uint32_t pos = st.samplePos;
        const uint8_t *src = e.flatPtr ? e.flatPtr : e.adpcm.data();
        for (uint32_t i = 0; i < n; ++i, ++pos) {
            const uint8_t byte = src[pos >> 1];
            const uint8_t nib = (pos & 1u) ? (byte >> 4) : (byte & 0x0Fu);
            out[i] = DecodeNibble(nib, pred, stepIdx);
        }
        st.pred = pred;
        st.stepIdx = stepIdx;
        st.samplePos = pos;
        return static_cast<int>(n);
    }

private:
    static constexpr int8_t kIndexTable[16] = {
        -1, -1, -1, -1, 2, 4, 6, 8,
        -1, -1, -1, -1, 2, 4, 6, 8
    };
    static constexpr int16_t kStepTable[89] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
        19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
        130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
        5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };

    static inline uint8_t EncodeNibble(int16_t sample, int32_t &pred, int32_t &stepIdx) {
        const int32_t step = kStepTable[stepIdx];
        int32_t diff = static_cast<int32_t>(sample) - pred;
        uint8_t nib = 0;
        if (diff < 0) {
            nib = 8;
            diff = -diff;
        }
        int32_t vpdiff = step >> 3;
        if (diff >= step) { nib |= 4; diff -= step; vpdiff += step; }
        if (diff >= (step >> 1)) { nib |= 2; diff -= (step >> 1); vpdiff += (step >> 1); }
        if (diff >= (step >> 2)) { nib |= 1; vpdiff += (step >> 2); }
        pred += (nib & 8) ? -vpdiff : vpdiff;
        pred = std::clamp(pred, -32768, 32767);
        stepIdx = std::clamp(stepIdx + kIndexTable[nib], 0, 88);
        return nib;
    }

    static inline int16_t DecodeNibble(uint8_t nib, int32_t &pred, int32_t &stepIdx) {
        const int32_t step = kStepTable[stepIdx];
        int32_t vpdiff = step >> 3;
        if (nib & 4) vpdiff += step;
        if (nib & 2) vpdiff += (step >> 1);
        if (nib & 1) vpdiff += (step >> 2);
        pred += (nib & 8) ? -vpdiff : vpdiff;
        pred = std::clamp(pred, -32768, 32767);
        stepIdx = std::clamp(stepIdx + kIndexTable[nib], 0, 88);
        return static_cast<int16_t>(pred);
    }

    PhraseEntry m_entries[256];
    std::vector<uint8_t> m_flatPack;
    size_t m_usedBytes = 0;
    bool m_prebaked = false;
};

} // namespace spelunk
