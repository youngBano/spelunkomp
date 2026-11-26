// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp; from cps2recomp: https://github.com/tmcleroy/cps2recomp

#include "m68k.h"

#include "../runtime/native68k.h"
#include "../runtime/state.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <algorithm>
#include <utility>

namespace spelunk {

namespace {
    int32_t Extend(uint32_t value, unsigned size) {
        return size == 0 ? static_cast<int8_t>(value) : size == 1 ? static_cast<int16_t>(value) : static_cast<int32_t>(value);
    }
    // What working out an operand's address and reading it costs: [long word][mode], mode 7 by register after
    constexpr uint8_t kOperandCycles[2][12] = {{0, 0, 4, 4, 6, 8, 10, 8, 12, 8, 10, 4}, {0, 0, 8, 8, 10, 12, 14, 12, 16, 12, 14, 8}};
    unsigned ModeIndex(unsigned mode, unsigned reg) {
        return mode < 7 ? mode : 7 + reg;
    }
} // namespace

void M68K::Reset(bool hard) {
    if (hard) {
        for (int i = 0; i < 8; i++) {
            d[i] = a[i] = 0;
        }
        m_interruptLevel = 0;
        m_irqFrom = 0;
    }
    m_lastTail = 4;
    a[7] = ReadProgram(0, 2); // (the reset vector is read as program)
    otherSP = 0;
    pc = ReadProgram(4, 2);
    flagC = flagV = flagZ = flagN = flagX = false;
    supervisor = true;
    trace = false;
    interruptMask = 7;
    stopped = false;
    depth = 0;
}

// --- memory ---

uint32_t M68K::Read(uint32_t address, unsigned size) {
    accessInto = 4u * ((fetchedWords ? fetchedWords - 1 : 0) + m_readCycles + m_writeCycles);
    m_readCycles += size == 2 ? 2 : 1;
    DataWait(address, false);
    if (size == 2) {
        accessInto += 4;
        DataWait(address + 2, false);
    }
#ifdef SPELUNK_CHECK68K
    if (check::side) {
        return native::CheckRead(*this, address, size);
    }
#endif
    switch (size) {
    case 0: return bus.Read8(address & 0xFFFFFF);
    case 1: return bus.Read16(address & 0xFFFFFE);
    default: {
        const uint32_t high = bus.Read16(address & 0xFFFFFE);
        return (high << 16) | bus.Read16((address + 2) & 0xFFFFFE);
    }
    }
}
void M68K::Write(uint32_t address, unsigned size, uint32_t value) {
    accessInto = 4u * ((fetchedWords ? fetchedWords - 1 : 0) + m_readCycles + m_writeCycles);
    // (an instruction that reads its operand and writes it back, ADDQ to memory and the like, fetches the next word
    // between the two: its write comes 4 cycles later. A MOVE writes straight after its read)
    if (m_readCycles && !m_moving) {
        accessInto += 4;
    }
    m_writeCycles += size == 2 ? 2 : 1;
    DataWait(address, true);
    if (size == 2) {
        accessInto += 4;
        DataWait(address + 2, true);
    }
#ifdef SPELUNK_CHECK68K
    if (check::side) {
        native::CheckWrite(*this, address, size, value);
        return;
    }
    if (check::noting) {
        check::Note(address, size, value);
    }
#endif
    if (noteWrites && (address & 0xFFFFFF) - ramBase < ramSize) [[unlikely]] {
        for (uint32_t at = address & 0xFFFFFE, end = (address & 0xFFFFFF) + kBytes[size]; at < end; at += 2) {
            const uint32_t i = (at - ramBase) & (ramSize - 1);
            notedWrites.push_back({at, static_cast<uint16_t>((ram[i] << 8) | ram[i + 1])});
        }
    }
    if (cycleTrace && traceWrites) { // (measurement: the writes too, with the time at the instruction's start)
        fprintf(cycleTrace, "W %06X %llu\n", address & 0xFFFFFF, static_cast<unsigned long long>(cycles));
    }
    if ((address & 0xFFFFFF) + kBytes[size] > watchLow && (address & 0xFFFFFF) <= watchHigh) {
        // (SPELUNK_WATCH_REPORTS=N for more: the board's memory test alone can take the first 2,000)
        static int reports = getenv("SPELUNK_WATCH_REPORTS") ? atoi(getenv("SPELUNK_WATCH_REPORTS")) : 2000;
        if (reports > 0) {
            reports--;
            fprintf(stderr, "watch: %06X writes %0*X to %06X (cycle %llu)\n", instruction, static_cast<int>(kBytes[size] * 2),
                    value & kMask[size], address & 0xFFFFFF, static_cast<unsigned long long>(cycles));
        }
    }
    switch (size) {
    case 0: bus.Write8(address & 0xFFFFFF, static_cast<uint8_t>(value)); break;
    case 1: bus.Write16(address & 0xFFFFFE, static_cast<uint16_t>(value)); break;
    default:
        bus.Write16(address & 0xFFFFFE, static_cast<uint16_t>(value >> 16));
        bus.Write16((address + 2) & 0xFFFFFE, static_cast<uint16_t>(value));
        break;
    }
}
uint32_t M68K::ReadProgram(uint32_t address, unsigned size) {
    m_readCycles += size == 2 ? 2 : 1;
    BusWait(address);
    if (size == 2) {
        BusWait(address + 2);
    }
    switch (size) {
    case 0: {
        const uint16_t word = bus.Fetch16(address & 0xFFFFFE);
        return (address & 1) ? (word & 0xFF) : (word >> 8);
    }
    case 1: return bus.Fetch16(address & 0xFFFFFE);
    default: {
        const uint32_t high = bus.Fetch16(address & 0xFFFFFE);
        return (high << 16) | bus.Fetch16((address + 2) & 0xFFFFFE);
    }
    }
}
uint16_t M68K::Fetch() {
    BusWait(pc); // (a word of the instruction)
    fetchedWords++;
    const uint16_t word = bus.Fetch16(pc & 0xFFFFFE);
    pc += 2;
    return word;
}
uint32_t M68K::Fetch32() {
    const uint32_t high = Fetch();
    return (high << 16) | Fetch();
}
void M68K::Push16(uint16_t value) {
    a[7] -= 2;
    Write(a[7], 1, value);
}
uint16_t M68K::Pop16() {
    const uint16_t value = static_cast<uint16_t>(Read(a[7], 1));
    a[7] += 2;
    return value;
}

// --- operands ---

// Where an instruction's operand is, from its mode and register fields (reading the extension words and
// stepping the address register where the mode says so)
// (an operand in memory: its read's bus cycle comes after the words fetched so far, M68K::readTime)
M68K::Operand M68K::Locate(unsigned mode, unsigned reg, unsigned size) {
    const Operand operand = LocateEa(mode, reg, size);
    if (operand.kind == Operand::Kind::Memory) {
        readTime = cycles + 4 * (fetchedWords - 1) + (mode == 4 || mode == 6 ? 2 : 0);
    }
    return operand;
}

M68K::Operand M68K::LocateEa(unsigned mode, unsigned reg, unsigned size) {
    using Kind = Operand::Kind;
    auto indexed = [&](uint32_t base) {
        m_early += 2; // (the index is added before the extension word is fetched)
        const uint16_t ext = Fetch();
        const uint32_t index = (ext & 0x8000) ? a[(ext >> 12) & 7] : d[(ext >> 12) & 7];
        return base + static_cast<int8_t>(ext) + ((ext & 0x0800) ? index : static_cast<uint32_t>(static_cast<int16_t>(index)));
    };
    switch (mode) {
    case 0: return {Kind::Data, reg};
    case 1: return {Kind::Address, reg};
    case 2: return {Kind::Memory, a[reg]};
    case 3: {
        const uint32_t at = a[reg];
        a[reg] += (size == 0 && reg == 7) ? 2 : kBytes[size]; // (the stack pointer stays even)
        return {Kind::Memory, at};
    }
    case 4:
        m_early += 2; // (the predecrement, before the read)
        a[reg] -= (size == 0 && reg == 7) ? 2 : kBytes[size];
        return {Kind::Memory, a[reg]};
    case 5: return {Kind::Memory, a[reg] + static_cast<int16_t>(Fetch())};
    case 6: return {Kind::Memory, indexed(a[reg])};
    default:
        switch (reg) {
        case 0: return {Kind::Memory, static_cast<uint32_t>(static_cast<int16_t>(Fetch()))};
        case 1: return {Kind::Memory, Fetch32()};
        case 2: {
            const uint32_t base = pc;
            return {Kind::Program, base + static_cast<int16_t>(Fetch())};
        }
        case 3: {
            const uint32_t base = pc;
            return {Kind::Program, indexed(base)};
        }
        default: return {Kind::Immediate, size == 2 ? Fetch32() : (Fetch() & kMask[size])};
        }
    }
}
uint32_t M68K::Load(const Operand &operand, unsigned size) {
    switch (operand.kind) {
    case Operand::Kind::Data: return d[operand.at] & kMask[size];
    case Operand::Kind::Address: return a[operand.at] & kMask[size];
    case Operand::Kind::Memory: return Read(operand.at, size);
    case Operand::Kind::Program: return ReadProgram(operand.at, size);
    default: return operand.at;
    }
}
void M68K::Store(const Operand &operand, unsigned size, uint32_t value) {
    switch (operand.kind) {
    case Operand::Kind::Data: d[operand.at] = (d[operand.at] & ~kMask[size]) | (value & kMask[size]); break;
    case Operand::Kind::Address: a[operand.at] = value; break;
    case Operand::Kind::Memory: Write(operand.at, size, value); break;
    default: break;
    }
}

// --- exceptions ---

void M68K::Exception(unsigned vector, uint32_t returnAddress, int level) {
    const uint16_t before = SR();
    if (!supervisor) {
        std::swap(a[7], otherSP);
        supervisor = true;
    }
    trace = false;
    if (level >= 0) {
        interruptMask = static_cast<uint8_t>(level);
    }
    Push32(returnAddress);
    Push16(before);
    pc = Read(vector * 4, 2);
    m_jumped = true;
}

void M68K::Illegal(uint16_t op, uint32_t address) {
    static unsigned reports = 8;
    if (reports) {
        reports--;
        fprintf(stderr, "68000: instruction %04X at %06X is not one this interpreter runs\n", op, address);
        if (keepHistory) {
            fprintf(stderr, "68000: the places before it:");
            for (unsigned n = 0; n < 64; n++) {
                fprintf(stderr, " %06X", history[(historyAt + n) & 63]);
            }
            fprintf(stderr, "\n");
        }
    }
    Exception((op >> 12) == 0xA ? 10 : (op >> 12) == 0xF ? 11 : 4, address, -1);
    m_cycles += 34;
}

std::vector<uint8_t> M68K::RamAt(uint64_t at) const {
    std::vector<uint8_t> out(ram, ram + ramSize);
    if (!(notedStart < at && notedEnd > at) || notedWrites.empty()) {
        return out;
    }
    // Each word written is a bus cycle of 4 near the instruction's end: after its last prefetch, or (a MOVE, a MOVEM)
    // before it. A long's words: a MOVE's and a MOVEM's high one first, the rest low first (a MOVEM to -(An) too, its
    // registers going down; fx68k). A word is in RAM 3 cycles into its cycle
    const uint16_t op = notedOp;
    const bool movem = (op & 0xFB80) == 0x4880;
    const bool before = ((op >> 12) >= 1 && (op >> 12) <= 3 && ((op >> 6) & 7) != 4) || movem;
    const bool highFirst = before && !(movem && ((op >> 3) & 7) == 4);
    const size_t n = notedWrites.size();
    std::vector<NotedWrite> order(notedWrites);
    if (!highFirst) {
        // (a long written as low word, then high: swap each pair the interpreter noted high first)
        for (size_t i = 0; i + 1 < n; i++) {
            if (order[i + 1].address == order[i].address + 2) {
                std::swap(order[i], order[i + 1]);
                i++;
            }
        }
    }
    const uint64_t last = notedEnd - (before ? 4 : 0); // (where the last write's cycle ends)
    for (size_t k = n; k-- > 0;) { // (last to first: a word written twice gets back what it had first)
        const uint64_t start = last - 4 * (n - k);
        if (start + 3 > at) {
            const uint32_t i = (order[k].address - ramBase) & (ramSize - 1);
            out[i] = static_cast<uint8_t>(order[k].before >> 8);
            out[i + 1] = static_cast<uint8_t>(order[k].before);
        }
    }
    return out;
}

// What a DIVU or DIVS takes (without its operand's time), by its operands: the 68000 divides a bit at a time, and
// each step's time depends on what it finds. 140 and 158 are the most (the times the port had charged). The
// rules are Jorge Cwik's (2005), measured on the 68000 and the same in fx68k's microcode.
uint32_t M68K::DivideTime(uint32_t dividend, uint16_t divisor, bool isSigned) {
    if (!isSigned) {
        if ((dividend >> 16) >= divisor) {
            return 10; // (the quotient would not fit)
        }
        unsigned cycles = 38;
        const uint32_t high = static_cast<uint32_t>(divisor) << 16;
        for (int n = 0; n < 15; n++) {
            const bool carry = dividend & 0x80000000u;
            dividend <<= 1;
            if (carry) {
                dividend -= high;
            } else {
                cycles += 2;
                if (dividend >= high) {
                    dividend -= high;
                    cycles--;
                }
            }
        }
        return cycles * 2;
    }
    const int32_t sDividend = static_cast<int32_t>(dividend);
    const int16_t sDivisor = static_cast<int16_t>(divisor);
    unsigned cycles = sDividend < 0 ? 7 : 6;
    const uint32_t magnitude = sDividend < 0 ? 0u - dividend : dividend;
    const uint32_t divisorMagnitude = static_cast<uint32_t>(sDivisor < 0 ? -sDivisor : sDivisor);
    if ((magnitude >> 16) >= divisorMagnitude) {
        return (cycles + 2) * 2; // (the quotient would not fit)
    }
    uint32_t quotient = magnitude / divisorMagnitude;
    cycles += 55;
    if (sDivisor >= 0) {
        cycles += sDividend >= 0 ? -1 : 1;
    }
    for (int n = 0; n < 15; n++) {
        if (!(quotient & 0x8000)) {
            cycles++;
        }
        quotient <<= 1;
    }
    return cycles * 2;
}

// The cycles from the point where an instruction looks at the interrupt lines to its end (SetInterruptLevel): as a
// rule the start of its last prefetch. From what it did on the bus: a branch, jump, return or trap ends with its
// prefetches; a MOVE (not to -(An)) or MOVEM writes before its last prefetch, other writes (read-modify-write, to
// -(An), PEA) come after it; otherwise the internal cycles (a long ALU operation's, a shift's, a multiply's) come after
// it, but for those an addressing mode adds before (an index, a predecrement). The forms that look elsewhere were
// found by running every instruction form of the games on fx68k alone with the line raised at every half cycle
// (docs/SLOWDOWN.md, "The 68000 against the Cave core running"): they are listed first
uint32_t M68K::InstructionTail(uint16_t op) const {
    const unsigned mode = (op >> 3) & 7, reg = op & 7;
    if (m_tail) {
        return m_tail; // (DIVU, DIVS: set by the divide)
    }
    if ((op >> 12) >= 1 && (op >> 12) <= 3 && m_writeCycles) { // MOVE to memory
        const unsigned to = (op >> 6) & 7;
        if (to == 3) {
            return 8; // (to (An)+: its last write, every size)
        }
        if (to == 4) {
            return 4 + 4u * m_writeCycles; // (to -(An): the prefetch, then the writes)
        }
        if ((op >> 12) == 2 && to != 7 && (mode < 2 || (mode == 7 && reg == 4))) {
            return 8; // (a long from a register or an immediate, but to an absolute address: its low word's write)
        }
        return 4;
    }
    if ((op & 0xFF80) == 0x4C80) { // MOVEM from memory: (An)+ before its reads, the others at the last
        return mode == 3 ? 8 + 4u * m_readCycles : 4;
    }
    if ((((op & 0xF130) == 0x9100 || (op & 0xF130) == 0xD100) && ((op >> 6) & 3) != 3) || (op & 0xB1F0) == 0x8100) {
        if (op & 0x0008) { // ABCD, SBCD, ADDX, SUBX -(An),-(An): 4 before its write
            return 8 + 4u * m_writeCycles;
        }
    }
    if ((op & 0xF138) == 0xB108 && ((op >> 6) & 3) != 3) { // CMPM: before its second operand's last word
        return 4 + 2u * m_readCycles;
    }
    if ((op & 0xF138) == 0x0108) { // MOVEP: to memory at its last write's start, from memory 4 before its end + 4
        return op & 0x0080 ? 4 : 8;
    }
    if ((op & 0xF1C0) == 0x41C0 && (mode == 6 || (mode == 7 && reg == 3))) { // LEA with an index
        return 4;
    }
    if ((op & 0xFFC0) == 0x4840 && mode >= 2) { // PEA: with an index 14, an absolute address 4
        if (mode == 6 || (mode == 7 && reg == 3)) {
            return 14;
        }
        if (mode == 7 && reg < 2) {
            return 4;
        }
    }
    if (op == 0x44FC || op == 0x003C || op == 0x023C || op == 0x0A3C || op == 0x46FC || op == 0x007C || op == 0x027C ||
        op == 0x0A7C) { // MOVE, ORI, ANDI, EORI to CCR or SR, immediate
        return 4;
    }
    if ((op & 0xFDC0) == 0x44C0) { // MOVE to CCR or SR from anything else: at its end too (its internal cycles come
        return 4;                  // before its last prefetch: fx68k, tools/fx68kbench mask.py)
    }
    if ((op & 0xFFF8) == 0x4E58) { // UNLK
        return 8;
    }
    const bool flow = (op >> 12) == 6 || (op & 0xF0F8) == 0x50C8 || (op & 0xFF80) == 0x4E80 || (op & 0xFFF0) == 0x4E70 ||
                      (op & 0xFFF0) == 0x4E40;
    if (flow) {
        return 4;
    }
    if (m_writeCycles) {
        return (op & 0xFB80) == 0x4880 ? 4 : 4 + 4u * m_writeCycles;
    }
    const int internal = static_cast<int>(m_cycles) - 4 * static_cast<int>(fetchedWords + m_readCycles) - m_early;
    return 4 + static_cast<uint32_t>(std::max(0, internal));
}

// Takes the interrupt on the lines if the mask lets it, and the last instruction's last prefetch saw it
bool M68K::TakeInterrupt() {
    if ((m_interruptLevel == 7 || m_interruptLevel > interruptMask) && (stopped || cycles >= m_irqFrom + m_lastTail)) {
        m_lastTail = 4; // (the exception ends with two prefetches)
        stopped = false;
        const uint8_t level = m_interruptLevel;
        Exception(24 + level, pc, level); // (the board asks for the autovector)
        bus.InterruptAck(level);
        // (with the board's bus waits: the acknowledge cycle, and the two words fetched at the handler)
        cycles += busWait * 3;
        waitsCounted += busWait * 3;
        return true;
    }
    return false;
}

uint32_t M68K::Step() {
    if (TakeInterrupt()) {
        return InterruptTime();
    }
    if (stopped) {
        return 4;
    }
    m_cycles = 0;
    ExecuteOne();
    return m_cycles;
}

void M68K::Interpret(uint32_t address) {
    pc = address;
    m_cycles = 0;
    ExecuteOne();
    cycles += m_cycles;
}

void M68K::Execute(uint16_t op) {
    using Kind = Operand::Kind;
    const uint32_t at = pc - 2; // the instruction's address
    instruction = at;
    if (cycleTrace) {
        fprintf(cycleTrace, "%06X %llu\n", at, static_cast<unsigned long long>(cycles + m_cycles));
    }
    const unsigned mode = (op >> 3) & 7, reg = op & 7, reg2 = (op >> 9) & 7;
    const unsigned size = (op >> 6) & 3; // (where the instruction has a size field)
    const unsigned modeIndex = ModeIndex(mode, reg);
    auto operandCycles = [&](unsigned sz) { return kOperandCycles[sz == 2][modeIndex]; };
    const bool isRegister = mode < 2;
    // (a long ALU operation into a register from a register or an immediate takes 2 cycles more: 8 + ea, not 6)
    auto Direct = [](unsigned m, unsigned r) { return m < 2 || (m == 7 && r == 4) ? 2u : 0u; };

    switch (op >> 12) {
    case 0x0: {
        if (op & 0x0100) {
            if (mode == 1) { // MOVEP: every other byte of memory to or from a data register
                const uint32_t address = a[reg] + static_cast<int16_t>(Fetch());
                const unsigned bytes = (op & 0x0040) ? 4 : 2;
                if (op & 0x0080) {
                    for (unsigned i = 0; i < bytes; i++) {
                        Write(address + i * 2, 0, d[reg2] >> ((bytes - 1 - i) * 8));
                    }
                } else {
                    uint32_t value = 0;
                    for (unsigned i = 0; i < bytes; i++) {
                        value = (value << 8) | Read(address + i * 2, 0);
                    }
                    d[reg2] = bytes == 4 ? value : (d[reg2] & 0xFFFF0000) | value;
                }
                m_cycles += bytes == 4 ? 24 : 16;
                return;
            }
        } else if (reg2 != 4 && reg2 != 7 && (op & 0x3F) == 0x3C && size < 2) {
            // ORI, ANDI, EORI to CCR (a byte) or SR (a word)
            const uint16_t value = Fetch();
            uint16_t sr = SR();
            const uint16_t affected = size == 0 ? 0x00FF : 0xFFFF;
            switch (reg2) {
            case 0: sr |= value & affected; break;
            case 1: sr &= value | ~affected; break;
            case 5: sr ^= value & affected; break;
            default: Illegal(op, at); return;
            }
            SetSR(size == 0 ? (SR() & 0xFF00) | (sr & 0x1F) : sr & 0xA71F);
            m_cycles += 20;
            return;
        }
        if ((op & 0x0100) || reg2 == 4) {
            // BTST, BCHG, BCLR, BSET: a bit of a data register (32 bits) or of a byte of memory
            const uint32_t number = (op & 0x0100) ? d[reg2] : Fetch();
            const unsigned kind = (op >> 6) & 3;
            const bool dynamic = op & 0x0100;
            if (mode == 0) {
                const uint32_t bit = 1u << (number & 31);
                flagZ = !(d[reg] & bit);
                switch (kind) {
                case 1: d[reg] ^= bit; break;
                case 2: d[reg] &= ~bit; break;
                case 3: d[reg] |= bit; break;
                default: break;
                }
                // (the times are for a bit above 15; BCHG, BCLR and BSET of a lower bit take 2 fewer)
                static constexpr uint8_t kCycles[4] = {6, 8, 10, 8};
                m_cycles += kCycles[kind] + (dynamic ? 0 : 4) - (kind != 0 && (number & 31) < 16 ? 2 : 0);
            } else {
                const Operand operand = Locate(mode, reg, 0);
                uint32_t value = Load(operand, 0);
                const uint32_t bit = 1u << (number & 7);
                flagZ = !(value & bit);
                switch (kind) {
                case 1: value ^= bit; break;
                case 2: value &= ~bit; break;
                case 3: value |= bit; break;
                default: break;
                }
                if (kind != 0) {
                    Store(operand, 0, value);
                }
                // (BTST Dn,#imm: 2 more, as fx68k's microcode has it)
                m_cycles += operandCycles(0) + (kind == 0 ? 4 : 8) + (dynamic ? 0 : 4) + (mode == 7 && reg == 4 ? 2 : 0);
            }
            return;
        }
        // ORI, ANDI, SUBI, ADDI, EORI, CMPI
        if (size == 3) {
            Illegal(op, at);
            return;
        }
        const uint32_t value = size == 2 ? Fetch32() : (Fetch() & kMask[size]);
        const Operand operand = Locate(mode, reg, size);
        const uint32_t target = Load(operand, size);
        uint32_t result = 0;
        switch (reg2) {
        case 0: result = target | value; Logic(result, size); break;
        case 1: result = target & value; Logic(result, size); break;
        case 2: result = Sub(target, value, size, false, false); break;
        case 3: result = Add(target, value, size, false, false); break;
        case 5: result = target ^ value; Logic(result, size); break;
        case 6: Sub(target, value, size, false, false, false); break;
        default: Illegal(op, at); return;
        }
        if (reg2 != 6) {
            Store(operand, size, result);
            m_cycles += mode == 0 ? (size == 2 ? 16 : 8) : operandCycles(size) + (size == 2 ? 20 : 12);
        } else {
            m_cycles += mode == 0 ? (size == 2 ? 14 : 8) : operandCycles(size) + (size == 2 ? 12 : 8);
        }
        return;
    }
    case 0x1:
    case 0x2:
    case 0x3: { // MOVE, MOVEA
        const unsigned sz = (op >> 12) == 1 ? 0 : (op >> 12) == 3 ? 1 : 2;
        const Operand source = Locate(mode, reg, sz);
        // (the read's bus cycle: after the source's address words, and an index's or a predecrement's 2 cycles: Locate)
        const uint32_t value = Load(source, sz);
        const unsigned dstMode = (op >> 6) & 7;
        m_cycles += 4 + operandCycles(sz);
        if (dstMode == 1) {
            a[reg2] = static_cast<uint32_t>(Extend(value, sz));
            return;
        }
        const Operand target = Locate(dstMode, reg2, sz);
        // (a destination of -(An) costs what (An) does: the 2 cycles of predecrement are a source's only)
        m_cycles += kOperandCycles[sz == 2][ModeIndex(dstMode, reg2)] - (dstMode == 4 ? 2 : 0);
        writeTime = cycles + m_cycles - (sz == 2 ? 12 : 8); // (the write's bus cycle, before the last prefetch)
        m_moving = true;
        Store(target, sz, value);
        m_moving = false;
        Logic(value, sz);
        return;
    }
    case 0x4: {
        if (op == 0x4E71) { // NOP
            m_cycles += 4;
            return;
        }
        if (op == 0x4E75) { // RTS
            pc = Pop32();
            m_cycles += 16;
            m_jumped = true;
            return;
        }
        if (op == 0x4E73) { // RTE
            const uint16_t sr = Pop16();
            pc = Pop32();
            SetSR(sr & 0xA71F);
            m_cycles += 20;
            m_returned = true;
            return;
        }
        if (op == 0x4E77) { // RTR
            const uint16_t ccr = Pop16();
            pc = Pop32();
            SetSR((SR() & 0xFF00) | (ccr & 0x1F));
            m_cycles += 20;
            m_jumped = true;
            m_returned = true;
            return;
        }
        if (op == 0x4E70) { // RESET
            m_cycles += 132;
            return;
        }
        if (op == 0x4E72) { // STOP
            SetSR(Fetch() & 0xA71F);
            stopped = true;
            m_cycles += 4;
            return;
        }
        if (op == 0x4E76) { // TRAPV
            if (flagV) {
                Exception(7, pc, -1);
                m_cycles += 34;
            } else {
                m_cycles += 4;
            }
            return;
        }
        if ((op & 0xFFF0) == 0x4E40) { // TRAP
            Exception(32 + (op & 15), pc, -1);
            m_cycles += 34;
            return;
        }
        if ((op & 0xFFF8) == 0x4E50) { // LINK
            Push32(a[reg]);
            a[reg] = a[7];
            a[7] += static_cast<int16_t>(Fetch());
            m_cycles += 16;
            return;
        }
        if ((op & 0xFFF8) == 0x4E58) { // UNLK
            a[7] = a[reg];
            a[reg] = Pop32();
            m_cycles += 12;
            return;
        }
        if ((op & 0xFFF0) == 0x4E60) { // MOVE to and from USP
            if (op & 8) {
                a[reg] = otherSP;
            } else {
                otherSP = a[reg];
            }
            m_cycles += 4;
            return;
        }
        if ((op & 0xFF80) == 0x4E80) { // JSR, JMP
            static constexpr uint8_t kJump[12] = {0, 0, 8, 0, 0, 10, 14, 10, 12, 10, 14, 0};
            const Operand target = Locate(mode, reg, 2);
            if (!(op & 0x0040)) {
                Push32(pc);
                m_cycles += 8;
            }
            pc = target.at;
            m_cycles += kJump[modeIndex];
            m_jumped = modeIndex < 7 || modeIndex == 10; // (through a register: not a fixed place)
            return;
        }
        if ((op & 0xF1C0) == 0x41C0) { // LEA
            static constexpr uint8_t kLea[12] = {0, 0, 4, 0, 0, 8, 12, 8, 12, 8, 12, 0};
            a[reg2] = Locate(mode, reg, 2).at;
            m_cycles += kLea[modeIndex];
            return;
        }
        if ((op & 0xF1C0) == 0x4180) { // CHK
            const int16_t bound = static_cast<int16_t>(Load(Locate(mode, reg, 1), 1));
            const int16_t value = static_cast<int16_t>(d[reg2]);
            m_cycles += 10 + operandCycles(1);
            if (value < 0 || value > bound) {
                flagN = value < 0;
                Exception(6, pc, -1);
                m_cycles += 28; // (38 in all, as fx68k)
            }
            m_tail = 4; // (in bounds or not: its last prefetch, at the end)
            return;
        }
        switch (op & 0xFF00) {
        case 0x4000: // NEGX, MOVE from SR
        case 0x4200: // CLR
        case 0x4400: // NEG, MOVE to CCR
        case 0x4600: // NOT, MOVE to SR
            if (size == 3) {
                if ((op & 0xFF00) == 0x4000) {
                    const Operand target = Locate(mode, reg, 1);
                    Store(target, 1, SR());
                    m_cycles += mode == 0 ? 6 : 8 + operandCycles(1);
                } else if ((op & 0xFF00) == 0x4200) {
                    Illegal(op, at);
                } else {
                    const uint16_t value = static_cast<uint16_t>(Load(Locate(mode, reg, 1), 1));
                    SetSR((op & 0xFF00) == 0x4400 ? (SR() & 0xFF00) | (value & 0x1F) : value & 0xA71F);
                    m_cycles += 12 + operandCycles(1);
                }
                return;
            } else {
                const Operand target = Locate(mode, reg, size);
                const uint32_t value = (op & 0xFF00) == 0x4200 ? 0 : Load(target, size);
                uint32_t result = 0;
                switch (op & 0xFF00) {
                case 0x4000: result = Sub(0, value, size, true, true); break;
                case 0x4200: Logic(0, size); break;
                case 0x4400: result = Sub(0, value, size, false, false); break;
                default: result = ~value & kMask[size]; Logic(result, size); break;
                }
                Store(target, size, result);
                m_cycles += isRegister ? (size == 2 ? 6 : 4) : operandCycles(size) + (size == 2 ? 12 : 8);
                return;
            }
        case 0x4800:
            if (size == 0) { // NBCD
                const Operand target = Locate(mode, reg, 0);
                const uint32_t value = Load(target, 0);
                int low = -static_cast<int>(value & 15) - flagX;
                int result = -static_cast<int>(value) - flagX;
                if (low < 0) {
                    result -= 6;
                }
                flagC = flagX = result < 0;
                if (result < 0) {
                    result -= 0x60;
                }
                flagZ = flagZ && (result & 0xFF) == 0;
                flagN = result & 0x80;
                Store(target, 0, static_cast<uint32_t>(result) & 0xFF);
                m_cycles += mode == 0 ? 6 : 8 + operandCycles(0);
                return;
            }
            if (size == 1) {
                if (mode == 0) { // SWAP
                    d[reg] = (d[reg] << 16) | (d[reg] >> 16);
                    Logic(d[reg], 2);
                    m_cycles += 4;
                } else { // PEA
                    static constexpr uint8_t kPea[12] = {0, 0, 12, 0, 0, 16, 20, 16, 20, 16, 20, 0};
                    Push32(Locate(mode, reg, 2).at);
                    m_cycles += kPea[modeIndex];
                }
                return;
            }
            if (mode == 0) { // EXT
                if (size == 2) {
                    d[reg] = (d[reg] & 0xFFFF0000) | (static_cast<uint32_t>(static_cast<int8_t>(d[reg])) & 0xFFFF);
                    Logic(d[reg], 1);
                } else {
                    d[reg] = static_cast<uint32_t>(static_cast<int16_t>(d[reg]));
                    Logic(d[reg], 2);
                }
                m_cycles += 4;
                return;
            }
            [[fallthrough]];
        case 0x4C00: { // MOVEM
            if (size < 2) {
                Illegal(op, at);
                return;
            }
            const unsigned sz = size == 3 ? 2 : 1;
            const uint16_t list = Fetch();
            const bool toRegisters = op & 0x0400;
            const unsigned count = static_cast<unsigned>(std::popcount(list));
            if (!toRegisters && mode == 4) {
                // (to memory going down: the list is the other way round, A7 first)
                uint32_t address = a[reg];
                for (int i = 0; i < 16; i++) {
                    if (list & (1u << i)) {
                        address -= kBytes[sz];
                        const unsigned r = 15 - i;
                        Write(address, sz, r < 8 ? d[r] : a[r - 8]);
                    }
                }
                a[reg] = address;
                m_cycles += 8 + count * (sz == 2 ? 8 : 4);
                return;
            }
            const bool program = mode == 7 && (reg == 2 || reg == 3);
            uint32_t address = mode == 3 ? a[reg] : Locate(mode, reg, sz).at;
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) {
                    continue;
                }
                if (toRegisters) {
                    const uint32_t value =
                        static_cast<uint32_t>(Extend(program ? ReadProgram(address, sz) : Read(address, sz), sz));
                    (i < 8 ? d[i] : a[i - 8]) = value;
                } else {
                    Write(address, sz, i < 8 ? d[i] : a[i - 8]);
                }
                address += kBytes[sz];
            }
            if (mode == 3) {
                a[reg] = address;
            }
            static constexpr uint8_t kToMemory[12] = {0, 0, 8, 0, 8, 12, 14, 12, 16, 0, 0, 0};
            static constexpr uint8_t kToRegisters[12] = {0, 0, 12, 12, 0, 16, 18, 16, 20, 16, 18, 0};
            m_cycles += (toRegisters ? kToRegisters : kToMemory)[modeIndex] + count * (sz == 2 ? 8 : 4);
            return;
        }
        case 0x4A00:
            if (size == 3) { // TAS
                const Operand target = Locate(mode, reg, 0);
                const uint32_t value = Load(target, 0);
                Logic(value, 0);
                Store(target, 0, value | 0x80);
                m_cycles += mode == 0 ? 4 : 10 + operandCycles(0);
                m_tail = 4; // (its read-modify-write is one bus cycle: the prefetch after it)
            } else { // TST
                Logic(Load(Locate(mode, reg, size), size), size);
                m_cycles += 4 + operandCycles(size);
            }
            return;
        default: break;
        }
        Illegal(op, at);
        return;
    }
    case 0x5: {
        if (size == 3) {
            const unsigned cc = (op >> 8) & 15;
            if (mode == 1) { // DBcc: count down and branch until the condition holds or the count runs out
                const uint32_t base = pc;
                const int16_t displacement = static_cast<int16_t>(Fetch());
                if (Condition(cc)) {
                    m_cycles += 12;
                } else {
                    const uint16_t count = static_cast<uint16_t>(d[reg] - 1);
                    d[reg] = (d[reg] & 0xFFFF0000) | count;
                    if (count != 0xFFFF) {
                        pc = base + displacement;
                        m_cycles += 10;
                    } else {
                        m_cycles += 14;
                    }
                }
            } else { // Scc
                const bool set = Condition(cc);
                Store(Locate(mode, reg, 0), 0, set ? 0xFF : 0);
                m_cycles += mode == 0 ? (set ? 6 : 4) : 8 + operandCycles(0);
            }
            return;
        }
        // ADDQ, SUBQ
        const uint32_t value = reg2 ? reg2 : 8;
        if (mode == 1) {
            a[reg] += (op & 0x0100) ? 0u - value : value;
            m_cycles += 8;
            return;
        }
        const Operand target = Locate(mode, reg, size);
        const uint32_t before = Load(target, size);
        Store(target, size, (op & 0x0100) ? Sub(before, value, size, false, false) : Add(before, value, size, false, false));
        m_cycles += mode == 0 ? (size == 2 ? 8 : 4) : operandCycles(size) + (size == 2 ? 12 : 8); // (ADDQ and SUBQ alike: 8 + ea, a long 12 + ea)
        return;
    }
    case 0x6: { // Bcc, BRA, BSR
        const uint32_t base = pc;
        const unsigned cc = (op >> 8) & 15;
        int32_t displacement = static_cast<int8_t>(op);
        const bool wide = displacement == 0;
        if (wide) {
            displacement = static_cast<int16_t>(Fetch());
        }
        if (cc == 1) {
            Push32(pc);
            pc = base + displacement;
            m_cycles += 18;
        } else if (Condition(cc)) {
            pc = base + displacement;
            m_cycles += 10;
        } else {
            m_cycles += wide ? 12 : 8;
        }
        return;
    }
    case 0x7: // MOVEQ
        d[reg2] = static_cast<uint32_t>(static_cast<int8_t>(op));
        Logic(d[reg2], 2);
        m_cycles += 4;
        return;
    case 0x8:
    case 0xC: {
        const bool isAnd = (op >> 12) == 0xC;
        if (size == 3) { // DIVU, DIVS / MULU, MULS
            const uint32_t source = Load(Locate(mode, reg, 1), 1);
            const bool isSigned = op & 0x0100;
            if (isAnd) {
                if (isSigned) {
                    d[reg2] = static_cast<uint32_t>(static_cast<int16_t>(d[reg2]) * static_cast<int16_t>(source));
                    m_cycles += 38 + 2 * std::popcount(static_cast<uint16_t>(source ^ (source << 1)));
                } else {
                    d[reg2] = (d[reg2] & 0xFFFF) * source;
                    m_cycles += 38 + 2 * std::popcount(source);
                }
                Logic(d[reg2], 2);
                m_cycles += operandCycles(1);
                return;
            }
            if (source == 0) {
                Exception(5, pc, -1);
                flagN = flagZ = flagV = flagC = false;
                m_cycles += 38;
                return;
            }
            flagZ = flagC = false;
            m_cycles += DivideTime(d[reg2], static_cast<uint16_t>(source), isSigned);
            // (it looks at the interrupt lines in its last prefetch, at the end; a DIVS by a negative number 2 after)
            m_tail = isSigned && static_cast<int16_t>(source) < 0 ? 6 : 4;
            if (isSigned) {
                const int32_t dividend = static_cast<int32_t>(d[reg2]);
                const int16_t divisor = static_cast<int16_t>(source);
                const int64_t quotient = static_cast<int64_t>(dividend) / divisor;
                const int64_t remainder = static_cast<int64_t>(dividend) % divisor;
                const uint32_t magnitude = static_cast<uint32_t>(dividend < 0 ? -static_cast<int64_t>(dividend) : dividend);
                const uint32_t limit = static_cast<uint32_t>(std::abs(static_cast<int32_t>(divisor))) << 16;
                if (magnitude >= limit && divisor != -0x8000) {
                    flagN = flagV = true;
                } else {
                    flagZ = quotient == 0;
                    flagV = quotient != static_cast<int16_t>(quotient);
                    if (flagV) {
                        flagN = true;
                    } else {
                        d[reg2] = static_cast<uint16_t>(quotient) | (static_cast<uint32_t>(static_cast<uint16_t>(remainder)) << 16);
                        flagN = quotient & 0x8000;
                    }
                }
            } else {
                const uint32_t quotient = d[reg2] / source, remainder = d[reg2] % source;
                flagZ = quotient == 0;
                flagV = quotient > 0xFFFF;
                if (flagV) {
                    flagN = true;
                } else {
                    d[reg2] = quotient | (remainder << 16);
                    flagN = quotient & 0x8000;
                }
            }
            m_cycles += operandCycles(1);
            return;
        }
        if ((op & 0x01F0) == 0x0100) { // SBCD / ABCD
            const bool memory = op & 8;
            Operand source{Kind::Data, reg}, target{Kind::Data, reg2};
            if (memory) {
                source = Locate(4, reg, 0);
                target = Locate(4, reg2, 0);
            }
            const uint32_t x = Load(source, 0), y = Load(target, 0);
            Store(target, 0, Bcd(isAnd, x, y));
            m_cycles += memory ? 18 : 6;
            return;
        }
        if (isAnd && (op & 0x0100) && (mode < 2) && size != 0) { // EXG
            if (size == 1) {
                uint32_t &x = mode == 0 ? d[reg2] : a[reg2], &y = mode == 0 ? d[reg] : a[reg];
                std::swap(x, y);
            } else {
                std::swap(d[reg2], a[reg]);
            }
            m_cycles += 6;
            return;
        }
        // OR, AND: into the data register, or from it into memory
        const Operand operand = Locate(mode, reg, size);
        const uint32_t value = Load(operand, size);
        const uint32_t result = isAnd ? (value & d[reg2]) : (value | d[reg2]);
        Logic(result, size);
        if (op & 0x0100) {
            Store(operand, size, result);
            m_cycles += operandCycles(size) + (size == 2 ? 12 : 8);
        } else {
            d[reg2] = (d[reg2] & ~kMask[size]) | (result & kMask[size]);
            m_cycles += operandCycles(size) + (size == 2 ? 6 + Direct(mode, reg) : 4);
        }
        return;
    }
    case 0x9:
    case 0xD: {
        const bool isAdd = (op >> 12) == 0xD;
        if (size == 3) { // SUBA, ADDA
            const unsigned sz = (op & 0x0100) ? 2 : 1;
            const uint32_t value = static_cast<uint32_t>(Extend(Load(Locate(mode, reg, sz), sz), sz));
            a[reg2] = isAdd ? a[reg2] + value : a[reg2] - value;
            m_cycles += operandCycles(sz) + (sz == 2 ? 6 + Direct(mode, reg) : 8);
            return;
        }
        if ((op & 0x0130) == 0x0100) { // SUBX, ADDX
            const bool memory = op & 8;
            Operand source{Kind::Data, reg}, target{Kind::Data, reg2};
            if (memory) {
                source = Locate(4, reg, size);
                target = Locate(4, reg2, size);
            }
            const uint32_t x = Load(source, size), y = Load(target, size);
            Store(target, size, isAdd ? Add(y, x, size, true, true) : Sub(y, x, size, true, true));
            m_cycles += memory ? (size == 2 ? 30 : 18) : (size == 2 ? 8 : 4); // (SUBX as ADDX: 4, a long 8)
            return;
        }
        const Operand operand = Locate(mode, reg, size);
        const uint32_t value = Load(operand, size);
        if (op & 0x0100) {
            Store(operand, size, isAdd ? Add(value, d[reg2], size, false, false) : Sub(value, d[reg2], size, false, false));
            m_cycles += operandCycles(size) + (size == 2 ? 12 : 8);
        } else {
            const uint32_t result = isAdd ? Add(d[reg2], value, size, false, false) : Sub(d[reg2], value, size, false, false);
            d[reg2] = (d[reg2] & ~kMask[size]) | result;
            m_cycles += operandCycles(size) + (size == 2 ? 6 + Direct(mode, reg) : 4);
        }
        return;
    }
    case 0xB: {
        if (size == 3) { // CMPA
            const unsigned sz = (op & 0x0100) ? 2 : 1;
            const uint32_t value = static_cast<uint32_t>(Extend(Load(Locate(mode, reg, sz), sz), sz));
            Sub(a[reg2], value, 2, false, false, false);
            m_cycles += 6 + operandCycles(sz);
            return;
        }
        if (!(op & 0x0100)) { // CMP
            const uint32_t value = Load(Locate(mode, reg, size), size);
            Sub(d[reg2], value, size, false, false, false);
            m_cycles += operandCycles(size) + (size == 2 ? 6 : 4);
            return;
        }
        if (mode == 1) { // CMPM
            const uint32_t x = Load(Locate(3, reg, size), size);
            const uint32_t y = Load(Locate(3, reg2, size), size);
            Sub(y, x, size, false, false, false);
            m_cycles += size == 2 ? 20 : 12;
            return;
        }
        // EOR
        const Operand operand = Locate(mode, reg, size);
        const uint32_t result = Load(operand, size) ^ (d[reg2] & kMask[size]);
        Logic(result, size);
        Store(operand, size, result);
        // (into a data register: 4, a long 8, as the other ALU operations)
        m_cycles += mode == 0 ? (size == 2 ? 8 : 4) : operandCycles(size) + (size == 2 ? 12 : 8);
        return;
    }
    case 0xE: { // shifts and rotates
        const bool left = op & 0x0100;
        if (size == 3) { // of a word of memory, by one
            const Operand target = Locate(mode, reg, 1);
            Store(target, 1, Shift(reg2 & 3, left, Load(target, 1), 1, 1));
            m_cycles += 8 + operandCycles(1);
            return;
        }
        const unsigned count = (op & 0x0020) ? (d[reg2] & 63) : (reg2 ? reg2 : 8);
        const uint32_t result = Shift((op >> 3) & 3, left, d[reg], count, size);
        d[reg] = (d[reg] & ~kMask[size]) | result;
        m_cycles += (size == 2 ? 8 : 6) + 2 * count;
        return;
    }
    default: break;
    }
    Illegal(op, at);
}

void M68K::Serialize(Archive &ar) {
    ar.Mark(0x4D36384B);
    ar.Pod(d);
    ar.Pod(a);
    ar.Pod(otherSP);
    ar.Pod(pc);
    ar.Pod(flagC);
    ar.Pod(flagV);
    ar.Pod(flagZ);
    ar.Pod(flagN);
    ar.Pod(flagX);
    ar.Pod(supervisor);
    ar.Pod(trace);
    ar.Pod(interruptMask);
    ar.Pod(stopped);
    ar.Pod(m_interruptLevel);
    ar.Pod(m_irqFrom);
    ar.Pod(m_lastTail);
    ar.Pod(cycles);
}

} // namespace spelunk
