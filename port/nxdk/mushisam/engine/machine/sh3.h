// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp; the CPU core is the author's SH-4 core (from a port not yet public) made into an SH-3.
// The CV1000's CPU: a Hitachi SH-3 (SH7709S at 102.4 MHz) in big-endian mode, with the parts of its on-chip
// peripherals the games use (the interrupt controller with its IRQ pins, the three timers, the I/O ports). The MMU
// is not modelled: addresses are physical (the low 29 bits) in every area but P4.
//
// The instructions themselves are in sh3_ops.h, written once and used by both this interpreter and the translated
// code, so the two cannot disagree about what an instruction does.
//
// Main RAM is kept as the board holds it (big-endian bytes), so what the blitter and the checks read from it is the
// game's own layout; the accessors swap on the way in and out.
#pragma once

#ifndef XBOX_FAST_CPU
#include "bus_timing.h"
#ifdef SPELUNK_SHADOW_TIMING
#include "bus_timing_shadow.h"
#endif
#endif

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

static_assert(std::endian::native == std::endian::little, "spelunkomp runs on little-endian hosts only");

namespace spelunk {

struct NativeBlock;

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
using sint8 = int8_t;
using sint16 = int16_t;
using sint32 = int32_t;
using sint64 = int64_t;

class Archive;

// What the CPU sees outside main RAM: the board (machine/cv1000.cpp). Sizes are 1, 2 or 4 bytes; values are
// numbers (the bus does not see byte order).
class Bus {
public:
    virtual ~Bus() = default;
    virtual uint32 Read(uint32 address, unsigned size) = 0;
    virtual void Write(uint32 address, uint32 value, unsigned size) = 0;
    // Optional bulk DMA read from a fixed hardware port (e.g. NAND data port 0x10000000) into main RAM.
    virtual bool ReadBulk(uint32 /*address*/, uint8 * /*dst*/, uint32 /*count*/) { return false; }
    // An instruction fetched from outside main RAM (the boot flash)
    virtual uint16 Fetch(uint32 address) = 0;
    // The CPU's I/O ports (A = 0, B = 1 ... H = 7, J = 8, K = 9, L = 10): what the board drives, what it is sent
    virtual uint8 PortRead(unsigned port) {
        return 0xFF;
    }
    virtual void PortWrite(unsigned port, uint8 value) {}
    // The cycle the board asked for (SH3::Schedule) has come
    virtual void Event() {}
};

class SH3;
using OpFn = void (*)(SH3 &, uint16);

// The status register's bits
enum : uint32 {
    kSrT = 1u << 0,
    kSrS = 1u << 1,
    kSrImask = 0xFu << 4,
    kSrQ = 1u << 8,
    kSrM = 1u << 9,
    kSrBL = 1u << 28,
    kSrRB = 1u << 29,
    kSrMD = 1u << 30,
    kSrMask = 0x700003F3u,
};

class SH3 {
public:
    SH3();

    // --- state, as the instructions and the translated code work on it ---
    std::array<uint32, 16> R{};
    std::array<uint32, 8> bank{}; // the other bank of R0-R7 (swapped in and out as SR.MD && SR.RB change)
    uint32 PC = 0, PR = 0, GBR = 0, VBR = 0, SSR = 0, SPC = 0;
    uint32 MACH = 0, MACL = 0;
    uint32 sr = 0; // all but T, kept on its own (read and written through GetSR / SetSR)
    uint32 T = 0;

    uint64 cycles = 0; // CPU cycles run since reset (one per instruction, plus what memory adds)
    bool branched = false; // the instruction just run set PC itself
    bool sleeping = false;
    bool inSlot = false; // running a delay slot instruction

    void Reset();
    uint32 GetSR() const {
        return sr | T;
    }
    void SetSR(uint32 value);
    // The code of the interrupt last taken (INTEVT2: the source's own code)
    uint32 InterruptEvent() const {
        return INTEVT2;
    }

    // The interrupt the controller would take now: its code and level (0: none). Worked out by RecalcInterrupts
    // from the on-chip sources and the IRQ pins the board drives.
    uint32 pendingCode = 0, pendingLevel = 0;
    bool interruptReady = false; // pendingLevel above the mask, and SR.BL clear
    // An IRQ pin (0-5) asserted by the board; it stays asserted until the interrupt is taken
    void RaiseIrq(unsigned line) {
        irqLines |= 1u << line;
        RecalcInterrupts();
    }
    void RecalcInterrupts();
    // Takes the pending interrupt (the caller has checked interruptReady)
    void TakeInterrupt();
    // A general exception (TRAPA, illegal instruction): EXPEVT code, entered at VBR + 0x100
    void TakeException(uint32 code, uint32 returnPc);

    // Runs instructions until `cycles` reaches `until`, taking interrupts between instructions
    void Run(uint64 until);
    // One instruction at PC (or one taken interrupt)
    void Step();

    // --- memory ---
    uint8 *ram = nullptr; // main RAM at 0x0C000000: 8 MB on CV1000-B (Xbox), 16 MB on CV1000-D
#ifdef XBOX_CV1000_B
    static constexpr uint32 kRamSize = 0x00800000u;
#else
    static constexpr uint32 kRamSize = 0x01000000u;
#endif
    Bus *bus = nullptr;
    static bool IsRam(uint32 a) {
        return (a & 0x1F000000u) == 0x0C000000u && a < 0xE0000000u;
    }
    // The program flash (U4), where the board starts: its boot code (the copy of the program into RAM) runs from there
    static bool IsFlash(uint32 a) {
        return (a & 0x1FFFFFFFu) < 0x400000u && a < 0xE0000000u;
    }
#ifdef XBOX_FAST_CPU
    // Xbox fast CPU mode: omit Sh3Timing, Sh3Paths (~600 KB+), and PollLoop; dead-code-eliminate timing/watching checks.
    static constexpr bool timing = false;
    static constexpr bool watching = false;
    struct DummyAt {
        inline DummyAt &operator=(uint32) { return *this; }
    };
    DummyAt at{};
    inline void TimeAccess(uint32, uint32, bool) {}
    inline void TimeBranch(uint32, uint32) {}
    inline void PollForget() {}
#else
    // The board's slowdown (bus_timing.h): with `timing` on, every data access is charged what the pipeline, the
    // cache and the bus make it cost beyond the instruction's own cycles. `at` is the address of the instruction
    // making it (set as each instruction that touches memory begins); the model reads the code it walks from RAM
    bool timing = false;
    Sh3Timing memoryTiming;
    Sh3Paths timingPaths; // (not part of the state: forgotten when one is loaded)
    uint32 at = 0;
    uint16 TimingCode(uint32 p) const {
        if (!IsRam(p)) {
            return 0;
        }
        uint16 op;
        std::memcpy(&op, ram + (p & (kRamSize - 2)), 2);
        return __builtin_bswap16(op);
    }
    void TimeAccess(uint32 a, uint32 bytes, bool write) {
#ifdef SPELUNK_SHADOW_TIMING
        const uint32 want = refTiming.Access(refPaths, at, a, bytes, write, inSlot, cycles, [this](uint32 p) -> uint16 { return TimingCode(p); });
        const uint32 got = memoryTiming.Access(timingPaths, at, a, bytes, write, inSlot, cycles,
                                               [this](uint32 p) -> uint16 { return TimingCode(p); });
        ShadowEvent(write ? 'W' : 'R', at, a, bytes, got, want);
        cycles += got;
#else
        cycles += memoryTiming.Access(timingPaths, at, a, bytes, write, inSlot, cycles,
                                      [this](uint32 p) -> uint16 { return TimingCode(p); });
#endif
        if (at == poll.slot) [[unlikely]] {
            poll.read = cycles; // (the poll loop's read: when it is made, after what the model charged)
        }
    }
    // A taken branch (or any instruction that set the PC) at `branch`, its delay slot run and counted: PC is where it
    // went. Translated code calls it at the same point
    void TimeBranch(uint32 branch, uint32 target) {
#ifdef SPELUNK_SHADOW_TIMING
        const uint32 want = refTiming.Taken(refPaths, branch, target, cycles, [this](uint32 p) -> uint16 { return TimingCode(p); });
        const uint32 got = memoryTiming.Taken(timingPaths, branch, target, cycles, [this](uint32 p) -> uint16 { return TimingCode(p); });
        ShadowEvent('B', branch, target, 0, got, want);
        cycles += got;
#else
        cycles += memoryTiming.Taken(timingPaths, branch, target, cycles, [this](uint32 p) -> uint16 { return TimingCode(p); });
#endif
        if (branch == target + 4) [[unlikely]] {
            PollTurn(target);
        }
    }
    // The games' wait for a timer (docs/SLOWDOWN.md, "The poll loop"): a loop of four instructions, `sub Rm,Rn;
    // cmp/hi Rm,Rn; bt/s back; mov.l @Rm,Rn` reading a timer's TCNT in the delay slot, that turns until enough of it has
    // counted. Most of a busy frame's timing events are its turns, each the same as the one before. Once two turns in a
    // row have cost the same and moved the model's state the same way (every field the same, or later by the turn's
    // cycles), the turns after them, up to the loop's end or the block's stop, are worked out here instead of run: the
    // registers as the instructions leave them, TCNT as it reads at each turn's read, the model's state moved on by
    // the same steps. Native mode only (the interpreter's blocks mode, which tools/check.sh compares with, runs every
    // turn). The shadow build (tools/shadow/) runs every skip both ways and compares them
    struct PollLoop {
        uint32 head = 0, slot = 0xFFFFFFFFu; // (the loop's first instruction and its delay slot; none: 0, and no slot)
        uint8 n0 = 0, m0 = 0, n1 = 0, m1 = 0, n3 = 0, m3 = 0; // (sub Rm0,Rn0; cmp/hi Rm1,Rn1; mov.l @Rm3,Rn3)
        int turns = 0;                // (turns seen in a row, each ending where the one before did)
        uint64 end = 0, read = 0;     // (the last turn's end, and this turn's read)
        uint64 cost = 0, readAt = 0;  // (the last turn's cycles, and its read's cycles into the turn)
        uint32 moved = 0;             // (the model's time fields the last turn moved, by its cycles: kPoll... bits)
        Sh3Timing before;             // (the model at the last turn's end)
    } poll;
    bool pollSkip = true; // (SPELUNK_NO_POLL_SKIP: every turn run)
    bool pollInside = false; // (the shadow build's turns run the slow way: no skip within them)
    uint64 pollSkips = 0, pollTurnsSkipped = 0;
    void PollTurn(uint32 target);
    void PollForget() {
        poll.head = 0;
        poll.slot = 0xFFFFFFFFu;
        poll.turns = 0;
    }
#ifdef SPELUNK_SHADOW_TIMING
    // The shadow build (tools/shadow/): the reference model (machine/bus_timing_ref.h) run beside the one in use, at
    // every event, and the two compared; the first difference stops the run with both states (ShadowFail)
    ref::Sh3Timing refTiming;
    ref::Sh3Paths refPaths;
    uint64 shadowEvents = 0;
    uint64 shadowSkips = 0, shadowSkipTurns = 0; // (poll-loop skips checked against the turns run, PollTurn)
    struct ShadowRecord {
        char kind;
        uint32 insn, address, bytes, got, want;
        uint64 cycles;
    };
    ShadowRecord shadowLog[64] = {};
    void ShadowEvent(char kind, uint32 insn, uint32 address, uint32 bytes, uint32 got, uint32 want) {
        shadowLog[shadowEvents & 63] = {kind, insn, address, bytes, got, want, cycles};
        shadowEvents++;
        if (got != want || !TimingScalarsEqual(memoryTiming, refTiming) ||
            ((shadowEvents & 255) == 0 && !TimingCacheEqual(memoryTiming, refTiming))) [[unlikely]] {
            ShadowFail("event");
        }
    }
    // Both whole, at a frame's end (the board's), and wherever the model was changed outside an event
    void ShadowCheckWhole(const char *where) {
        if (!TimingScalarsEqual(memoryTiming, refTiming) || !TimingCacheEqual(memoryTiming, refTiming)) {
            ShadowFail(where);
        }
    }
    [[noreturn]] void ShadowFail(const char *where);
    // (a state loaded: the reference given the same state, and its own paths forgotten as the model's are)
    void ShadowSync() {
        TimingCopy(memoryTiming, refTiming);
        refPaths.Forget();
    }
#endif
#endif // !XBOX_FAST_CPU
    uint8 Read8(uint32 a) {
#ifndef XBOX_FAST_CPU
        if (timing) [[unlikely]] {
            TimeAccess(a, 1, false);
        }
#endif
        if (IsRam(a)) [[likely]] {
            return ram[a & (kRamSize - 1)];
        }
        return static_cast<uint8>(ReadSlow(a, 1));
    }
    uint16 Read16(uint32 a) {
#ifndef XBOX_FAST_CPU
        if (timing) [[unlikely]] {
            TimeAccess(a, 2, false);
        }
#endif
        if (IsRam(a)) [[likely]] {
            uint16 v;
            __builtin_memcpy(&v, ram + (a & (kRamSize - 2)), 2);
            return __builtin_bswap16(v);
        }
        return static_cast<uint16>(ReadSlow(a & ~1u, 2));
    }
    uint32 Read32(uint32 a) {
#ifndef XBOX_FAST_CPU
        if (timing) [[unlikely]] {
            TimeAccess(a, 4, false);
        }
#endif
        if (IsRam(a)) [[likely]] {
            uint32 v;
            __builtin_memcpy(&v, ram + (a & (kRamSize - 4)), 4);
            return __builtin_bswap32(v);
        }
        return ReadSlow(a & ~3u, 4);
    }
    void Write8(uint32 a, uint8 v) {
#ifndef XBOX_FAST_CPU
        if (timing) [[unlikely]] {
            TimeAccess(a, 1, true);
        }
        if (watching) [[unlikely]] {
            Watch(a, v, 1);
        }
#endif
        if (IsRam(a)) [[likely]] {
            NoteCodeWrite(a);
            ram[a & (kRamSize - 1)] = v;
            return;
        }
        WriteSlow(a, v, 1);
    }
    void Write16(uint32 a, uint16 v) {
#ifndef XBOX_FAST_CPU
        if (timing) [[unlikely]] {
            TimeAccess(a, 2, true);
        }
        if (watching) [[unlikely]] {
            Watch(a, v, 2);
        }
#endif
        if (IsRam(a)) [[likely]] {
            NoteCodeWrite(a);
            v = __builtin_bswap16(v);
            __builtin_memcpy(ram + (a & (kRamSize - 2)), &v, 2);
            return;
        }
        WriteSlow(a & ~1u, v, 2);
    }
    void Write32(uint32 a, uint32 v) {
#ifndef XBOX_FAST_CPU
        if (timing) [[unlikely]] {
            TimeAccess(a, 4, true);
        }
        if (watching) [[unlikely]] {
            Watch(a, v, 4);
        }
#endif
        if (IsRam(a)) [[likely]] {
            NoteCodeWrite(a);
            v = __builtin_bswap32(v);
            __builtin_memcpy(ram + (a & (kRamSize - 4)), &v, 4);
            return;
        }
        WriteSlow(a & ~3u, v, 4);
    }
    uint16 Fetch(uint32 a) {
        if (IsRam(a)) [[likely]] {
            uint16 v;
            __builtin_memcpy(&v, ram + (a & (kRamSize - 2)), 2);
            return __builtin_bswap16(v);
        }
        return bus->Fetch(a & 0x1FFFFFFEu);
    }
    // --- blocks and translations (runtime/blocks.h, runtime/dispatch.cpp) ---
    enum class Mode { Interpreter, Blocks, Native };
    Mode mode = Mode::Interpreter;
    static constexpr uint32 kPages = kRamSize >> 12;
    uint32 codeAddrEnd = 0;        // upper bound (exclusive) of translated code in RAM; skips codeMark[] for data/stack writes
    uint8 codeMark[kPages] = {};   // pages of RAM that hold translated code
    uint32 pageEpoch[kPages] = {}; // moved on by each write to such a page
    void NoteCodeWrite(uint32 a) {
        const uint32 phys = a & (kRamSize - 1);
#ifdef XBOX_FAST_CPU
        if (phys >= codeAddrEnd) [[likely]] {
            return;
        }
#endif
        const uint32 page = phys >> 12;
        if (codeMark[page]) [[unlikely]] {
            ++pageEpoch[page];
        }
    }
    // Writes to RAM from outside the CPU
    void NoteRamWrite(uint32 a, uint32 length);
    void InstallTranslations();
    // One block by the interpreter (the reference the translations are checked against)
    void InterpretBlock();
    // Blocks seen (for the translator): written out by SaveRecording
    // (each version of a block's bytes at an address is recorded once). With `recordStatic`, blocks the recorded ones
    // can go to that never ran are found in RAM as they are recorded (branch and call targets, the way on past a
    // conditional branch or a call): `recordStatic` steps of that from each, written out marked as found that way
    bool recording = false;
    unsigned recordStatic = 0;
    void SaveRecording(const char *path) const;
    // The blocks the interpreter ran in native mode, by address, with how often (SPELUNK_INTERPRETED=FILE on the
    // runners: what is left for the translator)
    bool countInterpreted = false;
    void SaveInterpreted(const char *path);
    uint64 nativeBlocks = 0, interpretedBlocks = 0;
    uint32 lastInterpretedPc = 0;
    // The board's next event (the blitter's IRQ1): Bus::Event is called at the first instruction or block boundary at or
    // after it, and at the latest when Run returns. Not state: the board sets it again when a state is loaded
    uint64 eventAt = ~0ull;
    void Schedule(uint64 at) {
        eventAt = at;
        if (at < blockStop) {
            blockStop = at;
        }
    }
    uint64 blockStop = 0; // (where RunBlocks stops: a translated block that loops to itself checks it as the dispatcher would)

    // --- on-chip peripherals (0xFFFFFExx, and 0xA4000000 up) ---
    uint32 ReadRegister(uint32 a, unsigned size);
    void WriteRegister(uint32 a, uint32 value, unsigned size);
    // Timers: brought up to the CPU's time (their counters are worked out lazily); the cycle of the next underflow
    void SyncTimers();
    void SyncTimer(int i);
    uint64 NextTimerEvent() const;
#ifdef XBOX_FAST_CPU
    // O(1) exact fast-forward helpers for Mushihimesama's hot TMU poll loop (0x0C04A2AA) & delay loops (0x0C056756/68).
    bool FastForwardTmuPoll();
    bool FastForwardDelayLoop(uint32 loopPc);
#endif

    // Debugging: addresses where the registers are printed when reached (SPELUNK_BREAK=addr,addr...)
    std::vector<uint32> breakpoints;
    // Debugging: writes to main RAM in [watchLo, watchHi) are reported (SPELUNK_WATCH=lo-hi, physical)
    uint32 watchLo = 0, watchHi = 0;
    // Every write the CPU makes while `journal` is set is noted there first, with what it wrote over (the rewrites'
    // checks and their stepping aside: runtime/rewrite.h). A write outside main RAM is noted with `hardware` set
    struct Written {
        uint32 address;
        uint8 size;
        bool hardware;
        uint32 old;
        uint32 value;
    };
    std::vector<Written> *journal = nullptr;
#ifndef XBOX_FAST_CPU
    bool watching = false; // (either of the two: a write goes through Watch)
    void SetJournal(std::vector<Written> *j) {
        journal = j;
        watching = journal || watchHi;
    }
#else
    void SetJournal(std::vector<Written> *j) {
        journal = j;
    }
#endif
    // One block from PC, as the dispatcher runs it: C++ at the address first (unless `native` is false), then its
    // translation, else the interpreter (the rewrites' calls of the game's own routines: runtime/rewrite.h)
    void RunOneBlock(bool native = true);
    // C++ at an address (rewrites, hooks): when PC reaches it (as a block starts), `fn` runs instead of guest code,
    // and sets PC itself (to PR, for a call); or it returns false and the guest code there runs after all
    using NativeCall = bool (*)(SH3 &cpu, void *context);
    void SetNativeCall(uint32 address, NativeCall fn, void *context) {
        const uint32 phys = address & 0x1FFFFFFFu;
        m_nativeCalls[phys] = {fn, context};
        if (m_nativePages.empty()) {
            m_nativePages.assign(kPages, 0);
        }
        m_nativePages[(phys & (kRamSize - 1)) >> 12] = 1;
    }
    bool HasNativeCall(uint32 address) const {
        return m_nativeCalls.count(address & 0x1FFFFFFFu) != 0;
    }
    // (the page stays marked: a lookup that finds nothing there)
    void ClearNativeCall(uint32 address) {
        m_nativeCalls.erase(address & 0x1FFFFFFFu);
    }
    // (true if one ran: the caller goes on from the PC it set)
    bool RunNativeCall() {
        if (m_nativePages.empty() || !IsRam(PC) || !m_nativePages[(PC & (kRamSize - 1)) >> 12]) [[likely]] {
            return false;
        }
        const auto it = m_nativeCalls.find(PC & 0x1FFFFFFFu);
        if (it == m_nativeCalls.end()) {
            return false;
        }
        if (!it->second.first(*this, it->second.second)) {
            return false;
        }
        cycles += 4;
        return true;
    }
    // Debugging and checks: called once when PC first reaches `stopPc` (a block's start; physical compare)
    uint32 stopPc = 0xFFFFFFFFu;
    std::function<void()> onStopPc;
    void Watch(uint32 a, uint32 v, unsigned size);
    void Report(const char *why) const;

    // The decoding table: one function per opcode (sh3_ops.h)
    static OpFn s_ops[0x10000];
    static uint8 s_cycles[0x10000];

    void Serialize(Archive &ar);
    // Run-ahead: while set, reading the state back leaves the translations' page marks alone (the board, which puts
    // RAM back itself, moves on only the pages whose code changed)
    bool keepEpochs = false;
    // Run-ahead and rollback: while set, reading the state back keeps the timing model's paths (the board, which puts
    // RAM back itself, then has those over the blocks written since forgotten: Sh3Paths::PutBack)
    bool keepPaths = false;
    // Every register as text, one per line (checks: two runs compared by name)
    void Describe(FILE *out) const;

    struct Translation {
        void (*run)(SH3 &) = nullptr; // cached from block->run to avoid extra pointer chase on every block dispatch
        uint32 address = 0;           // cached from block->address
        uint32 lastPage = 0;          // cached (phys + length - 1) >> 12
        uint32 epoch = UINT32_MAX, epoch2 = UINT32_MAX;
        const struct NativeBlock *block = nullptr;
        Translation *next = nullptr; // (the same RAM run at another segment's address)
        bool valid = false;
    };

    uint32 ReadSlowPublic(uint32 a, unsigned size) { return ReadSlow(a, size); }
    void WriteSlowPublic(uint32 a, uint32 v, unsigned size) { WriteSlow(a, v, size); }

    using BlockFn = void (*)(SH3 &);

private:
    std::unordered_map<uint32, std::pair<NativeCall, void *>> m_nativeCalls;
    std::vector<uint8> m_nativePages;
    std::vector<Translation *> m_pages[kPages]; // (per page, one slot per halfword once the page has any)
    Translation **m_pagePtrs[kPages] = {};      // flat direct pointers to m_pages[p].data() for zero-indirection Lookup()
    std::vector<BlockFn> m_fnPages[kPages];     // direct function-pointer page table (XBOX_IMMUTABLE_CODE fast dispatch)
    BlockFn *m_pageFnPtrs[kPages] = {};         // flat direct pointers to m_fnPages[p].data()
    std::unordered_map<uint32, Translation *> m_flash; // (the flash's translations, by address: it never changes)
    std::vector<Translation> m_translations;
    struct Recorded {
        uint32 pc;
        bool found; // (found by following the code, not run)
        std::vector<uint8> bytes;
    };
    std::vector<Recorded> m_recorded;
    std::unordered_map<uint32, std::vector<uint32>> m_recordedAt; // (each address's versions: indexes into m_recorded)
    std::unordered_map<uint32, uint64> m_interpretedAt;
    // The block at `pc` as RAM holds it now (by the shared rule), recorded unless this version already is; true if new
    bool RecordBlock(uint32 pc, bool found);
    void FollowBlock(uint32 pc, unsigned steps);
    Translation *Lookup(uint32 pc);
    void RunBlocks(uint64 stop);

    uint32 ReadSlow(uint32 a, unsigned size);
    void WriteSlow(uint32 a, uint32 v, unsigned size);
    void SwapBanks();

    uint32 irqLines = 0; // the IRQ pins asserted and not yet taken

    // Interrupt controller
    uint16 ICR0 = 0, ICR1 = 0, ICR2 = 0, IPRA = 0, IPRB = 0, IPRC = 0, IPRD = 0, IPRE = 0;
    uint32 INTEVT = 0, INTEVT2 = 0, EXPEVT = 0, TRA = 0;
    // Timer unit
    struct Timer {
        uint32 TCOR = 0xFFFFFFFFu, TCNT = 0xFFFFFFFFu;
        uint16 TCR = 0;
        uint64 base = 0; // the cycle TCNT was last worked out at
    } tmu[3];
    uint8 TOCR = 0, TSTR = 0;
    // Cache and MMU registers kept as plain values
    uint32 CCR = 0, MMUCR = 0, PTEH = 0, PTEL = 0, TTB = 0, TEA = 0;
    // DMA controller: four channels (the games read the NAND flash a page at a time through channel 0)
    uint32 SAR[4] = {}, DAR[4] = {}, DMATCR[4] = {}, CHCR[4] = {}, DMAOR = 0;
    void RunDma(unsigned channel);
    std::array<uint8, 11> portOut{};   // what was last written to each port's data register
    std::array<uint32, 64> misc{};     // other registers games write and read back (by a small hash)
};

} // namespace spelunk
