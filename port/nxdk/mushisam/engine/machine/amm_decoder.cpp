// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// Written for this project from ISO/IEC 11172-3 and 13818-3 and from what MAME's ymz770.cpp and mpeg_audio.cpp do with
// AMM: see amm_decoder.h and THIRD_PARTY_NOTICES.md.
#include "amm_decoder.h"

#include <cmath>
#include <cstring>
#if __has_include(<numbers>)
#include <numbers>
#else
namespace std::numbers {
inline constexpr double pi = 3.14159265358979323846;
}
#endif

namespace spelunk {

namespace {

// The sample rates: the standard's three (11172-3) and the half rates (13818-3's ID bit 0); the fourth code names none
constexpr int kRates[2][4] = {{44100, 48000, 32000, 0}, {22050, 24000, 16000, 0}};

// The numbers of levels a band's samples can be quantised to, in the order the tables below count them: 3, 5, 7 and
// 9, then 2^b - 1 for b = 4 to 16
constexpr int kQuantisers = 17;
constexpr int Levels(int q) {
    return q < 4 ? 3 + 2 * q : (1 << q) - 1;
}
constexpr int Quantiser(int levels) {
    for (int q = 0; q < kQuantisers; q++) {
        if (Levels(q) == levels) {
            return q;
        }
    }
    return -1;
}

// The allocation tables: for runs of bands, the bits of each band's allocation and the levels each allocation names
// (0: the band is not sent). 11172-3's Tables 3-B.2a to d (by bit rate and sample rate; AMM names one) and 13818-3's
// Table B.1 (the half rates).
struct Run {
    int bands, bits;
    int levels[16];
};
struct Table {
    int runs;
    Run run[4];
};
#define LADDER_TO_65535 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535
const Table kTables[5] = {
    // 3-B.2a: 27 bands
    {4,
     {{3, 4, {0, 3, 7, 15, LADDER_TO_65535}},
      {8, 4, {0, 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535}},
      {12, 3, {0, 3, 5, 7, 9, 15, 31, 65535}},
      {4, 2, {0, 3, 5, 65535}}}},
    // 3-B.2b: 30 bands
    {4,
     {{3, 4, {0, 3, 7, 15, LADDER_TO_65535}},
      {8, 4, {0, 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535}},
      {12, 3, {0, 3, 5, 7, 9, 15, 31, 65535}},
      {7, 2, {0, 3, 5, 65535}}}},
    // 3-B.2c: 8 bands
    {2, {{2, 4, {0, 3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767}}, {6, 3, {0, 3, 5, 9, 15, 31, 63, 127}}}},
    // 3-B.2d: 12 bands
    {2, {{2, 4, {0, 3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767}}, {10, 3, {0, 3, 5, 9, 15, 31, 63, 127}}}},
    // 13818-3 B.1: 30 bands
    {3,
     {{4, 4, {0, 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383}},
      {7, 3, {0, 3, 5, 9, 15, 31, 63, 127}},
      {19, 2, {0, 3, 5, 9}}}},
};
#undef LADDER_TO_65535
constexpr int kTableCount = 5;

struct Tables {
    // Each table's bands: the bits of the allocation, the quantiser for each allocation (-1: not sent)
    struct Band {
        int bits;
        int quantiser[16];
    };
    Band band[kTableCount][32];
    int limit[kTableCount]; // (the bands sent at all)

    // A quantiser: its code bits (three samples together when grouped), the bits of one sample as the standard
    // requantises it (nb), and the standard's C
    struct Q {
        int levels, sampleBits, codeBits;
        bool grouped;
        double c;
    } q[kQuantisers];

    // The scalefactors (11172-3 Table 3-B.1: 2^(1 - i/3), as the standard prints them, to 14 decimals; 63 is not
    // used, and scales to nothing)
    double scale[64];
    // The matrixing (11172-3 3-A.2's N, for the 32 values of V that differ: cos(i(2k + 1)pi/64), i, k = 0 to 31)
    double cosine[32][32];
    // The synthesis window (Table 3-B.3's D, multiples of 2^-16; D[i] for i = 0 to 256, the rest of it D[512 - i] =
    // -D[i], but where i is a multiple of 64, D[i]), arranged for Synthesise: the two taps of each of the 8 pairs of
    // vectors each output sample sums, their signs folded in, and which value of the vector each tap reads
    double tap[2][32][8];
    int from[2][32];
};

// 11172-3 Table 3-B.3's D[0] to D[256], times 65536. (The decoder this one replaced had them as decimals, 187 of the 512
// rounded at the eighth or ninth figure other than the standard's: a sample in about 9,000 came out one step apart.)
constexpr int kWindow[257] = {
    0,      -1,     -1,     -1,     -1,     -1,     -1,     -2,     -2,     -2,     -2,     -3,     -3,     -4,     -4,     -5,
    -5,     -6,     -7,     -7,     -8,     -9,     -10,    -11,    -13,    -14,    -16,    -17,    -19,    -21,    -24,    -26,
    -29,    -31,    -35,    -38,    -41,    -45,    -49,    -53,    -58,    -63,    -68,    -73,    -79,    -85,    -91,    -97,
    -104,   -111,   -117,   -125,   -132,   -139,   -147,   -154,   -161,   -169,   -176,   -183,   -190,   -196,   -202,   -208,
    213,    218,    222,    225,    227,    228,    228,    227,    224,    221,    215,    208,    200,    189,    177,    163,
    146,    127,    106,    83,     57,     29,     -2,     -36,    -72,    -111,   -153,   -197,   -244,   -294,   -347,   -401,
    -459,   -519,   -581,   -645,   -711,   -779,   -848,   -919,   -991,   -1064,  -1137,  -1210,  -1283,  -1356,  -1428,  -1498,
    -1567,  -1634,  -1698,  -1759,  -1817,  -1870,  -1919,  -1962,  -2001,  -2032,  -2057,  -2075,  -2085,  -2087,  -2080,  -2063,
    2037,   2000,   1952,   1893,   1822,   1739,   1644,   1535,   1414,   1280,   1131,   970,    794,    605,    402,    185,
    -45,    -288,   -545,   -814,   -1095,  -1388,  -1692,  -2006,  -2330,  -2663,  -3004,  -3351,  -3705,  -4063,  -4425,  -4788,
    -5153,  -5517,  -5879,  -6237,  -6589,  -6935,  -7271,  -7597,  -7910,  -8209,  -8491,  -8755,  -8998,  -9219,  -9416,  -9585,
    -9727,  -9838,  -9916,  -9959,  -9966,  -9935,  -9863,  -9750,  -9592,  -9389,  -9139,  -8840,  -8492,  -8092,  -7640,  -7134,
    6574,   5959,   5288,   4561,   3776,   2935,   2037,   1082,   70,     -998,   -2122,  -3300,  -4533,  -5818,  -7154,  -8540,
    -9975,  -11455, -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289, -30112, -31947, -33791, -35640,
    -37489, -39336, -41176, -43006, -44821, -46617, -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835, -73415, -73908, -74313, -74630, -74856, -74992,
    75038,
};

double Window(int i) {
    const int n = i <= 256 ? kWindow[i] : (i % 64 == 0 ? kWindow[512 - i] : -kWindow[512 - i]);
    return n / 65536.0;
}

const Tables &Built() {
    static const Tables t = [] {
        Tables t{};
        for (int q = 0; q < kQuantisers; q++) {
            Tables::Q &one = t.q[q];
            one.levels = Levels(q);
            one.grouped = one.levels == 3 || one.levels == 5 || one.levels == 9;
            one.sampleBits = 0;
            while ((1 << one.sampleBits) < one.levels) {
                one.sampleBits++;
            }
            one.codeBits = 3 * one.sampleBits;
            if (one.grouped) {
                // (three samples in one code of levels^3 values)
                one.codeBits = 0;
                while ((1 << one.codeBits) < one.levels * one.levels * one.levels) {
                    one.codeBits++;
                }
            }
            // (C = 2^nb / levels: a code's fraction (2v + 1 - levels) / 2^nb, with D added, times C is the level)
            one.c = static_cast<double>(1 << one.sampleBits) / one.levels;
        }
        for (int table = 0; table < kTableCount; table++) {
            int band = 0;
            for (int r = 0; r < kTables[table].runs; r++) {
                const Run &run = kTables[table].run[r];
                for (int i = 0; i < run.bands; i++, band++) {
                    t.band[table][band].bits = run.bits;
                    for (int a = 0; a < 16; a++) {
                        t.band[table][band].quantiser[a] = a < (1 << run.bits) && run.levels[a] ? Quantiser(run.levels[a]) : -1;
                    }
                }
            }
            t.limit[table] = band;
            for (; band < 32; band++) {
                t.band[table][band].bits = 0;
                for (int &q : t.band[table][band].quantiser) {
                    q = -1;
                }
            }
        }
        for (int i = 0; i < 63; i++) {
            // Portable pow/floor avoids nxdk pdclib's unimplemented exp2()/round() stubs.
            t.scale[i] = std::floor(std::pow(2.0, 1.0 - i / 3.0) * 1e14 + 0.5) / 1e14;
        }
        t.scale[63] = 0;
        for (int i = 0; i < 32; i++) {
            for (int k = 0; k < 32; k++) {
                t.cosine[i][k] = std::cos(i * (2 * k + 1) * std::numbers::pi / 64);
            }
        }
        // Output sample j sums, for each pair p of vectors (the 2p'th newest and the one before it), U[64p + j] D[64p + j]
        // and U[64p + 32 + j] D[64p + 32 + j]; U is V taken in halves (3-A.2), and V's 64 values are the 32 kept with
        // their signs: V[n] = X[16 + n] (n < 16), 0 (n = 16), -X[48 - n] (n <= 48), -X[n - 48]
        for (int j = 0; j < 32; j++) {
            for (int p = 0; p < 8; p++) {
                const double first = Window(64 * p + j), second = Window(64 * p + 32 + j);
                if (j < 16) {
                    t.tap[0][j][p] = first;   // (V[j] of the newer: X[16 + j])
                    t.tap[1][j][p] = -second; // (V[32 + j] of the older: -X[16 - j])
                } else {
                    t.tap[0][j][p] = -first; // (V[j]: 0 at 16, else -X[48 - j])
                    t.tap[1][j][p] = -second; // (V[32 + j]: -X[j - 16])
                }
            }
            t.from[0][j] = j < 16 ? 16 + j : 48 - j;
            t.from[1][j] = j <= 16 ? 16 - j : j - 16;
        }
        return t;
    }();
    return t;
}

} // namespace

AmmDecoder::AmmDecoder(const uint8_t *data) : m_data(data) {
    Built();
    Forget();
}

void AmmDecoder::Forget() {
    std::memset(m_history, 0, sizeof(m_history));
    m_newest[0] = m_newest[1] = 0;
}

uint32_t AmmDecoder::Bits::Unchecked(int count) {
    uint32_t value = 0;
    while (count > 0) {
        const int inByte = 8 - (at & 7);
        const int take = count < inByte ? count : inByte;
        const uint32_t byte = data[at >> 3];
        value = (value << take) | ((byte >> (inByte - take)) & ((1u << take) - 1));
        at += take;
        count -= take;
    }
    return value;
}

uint32_t AmmDecoder::Bits::Take(int count) {
    if (overrun || at + count > end) {
        overrun = true;
        return 0;
    }
    return Unchecked(count);
}

// The sync word, twelve ones, then the ID and layer bits of Layer II (either ID); anything else is passed a bit at a
// time. False if it is not found with the layer bits and four more before the end
bool AmmDecoder::FindSync(Bits &in) const {
    uint32_t window = in.Unchecked(12);
    for (;;) {
        while (window != 0xFFF && in.at < in.end) {
            window = ((window << 1) | in.Unchecked(1)) & 0xFFF;
        }
        if (in.end - in.at < 4) {
            return false;
        }
        const uint32_t idLayer = in.Unchecked(3);
        if (idLayer == 6 || idLayer == 2) {
            in.at -= 3;
            return true;
        }
        // (not a frame: the window moves on by the first of those bits)
        in.at -= 3;
        window = ((window << 1) | in.Unchecked(1)) & 0xFFF;
    }
}

int AmmDecoder::DecodeFrame(int &bit, int end, int16_t *pcm, int cbrTable) {
    if (end - bit < 16) {
        return -1;
    }
    const Tables &t = Built();
    Bits in{m_data, bit, end, false};
    if (!FindSync(in)) {
        return -1;
    }
    const bool halfRates = in.Unchecked(3) == 2;

    // The header (AMM's)
    const bool older = in.Take(1);
    const int packets = static_cast<int>(in.Take(4));
    const int rate = static_cast<int>(in.Take(2));
    const int lastSlices = static_cast<int>(in.Take(2));
    const int mode = static_cast<int>(in.Take(2));
    const int extension = static_cast<int>(in.Take(2));
    const int named = static_cast<int>(in.Take(3));
    in.Take(1);
    if (in.overrun) {
        return -1;
    }
    const int slices = older ? 36 : 3 * packets + lastSlices;
    const int table = older ? cbrTable : named;
    if (table < 0 || table >= kTableCount) {
        return -1; // (no table of the standards': none is used)
    }
    const int channels = mode == 3 ? 1 : 2;
    const int limit = t.limit[table];
    int bound = limit;
    if (mode == 1 && 4 * (extension + 1) < limit) {
        bound = 4 * (extension + 1); // (joint stereo: above the bound, one allocation and one set of samples for both)
    }

    // The bit allocations
    int quantiser[2][32];
    for (int band = 0; band < limit; band++) {
        const Tables::Band &b = t.band[table][band];
        if (band < bound) {
            for (int ch = 0; ch < channels; ch++) {
                quantiser[ch][band] = b.quantiser[in.Take(b.bits)];
            }
        } else {
            quantiser[0][band] = quantiser[1][band] = b.quantiser[in.Take(b.bits)];
        }
    }
    // The scalefactor selection and the scalefactors: three for the frame's three parts, sent as three, two (the first
    // pair's, then the third's; or the first's, then the last pair's) or one
    int selection[2][32];
    for (int band = 0; band < limit; band++) {
        for (int ch = 0; ch < channels; ch++) {
            selection[ch][band] = quantiser[ch][band] >= 0 ? static_cast<int>(in.Take(2)) : 0;
        }
    }
    double scale[2][3][32];
    for (int band = 0; band < limit; band++) {
        for (int ch = 0; ch < channels; ch++) {
            if (quantiser[ch][band] < 0) {
                continue;
            }
            int f[3];
            switch (selection[ch][band]) {
            case 0:
                f[0] = static_cast<int>(in.Take(6));
                f[1] = static_cast<int>(in.Take(6));
                f[2] = static_cast<int>(in.Take(6));
                break;
            case 1:
                f[0] = f[1] = static_cast<int>(in.Take(6));
                f[2] = static_cast<int>(in.Take(6));
                break;
            case 2: f[0] = f[1] = f[2] = static_cast<int>(in.Take(6)); break;
            default:
                f[0] = static_cast<int>(in.Take(6));
                f[1] = f[2] = static_cast<int>(in.Take(6));
                break;
            }
            for (int part = 0; part < 3; part++) {
                scale[ch][part][band] = t.scale[f[part]];
            }
        }
    }
    if (in.overrun) {
        return -1;
    }

    // The samples: twelve granules of three for each band (four granules to each third of the frame, each third with
    // its scalefactor), requantised and scaled, each granule three slices through the filterbank
    int made = 0;
    double sample[2][3][32];
    for (int granule = 0; granule < 12 && made < slices; granule++) {
        const int part = granule / 4;
        std::memset(sample, 0, sizeof(sample));
        for (int band = 0; band < limit; band++) {
            for (int ch = 0; ch < (band < bound ? channels : 1); ch++) {
                const int q = quantiser[ch][band];
                if (q < 0) {
                    continue;
                }
                const Tables::Q &qu = t.q[q];
                uint32_t codes[3];
                if (qu.grouped) {
                    uint32_t code = in.Take(qu.codeBits);
                    for (uint32_t &c : codes) {
                        c = code % qu.levels;
                        code /= qu.levels;
                    }
                } else {
                    for (uint32_t &c : codes) {
                        c = in.Take(qu.sampleBits);
                    }
                }
                // (the standard's s''' = C (s'' + D): s'' + D is (2v + 1 - levels) / 2^nb, exactly)
                const double half = static_cast<double>(1 << qu.sampleBits);
                for (int s = 0; s < 3; s++) {
                    const double level = (2.0 * codes[s] + 1 - qu.levels) / half * qu.c;
                    sample[ch][s][band] = level * scale[ch][part][band];
                    if (band >= bound && channels == 2) {
                        sample[1][s][band] = level * scale[1][part][band];
                    }
                }
            }
        }
        if (in.overrun) {
            return -1;
        }
        for (int s = 0; s < 3 && made < slices; s++, made++) {
            for (int ch = 0; ch < channels; ch++) {
                Synthesise(ch, sample[ch][s], limit, pcm + 32 * channels * made + ch, channels);
            }
        }
    }
    bit = in.at;
    m_rate = kRates[halfRates][rate];
    m_channels = channels;
    return 32 * made;
}

// One slice of a channel's 32 bands to 32 samples (11172-3 3-A.2): matrixed into the newest vector, then each sample
// the window's sum over the last sixteen
void AmmDecoder::Synthesise(int channel, const double *subbands, int bandsUsed, int16_t *pcm, int stride) {
    const Tables &t = Built();
    double (*history)[32] = m_history[channel];
    const int newest = m_newest[channel] = (m_newest[channel] + 15) & 15;
    double *v = history[newest];
    for (int i = 0; i < 32; i++) {
        double sum = 0;
        // (the bands above the table's are nothing, and adding nothing changes no sum)
        for (int k = 0; k < bandsUsed; k++) {
            sum += subbands[k] * t.cosine[i][k];
        }
        v[i] = sum;
    }
    for (int j = 0; j < 32; j++) {
        double out = 0;
        const int a = t.from[0][j], b = t.from[1][j];
        if (j == 16) {
            for (int p = 0; p < 8; p++) {
                out += history[(newest + 2 * p + 1) & 15][b] * t.tap[1][j][p];
            }
        } else {
            for (int p = 0; p < 8; p++) {
                out += history[(newest + 2 * p) & 15][a] * t.tap[0][j][p] + history[(newest + 2 * p + 1) & 15][b] * t.tap[1][j][p];
            }
        }
        // (to 16 bits: half added, then the fraction dropped toward zero)
        const double scaled = out * 32768 + 0.5;
        pcm[j * stride] = scaled <= -32768 ? -32768 : scaled >= 32767 ? 32767 : static_cast<int16_t>(static_cast<int>(scaled));
    }
}

} // namespace spelunk
