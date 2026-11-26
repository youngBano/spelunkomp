// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
#include "sh3.h"
#include "sh3_ops.h"

#include "../runtime/state.h"

#include <cstdio>
#include <cstdlib>

#ifdef SPELUNK_SHADOW_TIMING
#define SHADOW_TIMING_ALSO(x) x
#else
#define SHADOW_TIMING_ALSO(x)
#endif

namespace spelunk {

OpFn SH3::s_ops[0x10000];
uint8 SH3::s_cycles[0x10000];

namespace {
    struct Pattern {
        uint16 mask, match;
        OpFn fn;
        uint8 cycles;
    };
    using namespace ops;
    // Every SH-3 instruction, by the bits that identify it (the fixed parts of the opcode)
    const Pattern kPatterns[] = {
        // 0000
        {0xF0FF, 0x0002, STC_SR, 1}, {0xF0FF, 0x0012, STC_GBR, 1}, {0xF0FF, 0x0022, STC_VBR, 1},
        {0xF0FF, 0x0032, STC_SSR, 1}, {0xF0FF, 0x0042, STC_SPC, 1}, {0xF08F, 0x0082, STC_BANK, 1},
        {0xF0FF, 0x0003, BSRF, 2}, {0xF0FF, 0x0023, BRAF, 2}, {0xF0FF, 0x0083, PREF, 1},
        {0xF00F, 0x0004, MOVB_S0, 1}, {0xF00F, 0x0005, MOVW_S0, 1}, {0xF00F, 0x0006, MOVL_S0, 1},
        {0xF00F, 0x0007, MULL, 2}, {0xFFFF, 0x0008, CLRT, 1}, {0xFFFF, 0x0018, SETT, 1},
        {0xFFFF, 0x0028, CLRMAC, 1}, {0xFFFF, 0x0038, LDTLB, 1}, {0xFFFF, 0x0048, CLRS, 1},
        {0xFFFF, 0x0058, SETS, 1}, {0xFFFF, 0x0009, NOP, 1}, {0xFFFF, 0x0019, DIV0U, 1}, {0xF0FF, 0x0029, MOVT, 1},
        {0xF0FF, 0x000A, STS_MACH, 1}, {0xF0FF, 0x001A, STS_MACL, 1}, {0xF0FF, 0x002A, STS_PR, 1},
        {0xFFFF, 0x000B, RTS, 2}, {0xFFFF, 0x001B, SLEEP, 4}, {0xFFFF, 0x002B, RTE, 4},
        {0xF00F, 0x000C, MOVB_L0, 1}, {0xF00F, 0x000D, MOVW_L0, 1}, {0xF00F, 0x000E, MOVL_L0, 1},
        {0xF00F, 0x000F, MACL, 2},
        // 0001, 0101
        {0xF000, 0x1000, MOVL_S4, 1}, {0xF000, 0x5000, MOVL_L4, 1},
        // 0010
        {0xF00F, 0x2000, MOVB_S, 1}, {0xF00F, 0x2001, MOVW_S, 1}, {0xF00F, 0x2002, MOVL_S, 1},
        {0xF00F, 0x2004, MOVB_M, 1}, {0xF00F, 0x2005, MOVW_M, 1}, {0xF00F, 0x2006, MOVL_M, 1},
        {0xF00F, 0x2007, DIV0S, 1}, {0xF00F, 0x2008, TST, 1}, {0xF00F, 0x2009, AND, 1}, {0xF00F, 0x200A, XOR, 1},
        {0xF00F, 0x200B, OR, 1}, {0xF00F, 0x200C, CMPSTR, 1}, {0xF00F, 0x200D, XTRCT, 1},
        {0xF00F, 0x200E, MULUW, 1}, {0xF00F, 0x200F, MULSW, 1},
        // 0011
        {0xF00F, 0x3000, CMPEQ, 1}, {0xF00F, 0x3002, CMPHS, 1}, {0xF00F, 0x3003, CMPGE, 1},
        {0xF00F, 0x3004, DIV1, 1}, {0xF00F, 0x3005, DMULU, 2}, {0xF00F, 0x3006, CMPHI, 1},
        {0xF00F, 0x3007, CMPGT, 1}, {0xF00F, 0x3008, SUB, 1}, {0xF00F, 0x300A, SUBC, 1}, {0xF00F, 0x300B, SUBV, 1},
        {0xF00F, 0x300C, ADD, 1}, {0xF00F, 0x300D, DMULS, 2}, {0xF00F, 0x300E, ADDC, 1}, {0xF00F, 0x300F, ADDV, 1},
        // 0100
        {0xF0FF, 0x4000, SHLL, 1}, {0xF0FF, 0x4001, SHLR, 1}, {0xF0FF, 0x4002, STSL_MACH, 1},
        {0xF0FF, 0x4003, STCL_SR, 1}, {0xF0FF, 0x4004, ROTL, 1}, {0xF0FF, 0x4005, ROTR, 1},
        {0xF0FF, 0x4006, LDSL_MACH, 1}, {0xF0FF, 0x4007, LDCL_SR, 7}, {0xF0FF, 0x4008, SHLL2, 1},
        {0xF0FF, 0x4009, SHLR2, 1}, {0xF0FF, 0x400A, LDS_MACH, 1}, {0xF0FF, 0x400B, JSR, 2},
        {0xF0FF, 0x400E, LDC_SR, 5}, {0xF0FF, 0x4010, DT, 1}, {0xF0FF, 0x4011, CMPPZ, 1},
        {0xF0FF, 0x4012, STSL_MACL, 1}, {0xF0FF, 0x4013, STCL_GBR, 1}, {0xF0FF, 0x4015, CMPPL, 1},
        {0xF0FF, 0x4016, LDSL_MACL, 1}, {0xF0FF, 0x4017, LDCL_GBR, 5}, {0xF0FF, 0x4018, SHLL8, 1},
        {0xF0FF, 0x4019, SHLR8, 1}, {0xF0FF, 0x401A, LDS_MACL, 1}, {0xF0FF, 0x401B, TASB, 3},
        {0xF0FF, 0x401E, LDC_GBR, 3}, {0xF0FF, 0x4020, SHAL, 1}, {0xF0FF, 0x4021, SHAR, 1},
        {0xF0FF, 0x4022, STSL_PR, 1}, {0xF0FF, 0x4023, STCL_VBR, 1}, {0xF0FF, 0x4024, ROTCL, 1},
        {0xF0FF, 0x4025, ROTCR, 1}, {0xF0FF, 0x4026, LDSL_PR, 1}, {0xF0FF, 0x4027, LDCL_VBR, 5},
        {0xF0FF, 0x4028, SHLL16, 1}, {0xF0FF, 0x4029, SHLR16, 1}, {0xF0FF, 0x402A, LDS_PR, 1},
        {0xF0FF, 0x402B, JMP, 2}, {0xF0FF, 0x402E, LDC_VBR, 3}, {0xF0FF, 0x4033, STCL_SSR, 1},
        {0xF0FF, 0x4037, LDCL_SSR, 5}, {0xF0FF, 0x403E, LDC_SSR, 3}, {0xF0FF, 0x4043, STCL_SPC, 1},
        {0xF0FF, 0x4047, LDCL_SPC, 5}, {0xF0FF, 0x404E, LDC_SPC, 3}, {0xF08F, 0x4083, STCL_BANK, 2},
        {0xF08F, 0x4087, LDCL_BANK, 5}, {0xF08F, 0x408E, LDC_BANK, 3}, {0xF00F, 0x400C, SHAD, 1},
        {0xF00F, 0x400D, SHLD, 1}, {0xF00F, 0x400F, MACW, 2},
        // 0110
        {0xF00F, 0x6000, MOVB_L, 1}, {0xF00F, 0x6001, MOVW_L, 1}, {0xF00F, 0x6002, MOVL_L, 1},
        {0xF00F, 0x6003, MOV, 1}, {0xF00F, 0x6004, MOVB_P, 1}, {0xF00F, 0x6005, MOVW_P, 1},
        {0xF00F, 0x6006, MOVL_P, 1}, {0xF00F, 0x6007, NOT, 1}, {0xF00F, 0x6008, SWAPB, 1},
        {0xF00F, 0x6009, SWAPW, 1}, {0xF00F, 0x600A, NEGC, 1}, {0xF00F, 0x600B, NEG, 1},
        {0xF00F, 0x600C, EXTUB, 1}, {0xF00F, 0x600D, EXTUW, 1}, {0xF00F, 0x600E, EXTSB, 1},
        {0xF00F, 0x600F, EXTSW, 1},
        // 0111
        {0xF000, 0x7000, ADD_I, 1},
        // 1000
        {0xFF00, 0x8000, MOVB_S4, 1}, {0xFF00, 0x8100, MOVW_S4, 1}, {0xFF00, 0x8400, MOVB_L4, 1},
        {0xFF00, 0x8500, MOVW_L4, 1}, {0xFF00, 0x8800, CMPEQ_I, 1}, {0xFF00, 0x8900, BT, 1},
        {0xFF00, 0x8B00, BF, 1}, {0xFF00, 0x8D00, BTS, 1}, {0xFF00, 0x8F00, BFS, 1},
        // 1001 - 1110
        {0xF000, 0x9000, MOVW_PC, 1}, {0xF000, 0xA000, BRA, 2}, {0xF000, 0xB000, BSR, 2},
        {0xFF00, 0xC000, MOVB_SG, 1}, {0xFF00, 0xC100, MOVW_SG, 1}, {0xFF00, 0xC200, MOVL_SG, 1},
        {0xFF00, 0xC300, TRAPA, 6}, {0xFF00, 0xC400, MOVB_LG, 1}, {0xFF00, 0xC500, MOVW_LG, 1},
        {0xFF00, 0xC600, MOVL_LG, 1}, {0xFF00, 0xC700, MOVA, 1}, {0xFF00, 0xC800, TST_I, 1},
        {0xFF00, 0xC900, AND_I, 1}, {0xFF00, 0xCA00, XOR_I, 1}, {0xFF00, 0xCB00, OR_I, 1},
        {0xFF00, 0xCC00, TSTB_G, 3}, {0xFF00, 0xCD00, ANDB_G, 3}, {0xFF00, 0xCE00, XORB_G, 3},
        {0xFF00, 0xCF00, ORB_G, 3}, {0xF000, 0xD000, MOVL_PC, 1}, {0xF000, 0xE000, MOV_I, 1},
    };

    bool g_built = [] {
        for (uint32 op = 0; op < 0x10000; op++) {
            SH3::s_ops[op] = ops::ILLEGAL;
            SH3::s_cycles[op] = 1;
            for (const Pattern &p : kPatterns) {
                if ((op & p.mask) == p.match) {
                    SH3::s_ops[op] = p.fn;
                    SH3::s_cycles[op] = p.cycles;
                    break;
                }
            }
        }
        return true;
    }();
} // namespace

namespace ops {
    void ILLEGAL(SH3 &c, uint16 op) {
        static int reported = 0;
        if (reported++ < 4) {
            fprintf(stderr, "sh3: illegal instruction %04X at %08X\n", op, c.PC);
            c.Report("illegal");
        }
        c.TakeException(c.inSlot ? 0x1A0 : 0x180, c.PC);
        c.branched = true;
    }

    void RunDelaySlot(SH3 &c, uint32 target) {
        const uint32 slot = c.PC + 2;
        const uint16 op = c.Fetch(slot);
        c.PC = slot;
        c.at = slot;
        c.inSlot = true;
        c.branched = false;
        SH3::s_ops[op](c, op);
        c.cycles += SH3::s_cycles[op];
        c.inSlot = false;
        c.PC = target;
        c.branched = true;
    }
} // namespace ops

SH3::SH3() {
    (void)g_built;
#ifndef XBOX_FAST_CPU
    Sh3Timing::opCycles = s_cycles;
    SHADOW_TIMING_ALSO(ref::Sh3Timing::opCycles = s_cycles);
    pollSkip = getenv("SPELUNK_NO_POLL_SKIP") == nullptr;
    if (const char *w = getenv("SPELUNK_WATCH")) {
        sscanf(w, "%x-%x", &watchLo, &watchHi);
        watching = watchHi != 0;
    }
#endif
    if (const char *list = getenv("SPELUNK_BREAK")) {
        for (const char *p = list; *p;) {
            char *end;
            breakpoints.push_back(static_cast<uint32>(strtoul(p, &end, 16)));
            p = *end ? end + 1 : end;
        }
    }
    Reset();
}

// Power on: privileged, the second register bank, interrupts blocked, running from the start of the boot flash
void SH3::Reset() {
    R.fill(0);
    bank.fill(0);
    sr = 0;
    T = 0;
    SetSR(kSrMD | kSrRB | kSrBL | kSrImask);
    VBR = 0;
    PC = 0xA0000000u;
    EXPEVT = 0;
    sleeping = false;
    irqLines = 0;
    eventAt = ~0ull;
    RecalcInterrupts();
}

void SH3::Watch(uint32 a, uint32 v, unsigned size) {
    const uint32 p = a & 0x1FFFFFFFu;
    if (journal) {
        Written w{a, static_cast<uint8>(size), !IsRam(a), 0, v};
        if (!w.hardware) {
            const uint8 *at = ram + (a & (kRamSize - size));
            for (unsigned i = 0; i < size; i++) {
                w.old = (w.old << 8) | at[i];
            }
        }
        journal->push_back(w);
    }
    if (p >= watchLo && p < watchHi) {
        fprintf(stderr, "watch %08X = %0*X (%u) pc %08X pr %08X\n", a, size * 2, v, size, PC, PR);
    }
}

void SH3::Report(const char *why) const {
    fprintf(stderr, "%s pc %08X pr %08X sr %08X | r0 %08X r1 %08X r2 %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X | r8 %08X r9 %08X r10 %08X r11 %08X r12 %08X r13 %08X r14 %08X r15 %08X\n",
            why, PC, PR, GetSR(), R[0], R[1], R[2], R[3], R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[11], R[12], R[13], R[14], R[15]);
}

void SH3::SwapBanks() {
    for (int i = 0; i < 8; i++) {
        std::swap(R[i], bank[i]);
    }
}

void SH3::SetSR(uint32 value) {
    value &= kSrMask;
    const bool oldBank = (sr & kSrMD) && (sr & kSrRB);
    const bool newBank = (value & kSrMD) && (value & kSrRB);
    if (oldBank != newBank) {
        SwapBanks();
    }
    T = value & kSrT;
    sr = value & ~kSrT;
    RecalcInterrupts();
}

// ---------------------------------------------------------------------------------------------------------
// Interrupts and exceptions

void SH3::RecalcInterrupts() {
    uint32 code = 0, level = 0;
    auto offer = [&](uint32 c, uint32 l) {
        if (l > level) {
            level = l;
            code = c;
        }
    };
    // (the IRQ pins: IRQ0-3 take their priorities from IPRC, IRQ4-5 from IPRD)
    for (unsigned i = 0; i < 6; i++) {
        if (irqLines & (1u << i)) {
            offer(0x600 + 0x20 * i, i < 4 ? (IPRC >> (4 * i)) & 15 : (IPRD >> (4 * (i - 4))) & 15);
        }
    }
    // (timers: underflow flag and its interrupt enabled)
    for (int i = 0; i < 3; i++) {
        if ((tmu[i].TCR & 0x0120) == 0x0120) {
            offer(0x400 + 0x20 * i, (IPRA >> (12 - 4 * i)) & 15);
        }
    }
    // (DMA channels: transfer ended and its interrupt enabled)
    for (int i = 0; i < 4; i++) {
        if ((CHCR[i] & 0x6) == 0x6) {
            offer(0x800 + 0x20 * i, (IPRE >> 12) & 15);
        }
    }
    pendingCode = code;
    pendingLevel = level;
    interruptReady = level > ((sr & kSrImask) >> 4) && !(sr & kSrBL);
}

void SH3::TakeInterrupt() {
    SPC = PC;
    SSR = GetSR();
    INTEVT2 = pendingCode;
    // (INTEVT holds the source's code for the on-chip sources, and for the IRQ pins a code by priority level)
    INTEVT = pendingCode >= 0x600 ? 0x3E0 - pendingLevel * 0x20 : pendingCode;
    if (pendingCode >= 0x600 && pendingCode < 0x6C0) {
        irqLines &= ~(1u << ((pendingCode - 0x600) >> 5)); // (the pin is held only until it is taken)
    }
    SetSR(GetSR() | kSrMD | kSrRB | kSrBL);
    PC = VBR + 0x600;
    sleeping = false;
    cycles += 5;
#ifndef XBOX_FAST_CPU
    if (timing) {
        memoryTiming.Redirect(PC, cycles);
        PollForget();
#ifdef SPELUNK_SHADOW_TIMING
        refTiming.Redirect(PC, cycles);
        ShadowCheckWhole("interrupt");
#endif
    }
#endif
}

void SH3::TakeException(uint32 code, uint32 returnPc) {
    if (code == 0x160) {
        TRA = (Fetch(PC) & 0xFF) << 2;
    }
    SPC = returnPc;
    SSR = GetSR();
    EXPEVT = code;
    SetSR(GetSR() | kSrMD | kSrRB | kSrBL);
    PC = VBR + 0x100;
}

// ---------------------------------------------------------------------------------------------------------
// Running

void SH3::Step() {
    if (interruptReady) [[unlikely]] {
        TakeInterrupt();
        return;
    }
    const uint32 pc = PC;
    const uint16 op = Fetch(pc);
    branched = false;
    s_ops[op](*this, op);
    cycles += s_cycles[op];
    if (!branched) {
        PC += 2;
    } else if (timing) [[unlikely]] {
        TimeBranch(pc, PC);
    }
}

void SH3::Run(uint64 until) {
    while (cycles < until) {
        if (cycles >= eventAt) [[unlikely]] {
            eventAt = ~0ull;
            bus->Event();
        }
        const uint64 timer = NextTimerEvent();
        if (cycles >= timer) {
            SyncTimers();
            RecalcInterrupts();
        }
        if (interruptReady) {
            TakeInterrupt();
            continue;
        }
        if (sleeping) {
            cycles = std::min(until, std::max(std::min(timer, eventAt), cycles + 1));
            continue;
        }
        const uint64 stop = std::min(std::min(until, timer), eventAt);
        if (mode != Mode::Interpreter) {
            RunBlocks(stop);
            continue;
        }
        blockStop = stop;
        while (cycles < blockStop && !interruptReady && !sleeping) {
            if (RunNativeCall()) [[unlikely]] {
                continue;
            }
            if (!breakpoints.empty()) [[unlikely]] {
                for (uint32 b : breakpoints) {
                    if ((PC & 0x1FFFFFFFu) == (b & 0x1FFFFFFFu)) {
                        Report("break");
                    }
                }
            }
            const uint32 pc = PC;
            const uint16 op = Fetch(pc);
            branched = false;
            at = pc;
            s_ops[op](*this, op);
            cycles += s_cycles[op];
            if (!branched) {
                PC += 2;
            } else if (timing) [[unlikely]] {
                TimeBranch(pc, PC);
            }
        }
    }
    // (an event the run reached is not left for the next one: a state kept now has it done)
    if (cycles >= eventAt) [[unlikely]] {
        eventAt = ~0ull;
        bus->Event();
    }
}

// ---------------------------------------------------------------------------------------------------------
// The poll loop's fast-forward (sh3.h's PollLoop)
#ifndef XBOX_FAST_CPU

namespace {
    // The model's fields of a time: each either the same at the end of every turn, and past (it is not read), or later
    // by the turn's cycles. The rest must stay the same
    constexpr int kPollTimes = 6;
    uint64 &PollTime(Sh3Timing &m, int i) {
        switch (i) {
        case 0: return m.sdramFree;
        case 1: return m.postedDone;
        case 2: return m.otherFree;
        case 3: return m.lastTime;
        case 4: return m.nextDecode;
        default: return m.mmEnd;
        }
    }
    bool PollSame(const Sh3Timing &a, const Sh3Timing &b) {
        return a.bcr2 == b.bcr2 && a.wcr1 == b.wcr1 && a.wcr2 == b.wcr2 && a.mcr == b.mcr && a.ccr == b.ccr && a.lastInsn == b.lastInsn &&
               a.from == b.from && a.lastFetchLine == b.lastFetchLine && a.lastLoad == b.lastLoad && a.nextLoad == b.nextLoad &&
               a.inSlot == b.inSlot && a.own == b.own && a.portMask == b.portMask && std::memcmp(a.ways, b.ways, sizeof a.ways) == 0 &&
               std::memcmp(a.order, b.order, sizeof a.order) == 0;
    }
    bool IsTcnt(uint32 a) {
        return a == 0xFFFFFE98u || a == 0xFFFFFEA4u || a == 0xFFFFFEB0u;
    }
    // (what the loop may read: a timer's count, worked out for each turn's read, or a word of main RAM, which nothing
    // writes while the loop turns: the CPU writes nothing in it, an interrupt or the board's event ends it first)
    bool PollReadable(uint32 a) {
        return IsTcnt(a) || (SH3::IsRam(a) && (a & 3) == 0);
    }
} // namespace

void SH3::PollTurn(uint32 target) {
    if (mode != Mode::Native || !pollSkip || pollInside || !timing) {
        return;
    }
    const uint64 now = cycles;
    if (poll.head != target) {
        // (a branch back two instructions: is this the loop?)
        PollForget();
        const uint16 op0 = TimingCode(target), op1 = TimingCode(target + 2), op2 = TimingCode(target + 4), op3 = TimingCode(target + 6);
        if ((op0 & 0xF00F) != 0x3008 || (op1 & 0xF00F) != 0x3006 || op2 != 0x8DFC || (op3 & 0xF00F) != 0x6002) {
            return;
        }
        poll.n0 = (op0 >> 8) & 15;
        poll.m0 = (op0 >> 4) & 15;
        poll.n1 = (op1 >> 8) & 15;
        poll.m1 = (op1 >> 4) & 15;
        poll.n3 = (op3 >> 8) & 15;
        poll.m3 = (op3 >> 4) & 15;
        if (poll.m3 == poll.n0 || poll.m3 == poll.n3 || poll.n3 != poll.n0 || !PollReadable(R[poll.m3])) {
            return;
        }
        poll.head = target;
        poll.slot = target + 6;
        poll.end = now;
        poll.before = memoryTiming;
        poll.turns = 0;
        return;
    }
    // (a turn: it began where the last ended, and its read was made within it)
    if (poll.read <= poll.end || poll.read > now || !PollReadable(R[poll.m3])) {
        poll.end = now;
        poll.before = memoryTiming;
        poll.turns = 0;
        return;
    }
    const uint64 cost = now - poll.end, readAt = poll.read - poll.end;
    uint32 moved = 0;
    bool steady = PollSame(memoryTiming, poll.before) && (memoryTiming.portBase == poll.before.portBase + cost ||
                                                          (memoryTiming.portBase == poll.before.portBase && memoryTiming.portBase + 64 <= poll.end));
    for (int i = 0; i < kPollTimes && steady; i++) {
        const uint64 was = PollTime(poll.before, i), is = PollTime(memoryTiming, i);
        if (is == was + cost) {
            moved |= 1u << i;
        } else if (!(is == was && is <= poll.end)) {
            steady = false;
        }
    }
    if (memoryTiming.portBase != poll.before.portBase) {
        moved |= 1u << kPollTimes;
    }
    const bool same = steady && poll.turns > 0 && cost == poll.cost && readAt == poll.readAt && moved == poll.moved;
    poll.turns = steady ? poll.turns + 1 : 0;
    poll.cost = cost;
    poll.readAt = readAt;
    poll.moved = moved;
    poll.end = now;
    poll.before = memoryTiming;
    if (!same) {
        return;
    }

    // Two turns alike: the turns after them worked out, each as the last, while the loop goes on and the block would
    // run another (the translated block's own test: under its stop, no interrupt ready)
#ifdef SPELUNK_SHADOW_TIMING
    const std::array<uint32, 16> keepR = R;
    const uint32 keepT = T, keepPC = PC;
    const uint64 keepCycles = cycles;
    Timer keepTmu[3];
    std::memcpy(keepTmu, tmu, sizeof tmu);
    const Sh3Timing keepModel = memoryTiming;
#endif
    uint64 turns = 0, lastRead = 0;
    while (cycles < blockStop && !interruptReady) {
        const uint32 r0 = R[poll.n0] - R[poll.m0];
        const uint32 rn = poll.n1 == poll.n0 ? r0 : R[poll.n1], rm = poll.m1 == poll.n0 ? r0 : R[poll.m1];
        if (!(rn > rm)) {
            break; // (the loop ends this turn: the block runs it)
        }
        // (the word as it reads then: main RAM's, or TCNT with the timers brought up to that cycle on a copy, as
        // ReadSlow would)
        const uint64 readCycle = cycles + poll.readAt;
        const uint32 address = R[poll.m3];
        uint32 value;
        if (IsRam(address)) {
            std::memcpy(&value, ram + (address & (kRamSize - 4)), 4);
            value = __builtin_bswap32(value);
        } else {
            Timer was[3];
            std::memcpy(was, tmu, sizeof tmu);
            const uint64 nowWas = cycles;
            cycles = readCycle;
            value = ReadSlow(address & ~3u, 4);
            std::memcpy(tmu, was, sizeof tmu);
            cycles = nowWas;
        }
        R[poll.n0] = r0;
        R[poll.n3] = value;
        T = 1;
        cycles += poll.cost;
        for (int i = 0; i < kPollTimes; i++) {
            if (poll.moved & (1u << i)) {
                PollTime(memoryTiming, i) += poll.cost;
            }
        }
        if (poll.moved & (1u << kPollTimes)) {
            memoryTiming.portBase += poll.cost;
        }
        lastRead = readCycle;
        turns++;
    }
    if (!turns) {
        return;
    }
    // (the timers as the last turn's read left them)
    if (!IsRam(R[poll.m3])) {
        const uint64 endCycles = cycles;
        cycles = lastRead;
        SyncTimers();
        cycles = endCycles;
    }
    at = poll.slot;
    inSlot = false;
    pollSkips++;
    pollTurnsSkipped += turns;
    poll.end = cycles;
    poll.before = memoryTiming;
#ifdef SPELUNK_SHADOW_TIMING
    // The same turns the slow way, from where they began (the interpreter, turn by turn, both models at every event),
    // and the two compared: the skip's state must be the turns' to the last field. The run goes on from the slow way's
    const std::array<uint32, 16> fastR = R;
    const uint32 fastT = T;
    const uint64 fastCycles = cycles;
    Timer fastTmu[3];
    std::memcpy(fastTmu, tmu, sizeof tmu);
    const Sh3Timing fastModel = memoryTiming;
    R = keepR;
    T = keepT;
    cycles = keepCycles;
    std::memcpy(tmu, keepTmu, sizeof tmu);
    memoryTiming = keepModel;
    pollInside = true;
    for (uint64 i = 0; i < turns; i++) {
        PC = poll.head;
        InterpretBlock();
    }
    pollInside = false;
    std::string diff;
    char line[200];
    for (int r = 0; r < 16; r++) {
        if (R[r] != fastR[r]) {
            snprintf(line, sizeof line, "  R%d: skipped %08X, run %08X\n", r, fastR[r], R[r]);
            diff += line;
        }
    }
    if (T != fastT || cycles != fastCycles || PC != poll.head) {
        snprintf(line, sizeof line, "  T %u / %u, cycles %llu / %llu, pc %08X\n", fastT, T, static_cast<unsigned long long>(fastCycles),
                 static_cast<unsigned long long>(cycles), PC);
        diff += line;
    }
    if (std::memcmp(tmu, fastTmu, sizeof tmu) != 0) {
        diff += "  the timers differ\n";
    }
    if (!PollSame(memoryTiming, fastModel) || memoryTiming.portBase != fastModel.portBase) {
        diff += "  the model's fields differ\n";
    }
    for (int i = 0; i < kPollTimes; i++) {
        if (PollTime(memoryTiming, i) != PollTime(const_cast<Sh3Timing &>(fastModel), i)) {
            snprintf(line, sizeof line, "  the model's time %d: skipped %llu, run %llu\n", i,
                     static_cast<unsigned long long>(PollTime(const_cast<Sh3Timing &>(fastModel), i)),
                     static_cast<unsigned long long>(PollTime(memoryTiming, i)));
            diff += line;
        }
    }
    shadowSkips++;
    shadowSkipTurns += turns;
    if (!diff.empty()) {
        fprintf(stderr, "shadow: MISMATCH at a poll loop's skip of %llu turns at %08X, cycle %llu:\n%s", static_cast<unsigned long long>(turns),
                poll.head, static_cast<unsigned long long>(keepCycles), diff.c_str());
        fflush(stderr);
        std::_Exit(3);
    }
    PC = keepPC;
    poll.end = cycles;
    poll.before = memoryTiming;
#endif
}
#endif // !XBOX_FAST_CPU

// ---------------------------------------------------------------------------------------------------------
// Timers: TCNT counts down at the peripheral clock (a quarter of the CPU's) divided by 4, 16, 64 or 256

namespace {
    // Peripheral clock = CPU/4; dividers = 4, 16, 64, 256 -> shifts 4, 6, 8, 10 (eliminates 64-bit __udivdi3).
    constexpr uint8 kTimerShift[8] = {4, 6, 8, 10, 10, 10, 10, 10};
    inline uint8 TimerShift(uint16 tcr) {
        return kTimerShift[tcr & 7];
    }
} // namespace

void SH3::SyncTimer(int i) {
    Timer &t = tmu[i];
    if (!(TSTR & (1 << i))) {
        t.base = cycles;
        return;
    }
    const uint8 shift = TimerShift(t.TCR);
    uint64 counts = (cycles - t.base) >> shift;
    if (!counts) {
        return;
    }
    t.base += counts << shift;
    if (counts <= t.TCNT) {
        t.TCNT -= static_cast<uint32>(counts);
        return;
    }
    counts -= static_cast<uint64>(t.TCNT) + 1;
    t.TCR |= 0x0100; // UNF
    const uint64 reload = static_cast<uint64>(t.TCOR) + 1;
    t.TCNT = t.TCOR - static_cast<uint32>(counts % reload);
}

void SH3::SyncTimers() {
    SyncTimer(0);
    SyncTimer(1);
    SyncTimer(2);
}

uint64 SH3::NextTimerEvent() const {
    uint64 next = UINT64_MAX;
    for (int i = 0; i < 3; i++) {
        const Timer &t = tmu[i];
        if ((TSTR & (1 << i)) && (t.TCR & 0x0020) && !(t.TCR & 0x0100)) {
            const uint64 at = t.base + ((static_cast<uint64>(t.TCNT) + 1) << TimerShift(t.TCR));
            next = std::min(next, at);
        }
    }
    return next;
}

#ifdef XBOX_FAST_CPU
// Exact O(1) fast-forward for Mushihimesama's 4-instruction TMU poll loop at 0x0C04A2AA:
//   sub R1, R0; cmp/hi R0, R4; bt/s 0x0C04A2AA; mov.l @R2, R0
// Skips conservatively to within 1 timer tick of loop exit, underflow, or blockStop, then lets
// the exact 4-instruction loop finish the final few iterations.
bool SH3::FastForwardTmuPoll() {
    if (interruptReady || cycles + 4 >= blockStop) {
        return false;
    }
    const uint32 addr = R[2];
    // Case 1: Mushihimesama's main VSync idle loop polls a RAM counter (e.g. 0x0C0024D8).
    // While spinning with !interruptReady, RAM cannot change until blockStop (IRQ2 VSync / timer / blitter IRQ1).
    if (IsRam(addr)) {
        const uint32 diff0 = R[0] - R[1];
        if (!(R[4] > diff0)) {
            return false;
        }
        const uint32 memVal = Read32(addr);
        if (!(R[4] > (memVal - R[1]))) {
            return false;
        }
        // Advance exact multiple of 4-cycle iterations to reach blockStop.
        const uint64 iters = (blockStop - cycles + 3ull) >> 2;
        if (iters < 2) {
            return false;
        }
        cycles += iters << 2;
        R[0] = memVal;
        T = 1;
        PC = 0x0C04A2AAu;
        return true;
    }

    // Case 2: TMU counter poll (0xFFFFFE98 / A4 / B0).
    int idx = -1;
    if (addr == 0xFFFFFE98u) idx = 0;
    else if (addr == 0xFFFFFEA4u) idx = 1;
    else if (addr == 0xFFFFFEB0u) idx = 2;
    else return false;

    if (!(TSTR & (1 << idx)) || cycles + 8 >= blockStop) {
        return false;
    }
    // Check if the current iteration will take the back-edge
    const uint32 diff0 = R[0] - R[1];
    if (!(R[4] > diff0)) {
        return false;
    }
    SyncTimer(idx);
    Timer &t = tmu[idx];
    const uint8 shift = TimerShift(t.TCR);
    const uint64 period = 1ull << shift;
    const uint32 curDiff = t.TCNT - R[1];
    if (!(R[4] > curDiff)) {
        return false;
    }
    // TCNT counts down by 1 every `period` cycles; curDiff decreases by 1 per tick until it hits 0 and wraps.
    // Stay strictly before: (1) curDiff reaching 0 (exit), (2) TCNT underflowing, and (3) blockStop.
    const uint32 safeTicks = std::min(curDiff, t.TCNT);
    if (safeTicks <= 1) {
        return false;
    }
    const uint64 maxCyclesByTicks = static_cast<uint64>(safeTicks - 1) << shift;
    const uint64 maxCyclesByStop = blockStop - cycles - 4;
    const uint64 availCycles = std::min(maxCyclesByTicks, maxCyclesByStop);
    const uint64 skipIters = availCycles >> 2;
    if (skipIters < 2) {
        return false;
    }
    // Advance `skipIters` full 4-cycle iterations; R[0] takes TCNT at the last skipped iteration's delay slot (+2).
    const uint64 lastReadCycle = cycles + ((skipIters - 1) << 2) + 2;
    cycles += skipIters << 2;
    const uint64 ticksAtRead = (lastReadCycle > t.base) ? ((lastReadCycle - t.base) >> shift) : 0;
    t.base += ticksAtRead << shift;
    t.TCNT -= static_cast<uint32>(ticksAtRead);
    R[0] = t.TCNT;
    T = 1;
    PC = 0x0C04A2AAu;
    return true;
}

// Exact O(1) fast-forward for pure delay spin loops (nop; dt R9; bf/s loop; nop) at 0x0C056756 / 0x0C056768.
bool SH3::FastForwardDelayLoop(uint32 loopPc) {
    if (R[9] <= 2 || interruptReady || cycles + 8 >= blockStop) {
        return false;
    }
    const uint64 maxSkip = (blockStop - cycles - 4) >> 2;
    const uint64 skip = std::min<uint64>(R[9] - 2, maxSkip);
    if (skip < 2) {
        return false;
    }
    R[9] -= static_cast<uint32>(skip);
    cycles += skip << 2;
    T = 0;
    PC = loopPc;
    return true;
}
#endif

// ---------------------------------------------------------------------------------------------------------
// Memory outside main RAM

uint32 SH3::ReadSlow(uint32 a, unsigned size) {
    if (a >= 0xE0000000u) {
        if (a >= 0xFFFFFD00u) {
            return ReadRegister(a, size);
        }
        return 0; // (the cache's address and data arrays)
    }
    a &= 0x1FFFFFFFu;
    if ((a & 0x1C000000u) == 0x04000000u) {
        return ReadRegister(a | 0xA0000000u, size);
    }
    return bus->Read(a, size);
}

void SH3::WriteSlow(uint32 a, uint32 v, unsigned size) {
    if (a >= 0xE0000000u) {
        if (a >= 0xFFFFFD00u) {
            WriteRegister(a, v, size);
        } else if ((a & 0xFF000000u) == 0xF0000000u) {
#ifndef XBOX_FAST_CPU
            // (the cache's address array: a dirty line is written back)
            const uint32 cost = memoryTiming.ArrayWrite(a & 0x00FFFFFFu, v, cycles);
#ifdef SPELUNK_SHADOW_TIMING
            ShadowEvent('A', at, a, 4, cost, refTiming.ArrayWrite(a & 0x00FFFFFFu, v, cycles));
            ShadowCheckWhole("cache array write");
#endif
            if (timing) {
                cycles += cost;
            }
#endif
        }
        return;
    }
    a &= 0x1FFFFFFFu;
    if ((a & 0x1C000000u) == 0x04000000u) {
        WriteRegister(a | 0xA0000000u, v, size);
        return;
    }
    bus->Write(a, v, size);
}

// ---------------------------------------------------------------------------------------------------------
// On-chip registers (SH7709S: 0xFFFFFExx up, and 0xA4000000 up)

uint32 SH3::ReadRegister(uint32 a, unsigned size) {
    (void)size;
    // (the ports' data registers: PADR at 0xA4000120, two apart, to PLDR at 0xA4000134)
    if (a >= 0xA4000120u && a <= 0xA4000135u) {
        return bus->PortRead((a - 0xA4000120u) >> 1);
    }
    switch (a) {
    case 0xFFFFFFD0: return TRA;
    case 0xFFFFFFD4: return EXPEVT;
    case 0xFFFFFFD8: return INTEVT;
    case 0xFFFFFFE0: return MMUCR;
    case 0xFFFFFFEC: return CCR;
    case 0xFFFFFFF0: return PTEH;
    case 0xFFFFFFF4: return PTEL;
    case 0xFFFFFFF8: return TTB;
    case 0xFFFFFFFC: return TEA;
    case 0xFFFFFEE0: return ICR0;
    case 0xFFFFFEE2: return IPRA;
    case 0xFFFFFEE4: return IPRB;
    case 0xFFFFFE90: return TOCR;
    case 0xFFFFFE92: return TSTR;
    case 0xFFFFFE94: case 0xFFFFFEA0: case 0xFFFFFEAC: return tmu[(a - 0xFFFFFE94) / 12].TCOR;
    case 0xFFFFFE98: case 0xFFFFFEA4: case 0xFFFFFEB0: {
        const int idx = (a == 0xFFFFFE98u) ? 0 : (a == 0xFFFFFEA4u) ? 1 : 2;
        SyncTimer(idx);
        return tmu[idx].TCNT;
    }
    case 0xFFFFFE9C: case 0xFFFFFEA8: case 0xFFFFFEB4: {
        const int idx = (a == 0xFFFFFE9Cu) ? 0 : (a == 0xFFFFFEA8u) ? 1 : 2;
        SyncTimer(idx);
        return tmu[idx].TCR;
    }
    case 0xA4000020: case 0xA4000030: case 0xA4000040: case 0xA4000050: return SAR[((a >> 4) & 7) - 2];
    case 0xA4000024: case 0xA4000034: case 0xA4000044: case 0xA4000054: return DAR[((a >> 4) & 7) - 2];
    case 0xA4000028: case 0xA4000038: case 0xA4000048: case 0xA4000058: return DMATCR[((a >> 4) & 7) - 2];
    case 0xA400002C: case 0xA400003C: case 0xA400004C: case 0xA400005C: return CHCR[((a >> 4) & 7) - 2];
    case 0xA4000060: return DMAOR;
    case 0xA4000000: return INTEVT2;
    case 0xA4000004: return irqLines & 0x3F; // IRR0: the IRQ pins waiting
    case 0xA4000010: return ICR1;
    case 0xA4000012: return ICR2;
    case 0xA4000016: return IPRC;
    case 0xA4000018: return IPRD;
    case 0xA400001A: return IPRE;
    default: break;
    }
    if (getenv("SPELUNK_TRACE_ONCHIP")) {
        fprintf(stderr, "sh3: on-chip read %08X (pc %08X)\n", a, PC);
    }
    return misc[(a ^ (a >> 7)) & 63];
}

void SH3::WriteRegister(uint32 a, uint32 v, unsigned size) {
    (void)size;
    if (a >= 0xA4000120u && a <= 0xA4000135u) {
        const unsigned port = (a - 0xA4000120u) >> 1;
        portOut[port] = static_cast<uint8>(v);
        bus->PortWrite(port, static_cast<uint8>(v));
        return;
    }
    switch (a) {
    case 0xFFFFFFD0: TRA = v; return;
    case 0xFFFFFFD4: EXPEVT = v; return;
    case 0xFFFFFFD8: INTEVT = v; return;
    case 0xFFFFFFE0: MMUCR = v; return;
    case 0xFFFFFFEC:
        // (the CF bit throws the cache away; it is not kept)
#ifndef XBOX_FAST_CPU
        if (v & 8) {
            if (timing) {
                cycles += Sh3Timing::kFlushCost;
            }
            memoryTiming.Flush();
#ifdef SPELUNK_SHADOW_TIMING
            refTiming.Flush();
#endif
        }
        CCR = v & ~8u;
        memoryTiming.ccr = CCR;
#ifdef SPELUNK_SHADOW_TIMING
        refTiming.ccr = CCR;
        ShadowCheckWhole("CCR");
#endif
#else
        CCR = v & ~8u;
#endif
        return;
#ifndef XBOX_FAST_CPU
    // The bus state controller: what the bus timing goes by
    case 0xFFFFFF62: memoryTiming.bcr2 = static_cast<uint16>(v); SHADOW_TIMING_ALSO(refTiming.bcr2 = static_cast<uint16>(v)); return;
    case 0xFFFFFF64: memoryTiming.wcr1 = static_cast<uint16>(v); SHADOW_TIMING_ALSO(refTiming.wcr1 = static_cast<uint16>(v)); return;
    case 0xFFFFFF66: memoryTiming.wcr2 = static_cast<uint16>(v); SHADOW_TIMING_ALSO(refTiming.wcr2 = static_cast<uint16>(v)); return;
    case 0xFFFFFF68: memoryTiming.mcr = static_cast<uint16>(v); SHADOW_TIMING_ALSO(refTiming.mcr = static_cast<uint16>(v)); return;
#endif
    case 0xFFFFFFF0: PTEH = v; return;
    case 0xFFFFFFF4: PTEL = v; return;
    case 0xFFFFFFF8: TTB = v; return;
    case 0xFFFFFFFC: TEA = v; return;
    case 0xFFFFFEE0: ICR0 = static_cast<uint16>(v); return;
    case 0xFFFFFEE2: IPRA = static_cast<uint16>(v); RecalcInterrupts(); return;
    case 0xFFFFFEE4: IPRB = static_cast<uint16>(v); RecalcInterrupts(); return;
    case 0xFFFFFE90: TOCR = static_cast<uint8>(v); return;
    case 0xFFFFFE92:
        SyncTimers();
        TSTR = static_cast<uint8>(v);
        for (auto &t : tmu) {
            t.base = cycles;
        }
        return;
    case 0xFFFFFE94: case 0xFFFFFEA0: case 0xFFFFFEAC: tmu[(a - 0xFFFFFE94) / 12].TCOR = v; return;
    case 0xFFFFFE98: case 0xFFFFFEA4: case 0xFFFFFEB0:
        SyncTimers();
        tmu[(a - 0xFFFFFE98) / 12].TCNT = v;
        return;
    case 0xFFFFFE9C: case 0xFFFFFEA8: case 0xFFFFFEB4: {
        SyncTimers();
        Timer &t = tmu[(a - 0xFFFFFE9C) / 12];
        // (UNF is cleared by writing 0 to it, and cannot be set by a write)
        t.TCR = static_cast<uint16>((v & 0x02FF & ~0x0100u) | (t.TCR & v & 0x0100));
        RecalcInterrupts();
        return;
    }
    case 0xA4000020: case 0xA4000030: case 0xA4000040: case 0xA4000050: SAR[((a >> 4) & 7) - 2] = v; return;
    case 0xA4000024: case 0xA4000034: case 0xA4000044: case 0xA4000054: DAR[((a >> 4) & 7) - 2] = v; return;
    case 0xA4000028: case 0xA4000038: case 0xA4000048: case 0xA4000058: DMATCR[((a >> 4) & 7) - 2] = v & 0x00FFFFFF; return;
    case 0xA400002C: case 0xA400003C: case 0xA400004C: case 0xA400005C: {
        const unsigned channel = ((a >> 4) & 7) - 2;
        // (the transfer-ended flag is cleared by writing 0 to it, and cannot be set by a write)
        CHCR[channel] = (v & ~2u) | (CHCR[channel] & v & 2);
        RunDma(channel);
        RecalcInterrupts();
        return;
    }
    case 0xA4000060:
        DMAOR = v;
        for (unsigned channel = 0; channel < 4; channel++) {
            RunDma(channel);
        }
        RecalcInterrupts();
        return;
    case 0xA4000000: INTEVT2 = v; return;
    case 0xA4000004: // IRR0: a waiting IRQ is cleared by writing 0 to its bit
        irqLines &= v | ~0x3Fu;
        RecalcInterrupts();
        return;
    case 0xA4000010: ICR1 = static_cast<uint16>(v); return;
    case 0xA4000012: ICR2 = static_cast<uint16>(v); return;
    case 0xA4000016: IPRC = static_cast<uint16>(v); RecalcInterrupts(); return;
    case 0xA4000018: IPRD = static_cast<uint16>(v); RecalcInterrupts(); return;
    case 0xA400001A: IPRE = static_cast<uint16>(v); RecalcInterrupts(); return;
    default: break;
    }
    if (getenv("SPELUNK_TRACE_ONCHIP")) {
        fprintf(stderr, "sh3: on-chip write %08X = %X (pc %08X)\n", a, v, PC);
    }
    misc[(a ^ (a >> 7)) & 63] = v;
}

// A DMA channel that is enabled and asked to go by itself (auto request) runs its whole transfer at once: the games
// wait for its end flag. The time it takes is the bus's: a byte from the NAND flash is five bus cycles, ten of the
// CPU's (MAME's cv1k.cpp notes the figure; the games' loading stalls depend on it). With the board's slowdown on, each
// transfer is the read and the write the controller makes on the bus instead (bus_timing.h).
void SH3::RunDma(unsigned channel) {
    uint32 &chcr = CHCR[channel];
    if (!(DMAOR & 1) || (chcr & 3) != 1 || ((chcr >> 8) & 15) != 4) {
        return;
    }
    const unsigned size = 1u << ((chcr >> 3) & 3); // (1, 2 or 4 bytes a transfer)
    const int sourceStep = ((chcr >> 12) & 3) == 1 ? size : ((chcr >> 12) & 3) == 2 ? -static_cast<int>(size) : 0;
    const int destStep = ((chcr >> 14) & 3) == 1 ? size : ((chcr >> 14) & 3) == 2 ? -static_cast<int>(size) : 0;
    uint32 count = DMATCR[channel] ? DMATCR[channel] : 0x01000000u;
#ifdef XBOX_FAST_CPU
    cycles += static_cast<uint64>(count) * 10;
    // Fast path: bulk NAND-port-to-RAM DMA (replaces 2,048 virtual Read8+Write8 calls per NAND page with one memcpy).
    if (size == 1 && sourceStep == 0 && destStep == 1 && IsRam(DAR[channel])) {
        const uint32 dstPhys = DAR[channel] & (kRamSize - 1);
        const uint32 srcPhys = SAR[channel] & 0x1FFFFFFFu;
        if (dstPhys + count <= kRamSize && !IsRam(SAR[channel]) && SAR[channel] < 0xE0000000u &&
            (srcPhys & 0x1C000000u) != 0x04000000u && bus->ReadBulk(srcPhys, ram + dstPhys, count)) {
            if (dstPhys < codeAddrEnd) [[unlikely]] {
                NoteRamWrite(DAR[channel], count);
            }
            DAR[channel] += count;
            DMATCR[channel] = 0;
            chcr |= 2; // TE
            return;
        }
    }
    for (; count; count--) {
        switch (size) {
        case 1: Write8(DAR[channel], Read8(SAR[channel])); break;
        case 2: Write16(DAR[channel], Read16(SAR[channel])); break;
        default: Write32(DAR[channel], Read32(SAR[channel])); break;
        }
        SAR[channel] += sourceStep;
        DAR[channel] += destStep;
    }
#else
    const bool timed = timing;
    timing = false; // (the transfers are not the CPU's own accesses)
    if (!timed) {
        timingPaths.Forget(); // (what they write may be code: with the model on, each write is noted below)
        SHADOW_TIMING_ALSO(refPaths.Forget());
        cycles += static_cast<uint64>(count) * 10;
    }
    for (; count; count--) {
        if (timed) {
            cycles += memoryTiming.DmaTransfer(SAR[channel], DAR[channel], size);
#ifdef SPELUNK_SHADOW_TIMING
            if (memoryTiming.DmaTransfer(SAR[channel], DAR[channel], size) != refTiming.DmaTransfer(SAR[channel], DAR[channel], size)) {
                ShadowFail("DMA transfer");
            }
#endif
        }
        switch (size) {
        case 1: Write8(DAR[channel], Read8(SAR[channel])); break;
        case 2: Write16(DAR[channel], Read16(SAR[channel])); break;
        default: Write32(DAR[channel], Read32(SAR[channel])); break;
        }
        if (timed) {
            timingPaths.Written(DAR[channel], size); // (a path kept over it is found again)
            SHADOW_TIMING_ALSO(refPaths.Written(DAR[channel], size));
        }
        SAR[channel] += sourceStep;
        DAR[channel] += destStep;
    }
    timing = timed;
#endif
    DMATCR[channel] = 0;
    chcr |= 2; // TE
}

void SH3::Serialize(Archive &ar) {
    ar.Mark(0x53483320);
    ar.Pod(R);
    ar.Pod(bank);
    ar.Pod(PC);
    ar.Pod(PR);
    ar.Pod(GBR);
    ar.Pod(VBR);
    ar.Pod(SSR);
    ar.Pod(SPC);
    ar.Pod(MACH);
    ar.Pod(MACL);
    ar.Pod(sr);
    ar.Pod(T);
    ar.Pod(cycles);
    ar.Pod(sleeping);
    ar.Pod(irqLines);
    ar.Pod(ICR0);
    ar.Pod(ICR1);
    ar.Pod(ICR2);
    ar.Pod(IPRA);
    ar.Pod(IPRB);
    ar.Pod(IPRC);
    ar.Pod(IPRD);
    ar.Pod(IPRE);
    ar.Pod(INTEVT);
    ar.Pod(INTEVT2);
    ar.Pod(EXPEVT);
    ar.Pod(TRA);
    ar.Pod(tmu);
    ar.Pod(TOCR);
    ar.Pod(TSTR);
    ar.Pod(CCR);
#ifndef XBOX_FAST_CPU
    ar.Pod(memoryTiming);
    if (!ar.Saving()) {
        if (!keepPaths) {
            timingPaths.Forget(); // (found over the code that was in RAM)
        }
        PollForget();
        SHADOW_TIMING_ALSO(ShadowSync());
    }
#endif
    ar.Pod(MMUCR);
    ar.Pod(PTEH);
    ar.Pod(PTEL);
    ar.Pod(TTB);
    ar.Pod(TEA);
    ar.Pod(SAR);
    ar.Pod(DAR);
    ar.Pod(DMATCR);
    ar.Pod(CHCR);
    ar.Pod(DMAOR);
    ar.Pod(portOut);
    ar.Pod(misc);
    if (!ar.Saving()) {
        // (every page of translated code is compared again; the interrupt state is worked out again)
        if (!keepEpochs) {
            for (uint32 &e : pageEpoch) {
                e++;
            }
        }
        RecalcInterrupts();
    }
}

void SH3::Describe(FILE *out) const {
    for (int i = 0; i < 16; i++) {
        fprintf(out, "sh3.R%d %08X\n", i, R[i]);
    }
    for (int i = 0; i < 8; i++) {
        fprintf(out, "sh3.bank%d %08X\n", i, bank[i]);
    }
    fprintf(out, "sh3.PC %08X\nsh3.PR %08X\nsh3.GBR %08X\nsh3.VBR %08X\nsh3.SSR %08X\nsh3.SPC %08X\n", PC, PR, GBR, VBR, SSR, SPC);
    fprintf(out, "sh3.MACH %08X\nsh3.MACL %08X\nsh3.SR %08X\nsh3.cycles %llu\n", MACH, MACL, GetSR(), static_cast<unsigned long long>(cycles));
    fprintf(out, "sh3.IPRA %04X\nsh3.IPRB %04X\nsh3.IPRC %04X\nsh3.IPRD %04X\nsh3.INTEVT %08X\nsh3.INTEVT2 %08X\nsh3.EXPEVT %08X\n", IPRA, IPRB,
            IPRC, IPRD, INTEVT, INTEVT2, EXPEVT);
    for (int i = 0; i < 3; i++) {
        fprintf(out, "sh3.TMU%d.TCOR %08X\nsh3.TMU%d.TCR %04X\n", i, tmu[i].TCOR, i, tmu[i].TCR);
    }
}

#ifdef SPELUNK_SHADOW_TIMING
void SH3::ShadowFail(const char *where) {
    fprintf(stderr, "shadow: MISMATCH at %s: cycle %llu, pc %08X, event %llu\n", where, static_cast<unsigned long long>(cycles), PC,
            static_cast<unsigned long long>(shadowEvents));
    const uint64 first = shadowEvents > 64 ? shadowEvents - 64 : 0;
    for (uint64 i = first; i < shadowEvents; i++) {
        const ShadowRecord &r = shadowLog[i & 63];
        fprintf(stderr, "shadow:   #%llu %c insn %08X addr %08X bytes %u at cycle %llu: %u, reference %u\n",
                static_cast<unsigned long long>(i), r.kind, r.insn, r.address, r.bytes, static_cast<unsigned long long>(r.cycles), r.got, r.want);
    }
    fprintf(stderr, "shadow: the fields that differ (in use, reference):\n%s", TimingDiff(memoryTiming, refTiming).c_str());
    fflush(stderr);
    std::_Exit(3);
}
#endif

} // namespace spelunk
