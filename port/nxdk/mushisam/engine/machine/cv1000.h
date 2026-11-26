// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The Cave CV1000 board (CV1000-B and CV1000-D): the SH-3, its main RAM, the boot flash with the program (U4), the
// NAND flash with the graphics (U2), the blitter, the sound chip with its two flash ROMs (U23, U24), the clock and
// EEPROM (U10) and the JAMMA inputs on the CPU's ports.
//
//   0x00000000  U4, the program (2 MB on a B board, 4 MB on a D; the B's is seen twice)
//   0x0C000000  main RAM (8 MB on a B board, 16 MB on a D)
//   0x10000000  the NAND flash: data, command, address
//   0x10400000  the sound chip: register number, value
//   0x10C00000  the clock and EEPROM's serial lines; the NAND's chip enable
//   0x18000000  the blitter's registers
//   ports C, D, F, L: inputs; port E: the NAND's ready line; port J: the FPGA's configuration lines
//
// The memory map and the ports are as MAME's cv1k.cpp describes them (BSD-3-Clause, David Haywood, Luca Elia and
// MetalliC): see THIRD_PARTY_NOTICES.md.
#pragma once

#include "blitter.h"
#include "inputs.h"
#include "nand.h"
#include "rtc9701.h"
#include "sh3.h"
#include "ymz770.h"

#include <memory>
#include <string>
#include <vector>

namespace spelunk {

class Cv1000 final : public Bus {
public:
    Cv1000();

    SH3 cpu;
    Blitter blitter;
    Nand nand;
    Rtc9701 rtc;
    Ymz770 sound;
    std::vector<uint8_t> ram;      // 16 MB, big-endian as the board holds it
    std::vector<uint8_t> program;  // U4 as the CPU sees it (4 MB, big-endian)
    std::vector<uint8_t> soundRom; // U23 then U24 (8 MB, big-endian words)
    bool boardD = false;           // a CV1000-D (16 MB of RAM)

    // After the set is loaded: sizes the RAM as the board has it and starts the CPU at the boot flash
    void Reset();

    // What is held: each player's stick and buttons, and the coin, start and service switches
    uint32_t inputs[2] = {0, 0};
    uint32_t system = 0;
    // Called when the game reads the players' ports (the port's frontend polls the host's devices then, so what the
    // game gets is as new as it can be)
    void (*onInputRead)(void *context) = nullptr;
    void *onInputReadContext = nullptr;

    // One frame of the board's time (262 lines of 407 pixels at 6.4 MHz: 60.0184 Hz), from one vertical sync, with
    // its interrupt, to the next
    void RunFrame();
    static constexpr uint64_t kCpuHz = 102400000;
    static constexpr uint64_t kFrameCycles = 262ull * 407 * 16; // (the CPU's clock is 16 times the pixel clock)
    uint64_t frame = 0;

    // Per-frame RDTSC breakdown (populated inside RunFrame/Write for Xbox profiling)
    uint64_t profSh3Tsc = 0;
    uint64_t profAudioTsc = 0;
    uint64_t profBlitterTsc = 0;
    // The picture on show: 320 x 240, 32 bits a pixel, red in the low byte
    void Picture(uint32_t *out) const {
        blitter.Picture(out);
    }

    // A hash of what the game can see (main RAM, the CPU's registers, the EEPROM), for comparing two runs
    uint64_t StateHash() const;
    void Serialize(Archive &ar);
    // Run-ahead: the board as it is now kept (KeepNow), frames run on from there for their picture only, and the
    // board put back (PutBack). The sound chip stands still meanwhile; VRAM is kept by the rows written; nothing
    // is allocated per frame. Exact (online play's rollback, port/netplay.h): the sound chip runs on meanwhile and is
    // kept and put back with the rest, so the frames run from the moment kept are the game's own.
    // Two moments can be kept at once (`which`: online play's anchor and the next one); Drop lets one go.
    // `again`: the moment can be put back again later (online play); run-ahead's put back, which never is, takes the
    // kept RAM itself as main RAM (no copy) and leaves the copy as it was for the next KeepNow to fill
    void KeepNow(bool exact = false, int which = 0);
    void PutBack(int which = 0, bool again = true);
    void Drop(int which);
    // The input lag readout's looks (port/main.cpp), run a frame at a time between the game's own: the board as it is
    // now set aside (SetAside: main RAM, the small state, and VRAM where the software blitter draws; the GPU's VRAM is
    // the port's), and later the board and a moment set aside changing places (Trade), so that a frame can be run on
    // from the moment while the game's own state waits, and they change back. Pages of translated code that differ
    // between the two are compared with their translations again, as at PutBack. `hold`: the sound chip stands still
    // (the game only writes to it), as in run-ahead's frames.
    struct Aside {
        std::vector<uint8_t> ram, small;
        std::vector<uint16_t> vram;
    };
    void SetAside(Aside &to);
    void Trade(Aside &with, bool hold);
    void SerializeSmall(Archive &ar);
    // A moment of play kept and gone back to (runtime/timeline.h): everything but main RAM and VRAM's pixels into
    // `out`; and the board set to such a moment, with the RAM it had then
    void KeepMoment(std::vector<uint8_t> &out);
    void SetMoment(std::vector<uint8_t> &moment, const uint8_t *ramThen);
    size_t RamSize() const {
        return boardD ? 0x1000000 : 0x800000;
    }

    // Bus
    uint32 Read(uint32 address, unsigned size) override;
    void Write(uint32 address, uint32 value, unsigned size) override;
    bool ReadBulk(uint32 address, uint8 *dst, uint32 count) override;
    uint16 Fetch(uint32 address) override;
    void Event() override;
    void ScheduleIrq1();
    uint8 PortRead(unsigned port) override;
    void PortWrite(unsigned port, uint8 value) override;

private:
    std::vector<uint8_t> m_kept[2], m_keptRam[2], m_traded;
    bool m_keptExact[2] = {};
    uint32_t m_keptPathsClock[2] = {}, m_keptPathsGeneration[2] = {}; // (the timing model's paths as they were: PutBack)
    uint8_t m_soundRegister = 0;
    uint64_t m_blitterDone = 0; // the CPU cycle the blitter is ready again at
    bool m_pollSeen = false;    // (SPELUNK_BLIT_LOG only: not state)
    uint64_t m_firstPoll = 0;
    uint64_t m_frameEnd = 0;
    uint8_t m_fpgaPort = 0;
    uint32_t m_fpgaBits = 0;
};

} // namespace spelunk
