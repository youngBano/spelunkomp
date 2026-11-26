// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp; from cps2recomp: https://github.com/tmcleroy/cps2recomp
//
// What translated 68000 code is written in terms of (tools/m68k/recomp.py writes the code; this is included by it).
//
// A translated routine is a C++ function over the CPU's state: the game's instructions as statements, its
// branches as gotos. It is entered at the block cpu.pc names and returns when control leaves the routine
// (a return, a computed jump, a call that did not come back where expected) or when time is up, with
// cpu.pc saying where to go on. Nothing is kept on the host's stack that is not also in the CPU's state, so
// returning early is always right, and calls between routines on the host's stack are only a short cut.
#pragma once

#include "../machine/m68k.h"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace spelunk {

// A block's start and the routine it is in; and the stretch of program a block covers
struct NativeEntry {
    uint32_t address;
    NativeFn fn;
};
struct NativeRange {
    uint32_t start, end;
};
struct NativeCode {
    const NativeEntry *entries;
    size_t entryCount;
    const NativeRange *ranges;
    size_t rangeCount;
    const uint32_t *hooks; // the addresses at which the translated code stops by for a hook
    size_t hookCount;
};
// The game's translated code (generated; an empty set when there is none)
extern const NativeCode kNativeCode;
bool HasNativeCode();

// A hook: the game's sources' own code at a place in the program (where a block starts). True: it has done the
// work of the code there and set the PC (Return leaves a routine); false: the game's code runs as ever.
// tools/m68k/recomp.py reads the sources for AddHook(0x...), so that translated code stops by there.
using HookFn = bool (*)(M68K &c);
void AddHook(uint32_t address, const char *name, HookFn fn);
void ClearHooks();
bool RunHook(M68K &c, uint32_t address);
inline void Return(M68K &c) {
    c.pc = c.Pop32();
}

#ifdef SPELUNK_CHECK68K
namespace check {
    extern bool side, noting;
    void Note(uint32_t address, unsigned size, uint32_t value);
} // namespace check
#endif
uint64_t BlocksChecked();
// (blocks the check ran but could not compare: they read a chip they had written to)
uint64_t BlocksUnsure();

namespace native {

    constexpr unsigned kMaxDepth = 64;

    // A routine was entered at an address none of its blocks starts at (the tables are wrong)
    [[noreturn]] void NotHere(M68K &c, uint32_t routine);

#ifdef SPELUNK_CHECK68K
    // The check build: translated code runs a block at a time on the side, its writes kept in a list and
    // compared with what the interpreter does (runtime/recomp68k.cpp)
    uint32_t CheckRead(M68K &c, uint32_t address, unsigned size);
    void CheckWrite(M68K &c, uint32_t address, unsigned size, uint32_t value);
    // (the board's waits as the interpreter's Read and Write have them: out of line, as they are rare)
    uint32_t WaitedCheckRead(M68K &c, uint32_t address, unsigned size);
    void WaitedCheckWrite(M68K &c, uint32_t address, unsigned size, uint32_t value);
    inline uint32_t R8(M68K &c, uint32_t address) { return WaitedCheckRead(c, address, 0); }
    inline uint32_t R16(M68K &c, uint32_t address) { return WaitedCheckRead(c, address, 1); }
    inline uint32_t R32(M68K &c, uint32_t address) { return WaitedCheckRead(c, address, 2); }
    inline void W8(M68K &c, uint32_t address, uint32_t value) { WaitedCheckWrite(c, address, 0, value); }
    inline void W16(M68K &c, uint32_t address, uint32_t value) { WaitedCheckWrite(c, address, 1, value); }
    inline void W32(M68K &c, uint32_t address, uint32_t value) { WaitedCheckWrite(c, address, 2, value); }
#else
    // Memory as data: work RAM and the ROM directly (no board waits there), everything else through the board with
    // its waits (M68K::DataWait; out of line, runtime/recomp68k.cpp: inlined into every access of a large routine
    // it made the compiler take hours)
    uint32_t BusRead8(M68K &c, uint32_t address);
    uint32_t BusRead16(M68K &c, uint32_t address);
    void BusWrite8(M68K &c, uint32_t address, uint32_t value);
    void BusWrite16(M68K &c, uint32_t address, uint32_t value);
    inline uint32_t R8(M68K &c, uint32_t address) {
        address &= 0xFFFFFF;
        if (address - c.ramBase < c.ramSize) {
            return c.ram[address - c.ramBase];
        }
        if (address < c.romSize) {
            return c.rom[address];
        }
        return BusRead8(c, address);
    }
    inline uint32_t R16(M68K &c, uint32_t address) {
        address &= 0xFFFFFE;
        if (address - c.ramBase < c.ramSize) {
            const uint8_t *p = &c.ram[address - c.ramBase];
            return static_cast<uint32_t>((p[0] << 8) | p[1]);
        }
        if (address < c.romSize) {
            const uint8_t *p = &c.rom[address];
            return static_cast<uint32_t>((p[0] << 8) | p[1]);
        }
        return BusRead16(c, address);
    }
    inline uint32_t R32(M68K &c, uint32_t address) {
        const uint32_t high = R16(c, address);
        c.accessInto += 4;
        return (high << 16) | R16(c, address + 2);
    }
    inline void W8(M68K &c, uint32_t address, uint32_t value) {
        address &= 0xFFFFFF;
        if (address - c.ramBase < c.ramSize) {
            c.ram[address - c.ramBase] = static_cast<uint8_t>(value);
        } else {
            BusWrite8(c, address, value);
        }
    }
    inline void W16(M68K &c, uint32_t address, uint32_t value) {
        address &= 0xFFFFFE;
        if (address - c.ramBase < c.ramSize) {
            uint8_t *p = &c.ram[address - c.ramBase];
            p[0] = static_cast<uint8_t>(value >> 8);
            p[1] = static_cast<uint8_t>(value);
        } else {
            BusWrite16(c, address, value);
        }
    }
    inline void W32(M68K &c, uint32_t address, uint32_t value) {
        W16(c, address, value >> 16);
        c.accessInto += 4;
        W16(c, address + 2, value);
    }
#endif

    // Memory as program (what addressing relative to the PC reads)
    inline uint32_t P16(M68K &c, uint32_t address) {
        address &= 0xFFFFFE;
        if (address < c.romSize) {
            const uint8_t *p = &c.rom[address];
            return static_cast<uint32_t>((p[0] << 8) | p[1]);
        }
        return c.bus.Fetch16(address);
    }
    inline uint32_t P8(M68K &c, uint32_t address) {
        const uint32_t word = P16(c, address);
        return (address & 1) ? (word & 0xFF) : (word >> 8);
    }
    inline uint32_t P32(M68K &c, uint32_t address) {
        const uint32_t high = P16(c, address);
        return (high << 16) | P16(c, address + 2);
    }

    inline uint32_t S8(uint32_t value) {
        return static_cast<uint32_t>(static_cast<int8_t>(value));
    }
    inline uint32_t S16(uint32_t value) {
        return static_cast<uint32_t>(static_cast<int16_t>(value));
    }
    // A register's low byte or word replaced
    inline void Set8(uint32_t &reg, uint32_t value) {
        reg = (reg & 0xFFFFFF00) | (value & 0xFF);
    }
    inline void Set16(uint32_t &reg, uint32_t value) {
        reg = (reg & 0xFFFF0000) | (value & 0xFFFF);
    }

} // namespace native

} // namespace spelunk

// --- how a routine's control flow is written ---

// The start of a block: time is checked here, and nowhere else
#define BLOCK(label, address)                                                                                          \
    label:                                                                                                             \
    if (c.cycles >= c.budget) {                                                                                        \
        c.pc = address;                                                                                                \
        return;                                                                                                        \
    }
// A block the game's sources have a hook at
#define HOOK(address)                                                                                                  \
    if (spelunk::RunHook(c, address)) {                                                                                \
        return;                                                                                                        \
    }
#define WATCH(address)
// Leaving the routine for the block at `address`
#define LEAVE(address)                                                                                                 \
    do {                                                                                                               \
        c.pc = address;                                                                                                \
        return;                                                                                                        \
    } while (0)

#ifdef SPELUNK_CHECK68K
// (the check build runs one block per call)
#define GOTO(label, address) LEAVE(address)
#define FALL(address) LEAVE(address)
#define CALL(fn, address, label, back) LEAVE(address)
#define CALL_AWAY(fn, address) LEAVE(address)
#else
// To a block of this routine
#define GOTO(label, address) goto label
// Running on into the next block
#define FALL(address)
// A call of the game's (the return address is already on its stack) to the block at `address`, in routine
// `fn`; `label` is the block after the call, at `back`. If the routine comes back anywhere else (its time ran
// out, it switched stacks, it dropped its return address), so does this one.
#define CALL(fn, address, label, back)                                                                                 \
    do {                                                                                                               \
        c.pc = address;                                                                                                \
        if (c.depth >= spelunk::native::kMaxDepth) {                                                                   \
            return;                                                                                                    \
        }                                                                                                              \
        c.depth++;                                                                                                     \
        fn(c);                                                                                                         \
        c.depth--;                                                                                                     \
        if (c.pc != back) {                                                                                            \
            return;                                                                                                    \
        }                                                                                                              \
        goto label;                                                                                                    \
    } while (0)
// The same where the block after the call is not in this routine
#define CALL_AWAY(fn, address) LEAVE(address)
#endif
