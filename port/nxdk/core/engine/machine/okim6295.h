#pragma once
// Standalone Original Xbox DoDonPachi build: OKI M6295 is unused (YMZ280B only).
#include <cstdint>
#include <vector>

namespace spelunk {
class Archive;
class OkiPair {
public:
    const uint8_t *rom[2] = {};
    uint32_t romSize[2] = {};
    std::vector<int16_t> out;
    bool held = false;
    void Reset() {}
    void RunTo(uint64_t, uint64_t) {}
    uint8_t Read(int, uint64_t = 0) const { return 0xFF; }
    void Write(int, uint8_t, uint64_t = 0) {}
    void SetBank(int, uint8_t) {}
    void Serialize(Archive &) {}
};
} // namespace spelunk
