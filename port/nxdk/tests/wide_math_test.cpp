#include "../../../spelunkomp/machine/wide_math.h"

#include <cassert>
#include <cstdint>
#include <limits>

static uint64_t RefFloor(uint64_t a, uint64_t b, uint64_t d) {
    return static_cast<uint64_t>((static_cast<unsigned __int128>(a) * b) / d);
}

static uint64_t RefCeil(uint64_t a, uint64_t b, uint64_t d) {
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    return static_cast<uint64_t>((product + d - 1) / d);
}

int main() {
    struct Case { uint64_t a, b, d; };
    constexpr Case cases[] = {
        {0, 96000, 12000000},
        {1, 96000, 12000000},
        {12000000, 96000, 12000000},
        {123456789, 96000, 12000000},
        {std::numeric_limits<uint64_t>::max(), 96000, 12000000},
        {std::numeric_limits<uint64_t>::max(), 384000, 3072000},
        {9876543210123ull, 3000000, 384000},
    };

    for (const Case &c : cases) {
        assert(spelunk::MulDivFloor(c.a, c.b, c.d) == RefFloor(c.a, c.b, c.d));
        assert(spelunk::MulDivCeil(c.a, c.b, c.d) == RefCeil(c.a, c.b, c.d));
    }
    assert(spelunk::MulDivCeil(1, 1, 2) == 1);
    assert(spelunk::MulDivCeil(2, 1, 2) == 1);
}
