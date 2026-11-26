// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp; from cps2recomp: https://github.com/tmcleroy/cps2recomp
//
// The Motorola 68000 of Cave's first boards (machine/cave68k.h), which runs the game's program from ROM.
//
// An interpreter: Step runs one instruction (or takes an interrupt) and says how many clock cycles it took.
// Bus and address errors are not modelled.
#pragma once

#include <cstdio>

#include <cstdint>
#include <utility>
#include <vector>

namespace spelunk {

class Archive;

// What the CPU is connected to: 24-bit addresses, big-endian words
struct M68KBus {
    virtual ~M68KBus() = default;
    virtual uint8_t Read8(uint32_t address) = 0;
    virtual uint16_t Read16(uint32_t address) = 0;
    virtual void Write8(uint32_t address, uint8_t value) = 0;
    virtual void Write16(uint32_t address, uint16_t value) = 0;
    // A word read as program
    virtual uint16_t Fetch16(uint32_t address) = 0;
    // The CPU takes the interrupt of this level
    virtual void InterruptAck(unsigned level) {}
    // False where a read does not give back what was written (a chip's registers): the block checker's side run then
    // reads the chip, not its own writes
    virtual bool IsMemory(uint32_t address) const {
        return true;
    }
    // Set while the block checker runs a translated block on the side: reads must change nothing (a register that
    // clears when read is left as it is, for the interpreter's run of the same block to find)
    // For the block checker: whether what a read at `read` gives can depend on a write at `written` to something that
    // is not memory (the same chip: its side run only notes its writes, so such a block cannot be compared). The
    // same 64 KB, unless the board knows more
    virtual bool SameChip(uint32_t written, uint32_t read) const {
        return ((written ^ read) & 0xFF0000) == 0;
    }
    bool sideRun = false;
    // (set by a read in a side run that could not give what the chip would: the block is then not compared)
    bool sideReadUnsure = false;
    uint32_t sideRuns = 0; // (one more at each side run's start: what a bus keeps for a side run is begun afresh)
};

class M68K;
// A translated routine (runtime/native68k.h): runs from the block at cpu.pc until it leaves the routine
using NativeFn = void (*)(M68K &);

class M68K {
public:
    explicit M68K(M68KBus &bus)
        : bus(bus) {}

    // How Run goes about it
    enum class Mode {
        Interpreter, // an instruction at a time
        Blocks,      // the interpreter, stopping only where translated code would (the reference for Native)
        Native,      // translated code; the interpreter for what has no translation
        Check,       // both, a block at a time, compared (the check build only)
    };

    // The reset line: the stack pointer and PC from the first two vectors
    void Reset(bool hard);
    // The level on the interrupt lines (0: none), from the time `at` (the time now when not given). A level above
    // the mask in SR is taken between instructions, but not at once: the 68000 passes the lines through two samples
    // and looks at them in each instruction's last prefetch, so a level that has risen is taken after the first
    // instruction whose last prefetch begins irqSync cycles or more after the rise (InstructionTail). Found against
    // Arcade-Cave_MiSTer's fx68k (docs/SLOWDOWN.md, "Cave's 68000 board").
    void SetInterruptLevel(uint8_t level, uint64_t at) {
        if (level > m_interruptLevel) {
            m_irqFrom = at + irqSync;
        }
        m_interruptLevel = level;
    }
    void SetInterruptLevel(uint8_t level) {
        SetInterruptLevel(level, cycles);
    }
    unsigned irqSync = 4;
    uint8_t InterruptLevel() const {
        return m_interruptLevel;
    }
    // One instruction or interrupt; the clock cycles it took (not added to `cycles`)
    uint32_t Step();
    // Runs until `cycles` reaches `until` (a little past it: to the end of the instruction or block). `exact`: to the
    // end of the first instruction that ends at or after `until`, not of a block (where the board raises an
    // interrupt at `until`: the last stretch is run an instruction at a time, kExactTail cycles of it)
    void Run(uint64_t until, Mode mode, bool exact = false);
    static constexpr uint64_t kExactTail = 2048;
    // (as run: more where the board's waits can make a block that long, the PGM's video memory holding an access for
    // up to a line's first 368 cycles: a block begun before the stretch would otherwise run on past `until`)
    uint64_t exactTail = kExactTail;
    // For translated code: the instruction at `address` done by the interpreter (the rare ones)
    void Interpret(uint32_t address);

    uint32_t d[8] = {}, a[8] = {}; // a[7] is the stack pointer in use
    uint32_t otherSP = 0;          // the user or supervisor stack pointer not in use
    uint32_t pc = 0;               // of the next instruction
    bool flagC = false, flagV = false, flagZ = false, flagN = false, flagX = false;
    bool supervisor = true, trace = false;
    uint8_t interruptMask = 7;
    bool stopped = false; // (after STOP: until an interrupt)

    uint64_t cycles = 0; // clock cycles since reset
    // Waits a board adds to each word of program fetched (none on these boards: the translated code counts with it
    // all the same)
    unsigned busWait = 0;
    void BusWait(uint32_t) {}
    unsigned fetchedWords = 0;
    // The board's waits on a read or write of data (the feature "Slowdown as the board": Pgm::SetBoardTiming,
    // docs/SLOWDOWN.md): the clock cycles the board holds the CPU for an access at `address` begun at `cycles` (the
    // time at the start of the instruction, in the interpreter and translated code alike). Null: none, as the port
    // has always run the 68000. The board says 0 for work RAM and the program (translated code reads those directly).
    uint32_t (*accessWait)(const void *board, uint32_t address, uint64_t cycles, bool write) = nullptr;
    const void *accessWaitBoard = nullptr;
    // Where in the instruction its access falls, in cycles (four for each extension word read before it and for each
    // word read or written before it): the interpreter counts it, translated code is told it (At, recomp.py's
    // --access-times, made for a board whose waits depend on the time of the bus cycle: the PGM's)
    uint32_t accessInto = 0;
    bool m_moving = false; // (the interpreter: a MOVE's write, made with no prefetch after its read: Write)
    M68K &At(uint32_t into) {
        accessInto = into;
        return *this;
    }
    void DataWait(uint32_t address, bool write) {
        if (accessWait) [[unlikely]] {
            const uint32_t wait = accessWait(accessWaitBoard, address & 0xFFFFFF, cycles + accessInto, write);
            cycles += wait;
            waitsCounted += wait;
        }
    }
    uint64_t waitsCounted = 0; // (all the waits since reset: the headless runners' --frame-log)
    // When a MOVE to memory makes its write: the bus cycle comes just before the instruction's last prefetch, 8 cycles
    // from its end (a long's high word 12). A board that takes a write when the bus makes it (the YMZ280B: a key-on
    // that lands just after one of the chip's ticks is the next tick's) uses this while it is not before `cycles` (the
    // instruction's start: an older one is a past instruction's), and the instruction's start otherwise. Set by the
    // interpreter and translated code alike
    uint64_t writeTime = 0;
    uint64_t WriteTime() const {
        return writeTime >= cycles ? writeTime : cycles;
    }
    // When an instruction's read of memory makes its bus cycle: after the words of the instruction and its operand's
    // address are fetched (4 cycles each), and 2 more for -(An) and an index (DoDonPachi's `move.w $800000,d0`: 8
    // cycles in, where it reads the blank's bit; ESP Ra.De.'s `btst #2,$800001`: 12). As writeTime
    uint64_t readTime = 0;
    uint64_t ReadTime() const {
        return readTime >= cycles ? readTime : cycles;
    }
    // (the headless runners' --spin: how often the interpreter came to the instruction at spinAt, a game's idle loop)
    uint32_t spinAt = 1;
    uint64_t spins = 0;
    // One instruction by the interpreter
    void ExecuteOne() {
        fetchedWords = 0;
        m_returned = false;
        m_readCycles = m_writeCycles = m_early = m_tail = 0;
        if (pc == spinAt) [[unlikely]] {
            spins++;
        }
        if (noteWrites) [[unlikely]] {
            notedWrites.clear();
            notedStart = cycles;
        }
        const uint16_t op = Fetch();
        Execute(op);
        m_lastTail = InstructionTail(op);
        if (noteWrites) [[unlikely]] {
            notedEnd = cycles + m_cycles;
            notedOp = op;
        }
    }
    // The last instruction was an RTE or RTR: translated code leaves its block there (and looks for an interrupt the
    // mask now lets in), whatever it returned to, so the interpreter running a block's rest stops there too
    bool Returned() const {
        return m_returned;
    }
    uint64_t budget = 0; // translated code stops at the first block that starts at or after this
    unsigned depth = 0;  // of translated routines calling one another on the host's stack

    M68KBus &bus;
    uint8_t *ram = nullptr;       // the board's work RAM (64 KB on Cave's boards, 128 KB on the PGM), at ramBase
    uint32_t ramBase = 0x100000;
    uint32_t ramSize = 0x10000;
    const uint8_t *rom = nullptr; // the program
    uint32_t romSize = 0;

    bool recording = false;
    std::vector<uint32_t> newEntries;
    // (for --stats: instructions the interpreter ran while translated code was meant to, and all it ran)
    uint64_t interpreted = 0;
    // For finding out who writes somewhere (--watch): writes by the interpreter to addresses from watchLow to
    // watchHigh are reported with the instruction's address. Translated code is not watched (run with
    // --cpu blocks, which does the same things).
    uint32_t watchLow = 1, watchHigh = 0;
    // (SPELUNK_PC_HISTORY=1: the places the run loop last came to, translated routines and instructions the
    // interpreter ran, printed when it meets something that is not an instruction)
    bool keepHistory = false;
    uint32_t history[64] = {};
    unsigned historyAt = 0;
    void Remember(uint32_t place) {
        history[historyAt++ & 63] = place;
    }
    uint32_t instruction = 0; // the address of the instruction the interpreter is doing
    // The time an autovectored interrupt takes, from the end of the instruction before it to the handler's first
    // instruction: 36 cycles and the acknowledge, whose end waits for the E clock (the CPU's clock divided by ten,
    // free-running): 13 to 22 cycles by the E clock's phase when it begins, 10 cycles after the exception does
    // (`cycles` + eclockPhase: the E clock's count; measured on fx68k in Arcade-Cave_MiSTer, docs/SLOWDOWN.md)
    unsigned eclockPhase = 6;
    uint32_t InterruptTime() const {
        return 58u - static_cast<uint32_t>((cycles + eclockPhase) % 10);
    }
    // (measurement: each instruction the interpreter starts, its address and the clock then, a line each; set by
    // headless68k's SPELUNK_CYCLE_TRACE to compare a frame's timing with a reference's bus log)
    FILE *cycleTrace = nullptr;
    bool traceWrites = false;
    // (measurement, headless68k --ram-log: the interpreter's last instruction, its time and its writes to work RAM
    // with what they replaced, so that a log taken at a blank that falls inside an instruction leaves out the writes
    // the bus had not made yet, as a reference's would: RamAt)
    bool noteWrites = false;
    struct NotedWrite {
        uint32_t address; // (a word's, in work RAM)
        uint16_t before;
    };
    std::vector<NotedWrite> notedWrites;
    uint64_t notedStart = 0, notedEnd = 0;
    uint16_t notedOp = 0;
    // Work RAM as the bus had it at `at`, if the last instruction run ends after it
    std::vector<uint8_t> RamAt(uint64_t at) const;

    uint16_t SR() const {
        return static_cast<uint16_t>(flagC | (flagV << 1) | (flagZ << 2) | (flagN << 3) | (flagX << 4) | (interruptMask << 8) |
                                     (supervisor << 13) | (trace << 15));
    }
    void SetSR(uint16_t value) {
        flagC = value & 1;
        flagV = value & 2;
        flagZ = value & 4;
        flagN = value & 8;
        flagX = value & 16;
        interruptMask = (value >> 8) & 7;
        trace = value & 0x8000;
        const bool super = value & 0x2000;
        if (super != supervisor) {
            std::swap(a[7], otherSP);
            supervisor = super;
        }
    }
    void Serialize(Archive &ar); // save states (runtime/state.h)

    // --- arithmetic and flags, shared by the interpreter and translated code ---
    // Sizes: 0 a byte, 1 a word, 2 a long word

    bool Condition(unsigned cc) const {
        switch (cc) {
        case 0: return true;
        case 1: return false;
        case 2: return !flagC && !flagZ;
        case 3: return flagC || flagZ;
        case 4: return !flagC;
        case 5: return flagC;
        case 6: return !flagZ;
        case 7: return flagZ;
        case 8: return !flagV;
        case 9: return flagV;
        case 10: return !flagN;
        case 11: return flagN;
        case 12: return flagN == flagV;
        case 13: return flagN != flagV;
        case 14: return !flagZ && flagN == flagV;
        default: return flagZ || flagN != flagV;
        }
    }
    uint32_t Add(uint32_t x, uint32_t y, unsigned size, bool withX, bool keepZ) {
        const uint64_t sum = static_cast<uint64_t>(x & kMask[size]) + (y & kMask[size]) + (withX && flagX);
        const uint32_t result = static_cast<uint32_t>(sum) & kMask[size];
        flagC = flagX = (sum >> (kBytes[size] * 8)) & 1;
        flagV = (~(x ^ y) & (x ^ result) & kSign[size]) != 0;
        flagN = result & kSign[size];
        flagZ = keepZ ? (flagZ && result == 0) : result == 0;
        return result;
    }
    // x - y
    uint32_t Sub(uint32_t x, uint32_t y, unsigned size, bool withX, bool keepZ, bool setX = true) {
        const uint64_t difference = static_cast<uint64_t>(x & kMask[size]) - (y & kMask[size]) - (withX && flagX);
        const uint32_t result = static_cast<uint32_t>(difference) & kMask[size];
        flagC = (difference >> (kBytes[size] * 8)) & 1;
        if (setX) {
            flagX = flagC;
        }
        flagV = ((x ^ y) & (x ^ result) & kSign[size]) != 0;
        flagN = result & kSign[size];
        flagZ = keepZ ? (flagZ && result == 0) : result == 0;
        return result;
    }
    void Logic(uint32_t result, unsigned size) {
        flagN = result & kSign[size];
        flagZ = (result & kMask[size]) == 0;
        flagV = flagC = false;
    }
    // kind: 0 arithmetic shift, 1 logical shift, 2 rotate through X, 3 rotate
    uint32_t Shift(unsigned kind, bool left, uint32_t value, unsigned count, unsigned size) {
        const uint32_t sign = kSign[size];
        value &= kMask[size];
        flagV = false;
        flagC = kind == 2 ? flagX : false;
        for (unsigned i = 0; i < count; i++) {
            const bool out = left ? (value & sign) : (value & 1);
            uint32_t in = 0;
            switch (kind) {
            case 0: in = left ? 0 : (value & sign); break;
            case 1: in = 0; break;
            case 2: in = flagX ? (left ? 1 : sign) : 0; break;
            default: in = out ? (left ? 1 : sign) : 0; break;
            }
            const uint32_t before = value;
            value = left ? ((value << 1) & kMask[size]) | in : (value >> 1) | in;
            flagC = out;
            if (kind != 3) {
                flagX = out;
            }
            if (kind == 0 && left && ((before ^ value) & sign)) {
                flagV = true; // (the sign changed at some point)
            }
        }
        flagN = value & sign;
        flagZ = value == 0;
        return value;
    }
    // Decimal arithmetic on two digits a byte: y + x or y - x, with X
    uint32_t Bcd(bool add, uint32_t x32, uint32_t y32) {
        const int x = static_cast<int>(x32 & 0xFF), y = static_cast<int>(y32 & 0xFF);
        int result;
        if (add) {
            const int low = (x & 15) + (y & 15) + flagX;
            result = x + y + flagX;
            if (low > 9) {
                result += 6;
            }
            flagC = flagX = result > 0x99;
            if (flagC) {
                result += 0x60;
            }
        } else {
            const int low = (y & 15) - (x & 15) - flagX;
            result = y - x - flagX;
            if (low < 0) {
                result -= 6;
            }
            flagC = flagX = result < 0;
            if (flagC) {
                result -= 0x60;
            }
        }
        flagZ = flagZ && (result & 0xFF) == 0;
        flagN = result & 0x80;
        return static_cast<uint32_t>(result) & 0xFF;
    }
    void Push32(uint32_t value) {
        a[7] -= 4;
        Write(a[7], 2, value);
    }
    uint32_t Pop32() {
        const uint32_t value = Read(a[7], 2);
        a[7] += 4;
        return value;
    }
    uint32_t Read(uint32_t address, unsigned size);
    void Write(uint32_t address, unsigned size, uint32_t value);
    uint32_t ReadProgram(uint32_t address, unsigned size);

    static constexpr uint32_t kMask[3] = {0xFF, 0xFFFF, 0xFFFFFFFF};
    static constexpr uint32_t kSign[3] = {0x80, 0x8000, 0x80000000};
    static constexpr uint32_t kBytes[3] = {1, 2, 4};

private:
    uint8_t m_interruptLevel = 0;
    uint64_t m_irqFrom = 0;  // the rise of the level on the lines, and irqSync: a last prefetch from then on sees it
    uint32_t m_lastTail = 4; // the cycles from the start of the last instruction's last prefetch to its end
    // (what the instruction being run did on the bus, for InstructionTail: data cycles read and written, and the
    // internal cycles before its last prefetch that an addressing mode adds)
    uint8_t m_readCycles = 0, m_writeCycles = 0, m_early = 0, m_tail = 0;
    uint32_t InstructionTail(uint16_t op) const;
    static uint32_t DivideTime(uint32_t dividend, uint16_t divisor, bool isSigned);
    // (an interrupt is above the mask but waits for an instruction's last prefetch)
    bool InterruptWaiting() const {
        return (m_interruptLevel == 7 || m_interruptLevel > interruptMask) && !stopped;
    }
    uint32_t m_cycles = 0;  // of the instruction being run
    bool m_jumped = false;  // the instruction went somewhere the translator cannot follow
    bool m_returned = false; // (Returned)

    struct Operand {
        enum class Kind : uint8_t { Data, Address, Memory, Program, Immediate } kind; // Program: memory, read as program
        uint32_t at; // register number, memory address or immediate value
    };
    uint16_t Fetch();
    uint32_t Fetch32();
    Operand Locate(unsigned mode, unsigned reg, unsigned size);
    Operand LocateEa(unsigned mode, unsigned reg, unsigned size);
    uint32_t Load(const Operand &operand, unsigned size);
    void Store(const Operand &operand, unsigned size, uint32_t value);
    void Push16(uint16_t value);
    uint16_t Pop16();
    void Exception(unsigned vector, uint32_t returnAddress, int level);
    void Execute(uint16_t op);
    void Illegal(uint16_t op, uint32_t address);
    bool TakeInterrupt();

public:
    // For translated code: the instructions that have to do with exceptions and the status register, done as
    // the interpreter does them (the time they take is added by the caller)
    void Trap(unsigned number, uint32_t returnAddress) {
        pc = returnAddress;
        Exception(32 + number, returnAddress, -1);
    }
    void ReturnFromException() {
        const uint16_t sr = Pop16();
        pc = Pop32();
        SetSR(sr & 0xA71F);
    }
    void MoveToSR(uint16_t value) {
        SetSR(value & 0xA71F);
    }
    // The registers kept and put back (for C++ that borrows the CPU to run a routine of the game's)
    struct Kept {
        uint32_t d[8], a[8];
        uint16_t sr;
    };
    Kept Keep() const {
        Kept kept;
        for (int n = 0; n < 8; n++) {
            kept.d[n] = d[n];
            kept.a[n] = a[n];
        }
        kept.sr = SR();
        return kept;
    }
    void PutBack(const Kept &kept) {
        SetSR(kept.sr);
        for (int n = 0; n < 8; n++) {
            d[n] = kept.d[n];
            a[n] = kept.a[n];
        }
    }
    // The game's routine at `address`, called from C++: run until it returns
    void CallGuest(uint32_t address);

private:
    void Record(uint32_t address);
    void RunChecked(uint64_t until, bool exact);
};

} // namespace spelunk
