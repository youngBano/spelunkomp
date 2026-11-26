#pragma once

// Native Pentium III C++20 / MMX / SSE1 replacements for hot Mushihimesama SH-3 routines.
// Every routine preserves exact SH-3 register, RAM, and cycle semantics so differential
// parity tests against the reference SH-3 block interpreter pass with 0 mismatches.
#include "../machine/sh3.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace spelunk::game_native {

// 0x0C04F37C: byte-by-byte memcpy loop:
//   L: mov.b @r5+, r0; mov.b r0, @r4; dt r6; bf/s L; add #1, r4; rts; nop
// 5 cycles per taken iteration; final iteration: mov.b(1) + mov.b(1) + dt(1) + bf/s(1) + add(1) + rts(2) + nop(1) = 8 cycles.
inline bool FastMemcpyByteLoop(SH3 &c) {
    const uint32 dst = c.R[4];
    const uint32 src = c.R[5];
    const uint32 cnt = c.R[6];
    if (cnt == 0 || !SH3::IsRam(dst) || !SH3::IsRam(src) || c.interruptReady) {
        return false;
    }
    const uint32 dPhys = dst & (SH3::kRamSize - 1);
    const uint32 sPhys = src & (SH3::kRamSize - 1);
    if (dPhys + cnt > SH3::kRamSize || sPhys + cnt > SH3::kRamSize) {
        return false;
    }
    // Stay strictly before blockStop so frame/timer/IRQ boundaries remain exact.
    const uint64 totalCyc = static_cast<uint64>(cnt - 1) * 5ull + 8ull;
    if (c.cycles + totalCyc >= c.blockStop) {
        return false;
    }
    // Non-overlapping fast path (std::memmove handles overlap safely too, though byte-by-byte forward copy matches memmove when dst <= src).
    if (dPhys + cnt <= sPhys || sPhys + cnt <= dPhys) {
        std::memcpy(c.ram + dPhys, c.ram + sPhys, cnt);
    } else if (dPhys < sPhys) {
        std::memmove(c.ram + dPhys, c.ram + sPhys, cnt);
    } else {
        for (uint32 i = 0; i < cnt; ++i) {
            c.ram[dPhys + i] = c.ram[sPhys + i];
        }
    }
    c.R[0] = static_cast<uint32>(static_cast<sint32>(static_cast<sint8>(c.ram[dPhys + cnt - 1])));
    c.R[4] = dst + cnt;
    c.R[5] = src + cnt;
    c.R[6] = 0;
    c.T = 1;
    c.cycles += totalCyc;
    c.PC = c.PR;
    return true;
}

// 0x0C04F370: byte-by-byte memset loop:
//   L: mov.b r5, @r4; dt r6; bf/s L; add #1, r4; rts; nop
// 4 cycles per taken iteration; final iteration: mov.b(1) + dt(1) + bf/s(1) + add(1) + rts(2) + nop(1) = 7 cycles.
inline bool FastMemsetByteLoop(SH3 &c) {
    const uint32 dst = c.R[4];
    const uint8 val = static_cast<uint8>(c.R[5]);
    const uint32 cnt = c.R[6];
    if (cnt == 0 || !SH3::IsRam(dst) || c.interruptReady) {
        return false;
    }
    const uint32 dPhys = dst & (SH3::kRamSize - 1);
    if (dPhys + cnt > SH3::kRamSize) {
        return false;
    }
    const uint64 totalCyc = static_cast<uint64>(cnt - 1) * 4ull + 7ull;
    if (c.cycles + totalCyc >= c.blockStop) {
        return false;
    }
    std::memset(c.ram + dPhys, val, cnt);
    c.R[4] = dst + cnt;
    c.R[6] = 0;
    c.T = 1;
    c.cycles += totalCyc;
    c.PC = c.PR;
    return true;
}

} // namespace spelunk::game_native
