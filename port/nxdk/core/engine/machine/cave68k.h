// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// Cave's first arcade boards (1994 to 2001; DonPachi, DoDonPachi, ESP Ra.De., Dangun Feveron, Guwange and others):
// a 68000 at 16 MHz with 64 KB of work RAM, the sprite chip and tile layer chips of machine/cave_video.h, a
// YMZ280B for sound (machine/ymz280b.h; DonPachi's: two OKI chips, machine/okim6295.h; Mazinger Z's and Air Gallet's:
// a Z80 with an FM chip and OKI chips, machine/cave_z80.h) and a 93C46 EEPROM for the settings. Each game's board has the same parts at its own addresses: `Map` says where, by the board's name in
// game.cfg (`board = `).
//
// DoDonPachi's (board = ddonpach; the others' are in kMaps, machine/cave68k.cpp):
//   000000  the program (1 MB)            100000  work RAM (64 KB)
//   300000  the YMZ280B                   400000  the sprite lists (64 KB)
//   500000, 600000, 700000  the three layers' memory
//   800000  the sprite chip's registers; read: what interrupts are waiting
//   900000, A00000, B00000  the three layers' registers
//   C00000  the palette (64 KB)           D00000, D00002  the players' controls, the EEPROM's data line
//   E00000  the EEPROM's other lines
//
// The memory maps, the interrupts and the frame's timing are as MAME's cave.cpp describes them (BSD-3-Clause, Luca
// Elia): see THIRD_PARTY_NOTICES.md.
#pragma once

#include "cave_video.h"
#include "cave_z80.h"
#include "eeprom93c46.h"
#include "inputs.h"
#include "m68k.h"
#include "okim6295.h"
#include "ymz280b.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace spelunk {

class Archive;

class Cave68k : public M68KBus {
public:
    Cave68k();

    M68K cpu;
    CaveVideo video;
    Ymz280b sound;
    OkiPair oki; // (DonPachi's board, in place of the YMZ280B)
    Eeprom93c46 eeprom;
    std::vector<uint8_t> ram;      // 64 KB, big-endian as the board holds it
    std::vector<uint8_t> extraRam; // (Dangun Feveron's board: 64 KB more, at Map::extraRam)
    std::vector<uint8_t> program;  // as the CPU sees it (big-endian)
    std::vector<uint8_t> soundRom, soundRom2; // (the second: the second OKI chip's)
    CaveZ80 z80;                   // (Mazinger Z's and Air Gallet's boards: the sound side, its OKIs' ROMs soundRom and soundRom2)
    std::vector<uint8_t> extraRom; // (Mazinger Z's board: a second ROM of data, at Map::extraRom)
    // The sound made, 16 bits, one channel, at 44,100 a second (the YMZ280B's, the OKI chips' or the Z80 side's)
    std::vector<int16_t> &SoundOut() {
        return map && map->z80 ? z80.out : map && map->oki ? oki.out : sound.out;
    }

    // Where a game's board has its parts (the top byte of each address; 0xFF: the board has none)
    struct Map {
        const char *name;
        uint8_t ram, sound, sprites, layer[4], spriteRegs, layerRegs[4], palette, inputs, eeprom;
        uint32_t irqMicroseconds; // (the boards with a Z80) when the picture is made after the interrupt, as MAME has it
        uint16_t paletteAt = 0;   // where in its 64 KB the palette begins (the older boards': 0x8000, 4 KB)
        uint16_t inputsAt = 0;    // where in its 64 KB the two input words are
        bool guwangeIo = false;   // Guwange's: both players in the first word, coins in the second, the EEPROM written at the first's low byte
        uint8_t extraRam = 0xFF;  // 64 KB more of RAM (0xFF: none)
        bool oki = false;         // two OKI chips and an NMK112 at `sound` in place of a YMZ280B
        // The boards with a Z80 for sound (machine/cave_z80.h): the command latch and the Z80's answers are among the
        // sprite chip's registers (at 0x6E; whether an answer waits, at 0x6C)
        bool z80 = false;
        CaveZ80::Kind z80Kind = CaveZ80::Kind::Mazinger;
        bool blankEndIrq = false;      // a second interrupt as the blank ends, 2 ms after the first (Mazinger Z)
        uint8_t extraRom = 0xFF;       // a second ROM of data (Mazinger Z: 512 KB at D00000)
        bool ramBelowPalette = false;  // RAM in the palette's 64 KB below where the palette begins (Air Gallet)
        bool tileBank = false;         // bit 0 of the EEPROM's byte pages layer 2's first 64 K tiles (Air Gallet)
        bool startupReset = false;     // the board resets the 68000 again a millisecond after power on (Air Gallet)
        // A word of RAM just past the end of each memory (Air Gallet's memory check tests one word too many of each):
        // at the start of these 64 KB, and after each layer's 32 KB
        uint8_t wordsPast[3] = {0xFF, 0xFF, 0xFF};
        bool watchdog = false;         // a watchdog (written at the sprite chip's register 0x68) that resets the board (Mazinger Z)
    };
    const Map *map = nullptr;
    // Sets the board up as game.cfg's `board` names it; false if there is no such board
    bool SetBoard(const std::string &name);
    M68K::Mode mode = M68K::Mode::Interpreter;

    // After the set is loaded: starts the CPU at the program's reset vector
    void Reset();

    // What is held: each player's stick and buttons, and the coin, start and service switches
    uint32_t inputs[2] = {0, 0};
    uint32_t system = 0;
    // Switches a game's feature holds for the player (free play's Start), read with `system`; and a few words a
    // game's features may keep with the board's state (kept and put back with it, as run-ahead and rewind need)
    uint32_t systemHeld = 0;
    uint32_t featureWords[4] = {};
    // Called when the game reads the players' ports (the port's frontend polls the host's devices then, so what the
    // game gets is as new as it can be)
    void (*onInputRead)(void *context) = nullptr;
    void *onInputReadContext = nullptr;

    // One frame of the board's time (272 lines at 15,625 a second: 57.44 Hz, as nullobject probed on a Dangun
    // Feveron board for the Cave MiSTer core; MAME's 271.5 lines, 57.55 Hz, is marked unmeasured), from one vertical
    // blank's interrupt to the next. The sprite chip draws its list at the interrupt; the picture is made a little after, at the
    // blank itself, from the layers as they are then.
    void RunFrame();
    static constexpr uint64_t kCpuHz = 16000000;
    static constexpr uint64_t kFrameCycles = 278528; // (16 MHz x 272 / 15,625: 1,024 cycles a line)
    static constexpr uint64_t kFrameNanoseconds = 17408000;
    static constexpr uint64_t kBlankCycles = 32768; // (the 32 lines of the 272 not shown: the picture is 240 high)
    // The frame's and the blank's lengths as run (kFrameCycles, kBlankCycles unless a measurement asks otherwise:
    // headless68k's --frame-lines and --blank-lines, to run a frame as long as a reference's for a comparison)
    uint64_t frameCycles = kFrameCycles, blankCycles = kBlankCycles;
    // (measurement, headless68k's SPELUNK_TRACE_IRQ: a line each time the interrupt line changes)
    FILE *irqTrace = nullptr;
    // (measurement, headless68k's --extra-irq: in the next frame run, the blank's interrupt line raised once more this
    // many cycles into it, as a sound interrupt would come there; ~0: never)
    uint64_t extraIrqAt = ~0ull;
    static constexpr unsigned kSoundSteps = 16;
    // (a board with a Z80: its side brought up to the 68000's time this often, so its answers are never long in coming;
    // and given this much time at once when a command is written, for the answer the game reads straight after)
    static constexpr unsigned kZ80Steps = 64;
    static constexpr uint64_t kCommandCycles = 800; // (50 microseconds)
    // (Mazinger Z's watchdog: 3 seconds, MAME's guess. The game counts on it: at power on it marks its RAM and waits
    // to be reset)
    static constexpr uint64_t kWatchdogCycles = 3 * kCpuHz;
    uint64_t frame = 0;
    // Frames in which the game gave the sprite chip a new list (its own speed: one a frame when it keeps up)
    uint32_t pictures = 0;
    // The picture on show: 320 x 240, 32 bits a pixel, red in the low byte
    const uint32_t *PictureData() const {
        return m_picture.data();
    }
    void Picture(uint32_t *out) const {
        std::memcpy(out, m_picture.data(), m_picture.size() * 4);
    }
    // While set no picture is made (frames run only for what the game does: a game run unseen)
    bool noPicture = false;

    // A hash of what the game can see (work RAM, the CPU's registers, the video's memory, the EEPROM), for comparing two runs
    uint64_t StateHash() const;
    // (the picture on show and the sprite chip's buffer are not part of it: both are made anew every frame. After a
    // state is read back, Redraw makes the picture of the game as it is then)
    void Serialize(Archive &ar);
    void Redraw();
    // Run-ahead: the board as it is now kept (KeepNow), frames run on from there for their picture only, and the
    // board put back (PutBack). The sound chip stands still meanwhile.
    // (exact: the sound chips run on meanwhile; `which`: one of two moments kept at once: online play's rollback)
    void KeepNow(bool exact = false, int which = 0);
    void PutBack(int which = 0);
    // The input lag readout's looks (port/main.cpp), run a frame at a time between the game's own: the board as it is
    // now set aside (SetAside), and later the board and a moment set aside changing places (Trade), so that a frame
    // can be run on from the moment while the game's own state waits, and they change back. The picture on show goes
    // with it. `hold`: the sound chips stand still, as in run-ahead's frames (the moment's frames are not heard).
    struct Aside {
        std::vector<uint8_t> state;
        std::vector<uint32_t> picture;
    };
    void SetAside(Aside &to);
    void Trade(Aside &with, bool hold);

    // M68KBus
    uint8_t Read8(uint32_t address) override;
    uint16_t Read16(uint32_t address) override;
    void Write8(uint32_t address, uint8_t value) override;
    void Write16(uint32_t address, uint16_t value) override;
    uint16_t Fetch16(uint32_t address) override;
    // (the EEPROM's data line is read with the players' controls, its other lines written elsewhere)
    bool SameChip(uint32_t written, uint32_t read) const override {
        return ((written ^ read) & 0xFF0000) == 0 || (m_parts[(written >> 16) & 0xFF] == Part::Eeprom && m_parts[(read >> 16) & 0xFF] == Part::Inputs);
    }
    bool IsMemory(uint32_t address) const override {
        const Part part = m_parts[(address >> 16) & 0xFF];
        return part == Part::Ram || part == Part::ExtraRam || part == Part::Sprites || part == Part::Layer || part == Part::Palette;
    }

private:
    enum class Part : uint8_t { None, Program, Ram, Sound, Sprites, Layer, SpriteRegs, LayerRegs, Palette, Inputs, Eeprom, ExtraRam, ExtraRom, WordPast };
    Part m_parts[256] = {};
    uint8_t m_which[256] = {};
    std::vector<uint32_t> m_picture;
    std::vector<uint8_t> m_kept[2], m_traded;
    bool m_vblankIrq = false, m_otherIrq = false;
    // The block checker's side run reads without changing anything: what its own reads would have changed
    struct Side {
        uint32_t run = 0; // (the side run it is for)
        bool vblankIrq = false, otherIrq = false, statusRead = false;
        unsigned answersRead = 0; // (the Z80's answers it has read)
    } m_side;
    uint64_t m_frameStart = 0;
    uint16_t m_bankBefore = 0xFFFF;
    uint32_t m_spriteWrites = 0;
    uint16_t m_wordsPast[8] = {}; // (Map::wordsPast's, then the layers')
    uint64_t m_watchdogFed = 0; // (the 68000's time when the watchdog was last written)
    void UpdateIrq(uint64_t at);
    void UpdateIrq() {
        UpdateIrq(cpu.cycles);
    }
    void RunSound();
    void RunFrameZ80();
    void RunSoundTo(uint64_t cycles);
    uint16_t GuwangeInputs(uint32_t low);
    void WriteByteOrWord(uint32_t address, uint16_t value, uint16_t mask);
};

} // namespace spelunk
