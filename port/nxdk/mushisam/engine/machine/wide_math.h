#pragma once

#include <cstdint>

namespace spelunk {

// Exact floor(a * b / divisor) without a wider-than-64-bit integer.
// Callers use bounded clock-rate multipliers so (a % divisor) * b fits uint64_t.
inline uint64_t MulDivFloor(uint64_t a, uint64_t b, uint64_t divisor) {
    const uint64_t quotient = a / divisor;
    const uint64_t remainder = a % divisor;
    return quotient * b + (remainder * b) / divisor;
}

// Exact ceiling(a * b / divisor) under the same bounded-multiplier condition.
inline uint64_t MulDivCeil(uint64_t a, uint64_t b, uint64_t divisor) {
    const uint64_t remainder = ((a % divisor) * b) % divisor;
    return MulDivFloor(a, b, divisor) + (remainder != 0);
}

} // namespace spelunk
