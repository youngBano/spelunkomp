#pragma once

// Native Pentium III C++20 / MMX / SSE1 replacements for hot Mushihimesama SH-3 routines.
// Every routine preserves exact SH-3 register, RAM, and cycle semantics so differential
// parity tests against the reference SH-3 block interpreter pass with 0 mismatches.
#include "../machine/sh3.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace spelunk::game_native {

inline uint32 ReadRam32BE(const SH3 &c, uint32 phys) {
    uint32 v;
    __builtin_memcpy(&v, c.ram + (phys & ~3u), 4);
    return __builtin_bswap32(v);
}

inline void WriteRam32BE(SH3 &c, uint32 phys, uint32 v) {
    v = __builtin_bswap32(v);
    __builtin_memcpy(c.ram + (phys & ~3u), &v, 4);
}

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
    // Hot fast path: 8-byte non-overlapping copy (~82% of all calls from 0x0C0A9522!).
    if (cnt == 8u && (dPhys + 8u <= sPhys || sPhys + 8u <= dPhys)) [[likely]] {
        uint64_t tmp;
        __builtin_memcpy(&tmp, c.ram + sPhys, 8);
        __builtin_memcpy(c.ram + dPhys, &tmp, 8);
    } else if (dPhys + cnt <= sPhys || sPhys + cnt <= dPhys) {
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

// 0x0C0555DC: Sprite pivot alignment switch (inlines r5 == 4 [0x0C055608], r5 == 0 [0x0C0555F8], r5 == 3 [0x0C055604]).
// Eliminates ~23M RunBlocks() dispatches (both 0x0C055608 and caller 0x0C059F44 direct-call continuation!).
inline bool FastSpritePivot(SH3 &c) {
    const uint32 r4 = c.R[4];
    const uint32 mode = c.R[5];
    if (r4 == 0 || (mode != 4u && mode != 0u && mode != 3u) || c.interruptReady || !SH3::IsRam(r4)) {
        return false;
    }
    const uint32 p4 = r4 & (SH3::kRamSize - 1);
    if (p4 < c.codeAddrEnd || p4 + 44u > SH3::kRamSize) {
        return false;
    }
    if (mode == 4u) [[likely]] {
        if (c.cycles + 26ull >= c.blockStop) return false;
        uint32 r1 = ReadRam32BE(c, p4 + 36u);
        uint32 r2 = ReadRam32BE(c, p4 + 40u);
        const uint32 t = r2 & 1u;
        r1 = (0u - (r1 >> 1)) << 16;
        r2 = 0u - (r2 >> 1);
        const uint32 r5 = ReadRam32BE(c, p4 + 12u) + r1;
        const uint32 r6 = ReadRam32BE(c, p4 + 16u) + r2;
        WriteRam32BE(c, p4 + 4u, r5);
        WriteRam32BE(c, p4 + 8u, r6);
        c.R[0] = 0x0C055608u;
        c.R[1] = r1;
        c.R[2] = r2;
        c.R[5] = r5;
        c.R[6] = r6;
        c.T = t;
        c.cycles += 26ull;
        c.PC = c.PR;
        return true;
    }
    if (mode == 0u) {
        if (c.cycles + 19ull >= c.blockStop) return false;
        const uint32 r1 = ReadRam32BE(c, p4 + 12u);
        const uint32 r2 = ReadRam32BE(c, p4 + 16u);
        WriteRam32BE(c, p4 + 4u, r1);
        WriteRam32BE(c, p4 + 8u, r2);
        c.R[0] = 0x0C0555F8u;
        c.R[1] = r1;
        c.R[2] = r2;
        c.R[5] = 0u;
        c.T = 0u;
        c.cycles += 19ull;
        c.PC = c.PR;
        return true;
    }
    // mode == 3u (0x0C055604)
    if (c.cycles + 22ull >= c.blockStop) return false;
    uint32 r2 = ReadRam32BE(c, p4 + 40u);
    const uint32 t = r2 & 1u;
    r2 = 0u - (r2 >> 1);
    const uint32 r1 = ReadRam32BE(c, p4 + 12u);
    const uint32 r0 = ReadRam32BE(c, p4 + 16u) + r2;
    WriteRam32BE(c, p4 + 4u, r1);
    WriteRam32BE(c, p4 + 8u, r0);
    c.R[0] = r0;
    c.R[1] = r1;
    c.R[2] = r2;
    c.R[5] = 12u;
    c.T = t;
    c.cycles += 22ull;
    c.PC = c.PR;
    return true;
}

// 0x0C0556C8: Sprite blend/alpha flag builder (inlines both indirect jump-table calls into 0x0C055718[0..9]).
// Eliminates ~25M RunBlocks() dispatches (0x0C0556E8, 0x0C0556FA, 0x0C05574C/5C/68/etc.) with exact stack & cycle parity!
inline bool FastSpriteBlendFlags(SH3 &c) {
    const uint32 r4 = c.R[4];
    const uint32 r5In = c.R[5];
    const uint32 r6In = c.R[6];
    const uint32 r15 = c.R[15];
    if (r4 == 0 || r5In >= 10u || r6In >= 10u || c.interruptReady || !SH3::IsRam(r4) || !SH3::IsRam(r15) || r15 < 0x0C000008u) {
        return false;
    }
    const uint32 p4 = r4 & (SH3::kRamSize - 1);
    const uint32 p15 = r15 & (SH3::kRamSize - 1);
    if (p4 < c.codeAddrEnd || p4 + 4u > SH3::kRamSize || p15 < 8u || p15 + 4u > SH3::kRamSize || p15 - 8u < c.codeAddrEnd) {
        return false;
    }
    if (p4 == p15 - 4u || p4 == p15 - 8u) {
        return false;
    }
    auto evalBlend = [](uint32 idx, uint32 &r0, uint32 &r1, uint32 r2, uint32 &r3, bool &wroteR3) -> uint64 {
        switch (idx) {
        case 0: r0 = 0u; r1 = 0u; return 4ull;
        case 1: r0 = 0u; r1 = 0xFFu; return 5ull;
        case 2: r0 = 0u; return 4ull;
        case 3: r0 = 0u; r3 = r1; wroteR3 = true; r1 = 0xFFu - r3; return 6ull;
        case 4: r0 = 0u; r1 = r2; return 5ull;
        case 5: r0 = 0u; r1 = 0xFFu - r2; return 5ull;
        case 6: r0 = 1u; r1 = 0xFFu; return 5ull;
        case 7: r0 = 5u; r1 = 0xFFu; return 5ull;
        case 8: r0 = 2u; r1 = 0xFFu; return 5ull;
        default: r0 = 6u; r1 = 0xFFu; return 5ull;
        }
    };
    const uint32 r7 = c.R[7];
    const uint32 r2 = ReadRam32BE(c, p15);
    uint32 r0 = 0, r1 = r7, r3 = c.R[3];
    bool wroteR3 = false;
    const uint64 cyc1 = evalBlend(r6In, r0, r1, r2, r3, wroteR3);
    uint32 r8 = (ReadRam32BE(c, p4) | 0x02000000u) | (r0 << 16) | r1;
    r1 = r7;
    const uint64 cyc2 = evalBlend(r5In, r0, r1, r2, r3, wroteR3);
    const uint64 totalCyc = 35ull + cyc1 + cyc2;
    if (c.cycles + totalCyc >= c.blockStop) {
        return false;
    }
    r0 <<= 20;
    r1 <<= 8;
    r8 |= r0 | r1;
    // Mirror exact stack writes (push r8, push PR) so full-RAM parity hash matches bit-for-bit!
    WriteRam32BE(c, p15 - 4u, c.R[8]);
    WriteRam32BE(c, p15 - 8u, c.PR);
    WriteRam32BE(c, p4, r8);
    c.R[0] = r0;
    c.R[1] = r1;
    c.R[2] = r2;
    if (wroteR3) c.R[3] = r3;
    c.R[5] = 0x14u;
    c.R[6] = r6In << 2;
    c.T = 0u;
    c.cycles += totalCyc;
    c.PC = c.PR;
    return true;
}

// 0x0C0557D8: Blitter sprite command emitter (inlines the #1 path 0x0C055800 directly; 53 exact cycles!).
inline bool FastEmitBlitCmd(SH3 &c) {
    const uint32 r4 = c.R[4];
    if (c.interruptReady || !SH3::IsRam(r4) || c.cycles + 53ull >= c.blockStop) {
        return false;
    }
    const uint32 p4 = r4 & (SH3::kRamSize - 1);
    if (p4 < c.codeAddrEnd || p4 + 52u > SH3::kRamSize) {
        return false;
    }
    const uint32 r6 = ReadRam32BE(c, p4);
    const uint32 r5Off = ReadRam32BE(c, p4 + 44u);
    if ((r6 & 0x0C000000u) != 0u || r5Off != 0u) {
        return false; // 0x0C055804/08/0C/... handled via direct tail-dispatch in F_0C0557D8!
    }
    constexpr uint32 kListBasePhys = 0x00049B34u;
    const uint32 cmdPtr = ReadRam32BE(c, kListBasePhys + 8u);
    if (!SH3::IsRam(cmdPtr)) {
        return false;
    }
    const uint32 pCmd = cmdPtr & (SH3::kRamSize - 1);
    if (pCmd < c.codeAddrEnd || pCmd + 20u > SH3::kRamSize) {
        return false;
    }
    // Guard against pathological overlap with sprite descriptor or global list pointers.
    if (p4 == kListBasePhys || p4 == kListBasePhys + 4u || p4 == kListBasePhys + 8u) {
        return false;
    }
    WriteRam32BE(c, p4, 0u);
    uint32 r5 = ReadRam32BE(c, kListBasePhys) + ReadRam32BE(c, p4 + 4u);
    uint32 r3 = (ReadRam32BE(c, p4 + 8u) + ReadRam32BE(c, kListBasePhys + 4u)) & 0xFFFFu;
    r5 |= r3;
    r3 = ReadRam32BE(c, p4 + 48u);
    const uint32 r1 = (ReadRam32BE(c, p4 + 28u) << 16) | ReadRam32BE(c, p4 + 32u);
    const uint32 r2 = ((ReadRam32BE(c, p4 + 36u) << 16) | ReadRam32BE(c, p4 + 40u)) - 0x00010001u;
    WriteRam32BE(c, pCmd + 0u, r6);
    WriteRam32BE(c, pCmd + 4u, r1);
    WriteRam32BE(c, pCmd + 8u, r5);
    WriteRam32BE(c, pCmd + 12u, r2);
    WriteRam32BE(c, pCmd + 16u, r3);
    const uint32 r7 = cmdPtr + 20u;
#ifndef XBOX_IMMUTABLE_CODE
    c.NoteCodeWrite(0x0C049B3Cu);
#endif
    WriteRam32BE(c, kListBasePhys + 8u, r7);
    c.R[0] = 0x0C049B3Cu;
    c.R[1] = r1;
    c.R[2] = r2;
    c.R[3] = r3;
    c.R[4] = 0x00010001u;
    c.R[5] = r5;
    c.R[6] = r6;
    c.R[7] = r7;
    c.cycles += 53ull;
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
