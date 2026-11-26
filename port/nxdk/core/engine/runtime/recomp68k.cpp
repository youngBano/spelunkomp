// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp; from cps2recomp: https://github.com/tmcleroy/cps2recomp
//
// Running the 68000: the loop around translated code and the interpreter (M68K::Run).
//
// Time and interrupts are looked at only where a block starts (or at any instruction that has no
// translation). The interpreter can be run the same way (Mode::Blocks), and then does exactly what the
// translated code does, cycle for cycle: that is what translated code is checked against.
#include "native68k.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace spelunk {

namespace {
    // By address / 2: the routine a block starting there is in; whether a block starts there; whether the
    // instruction there is inside a translated block
    struct Tables {
        std::vector<NativeFn> fn;
        std::vector<uint8_t> covered;
        uint32_t limit = 0; // addresses below this are in the tables
        Tables() {
            for (size_t n = 0; n < kNativeCode.rangeCount; n++) {
                limit = std::max(limit, kNativeCode.ranges[n].end);
            }
            for (size_t n = 0; n < kNativeCode.entryCount; n++) {
                limit = std::max(limit, kNativeCode.entries[n].address + 2);
            }
            fn.assign(limit / 2 + 1, nullptr);
            covered.assign(limit / 2 + 1, 0);
            for (size_t n = 0; n < kNativeCode.entryCount; n++) {
                fn[kNativeCode.entries[n].address / 2] = kNativeCode.entries[n].fn;
            }
            for (size_t n = 0; n < kNativeCode.rangeCount; n++) {
                for (uint32_t at = kNativeCode.ranges[n].start; at < kNativeCode.ranges[n].end; at += 2) {
                    covered[at / 2] = 1;
                }
            }
        }
        NativeFn At(uint32_t pc) const {
            return (pc < limit && !(pc & 1)) ? fn[pc / 2] : nullptr;
        }
        // Inside a block, not at its start: time is not looked at here
        bool Inside(uint32_t pc) const {
            return pc < limit && !(pc & 1) && covered[pc / 2] && !fn[pc / 2];
        }
    };
    const Tables &GetTables() {
        static const Tables tables;
        return tables;
    }

    struct Hook {
        uint32_t address;
        const char *name;
        HookFn fn;
    };
    std::vector<Hook> g_hooks;
    std::vector<uint16_t> g_hooked; // by address / 2: the hook's number + 1
    bool g_hooksChecked = false;

    // Translated code only stops by at the hooks the translator knew of
    void CheckHooks() {
        g_hooksChecked = true;
        for (const Hook &hook : g_hooks) {
            bool known = false;
            for (size_t n = 0; n < kNativeCode.hookCount; n++) {
                known = known || kNativeCode.hooks[n] == hook.address;
            }
            if (!known && GetTables().At(hook.address)) {
                fprintf(stderr, "the hook %s at %06X was added after the game's code was translated: translate again\n", hook.name, hook.address);
                exit(1);
            }
            if (!known && GetTables().Inside(hook.address)) {
                fprintf(stderr, "the hook %s is at %06X, which is not where a block starts\n", hook.name, hook.address);
                exit(1);
            }
        }
    }
} // namespace

namespace native {
    void NotHere(M68K &c, uint32_t routine) {
        fprintf(stderr, "translated routine %06X entered at %06X, where it has no block\n", routine, c.pc);
        abort();
    }
} // namespace native

void AddHook(uint32_t address, const char *name, HookFn fn) {
    g_hooks.push_back({address, name, fn});
    if (g_hooked.size() <= address / 2) {
        g_hooked.resize(address / 2 + 1, 0);
    }
    g_hooked[address / 2] = static_cast<uint16_t>(g_hooks.size());
    g_hooksChecked = false;
}

void ClearHooks() {
    g_hooks.clear();
    g_hooked.clear();
}

bool RunHook(M68K &c, uint32_t address) {
    if (address / 2 >= g_hooked.size() || (address & 1) || !g_hooked[address / 2]) {
        return false;
    }
#ifdef SPELUNK_CHECK68K
    if (check::side) {
        return false; // (the run loop has asked the hook already)
    }
#endif
    c.pc = address; // (translated code does not keep it as it goes: a hook sees where it is)
    return g_hooks[g_hooked[address / 2] - 1].fn(c);
}

// The game's routine at `address`, called from C++: run until it returns to a place that is nowhere
void M68K::CallGuest(uint32_t address) {
    constexpr uint32_t kNowhere = 0x00FEDCBA & ~1u;
    const Tables &tables = GetTables();
    const uint64_t keptBudget = budget;
    const unsigned keptDepth = depth;
    budget = ~0ull;
    Push32(kNowhere);
    pc = address;
    for (uint64_t n = 0; pc != kNowhere; n++) {
        if (n > 50000000) {
            fprintf(stderr, "the game's routine at %06X, called from C++, did not come back (it must not wait for anything)\n", address);
            exit(1);
        }
        if (!g_hooked.empty() && RunHook(*this, pc)) {
            continue;
        }
#ifndef SPELUNK_CHECK68K
        if (const NativeFn fn = tables.At(pc)) {
            depth = 0;
            fn(*this);
            continue;
        }
#else
        (void)tables;
#endif
        m_cycles = 0;
        ExecuteOne();
        cycles += m_cycles;
    }
    budget = keptBudget;
    depth = keptDepth;
}

bool HasNativeCode() {
    return kNativeCode.entryCount != 0;
}

void M68K::Record(uint32_t address) {
    if (recording && address < romSize && !(address & 1) && !GetTables().At(address)) {
        newEntries.push_back(address);
        if (newEntries.size() > 100000) {
            // (kept small: the same places come again and again)
            std::sort(newEntries.begin(), newEntries.end());
            newEntries.erase(std::unique(newEntries.begin(), newEntries.end()), newEntries.end());
        }
    }
}

namespace native {
    uint32_t BusRead8(M68K &c, uint32_t address) {
        c.DataWait(address, false);
        return c.bus.Read8(address);
    }
    uint32_t BusRead16(M68K &c, uint32_t address) {
        c.DataWait(address, false);
        return c.bus.Read16(address);
    }
    void BusWrite8(M68K &c, uint32_t address, uint32_t value) {
        c.DataWait(address, true);
        c.bus.Write8(address, static_cast<uint8_t>(value));
    }
    void BusWrite16(M68K &c, uint32_t address, uint32_t value) {
        c.DataWait(address, true);
        c.bus.Write16(address, static_cast<uint16_t>(value));
    }
} // namespace native

void M68K::Run(uint64_t until, Mode mode, bool exact) {
    const Tables &tables = GetTables();
    budget = until;
    // (the interpreter stops by anywhere: a development trace may hook what it likes there)
    if (!g_hooksChecked && mode == Mode::Native) {
        CheckHooks();
    }
    if (mode == Mode::Check) {
        RunChecked(until, exact);
        return;
    }
    for (;;) {
        // (an interrupt can only have become takable since the last look where the loop is come back to:
        // the lines change between runs, and instructions that change the mask end their block and leave
        // their routine)
        if (TakeInterrupt()) {
            cycles += InterruptTime();
            Record(pc);
        }
        budget = until;
        if (cycles >= until) {
            return;
        }
        if (stopped) {
            cycles = until;
            return;
        }
        // An instruction at a time where each one's time counts: the last stretch of an exact run, and while an
        // interrupt waits for an instruction's last prefetch (the interpreter and translated code alike)
        const bool single = (exact && cycles + exactTail >= until) || InterruptWaiting();
        if (keepHistory) {
            Remember(pc);
        }
        if (mode == Mode::Native && !single) {
            if (const NativeFn fn = tables.At(pc)) {
                depth = 0;
                if (exact) {
                    budget = until > exactTail ? until - exactTail : 0;
                }
                fn(*this);  // (which stops by at hooks itself)
                Record(pc); // (where translated code came out, if no block starts there)
                m_lastTail = 4;
                continue;
            }
        }
        if (!g_hooked.empty() && RunHook(*this, pc)) {
            m_lastTail = 4;
            continue;
        }
        // The interpreter: one instruction; on through the rest of a block when inside one
        do {
            m_cycles = 0;
            m_jumped = false;
            if (keepHistory) {
                Remember(pc);
            }
            ExecuteOne();
            cycles += m_cycles;
            interpreted++;
            if (m_jumped) {
                Record(pc); // (a computed jump, a return, an exception: a place the translator should start from)
            }
        } while (mode != Mode::Interpreter && !single && !stopped && !Returned() && tables.Inside(pc));
        if (mode != Mode::Interpreter && !single) {
            m_lastTail = 4; // (as after translated code, which does not tell)
        }
    }
}

#ifndef SPELUNK_CHECK68K
void M68K::RunChecked(uint64_t, bool) {
    fprintf(stderr, "this build cannot check translated code (build with -DCHECK68K=ON)\n");
    exit(1);
}
uint64_t BlocksChecked() {
    return 0;
}
uint64_t BlocksUnsure() {
    return 0;
}
#else

namespace {
    struct Access {
        uint32_t address;
        uint8_t size;
        uint32_t value;
        bool operator==(const Access &) const = default;
    };
    std::vector<Access> g_nativeWrites, g_interpreterWrites;
    // The side run read a chip it had written to (its writes are only noted, so what it read is not what the chip
    // would say): the block is run but not compared
    bool g_sideUnsure = false;
    uint64_t g_blocksUnsure = 0;
    uint64_t g_blocksChecked = 0;

    uint32_t RawRead(M68K &c, uint32_t address, unsigned size) {
        switch (size) {
        case 0: return c.bus.Read8(address & 0xFFFFFF);
        case 1: return c.bus.Read16(address & 0xFFFFFE);
        default: {
            const uint32_t high = c.bus.Read16(address & 0xFFFFFE);
            return (high << 16) | c.bus.Read16((address + 2) & 0xFFFFFE);
        }
        }
    }

    // The bytes of the side run's own writes that cover a read
    uint32_t Overlay(M68K &c, uint32_t address, unsigned size, uint32_t value) {
        const uint32_t bytes = M68K::kBytes[size];
        for (const Access &w : g_nativeWrites) {
            const uint32_t wBytes = M68K::kBytes[w.size];
            for (uint32_t i = 0; i < bytes; i++) {
                const uint32_t at = (address + i) & 0xFFFFFF;
                const uint32_t offset = (at - w.address) & 0xFFFFFF;
                if (offset < wBytes) {
                    const uint32_t byte = (w.value >> ((wBytes - 1 - offset) * 8)) & 0xFF;
                    const unsigned shift = (bytes - 1 - i) * 8;
                    value = (value & ~(0xFFu << shift)) | (byte << shift);
                }
            }
        }
        return value;
    }
} // namespace

namespace native {
    uint32_t CheckRead(M68K &c, uint32_t address, unsigned size) {
        address &= size == 0 ? 0xFFFFFF : 0xFFFFFE;
        const uint32_t value = RawRead(c, address, size);
        if (c.bus.sideReadUnsure) {
            c.bus.sideReadUnsure = false;
            g_sideUnsure = true;
        }
        if (g_nativeWrites.empty()) {
            return value;
        }
        if (!c.bus.IsMemory(address)) {
            for (const Access &w : g_nativeWrites) {
                g_sideUnsure = g_sideUnsure || c.bus.SameChip(w.address, address);
            }
            return value;
        }
        return Overlay(c, address, size, value);
    }
    void CheckWrite(M68K &c, uint32_t address, unsigned size, uint32_t value) {
        address &= size == 0 ? 0xFFFFFF : 0xFFFFFE;
        g_nativeWrites.push_back({address, static_cast<uint8_t>(size), value & M68K::kMask[size]});
    }
    uint32_t WaitedCheckRead(M68K &c, uint32_t address, unsigned size) {
        c.DataWait(address, false);
        if (size == 2) {
            c.accessInto += 4;
            c.DataWait(address + 2, false);
        }
        return CheckRead(c, address, size);
    }
    void WaitedCheckWrite(M68K &c, uint32_t address, unsigned size, uint32_t value) {
        c.DataWait(address, true);
        if (size == 2) {
            c.accessInto += 4;
            c.DataWait(address + 2, true);
        }
        CheckWrite(c, address, size, value);
    }
} // namespace native

// The check's two hooks in the interpreter's memory access (m68k.cpp): during the side run the interpreter
// (doing a rare instruction for translated code) reads and writes as translated code does; during the real
// run its writes are noted as well as done
namespace check {
    bool side = false, noting = false;
    void Note(uint32_t address, unsigned size, uint32_t value) {
        if (size == 2) {
            g_interpreterWrites.push_back({address & 0xFFFFFE, 1, value >> 16});
            g_interpreterWrites.push_back({(address + 2) & 0xFFFFFE, 1, value & 0xFFFF});
        } else {
            g_interpreterWrites.push_back({address & (size == 0 ? 0xFFFFFFu : 0xFFFFFEu), static_cast<uint8_t>(size), value & M68K::kMask[size]});
        }
    }
} // namespace check

uint64_t BlocksChecked() {
    return g_blocksChecked;
}

uint64_t BlocksUnsure() {
    return g_blocksUnsure;
}

void M68K::RunChecked(uint64_t until, bool exact) {
    const Tables &tables = GetTables();
    struct Regs {
        uint32_t d[8], a[8], otherSP, pc;
        bool c, v, z, n, x, supervisor, trace, stopped;
        uint8_t mask;
        uint64_t cycles;
        bool operator==(const Regs &) const = default;
    };
    auto save = [&]() {
        Regs r;
        memcpy(r.d, d, sizeof(d));
        memcpy(r.a, a, sizeof(a));
        r.otherSP = otherSP;
        r.pc = pc;
        r.c = flagC, r.v = flagV, r.z = flagZ, r.n = flagN, r.x = flagX;
        r.supervisor = supervisor, r.trace = trace, r.stopped = stopped;
        r.mask = interruptMask;
        r.cycles = cycles;
        return r;
    };
    auto load = [&](const Regs &r) {
        memcpy(d, r.d, sizeof(d));
        memcpy(a, r.a, sizeof(a));
        otherSP = r.otherSP;
        pc = r.pc;
        flagC = r.c, flagV = r.v, flagZ = r.z, flagN = r.n, flagX = r.x;
        supervisor = r.supervisor, trace = r.trace, stopped = r.stopped;
        interruptMask = r.mask;
        cycles = r.cycles;
    };
    for (;;) {
        if (TakeInterrupt()) {
            cycles += InterruptTime();
        }
        budget = until; // (a line raised by the last block's writes has been looked at)
        if (cycles >= until) {
            return;
        }
        if (stopped) {
            cycles = until;
            return;
        }
        const bool single = (exact && cycles + exactTail >= until) || InterruptWaiting(); // (as Run)
        if (!g_hooked.empty() && RunHook(*this, pc)) {
            m_lastTail = 4;
            continue;
        }
        const NativeFn fn = single ? nullptr : tables.At(pc);
        Regs before = save(), native;
        if (fn) {
            // The translated block on the side: its writes go to a list, nothing else changes
            g_nativeWrites.clear();
            g_sideUnsure = false;
            depth = 0;
            bus.sideRun = true;
            bus.sideRuns++;
            check::side = true;
            fn(*this);
            check::side = false;
            bus.sideRun = false;
            native = save();
            load(before);
        }
        g_interpreterWrites.clear();
        check::noting = true;
        do {
            m_cycles = 0;
            ExecuteOne();
            cycles += m_cycles;
        } while (!single && !stopped && !Returned() && tables.Inside(pc));
        if (!single) {
            m_lastTail = 4;
        }
        check::noting = false;
        if (!fn) {
            continue;
        }
        if (g_sideUnsure) {
            g_blocksUnsure++;
            continue;
        }
        g_blocksChecked++;
        // (the interpreter writes a long word as two words; put the translated code's the same way)
        std::vector<Access> split;
        for (const Access &w : g_nativeWrites) {
            if (w.size == 2) {
                split.push_back({w.address, 1, w.value >> 16});
                split.push_back({(w.address + 2) & 0xFFFFFE, 1, w.value & 0xFFFF});
            } else {
                split.push_back(w);
            }
        }
        const Regs after = save();
        if (!(native == after) || split != g_interpreterWrites) {
            fprintf(stderr, "translated block at %06X differs from the interpreter (after %llu blocks checked)\n", before.pc,
                    static_cast<unsigned long long>(g_blocksChecked));
            auto show = [](const char *name, const Regs &r) {
                fprintf(stderr, "  %-12s pc %06X cycles +%llu  ccr %c%c%c%c%c mask %d%s\n", name, r.pc, static_cast<unsigned long long>(r.cycles),
                        r.x ? 'X' : '-', r.n ? 'N' : '-', r.z ? 'Z' : '-', r.v ? 'V' : '-', r.c ? 'C' : '-', r.mask, r.stopped ? " stopped" : "");
                fprintf(stderr, "    d");
                for (uint32_t v : r.d) {
                    fprintf(stderr, " %08X", v);
                }
                fprintf(stderr, "\n    a");
                for (uint32_t v : r.a) {
                    fprintf(stderr, " %08X", v);
                }
                fprintf(stderr, "\n");
            };
            Regs b = before, n = native, i = after;
            n.cycles -= before.cycles;
            i.cycles -= before.cycles;
            b.cycles = 0;
            show("before", b);
            show("interpreter", i);
            show("translated", n);
            auto list = [](const char *name, const std::vector<Access> &writes) {
                fprintf(stderr, "  %s writes:", name);
                for (const Access &w : writes) {
                    fprintf(stderr, " %06X.%c=%X", w.address, "bwl"[w.size], w.value);
                }
                fprintf(stderr, "\n");
            };
            list("interpreter", g_interpreterWrites);
            list("translated", split);
            exit(3);
        }
    }
}
#endif

} // namespace spelunk
