// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// What the CV1000's memory costs the SH-3: the board's slowdown (docs/SLOWDOWN.md). The SH7709S runs a five-stage
// pipeline out of one 16 KB cache that holds both code and data; whatever the cache does not hold the bus state
// controller fetches from the SDRAM, the program flash or the board's devices at their own speed, and the CPU waits.
// A busy frame of these games touches more than the cache holds, and takes longer than the screen's.
//
// The rules are the SH7709S hardware manual's (Renesas REJ09B0081: the cache, section 5; the bus state controller,
// section 10) and the SH-3 software manual's (the pipeline, section 10), with the numbers of cycles each case takes
// measured on HyperSquid3 ("HS3", ika-musume, BSD-2-Clause: a near cycle-accurate SH7709S in SystemVerilog) set up as
// the CV1000 sets its bus, and then fitted to a real board: buffi's CV1000 benchmark run on an Ibara PCB, where the
// board is slower than the manual and HS3 in a few places (each such rule says so where it is; docs/SLOWDOWN.md has
// both comparisons). Written afresh for this port by someone who had read MAME's sh7709s.cpp, which an earlier model
// here followed; none of its code is left.
//
// The port runs instructions at their plain count (one cycle each, plus the few that take more), and this model adds,
// at each data access an instruction makes and at each branch taken, what the hardware spends that the plain count
// does not:
//   - the code run since the last such event: its cache lines looked up (a line not held is fetched like data), the
//     fetches of instruction pairs that lose the cache's single port to data accesses and the multiplier, a load's
//     result used by the next instruction (a cycle), the multiplier's work waited for, and the bubble a conditional
//     branch taken leaves;
//   - the access itself: a hit costs nothing more, a miss fills its line (a dirty line it pushes out going to the
//     write-back buffer), an uncached access or a write-through write goes out to the bus, and the bus does one thing
//     at a time.
// The code between two events runs straight on (every branch taken is an event), so the path is found by walking on
// from the last event, and kept, for the loops that take it again, until the CPU or the DMA writes over that code
// (Sh3Paths). Everything else it keeps is plain data (it is part of save states and run-ahead), and it reads the
// instructions it walks from main RAM, so the interpreter and translated code, which call it at the same points, are
// charged alike.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#define SPELUNK_TIMING_HOT __attribute__((always_inline)) inline
#define SPELUNK_TIMING_COLD __attribute__((noinline))

namespace spelunk {

struct Sh3Paths;

struct Sh3Timing {
    // --- the bus state controller's and the cache's registers, as the game sets them (the values at power-on). The
    // SDRAM's costs below are for the MCR every CV1000 game sets; WCR1's idle cycles between areas are not modelled ---
    uint16_t bcr2 = 0x3FF0, wcr1 = 0x3FF3, wcr2 = 0xFFFF, mcr = 0;
    uint32_t ccr = 0;

    // --- the cache: 256 sets of 4 ways of 16-byte lines, code and data together (manual 5.1) ---
    static constexpr uint32_t kSets = 256, kWays = 4, kLineBytes = 16;
    struct Way {
        uint32_t line; // the physical address >> 4, the set's bits included; kNoLine when empty
        uint8_t dirty;
    };
    static constexpr uint32_t kNoLine = 0xFFFFFFFFu;
    Way ways[kSets][kWays];
    // Each set's ways from the most recently used to the least (the manual's six LRU bits say the same: with all six
    // clear, after a flush, way 3 is the one replaced first)
    uint8_t order[kSets][kWays];

    // --- the bus: when it can take the next access ---
    uint64_t sdramFree = 0;   // the SDRAM engine done with what it was given (a posted write, a line fill)
    uint64_t postedDone = 0;  // a posted write's end, as another posted write sees it
    uint64_t otherFree = 0;   // when an access to another area can go out after a posted write

    // --- the pipeline ---
    uint32_t lastInsn = 0;   // the instruction that made the last data access (0: none, or a branch since)
    uint64_t lastTime = 0;   // and the cycle it was done at
    uint32_t from = 0;       // where the code run since the last access or taken branch starts (bit 0: jumped to)
    uint32_t lastFetchLine = kNoLine;
    uint64_t nextDecode = 0; // when the pipeline decodes the instruction at `from`
    uint64_t mmEnd = 0;      // the multiplier's last cycle of work (manual 10.2.4)
    int8_t lastLoad = -1;    // the register the last access loaded (-1: none)
    int8_t nextLoad = -1;
    uint8_t inSlot = 0;      // it was a delay slot's
    uint8_t own = 1;         // its instruction's own cycles
    // The cycles of the last 64 in which the cache's port was used (by a data access or an instruction pair's fetch):
    // bit i is cycle portBase + i
    uint64_t portBase = 0, portMask = 0;

    Sh3Timing() {
        Reset();
    }
    // (the paths' memo, Sh3Paths, is kept apart from this: it is not part of the state)
    void Reset() {
        bcr2 = 0x3FF0;
        wcr1 = 0x3FF3;
        wcr2 = 0xFFFF;
        mcr = 0;
        ccr = 0;
        Flush();
        sdramFree = postedDone = otherFree = 0;
        lastInsn = 0;
        lastTime = 0;
        from = 0;
        lastFetchLine = kNoLine;
        nextDecode = 0;
        mmEnd = 0;
        lastLoad = -1;
        portBase = portMask = 0;
    }
    // CCR's CF bit: every line thrown away (dirty ones are not written back), the use order back to its start
    void Flush() {
        for (uint32_t s = 0; s < kSets; s++) {
            for (uint32_t w = 0; w < kWays; w++) {
                ways[s][w] = {kNoLine, 0};
                order[s][w] = static_cast<uint8_t>(w);
            }
        }
        lastFetchLine = kNoLine;
    }

    // --- costs, in CPU cycles (the bus runs at half the CPU's clock), measured on HS3 with the CV1000's settings
    // (CAS latency 2, RAS to CAS 2, auto-precharge) ---
    static constexpr uint32_t kFillClean = 18;      // a line read in from the SDRAM, until the CPU has its word
    static constexpr uint32_t kFillDirty = 36;      // the same with a dirty line written back first (the flash)
    static constexpr uint32_t kAllocClean = 20;     // a write that misses: the line read in, then written
    static constexpr uint32_t kWriteBack = 18;      // a dirty line written back from the write-back buffer
    static constexpr uint32_t kSdramRead = 12;      // one word read past the cache
    static constexpr uint32_t kPostedWrite = 2;     // one word written past the cache: the CPU goes on...
    static constexpr uint32_t kPostedBusy = 14;     // ...and the next write waits this long from its start,
    static constexpr uint32_t kPostedReadAfter = 12; // a read of the SDRAM this long,
    static constexpr uint32_t kPostedOtherAfter = 7; // an access to another area this long
    static constexpr uint32_t kHeldWrite = 10;      // an uncached write: until its command is out (the board)
    static constexpr uint32_t kSingleAfter = 10;    // the SDRAM busy this much longer after an uncached access (the board)
    static constexpr uint32_t kToBuffer = 4;        // a dirty line moved to the write-back buffer (manual 5.3.4)
    static constexpr uint32_t kSdramSingleBus = 5;  // a word to or from the SDRAM, in bus cycles (DMA)
    static constexpr uint32_t kOnChipBsc = 2;       // the bus state controller's and the cache's registers
    static constexpr uint32_t kOnChip = 3;          // the other on-chip registers, through the peripheral bus
    static constexpr uint32_t kUncachedFetch = 1;   // an instruction pair fetched past the cache, beyond the read
    static constexpr uint32_t kFlushCost = 256;     // CCR.CF: the cache walks its 256 sets

    // --- the address space (manual 3.1, 10.1) ---
    static uint32_t Physical(uint32_t a) {
        return a & 0x1FFFFFFFu;
    }
    static uint32_t Region(uint32_t a) { // (P0/U0 0-3, P1 4, P2 5, P3 6, P4 7)
        return a >> 29;
    }
    static bool Cached(uint32_t a) {
        return Region(a) != 5 && Region(a) != 7 && Area(a) != 1;
    }
    static uint32_t Area(uint32_t a) {
        return Physical(a) >> 26;
    }
    // Copy-back or write-through for a cached address: P1 by CCR.CB, the others by CCR.WT (manual 5.2.2)
    bool WriteThrough(uint32_t a) const {
        return Region(a) == 4 ? !(ccr & 4) : (ccr & 2) != 0;
    }
    // The bus width of an area in bytes (BCR2; area 0's is set by the board's mode pins: 16 bits on the CV1000)
    uint32_t Width(uint32_t area) const {
        if (area == 0) {
            return 2;
        }
        const uint32_t field = (bcr2 >> (area * 2)) & 3;
        return field == 1 ? 1 : field == 2 ? 2 : 4;
    }
    // The wait states of an ordinary area's access (WCR2: three bits an area, but two for area 2)
    uint32_t Waits(uint32_t area) const {
        static const uint8_t kThreeBits[8] = {0, 1, 2, 3, 4, 6, 8, 10};
        switch (area) {
        case 0: return kThreeBits[wcr2 & 7];
        case 2: return (wcr2 >> 3) & 3;
        case 4: return kThreeBits[(wcr2 >> 7) & 7];
        case 5: return kThreeBits[(wcr2 >> 10) & 7];
        case 6: return kThreeBits[(wcr2 >> 13) & 7];
        default: return 0;
        }
    }
    // An ordinary area's access of `bytes` (or a line, 16): T1, the waits, T2 for each beat of the bus's width, two
    // CPU cycles a bus cycle; and the cycles to send the access out and have its answer back
    uint32_t OrdinaryCost(uint32_t area, uint32_t bytes) const {
        const uint32_t width = Width(area);
        const uint32_t beats = bytes > width ? bytes / width : 1;
        const uint32_t dispatch = bytes == kLineBytes ? 4 : area == 0 ? 2 : 3;
        return beats * 2 * (2 + Waits(area)) + dispatch;
    }

    // --- the cache's own bookkeeping ---
    // (the way moved to the front, the ones before it each one place back: done on the four bytes at once, as the
    // model asks it at every access)
    void Touch(uint32_t set, uint32_t way) {
        if (order[set][0] == way) {
            return; // (already the most recent: most lookups, a loop's lines)
        }
        uint32_t o;
        std::memcpy(&o, order[set], 4);
        const uint32_t x = o ^ (way * 0x01010101u); // (a zero byte where `way` is: the only one)
        const uint32_t at = static_cast<uint32_t>(__builtin_ctz((x - 0x01010101u) & ~x & 0x80808080u)) & ~7u;
        const uint64_t below = (1ull << at) - 1, above = ~((1ull << (at + 8)) - 1);
        o = static_cast<uint32_t>((o & above) | ((o & below) << 8) | way);
        std::memcpy(order[set], &o, 4);
    }
    // The way that holds `line`, or -1
    int Find(uint32_t line) const {
        const Way *set = ways[line % kSets];
        const uint32_t hits = (set[0].line == line) | (set[1].line == line) << 1 | (set[2].line == line) << 2 |
                              (set[3].line == line) << 3;
        return hits ? __builtin_ctz(hits) : -1;
    }
    // Brings `line` in (the least recently used way replaced): true if that way held a dirty line
    bool Allocate(uint32_t line, bool dirty) {
        const uint32_t set = line % kSets;
        const uint32_t way = order[set][kWays - 1];
        Way &w = ways[set][way];
        const bool pushedDirty = w.line != kNoLine && w.dirty;
        w.line = line;
        w.dirty = dirty ? 1 : 0;
        Touch(set, way);
        return pushedDirty;
    }

    // --- the bus ---
    // A line filled from the SDRAM (or the flash) for an access at cycle `t`: the cycles until the CPU has its word
    SPELUNK_TIMING_COLD uint32_t Fill(uint32_t a, uint64_t t, bool write) {
        const uint32_t line = Physical(a) / kLineBytes;
        const bool pushedDirty = Allocate(line, write && !WriteThrough(a));
        uint64_t start = t > sdramFree ? t : sdramFree;
        if (Area(a) != 3) {
            start = t > otherFree ? t : otherFree;
            const uint32_t cost = OrdinaryCost(Area(a), kLineBytes) + (pushedDirty ? kFillDirty - kFillClean : 0);
            otherFree = start + cost;
            return static_cast<uint32_t>(start - t) + cost;
        }
        // (a dirty line pushed out is moved to the write-back buffer first, a longword a cycle, and the fill goes
        // before its write-back: the CPU has its word as from a clean line, those cycles later, and the line is
        // written back after the fill, the SDRAM busy meanwhile; manual 5.3.4, 5.3.5)
        const uint32_t cost = (write ? kAllocClean : kFillClean) + (pushedDirty ? kToBuffer : 0);
        sdramFree = start + cost + (pushedDirty ? kWriteBack : 0);
        return static_cast<uint32_t>(start - t) + cost;
    }
    // A write that goes out to the SDRAM past the cache (uncached, or write-through), posted
    uint32_t PostedWrite(uint64_t t) {
        const uint64_t start = t > postedDone ? t : postedDone;
        postedDone = start + kPostedBusy;
        sdramFree = start + kPostedReadAfter;
        otherFree = start + kPostedOtherAfter;
        return static_cast<uint32_t>(start - t) + kPostedWrite;
    }
    // An uncached write to the SDRAM (P2, or P0 with the cache off): not posted. The CPU waits for the SDRAM to be
    // free and for the write's command to go out; the SDRAM is then busy with it (the write, its recovery and the
    // precharge) for kSingleBusy. Only a write-through write is posted. (Measured on the board with buffi's
    // benchmark, docs/SLOWDOWN.md: the lists the CPU writes through P2.)
    uint32_t HeldWrite(uint64_t t) {
        uint64_t start = t > postedDone ? t : postedDone;
        start = start > sdramFree ? start : sdramFree;
        postedDone = start + kPostedBusy + kSingleAfter;
        sdramFree = start + kPostedBusy + kSingleAfter;
        otherFree = start + kPostedOtherAfter;
        return static_cast<uint32_t>(start - t) + kHeldWrite;
    }
    // A read or write past the cache, not posted
    uint32_t Single(uint32_t a, uint32_t bytes, bool write, uint64_t t) {
        const uint32_t area = Area(a);
        if (Region(a) == 7 || area == 1) {
            return Region(a) == 7 && a >= 0xFFFFFF50u ? kOnChipBsc : kOnChip;
        }
        if (area == 3) {
            if (write) {
                return HeldWrite(t);
            }
            const uint64_t start = t > sdramFree ? t : sdramFree;
            sdramFree = start + kSdramRead + kSingleAfter;
            return static_cast<uint32_t>(start - t) + kSdramRead;
        }
        const uint64_t start = t > otherFree ? t : otherFree;
        const uint32_t cost = OrdinaryCost(area, bytes);
        otherFree = start + cost;
        return static_cast<uint32_t>(start - t) + cost;
    }

    // A DMA transfer of `bytes` (the DMA controller's, past the cache): the bus cycles of its read and its write, the
    // CPU held off the bus meanwhile
    uint32_t DmaTransfer(uint32_t from, uint32_t to, uint32_t bytes) const {
        return 2 * (DmaBusCycles(from, bytes) + DmaBusCycles(to, bytes));
    }
    uint32_t DmaBusCycles(uint32_t a, uint32_t bytes) const {
        const uint32_t area = Area(a);
        if (area == 3) {
            return kSdramSingleBus;
        }
        const uint32_t width = Width(area);
        return (bytes > width ? bytes / width : 1) * (2 + Waits(area));
    }

    // --- the pipeline: the cache's one port, shared by data accesses and the instruction fetches ---
    bool PortBusy(uint64_t cycle) const {
        const uint64_t d = cycle - portBase; // (before portBase: a large number, not busy)
        return d < 64 && (portMask >> d & 1);
    }
    // The first cycle from `cycle` on in which the port is free
    uint64_t PortFree(uint64_t cycle) const {
        const uint64_t d = cycle - portBase;
        if (d >= 64) {
            return cycle;
        }
        // (the cycles past the 64 kept are free)
        const uint64_t free = ~portMask >> d | (d ? ~0ull << (64 - d) : 0);
        return free ? cycle + static_cast<uint64_t>(__builtin_ctzll(free)) : portBase + 64;
    }
    void PortTake(uint64_t cycle) {
        if (cycle < portBase) {
            return; // (older than anything still asked about)
        }
        const uint64_t d = cycle - portBase;
        const uint64_t shift = d >= 64 ? d - 63 : 0;
        portMask = (shift >= 64 ? 0 : portMask >> shift) | 1ull << (d - shift);
        portBase += shift;
    }

    // One instruction of the code between two events (a data access, a taken branch)
    struct Step {
        uint32_t address;
        uint8_t fetch;   // (its pair is fetched here: the first of a longword, or where a branch went)
        uint8_t advance; // (the cycles to the next one's decode: its own, and a taken branch's bubble)
        uint8_t flags;   // (what it asks of the multiplier: kMa1 ...)
    };
    // The multiplier (SH-3 software manual 10.2.4, 10.4.2): its instructions' MA stages take the cache's port like a
    // data access does (they contend with the fetches), and the multiplier then works on for two cycles (MULS.W,
    // MULU.W) or three (MUL.L, DMULS.L, DMULU.L, MAC.W, MAC.L) whatever the pipeline does. An instruction that reads
    // MACH or MACL (STS, STS.L) has its MA held until a cycle after that work ends; one that writes them or starts the
    // multiplier again (LDS, LDS.L, CLRMAC, another multiply) until it ends.
    enum : uint8_t {
        kMa1 = 1,       // one MA stage of its own (not a data access)
        kMa2 = 2,       // two (MUL.L, DMULS.L, DMULU.L)
        kMul2 = 4,      // starts the multiplier: two cycles
        kMul3 = 8,      // three
        kMacRead = 16,  // reads MACH/MACL
        kMacWait = 32,  // waits for the multiplier to finish
        kUses = 64,     // reads the register the instruction before it took from MACH/MACL (STS: a load's wait)
    };
    static uint8_t MultiplierFlags(uint16_t op) {
        switch (op & 0xF00F) {
        case 0x200E: case 0x200F: return kMa1 | kMul2 | kMacWait;          // mulu.w, muls.w
        case 0x0007: case 0x3005: case 0x300D: return kMa2 | kMul3 | kMacWait; // mul.l, dmulu.l, dmuls.l
        case 0x000F: case 0x400F: return kMul3 | kMacWait;                 // mac.l, mac.w (their MAs are reads)
        default: break;
        }
        switch (op & 0xF0FF) {
        case 0x000A: case 0x001A: return kMa1 | kMacRead;                  // sts mach/macl
        case 0x4002: case 0x4012: return kMacRead;                         // sts.l mach/macl (a write)
        case 0x400A: case 0x401A: return kMa1 | kMacWait;                  // lds mach/macl
        case 0x4006: case 0x4016: return kMacWait;                         // lds.l mach/macl (a read)
        default: break;
        }
        return op == 0x0028 ? kMa1 | kMacWait : 0;                         // clrmac
    }
    static constexpr uint32_t kMmShort = 3, kMmLong = 4; // (the multiplier's cycles: MULS.W, MULU.W; the others)
    static constexpr uint32_t kPathMax = 2 * 64 + 8;
    struct PathInfo {
        uint16_t count;
        uint8_t known;
        int8_t lastLoad, load;  // (the register the last access loaded; the one this access loads)
        uint8_t useStall;       // (the path's first instruction uses that register: the cycles it waits)
        uint8_t slot;           // (this access is in a delay slot: 1, a conditional branch's: 2, the CPU says)
        uint8_t own;            // (this access's instruction's own cycles)
    };
    // The plain cycles of each instruction (SH3::s_cycles: the count the CPU keeps)
    static inline const uint8_t *opCycles = nullptr;
    static uint8_t Cycles(uint16_t op) {
        return opCycles ? opCycles[op] : 1;
    }
    // The path from `from` to `insn` (an access's instruction, or a taken branch's: `branch`). Every taken branch is
    // an event of its own (Branch), so the code between two events runs straight on: through conditional branches
    // not taken, to `insn`. Not reached so (the first event, or code the board ran for the CPU): the straight run
    // before `insn`, back to the last branch, up to a few instructions.
    template <typename Code>
    void FindPath(uint32_t start, uint32_t insn, bool branch, Code code, Step *path, uint32_t &count, bool &known) {
        count = 0;
        known = false;
        bool jumped = (start & 1) != 0;
        if (start != 0) {
            uint32_t p = start & ~1u;
            for (uint32_t n = 0; n < 2 * kWalk; n++) {
                if (p == insn) {
                    known = true;
                    break;
                }
                const uint16_t op = code(p);
                const Branch kind = BranchKind(op);
                if (kind == Branch::Delayed && p + 2 == insn) {
                    path[count++] = {p, FetchAt(p, jumped), 1, 0}; // (the access is its slot)
                    jumped = false;
                    known = true;
                    break;
                }
                if (kind == Branch::Delayed || op == 0) {
                    break; // (a branch taken that was not seen)
                }
                path[count++] = {p, FetchAt(p, jumped), Cycles(op), MultiplierFlags(op)};
                jumped = false;
                p += 2;
            }
            if (known) {
                path[count++] = {insn, FetchAt(insn, jumped), 1,
                                 MultiplierFlags(code(insn))};
            }
        }
        if (!known) {
            FallBack(insn, code, path, count);
        }
        // (STS MACH/MACL, Rn is a load: the next instruction reading Rn waits a cycle, manual 10.4.4 (14))
        for (uint32_t i = 0; i + 1 < count; i++) {
            const uint16_t op = code(path[i].address);
            if ((op & 0xF0EF) == 0x000A && path[i + 1].address == path[i].address + 2 &&
                Reads(code(path[i + 1].address), (op >> 8) & 15)) {
                path[i + 1].flags |= kUses;
            }
        }
        (void)branch;
    }
    template <typename Code>
    void FallBack(uint32_t insn, Code code, Step *path, uint32_t &count) {
        {
            uint32_t first = insn;
            for (uint32_t n = 0; n < kBackWalk && first >= 2; n++) {
                const uint16_t op = code(first - 2);
                if (op == 0 || BranchKind(op) != Branch::None || IsDelayed(code(first - 4))) {
                    break;
                }
                first -= 2;
            }
            count = 0;
            for (uint32_t a = first; a <= insn; a += 2) {
                const uint16_t op = code(a);
                path[count++] = {a, static_cast<uint8_t>(a % 4 == 0 || a == first), Cycles(op), MultiplierFlags(op)};
            }
        }
    }
    // Whether the instruction at `p` has its pair fetched: the first of a longword (1), or a taken branch's target
    // (2), which is fetched once the branch is decided, two cycles before its decode (the others three: the bubble
    // after a taken branch is that cycle; HS3's early target fetch, docs/HS3_Core_Hardware.md)
    static uint8_t FetchAt(uint32_t p, bool jumped) {
        return jumped ? 2 : p % 4 == 0 ? 1 : 0;
    }
    // A taken branch's cycles, from its decode to its target's (the plain count's and the pipeline's bubble), put on
    // the path's last step: the branch's, or its slot's (whose own cycles `slotCycles` are counted in)
    static uint8_t TakenAdvance(uint16_t op, uint32_t slotCycles) {
        switch (BranchKind(op)) {
        case Branch::If: return 3;                                         // bt, bf: 3 cycles taken
        case Branch::DelayedIf: return static_cast<uint8_t>(slotCycles + 1); // bt/s, bf/s: 2, and the slot's
        case Branch::Delayed: return static_cast<uint8_t>(slotCycles + Cycles(op) - 1);
        default: return Cycles(op); // (an exception, a sleep: what the plain count says)
        }
    }

    // The code run since the last event, up to `insn`, and (for an access) this access's turn at the cache's port:
    // what they cost beyond the instructions' own cycles, at cycle `now`. `code` reads an instruction from main RAM (0
    // where there is none).
    //
    // The path's lines are looked up (or, past the cache, its instruction pairs read). Then the pipeline is stepped
    // along it from where the last event left it: an instruction pair (a longword: the second instruction comes with
    // the first) is fetched three cycles before its first is decoded, through the one port the data accesses and the
    // multiplier's MA stages also use, which they have first; a load's result used by the next instruction holds that
    // one a cycle; the multiplier holds what reads it; a taken branch leaves its bubbles. Where the pipeline is later
    // than the plain count, the difference is the cost
    template <typename Code>
    SPELUNK_TIMING_HOT uint32_t Stream(Sh3Paths &paths, uint32_t insn, bool branch, bool cpuSlot, uint64_t now, Code code);
    template <typename Code>
    SPELUNK_TIMING_COLD uint32_t StreamNew(Sh3Paths &paths, uint32_t insn, bool branch, bool cpuSlot, uint64_t now, Code code);
    // The path made ready to run, once, when it is found (and kept with it): the lines its code is fetched from (or,
    // past the cache, its instruction pairs), in order, and the pipeline's steps that depend on the time (a fetch at
    // the cache's port, the multiplier), with the cycles between them added up. Running it does what stepping the path
    // does, step by step, with the same result
    struct Plan {
        // (each line looked up, or past the cache each pair read: its address, bit 0 set for a pair past the cache)
        uint32_t fetch[kPathMax];
        // (a fetch at the port, `lead` cycles before its decode; or the multiplier's flags: after `add` cycles more)
        struct Op {
            uint16_t add;
            uint8_t kind; // (0: a fetch, `arg` its lead; 1: the multiplier, `arg` its flags)
            uint8_t arg;
        } op[2 * kPathMax];
        uint32_t fetches = 0, ops = 0;
        uint32_t tail = 0; // (the cycles after the last of them, to the decode of the path's last step or its target's)
        uint32_t span = 0; // (the cycles from the first step's decode to the last's, for a path not known)
    };
    static void MakePlan(const Step *path, const PathInfo &info, bool target, Plan &plan) {
        const uint32_t count = info.count;
        plan.fetches = plan.ops = plan.span = 0;
        uint32_t lineBefore = kNoLine;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t a = path[i].address;
            if (Cached(a)) {
                if (Physical(a) / kLineBytes != lineBefore) {
                    lineBefore = Physical(a) / kLineBytes;
                    plan.fetch[plan.fetches++] = a & ~1u;
                }
            } else if (path[i].fetch) {
                plan.fetch[plan.fetches++] = (a & ~3u) | 1u;
            }
        }
        if (!info.known) {
            for (uint32_t i = 0; i + 1 < count; i++) {
                plan.span += path[i].advance;
            }
        }
        uint32_t pending = 0;
        for (uint32_t i = 0; i < count; i++) {
            const Step &s = path[i];
            if (s.fetch && Cached(s.address)) {
                plan.op[plan.ops++] = {static_cast<uint16_t>(pending), 0, static_cast<uint8_t>(s.fetch == 2 ? 2 : 3)};
                pending = 0;
            }
            if (i == 0) {
                pending += info.useStall;
            }
            if (s.flags & kUses) {
                pending++;
            }
            if (s.flags & ~kUses) { // (kUses alone asks nothing of the multiplier)
                plan.op[plan.ops++] = {static_cast<uint16_t>(pending), 1, s.flags};
                pending = 0;
            }
            if (i + 1 < count || target) {
                pending += s.advance;
            }
        }
        plan.tail = pending;
    }
    // The fetches and the pipeline over a path's plan: the decode of its last step (or, for a branch, of its target)
    SPELUNK_TIMING_HOT uint32_t Run(const uint32_t *fetch, uint32_t fetches, const Plan::Op *op, uint32_t ops,
                                    uint32_t tail, uint32_t span, const PathInfo &info, bool branch, bool cpuSlot,
                                    uint64_t now);
    SPELUNK_TIMING_HOT uint64_t Walk(const uint32_t *fetch, uint32_t fetches, const Plan::Op *op, uint32_t ops,
                                     uint32_t tail, uint32_t span, bool known, uint64_t now, uint32_t &extra) {
        // (the code's lines, or past the cache its pairs)
        for (uint32_t i = 0; i < fetches; i++) {
            const uint32_t a = fetch[i];
            if (!(a & 1)) {
                extra += FetchLine(a, now + extra);
            } else {
                extra += Single(a & ~1u, 4, false, now + extra) + kUncachedFetch;
            }
        }
        // (the pipeline, stepped: where it is not known, the path is taken to end at the plain count)
        uint64_t decode = nextDecode;
        if (!known) {
            decode = now > nextDecode + span ? now - span : nextDecode;
        }
        for (uint32_t i = 0; i < ops; i++) {
            decode += op[i].add;
            if (op[i].kind == 0) {
                if (decode >= 3) {
                    const uint32_t lead = op[i].arg;
                    const uint64_t slot = PortFree(decode - lead);
                    PortTake(slot);
                    if (slot + lead > decode) {
                        decode = slot + lead;
                    }
                }
            } else [[unlikely]] {
                decode = Multiplier(op[i].arg, decode);
            }
        }
        return decode + tail;
    }
    SPELUNK_TIMING_COLD uint64_t Multiplier(uint8_t flags, uint64_t decode) {
        if (flags & kMacRead) {
            decode = decode > mmEnd + 1 ? decode : mmEnd + 1;
        } else if (flags & kMacWait) {
            decode = decode > mmEnd ? decode : mmEnd;
        }
        for (uint32_t k = 0; k < (flags & 3u); k++) {
            if (PortBusy(decode + k)) {
                decode++;
            }
            PortTake(decode + k);
        }
        if (flags & (kMul2 | kMul3)) {
            // (the multiplier's work: a cycle longer than the manual's two or three, and the multiply's MA stage holds
            // the cache's port until it is done, so no instruction is fetched meanwhile; fitted on the board's CPU
            // INTEGER and RAMP CPU LOGIC, docs/SLOWDOWN.md)
            const uint32_t ma = (flags & 3u) ? (flags & 3u) : 2; // (MAC.W, MAC.L: their two reads)
            mmEnd = decode + ma - 1 + ((flags & kMul3) ? kMmLong : kMmShort);
            for (uint64_t c = decode + ma; c <= mmEnd; c++) {
                PortTake(c);
            }
        }
        return decode;
    }
    static constexpr uint32_t kWalk = 64;
    static constexpr uint32_t kBackWalk = 4; // (where the path is not known: the instructions before the event)
    enum class Branch { None, If, DelayedIf, Delayed };
    static Branch BranchKind(uint16_t op) {
        switch (op & 0xFF00) {
        case 0x8900: case 0x8B00: return Branch::If;        // bt, bf
        case 0x8D00: case 0x8F00: return Branch::DelayedIf; // bt/s, bf/s
        default: break;
        }
        if ((op & 0xE000) == 0xA000 || op == 0x000B || op == 0x002B) {
            return Branch::Delayed; // bra, bsr, rts, rte
        }
        switch (op & 0xF0FF) {
        case 0x402B: case 0x400B: case 0x0023: case 0x0003: return Branch::Delayed; // jmp, jsr, braf, bsrf
        default: return Branch::None;
        }
    }
    static bool IsDelayed(uint16_t op) {
        const Branch kind = BranchKind(op);
        return kind == Branch::Delayed || kind == Branch::DelayedIf;
    }
    // An instruction line's lookup (a line not held is filled like data)
    SPELUNK_TIMING_HOT uint32_t FetchLine(uint32_t a, uint64_t t) { // (a cached address: a plan's)
        const uint32_t line = Physical(a) / kLineBytes;
        if (line == lastFetchLine) {
            return 0;
        }
        lastFetchLine = line;
        if (const int w = Find(line); w >= 0) {
            Touch(line % kSets, static_cast<uint32_t>(w));
            return 0;
        }
        return Fill(a, t, false);
    }

    // PR, as a register the last event wrote late: loaded from memory (LDS.L @Rm+,PR), or written by a call (JSR,
    // BSR, BSRF) as it goes. An instruction that reads PR next (RTS, STS PR) waits for it. The SH-3 software manual
    // says so of LDS.L to PR (table 10.2, figure 10.52: contention if the next instruction uses PR) without the cycles;
    // the cycles, and the call's case, are HS3's (int_pipe.sv holds a PR reader in ID until the write has left WB: 3
    // cycles after LDS.L, 1 at a call's target). Fitted nowhere: the board's CPU DIVIDE (GCC's __udivsi3, which saves
    // PR first thing and returns with LDS.L then RTS) comes nearer with them, and no other test moves past 1%.
    static constexpr int kPrLoaded = 16, kPrLinked = 17;
    static uint8_t UseWait(int loaded) {
        return loaded == kPrLoaded ? 3 : 1;
    }
    // A load whose result the next instruction reads waits a cycle for it (SH-3 software manual, 10.2): the register
    // a load writes (-1: not a load into a general register)
    static int LoadTarget(uint16_t op) {
        const int n = (op >> 8) & 15, low = op & 15;
        if ((op & 0xF0FF) == 0x4026) {
            return kPrLoaded; // lds.l @rm+,pr
        }
        switch (op >> 12) {
        case 0x0: return low >= 0xC && low <= 0xE ? n : -1;           // mov.x @(r0,rm),rn
        case 0x5: case 0x9: case 0xD: return n;                       // mov.l @(disp,rm),rn; pc-relative
        case 0x6: return low <= 2 || (low >= 4 && low <= 6) ? n : -1; // mov.x @rm,rn; @rm+,rn
        case 0x8: return n == 4 || n == 5 ? 0 : -1;                   // mov.x @(disp,rm),r0
        case 0xC: return n >= 4 && n <= 6 ? 0 : -1;                   // mov.x @(disp,gbr),r0
        default: return -1;
        }
    }
    // Whether an instruction reads general register `r`
    static bool Reads(uint16_t op, int r) {
        if (r >= kPrLoaded) { // (PR: rts, sts pr,rn, sts.l pr,@-rn)
            return op == 0x000B || (op & 0xF0FF) == 0x002A || (op & 0xF0FF) == 0x4022;
        }
        const int n = (op >> 8) & 15, m = (op >> 4) & 15, low = op & 15;
        switch (op >> 12) {
        case 0x0:
            if (low >= 4 && low <= 6) {
                return r == n || r == m || r == 0; // mov.x rm,@(r0,rn)
            }
            if (low == 7 || low == 0xF) {
                return r == n || r == m; // mul.l, mac.l
            }
            if (low >= 0xC && low <= 0xE) {
                return r == m || r == 0; // mov.x @(r0,rm),rn
            }
            return ((op & 0xFF) == 0x03 || (op & 0xFF) == 0x23 || (op & 0xFF) == 0x83) && r == n; // bsrf, braf, pref
        case 0x1: case 0x2: case 0x3: return r == n || r == m;
        case 0x4:
            if (low == 0xC || low == 0xD || low == 0xF) {
                return r == n || r == m; // shad, shld, mac.w
            }
            return r == n;
        case 0x5: case 0x6: return r == m;
        case 0x7: return r == n;
        case 0x8:
            if (n <= 1) {
                return r == 0 || r == m; // mov.x r0,@(disp,rm)
            }
            return n == 4 || n == 5 ? r == m : n == 8 && r == 0; // mov.x @(disp,rm),r0; cmp/eq #imm,r0
        case 0xC: return (n <= 2 || n >= 8) && r == 0;
        default: return false;
        }
    }

    // --- what the CPU calls ---
    // A data access of `bytes` at `address` by the instruction at `insn`, at cycle `now`: the cycles it costs beyond
    // the instruction's own. `code(address)` reads an instruction from main RAM; `cpuSlot`: the instruction is run as
    // a delay slot (the CPU knows whether a conditional delayed branch before it was taken)
    template <typename Code>
    SPELUNK_TIMING_HOT uint32_t Access(Sh3Paths &paths, uint32_t insn, uint32_t address, uint32_t bytes, bool write, bool cpuSlot, uint64_t now, Code code);
    // A branch taken (or any instruction that set the PC: an exception, a sleep) by the instruction at `branch`, its
    // delay slot run and the plain count at `now`: the cycles the code since the last event and the branch cost
    // beyond the plain count. The next event's path starts at `target`
    template <typename Code>
    SPELUNK_TIMING_HOT uint32_t Taken(Sh3Paths &paths, uint32_t branch, uint32_t target, uint64_t now, Code code);
    // An interrupt taken: the pipeline starts again at `target`
    void Redirect(uint32_t target, uint64_t now) {
        from = target | 1;
        lastInsn = 0;
        lastTime = now;
        nextDecode = now;
        lastLoad = -1;
    }
    // A write to the cache's address array (F0000000 on, manual 5.3.3; the games' flush loop): with the A bit, the
    // entry holding that address has its V and U bits written (a dirty line whose U is cleared is written back);
    // without it, the entry named by way and set is written whole (a dirty line it held written back first). The
    // cycles it costs
    uint32_t ArrayWrite(uint32_t offset, uint32_t data, uint64_t now) {
        const uint32_t set = (offset >> 4) & (kSets - 1);
        const bool associative = (offset & 8) != 0;
        const uint32_t named = (((data >> 10) & 0x7FFFF) << 6) | (set & 0x3F);
        const int way = associative ? Find(named) : static_cast<int>((offset >> 12) & 3);
        if (way < 0) {
            return 0;
        }
        Way &w = ways[set][way];
        uint32_t cost = 0;
        if (w.line != kNoLine && w.dirty && (!associative || !(data & 2))) {
            // (through the write-back buffer: the CPU waits for it to be free and moves the line in, then goes on
            // while the line is written back)
            const uint64_t start = now > sdramFree ? now : sdramFree;
            cost = static_cast<uint32_t>(start - now) + kToBuffer;
            sdramFree = start + kToBuffer + kWriteBack;
            w.dirty = 0;
        }
        if (!associative) {
            w.line = named;
        }
        if (!(data & 1)) {
            w.line = kNoLine;
        }
        w.dirty = w.line != kNoLine && (data & 2) ? 1 : 0;
        return cost;
    }
};

// The paths Sh3Timing::Stream has found, kept: a loop takes the same path every turn, and finding it is most of the
// model's time. A path depends on the code under it alone, so one found before is the same while that code is not
// written; which paths are kept changes no count. So this is not part of the state (a state loaded forgets them),
// and each block of main RAM a path read keeps the time of its last write, which a kept path checks.
struct Sh3Paths {
    static constexpr uint32_t kMemoBits = 13, kMemos = 1u << kMemoBits, kMemoSteps = 16, kMemoBlocks = 4;
    static constexpr uint32_t kBlockShift = 7, kRamBytes = 0x01000000u;
    // (the SH3's main RAM, as the walk reads it: elsewhere is read as 0, which no write changes)
    static bool IsRam(uint32_t a) {
        return (a & 0x1F000000u) == 0x0C000000u && a < 0xE0000000u;
    }
    static uint32_t Block(uint32_t a) {
        return (a & (kRamBytes - 1)) >> kBlockShift;
    }
    struct Memo {
        uint32_t from, to, stamp;
        uint32_t generation; // (kept while it is the table's: Forget starts a new one)
        uint8_t blocks;
        uint8_t fetches, ops;
        uint16_t tail, span;
        Sh3Timing::PathInfo info;
        uint32_t block[kMemoBlocks];
        uint32_t fetch[kMemoSteps];
        Sh3Timing::Plan::Op op[2 * kMemoSteps];
    };
    // The blocks of RAM a path being found reads (more than kMemoBlocks: it is not kept)
    struct Watching {
        uint32_t block[kMemoBlocks];
        uint32_t blocks = 0;
        bool tooMany = false;
        void Read(uint32_t a) {
            if (!IsRam(a)) {
                return;
            }
            const uint32_t b = Block(a);
            for (uint32_t i = 0; i < blocks; i++) {
                if (block[i] == b) {
                    return;
                }
            }
            if (blocks == kMemoBlocks) {
                tooMany = true;
                return;
            }
            block[blocks++] = b;
        }
    };
    std::vector<Memo> memos = std::vector<Memo>(kMemos);
    std::vector<uint8_t> recent = std::vector<uint8_t>(kMemos / 2); // (each set's place used last)
    uint32_t last = 0; // (the memo found last)
    std::vector<uint32_t> written = std::vector<uint32_t>(kRamBytes >> kBlockShift); // (each block's last write)
    uint32_t clock = 1;
    uint32_t generation = 1; // (the kept paths': a memo of another is not kept)

    Sh3Paths() {
        for (Memo &m : memos) {
            m.generation = 0;
        }
    }
    void Forget() {
        if (++generation == 0) {
            for (Memo &m : memos) {
                m.generation = 0;
            }
            generation = 1;
        }
    }
    // Main RAM put back as it was when `keptClock` and `keptGeneration` were read (run-ahead's frames, online play's
    // rollback): a block written since then holds other bytes now, so a path over one is found again; the others are
    // the same bytes, and their paths are kept. (A Forget meanwhile, as at a clock's wrap, leaves the times written
    // no longer comparable: then every path is found again)
    void PutBack(uint32_t keptClock, uint32_t keptGeneration) {
        if (generation != keptGeneration || clock == 0xFFFFFFFFu) {
            Forget();
            return;
        }
        const uint32_t now = ++clock;
        for (uint32_t &w : written) {
            if (w > keptClock) {
                w = now;
            }
        }
    }
    // A write the CPU makes: a path over it is found again
    void Written(uint32_t a, uint32_t bytes) {
        if (!IsRam(a)) {
            return;
        }
        if (++clock == 0) {
            Forget();
            std::fill(written.begin(), written.end(), 0);
            clock = 1;
        }
        written[Block(a)] = clock;
        written[Block(a + bytes - 1)] = clock;
    }
    // (two places for each path, in a set of the table: the one used less recently is replaced)
    static uint32_t Set(uint32_t from, uint32_t to, int8_t lastLoad) {
        const uint32_t h = from * 0x9E3779B1u + to * 0x85EBCA77u + static_cast<uint32_t>(lastLoad + 1) * 0xC2B2AE3Du;
        return (h ^ h >> 15) * 0x2C1B3C6Du >> (32 - kMemoBits + 1);
    }
    bool Holds(const Memo &m, uint32_t from, uint32_t to, int8_t lastLoad) const {
        return m.generation == generation && m.from == from && m.to == to && m.info.lastLoad == lastLoad;
    }
    const Memo *Find(uint32_t from, uint32_t to, int8_t lastLoad) { // (from's bit 0: jumped to; to's: a branch)
        const Memo *m = &memos[last];
        if (!Holds(*m, from, to, lastLoad)) { // (the one found last: a loop's, as often as not)
            const uint32_t set = Set(from, to, lastLoad);
            uint32_t way = 0;
            if (!Holds(memos[2 * set], from, to, lastLoad)) {
                if (!Holds(memos[2 * set + 1], from, to, lastLoad)) {
                    return nullptr;
                }
                way = 1;
            }
            recent[set] = static_cast<uint8_t>(way);
            last = 2 * set + way;
            m = &memos[last];
        }
        for (uint32_t i = 0; i < m->blocks; i++) {
            if (written[m->block[i]] > m->stamp) {
                return nullptr;
            }
        }
        return m;
    }
    void Keep(uint32_t from, uint32_t to, const Sh3Timing::PathInfo &info, const Sh3Timing::Plan &plan, const Watching &watching) {
        if (info.count > kMemoSteps || watching.tooMany || plan.tail > 0xFFFF || plan.span > 0xFFFF) {
            return;
        }
        const uint32_t set = Set(from, to, info.lastLoad);
        const uint32_t way = Holds(memos[2 * set], from, to, info.lastLoad)       ? 0
                             : Holds(memos[2 * set + 1], from, to, info.lastLoad) ? 1
                                                                                  : 1 - recent[set];
        recent[set] = static_cast<uint8_t>(way);
        Memo &m = memos[2 * set + way];
        m.generation = generation;
        m.from = from;
        m.to = to;
        m.stamp = clock;
        m.info = info;
        m.blocks = static_cast<uint8_t>(watching.blocks);
        std::memcpy(m.block, watching.block, sizeof m.block);
        m.fetches = static_cast<uint8_t>(plan.fetches);
        m.ops = static_cast<uint8_t>(plan.ops);
        m.tail = static_cast<uint16_t>(plan.tail);
        m.span = static_cast<uint16_t>(plan.span);
        std::memcpy(m.fetch, plan.fetch, plan.fetches * sizeof plan.fetch[0]);
        std::memcpy(m.op, plan.op, plan.ops * sizeof plan.op[0]);
    }
};

template <typename Code>
inline uint32_t Sh3Timing::Stream(Sh3Paths &paths, uint32_t insn, bool branch, bool cpuSlot, uint64_t now, Code code) {
    // (the path depends on the code alone: a path found before, its code not written since, is the same)
    if (const Sh3Paths::Memo *memo = paths.Find(from, insn | (branch ? 1u : 0u), lastLoad)) {
        return Run(memo->fetch, memo->fetches, memo->op, memo->ops, memo->tail, memo->span, memo->info, branch, cpuSlot, now);
    }
    return StreamNew(paths, insn, branch, cpuSlot, now, code);
}
// (a path not kept: found)
template <typename Code>
uint32_t Sh3Timing::StreamNew(Sh3Paths &paths, uint32_t insn, bool branch, bool cpuSlot, uint64_t now, Code code) {
    Step path[kPathMax];
    PathInfo info{};
    uint32_t count = 0;
    bool known = false;
    Sh3Paths::Watching watching;
    auto watched = [&](uint32_t a) -> uint16_t {
        watching.Read(a);
        return code(a);
    };
    FindPath(from, insn, branch, watched, path, count, known);
    if (branch) {
        // (the branch's own cycles and its bubble, after its slot where it has one)
        const uint16_t op = watched(insn);
        if (IsDelayed(op)) {
            const uint16_t slotOp = watched(insn + 2);
            path[count - 1].advance = 1;
            path[count++] = {insn + 2, static_cast<uint8_t>((insn + 2) % 4 == 0), TakenAdvance(op, Cycles(slotOp)),
                             MultiplierFlags(slotOp)};
        } else {
            path[count - 1].advance = TakenAdvance(op, 0);
        }
    }
    info.count = static_cast<uint16_t>(count);
    info.known = known;
    info.lastLoad = lastLoad;
    info.useStall = lastLoad >= 0 && Reads(watched(path[0].address), lastLoad) ? UseWait(lastLoad) : 0;
    if (!branch) {
        const Branch before = path[count - 1].address == insn && count >= 2 && path[count - 2].address == insn - 2
                                  ? BranchKind(watched(insn - 2))
                                  : known ? Branch::None : BranchKind(watched(insn - 2));
        info.slot = before == Branch::Delayed ? 1 : before == Branch::DelayedIf ? 2 : 0;
        info.load = static_cast<int8_t>(LoadTarget(watched(insn)));
        info.own = Cycles(watched(insn));
    }
    Plan plan;
    MakePlan(path, info, branch, plan);
    paths.Keep(from, insn | (branch ? 1u : 0u), info, plan, watching);
    return Run(plan.fetch, plan.fetches, plan.op, plan.ops, plan.tail, plan.span, info, branch, cpuSlot, now);
}

// (what Sh3Timing::Access, Taken, Stream and Walk do is said where they are declared)
SPELUNK_TIMING_HOT uint32_t Sh3Timing::Run(const uint32_t *fetch, uint32_t fetches, const Plan::Op *op, uint32_t ops,
                                                   uint32_t tail, uint32_t span, const PathInfo &info, bool branch,
                                                   bool cpuSlot, uint64_t now) {
    uint32_t extra = 0;
    uint64_t decode = Walk(fetch, fetches, op, ops, tail, span, info.known, now, extra);
    if (branch) {
        if (decode > now + extra) {
            extra = static_cast<uint32_t>(decode - now);
        }
        return extra;
    }
    if (PortBusy(decode)) {
        decode++; // (this access's turn at the port)
    }
    PortTake(decode);
    // (an access in a delay slot is made, in the plain count, before its branch's own cycle)
    const uint32_t slot = info.slot == 1 || (info.slot == 2 && cpuSlot) ? 1 : 0;
    if (decode > now + slot + extra) {
        extra = static_cast<uint32_t>(decode - now - slot);
    }
    inSlot = static_cast<uint8_t>(slot);
    nextLoad = info.load;
    own = info.own;
    return extra;
}

template <typename Code>
inline uint32_t Sh3Timing::Access(Sh3Paths &paths, uint32_t insn, uint32_t address, uint32_t bytes, bool write, bool cpuSlot, uint64_t now, Code code) {
    uint32_t extra = 0;
    const bool streamed = insn != lastInsn || now != lastTime; // (not the same instruction's second access)
    if (streamed) {
        extra += Stream(paths, insn, false, cpuSlot, now, code);
    }
    const uint64_t t = now + extra;
    if (!Cached(address)) {
        extra += Single(address, bytes, write, t);
    } else {
        const uint32_t line = Physical(address) / kLineBytes;
        const int w = Find(line);
        if (w >= 0) {
            Touch(line % kSets, static_cast<uint32_t>(w));
            if (write) {
                if (WriteThrough(address)) {
                    extra += PostedWrite(now + extra);
                } else {
                    ways[line % kSets][w].dirty = 1;
                }
            }
        } else if (write && WriteThrough(address)) {
            extra += Area(address) == 3 ? PostedWrite(t) : Single(address, bytes, true, t); // (no line kept)
        } else {
            extra += Fill(address, t, write);
        }
    }
    if (write) {
        paths.Written(address, bytes);
    }
    lastInsn = insn;
    lastTime = now + extra;
    from = insn + 2;
    lastLoad = write ? -1 : streamed ? nextLoad : static_cast<int8_t>(LoadTarget(code(insn)));
    nextDecode = now + inSlot + extra + own;
    return extra;
}

template <typename Code>
inline uint32_t Sh3Timing::Taken(Sh3Paths &paths, uint32_t branch, uint32_t target, uint64_t now, Code code) {
    uint32_t extra = 0;
    if (lastInsn == branch + 2 && from == branch + 4) {
        // (its slot made the last access: the branch was stepped on the way to it. The target's fetch then meets the
        // slot's MA at the cache's port and waits a cycle, SH-3 software manual 10.2.1: the port's count does that at
        // the next event; HS3 and the board agree, docs/SLOWDOWN.md)
        const uint16_t op = code(branch);
        const uint32_t slotCycles = Cycles(code(branch + 2));
        const uint64_t decode = nextDecode + TakenAdvance(op, slotCycles) - slotCycles;
        extra = decode > now ? static_cast<uint32_t>(decode - now) : 0;
    } else {
        extra = Stream(paths, branch, true, false, now, code);
    }
    from = target | 1;
    lastInsn = 0;
    lastTime = now + extra;
    nextDecode = now + extra;
    const uint16_t op = code(branch);
    lastLoad = (op & 0xF000) == 0xB000 || (op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x0003 ? kPrLinked : -1;
    return extra;
}

} // namespace spelunk
