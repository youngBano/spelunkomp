#pragma once
// Standalone Original Xbox DoDonPachi build: Z80/YMFM sound board is unused (YMZ280B only).
#include "okim6295.h"
#include <cstdint>
#include <vector>

namespace spelunk {
class Archive;
class CaveZ80 {
public:
    enum class Kind { Mazinger, AirGallet };
    std::vector<uint8_t> rom;
    OkiPair oki;
    std::vector<int16_t> out;
    bool held = false;
    void Reset(Kind, uint64_t = 0) {}
    void RunTo(uint64_t) {}
    void Command(uint16_t) {}
    bool AnswerWaits(unsigned = 0) const { return false; }
    uint8_t Answer(unsigned = 0) const { return 0xFF; }
    void TakeAnswer() {}
    void Serialize(Archive &) {}
    template <typename Mix>
    void Hash(Mix &&) const {}
};
} // namespace spelunk
