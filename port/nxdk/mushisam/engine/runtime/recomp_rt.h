#pragma once

// Zero-overhead runtime helpers for whole-function AOT-recompiled SH-3 code (recomp_sh3.py).
// Uses 32-bit `int32 &rem` (cycles remaining until blockStop) and single-compare 0x0Cxxxxxx RAM
// fast path so Pentium III keeps R0..R15, T, and `rem` in 32-bit GPRs with zero 64-bit spills.
#include "../machine/sh3.h"

namespace spelunk::recomp {

inline uint8 R8(SH3 &c, uint32 a, sint32 &rem) {
    const uint32 off = a - 0x0C000000u;
    if (off < SH3::kRamSize) [[likely]] {
        return c.ram[off];
    }
    if (SH3::IsRam(a)) {
        return c.ram[a & (SH3::kRamSize - 1)];
    }
    c.cycles = static_cast<uint64>(static_cast<sint64>(c.blockStop) - rem);
    const uint8 v = static_cast<uint8>(c.ReadSlowPublic(a, 1));
    rem = static_cast<sint32>(static_cast<sint64>(c.blockStop) - static_cast<sint64>(c.cycles));
    return v;
}

inline uint16 R16(SH3 &c, uint32 a, sint32 &rem) {
    const uint32 off = a - 0x0C000000u;
    if (off < SH3::kRamSize) [[likely]] {
        uint16 v;
        __builtin_memcpy(&v, c.ram + (off & ~1u), 2);
        return __builtin_bswap16(v);
    }
    if (SH3::IsRam(a)) {
        uint16 v;
        __builtin_memcpy(&v, c.ram + (a & (SH3::kRamSize - 2)), 2);
        return __builtin_bswap16(v);
    }
    c.cycles = static_cast<uint64>(static_cast<sint64>(c.blockStop) - rem);
    const uint16 v = static_cast<uint16>(c.ReadSlowPublic(a & ~1u, 2));
    rem = static_cast<sint32>(static_cast<sint64>(c.blockStop) - static_cast<sint64>(c.cycles));
    return v;
}

inline uint32 R32(SH3 &c, uint32 a, sint32 &rem) {
    const uint32 off = a - 0x0C000000u;
    if (off < SH3::kRamSize) [[likely]] {
        uint32 v;
        __builtin_memcpy(&v, c.ram + (off & ~3u), 4);
        return __builtin_bswap32(v);
    }
    if (SH3::IsRam(a)) {
        uint32 v;
        __builtin_memcpy(&v, c.ram + (a & (SH3::kRamSize - 4)), 4);
        return __builtin_bswap32(v);
    }
    c.cycles = static_cast<uint64>(static_cast<sint64>(c.blockStop) - rem);
    const uint32 v = c.ReadSlowPublic(a & ~3u, 4);
    rem = static_cast<sint32>(static_cast<sint64>(c.blockStop) - static_cast<sint64>(c.cycles));
    return v;
}

inline void W8(SH3 &c, uint32 a, uint8 v, sint32 &rem) {
    const uint32 off = a - 0x0C000000u;
    if (off < SH3::kRamSize) [[likely]] {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        c.ram[off] = v;
        return;
    }
    if (SH3::IsRam(a)) {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        c.ram[a & (SH3::kRamSize - 1)] = v;
        return;
    }
    c.cycles = static_cast<uint64>(static_cast<sint64>(c.blockStop) - rem);
    c.WriteSlowPublic(a, v, 1);
    rem = static_cast<sint32>(static_cast<sint64>(c.blockStop) - static_cast<sint64>(c.cycles));
}

inline void W16(SH3 &c, uint32 a, uint16 v, sint32 &rem) {
    const uint32 off = a - 0x0C000000u;
    if (off < SH3::kRamSize) [[likely]] {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        v = __builtin_bswap16(v);
        __builtin_memcpy(c.ram + (off & ~1u), &v, 2);
        return;
    }
    if (SH3::IsRam(a)) {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        v = __builtin_bswap16(v);
        __builtin_memcpy(c.ram + (a & (SH3::kRamSize - 2)), &v, 2);
        return;
    }
    c.cycles = static_cast<uint64>(static_cast<sint64>(c.blockStop) - rem);
    c.WriteSlowPublic(a & ~1u, v, 2);
    rem = static_cast<sint32>(static_cast<sint64>(c.blockStop) - static_cast<sint64>(c.cycles));
}

inline void W32(SH3 &c, uint32 a, uint32 v, sint32 &rem) {
    const uint32 off = a - 0x0C000000u;
    if (off < SH3::kRamSize) [[likely]] {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        v = __builtin_bswap32(v);
        __builtin_memcpy(c.ram + (off & ~3u), &v, 4);
        return;
    }
    if (SH3::IsRam(a)) {
#ifndef XBOX_IMMUTABLE_CODE
        c.NoteCodeWrite(a);
#endif
        v = __builtin_bswap32(v);
        __builtin_memcpy(c.ram + (a & (SH3::kRamSize - 4)), &v, 4);
        return;
    }
    c.cycles = static_cast<uint64>(static_cast<sint64>(c.blockStop) - rem);
    c.WriteSlowPublic(a & ~3u, v, 4);
    rem = static_cast<sint32>(static_cast<sint64>(c.blockStop) - static_cast<sint64>(c.cycles));
}

} // namespace spelunk::recomp
