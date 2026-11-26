#pragma once

// Zero-overhead runtime helpers for whole-function AOT-recompiled SH-3 code (recomp_sh3.py).
// Keeps R0..R15, T, and cycles in Pentium III registers across RAM loads/stores;
// syncs c.cycles only on the unlikely MMIO/peripheral slow path.
#include "../machine/sh3.h"

namespace spelunk::recomp {

inline uint8 R8(SH3 &c, uint32 a, uint64 &cyc) {
    if (SH3::IsRam(a)) [[likely]] {
        return c.ram[a & (SH3::kRamSize - 1)];
    }
    c.cycles = cyc;
    const uint8 v = static_cast<uint8>(c.ReadSlowPublic(a, 1));
    cyc = c.cycles;
    return v;
}

inline uint16 R16(SH3 &c, uint32 a, uint64 &cyc) {
    if (SH3::IsRam(a)) [[likely]] {
        uint16 v;
        __builtin_memcpy(&v, c.ram + (a & (SH3::kRamSize - 2)), 2);
        return __builtin_bswap16(v);
    }
    c.cycles = cyc;
    const uint16 v = static_cast<uint16>(c.ReadSlowPublic(a & ~1u, 2));
    cyc = c.cycles;
    return v;
}

inline uint32 R32(SH3 &c, uint32 a, uint64 &cyc) {
    if (SH3::IsRam(a)) [[likely]] {
        uint32 v;
        __builtin_memcpy(&v, c.ram + (a & (SH3::kRamSize - 4)), 4);
        return __builtin_bswap32(v);
    }
    c.cycles = cyc;
    const uint32 v = c.ReadSlowPublic(a & ~3u, 4);
    cyc = c.cycles;
    return v;
}

inline void W8(SH3 &c, uint32 a, uint8 v, uint64 &cyc) {
    if (SH3::IsRam(a)) [[likely]] {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        c.ram[a & (SH3::kRamSize - 1)] = v;
        return;
    }
    c.cycles = cyc;
    c.WriteSlowPublic(a, v, 1);
    cyc = c.cycles;
}

inline void W16(SH3 &c, uint32 a, uint16 v, uint64 &cyc) {
    if (SH3::IsRam(a)) [[likely]] {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        v = __builtin_bswap16(v);
        __builtin_memcpy(c.ram + (a & (SH3::kRamSize - 2)), &v, 2);
        return;
    }
    c.cycles = cyc;
    c.WriteSlowPublic(a & ~1u, v, 2);
    cyc = c.cycles;
}

inline void W32(SH3 &c, uint32 a, uint32 v, uint64 &cyc) {
    if (SH3::IsRam(a)) [[likely]] {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        v = __builtin_bswap32(v);
        __builtin_memcpy(c.ram + (a & (SH3::kRamSize - 4)), &v, 4);
        return;
    }
    c.cycles = cyc;
    c.WriteSlowPublic(a & ~3u, v, 4);
    cyc = c.cycles;
}

} // namespace spelunk::recomp
