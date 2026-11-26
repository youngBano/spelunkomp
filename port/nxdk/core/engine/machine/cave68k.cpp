// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The memory maps, the interrupts and the frame's timing are as MAME's cave.cpp describes them (BSD-3-Clause, Luca Elia).
#include "cave68k.h"

#include "../runtime/state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace spelunk {

namespace {
    // (the top byte of each part's address)
    constexpr Cave68k::Map kMaps[] = {
        //            ram   sound sprites layers                    sprite regs  layer regs                palette inputs eeprom irq (us)
        {"ddonpach", 0x10, 0x30, 0x40, {0x50, 0x60, 0x70, 0xFF}, 0x80, {0x90, 0xA0, 0xB0, 0xFF}, 0xC0, 0xD0, 0xE0, 90},
        // (DonPachi: its layers' memory in another order, the palette at A08000, two OKI chips and their pager at
        // B00000, B00010 and B00020)
        {"donpachi", 0x10, 0xB0, 0x50, {0x30, 0x20, 0x40, 0xFF}, 0x90, {0x70, 0x60, 0x80, 0xFF}, 0xA0, 0xC0, 0xD0, 90, 0x8000, 0, false,
         0xFF, true},
        // (Dangun Feveron: two layers, the palette at 708000, RAM at 710000 too)
        {"dfeveron", 0x10, 0x30, 0x40, {0x50, 0x60, 0xFF, 0xFF}, 0x80, {0x90, 0xA0, 0xFF, 0xFF}, 0x70, 0xB0, 0xC0, 100, 0x8000, 0, false, 0x71},
        {"esprade", 0x10, 0x30, 0x40, {0x50, 0x60, 0x70, 0xFF}, 0x80, {0x90, 0xA0, 0xB0, 0xFF}, 0xC0, 0xD0, 0xE0, 2000},
        // (Guwange: RAM at 200000, the inputs at D00010 with the EEPROM written there too)
        {"guwange", 0x20, 0x80, 0x40, {0x50, 0x60, 0x70, 0xFF}, 0x30, {0x90, 0xA0, 0xB0, 0xFF}, 0xC0, 0xD0, 0xFF, 2000, 0, 0x10, true},
        // (Mazinger Z: a Z80 for sound, its latches among the sprite chip's registers at 30006E; two layers, of 8 x 8
        // tiles only; the palette at C08000; a second interrupt as the blank ends, 2 ms after the first, and the
        // picture 2.1 ms after it; a second ROM at D00000; a watchdog at 300068)
        {"mazinger", 0x10, 0xFF, 0x20, {0x50, 0x40, 0xFF, 0xFF}, 0x30, {0x70, 0x60, 0xFF, 0xFF}, 0xC0, 0x80, 0x90, 2100, 0x8000, 0, false,
         0xFF, false, true, CaveZ80::Kind::Mazinger, true, 0xD0, false, false, false, {0xFF, 0xFF, 0xFF}, true},
        // (Air Gallet, on Sailor Moon's board: a Z80 for sound, its latches at B8006C and B8006E; the layers at
        // 800000, 880000 and 900000; the palette at 408000 with RAM under it; bit 0 of the EEPROM's byte pages
        // layer 2's tiles; the 68000 reset again a millisecond after power on)
        {"agallet", 0x10, 0xFF, 0x50, {0x80, 0x88, 0x90, 0xFF}, 0xB8, {0xA0, 0xA8, 0xB0, 0xFF}, 0x40, 0x60, 0x70, 100, 0x8000, 0, false,
         0xFF, false, true, CaveZ80::Kind::AirGallet, false, 0xFF, true, true, true, {0x11, 0x41, 0x51}},
    };
} // namespace

Cave68k::Cave68k()
    : cpu(*this)
    , ram(0x10000, 0)
    , m_picture(CaveVideo::kWidth * CaveVideo::kHeight, 0xFF000000u) {
    cpu.ram = ram.data();
}

bool Cave68k::SetBoard(const std::string &name) {
    for (const Map &m : kMaps) {
        if (name != m.name) {
            continue;
        }
        map = &m;
        std::memset(m_parts, 0, sizeof m_parts);
        auto put = [&](uint8_t top, Part part, uint8_t which = 0) {
            if (top != 0xFF) {
                m_parts[top] = part;
                m_which[top] = which;
            }
        };
        for (uint32_t top = 0; top < (program.size() + 0xFFFF) >> 16; top++) {
            put(static_cast<uint8_t>(top), Part::Program);
        }
        put(m.ram, Part::Ram);
        put(m.sound, Part::Sound);
        put(m.sprites, Part::Sprites);
        put(m.spriteRegs, Part::SpriteRegs);
        put(m.palette, Part::Palette);
        put(m.inputs, Part::Inputs);
        put(m.eeprom, Part::Eeprom);
        put(m.extraRam, Part::ExtraRam);
        extraRam.assign(m.extraRam != 0xFF || m.ramBelowPalette ? 0x10000 : 0, 0);
        if (m.extraRom != 0xFF) {
            for (uint32_t n = 0; n < (extraRom.size() + 0xFFFF) >> 16; n++) {
                put(static_cast<uint8_t>(m.extraRom + n), Part::ExtraRom, static_cast<uint8_t>(n));
            }
        }
        for (uint8_t n = 0; n < 3; n++) {
            put(m.wordsPast[n], Part::WordPast, n);
        }
        for (uint8_t n = 0; n < 4; n++) {
            put(m.layer[n], Part::Layer, n);
            put(m.layerRegs[n], Part::LayerRegs, n);
        }
        cpu.ramBase = static_cast<uint32_t>(m.ram) << 16;
        return true;
    }
    return false;
}

void Cave68k::Reset() {
    cpu.rom = program.data();
    cpu.romSize = static_cast<uint32_t>(program.size());
    cpu.cycles = 0;
    cpu.Reset(true);
    sound.rom = soundRom.data();
    sound.romSize = static_cast<uint32_t>(soundRom.size());
    sound.Reset();
    oki.rom[0] = soundRom.data();
    oki.romSize[0] = static_cast<uint32_t>(soundRom.size());
    oki.rom[1] = soundRom2.data();
    oki.romSize[1] = static_cast<uint32_t>(soundRom2.size());
    oki.Reset();
    if (map && map->z80) {
        z80.oki.rom[0] = soundRom.data();
        z80.oki.romSize[0] = static_cast<uint32_t>(soundRom.size());
        z80.oki.rom[1] = soundRom2.data();
        z80.oki.romSize[1] = static_cast<uint32_t>(soundRom2.size());
        z80.Reset(map->z80Kind);
    }
    video.tileBank = false;
    video.listBefore = 0;
    m_watchdogFed = 0;
    std::memset(m_wordsPast, 0, sizeof m_wordsPast);
    m_vblankIrq = m_otherIrq = false;
    m_frameStart = 0;
    frame = 0;
}

void Cave68k::UpdateIrq(uint64_t at) {
    const uint8_t level = m_vblankIrq || m_otherIrq || (!map->oki && sound.Irq()) ? 1 : 0;
    // (raised in the middle of a run, by a write to the sound chip: translated code stops at its next block, where
    // the interpreter would look for the interrupt too)
    if (level > cpu.InterruptLevel()) {
        cpu.budget = std::min(cpu.budget, cpu.cycles);
    }
    if (irqTrace && level != cpu.InterruptLevel()) [[unlikely]] {
        // (measurement: when the line changes, in the frame, and why: the blank's or the sound chip's)
        fprintf(irqTrace, "%llu %lld ipl%d v%d s%d\n", static_cast<unsigned long long>(frame), static_cast<long long>(at - m_frameStart), level,
                m_vblankIrq ? 1 : 0, sound.Irq() ? 1 : 0);
    }
    cpu.SetInterruptLevel(level, at);
}

void Cave68k::RunSound() {
    RunSoundTo(cpu.cycles);
}

void Cave68k::RunSoundTo(uint64_t cycles) {
    if (map->z80) {
        z80.RunTo(cycles);
    } else if (map->oki) {
        oki.RunTo(cycles, kCpuHz);
    } else {
        sound.RunTo(cycles, kCpuHz);
    }
}

void Cave68k::RunFrame() {
    // The interrupt comes a little before the blank; the sprite chip draws the list on show then
    m_vblankIrq = true;
    UpdateIrq(m_frameStart);
    const uint16_t bank = video.spriteRegs[4] & 3;
    pictures += bank != m_bankBefore || m_spriteWrites != 0;
    m_bankBefore = bank;
    m_spriteWrites = 0;
    if (!noPicture) {
        video.LatchSprites();
    }
    video.listBefore = bank; // (a sprite chip that shows a list a frame late: Mazinger Z's)
    if (map->z80) {
        RunFrameZ80();
        m_frameStart += frameCycles;
        frame++;
        return;
    }
    // The picture: the layer chips and the palette make each line as it is shown, from the blank's end (32 lines after
    // the interrupt) to the frame's end; the picture is made at once, with them as they are halfway down the screen
    // (its 120th line). Made at the blank's interrupt and 90 microseconds to 2 ms after it, as MAME's times have it,
    // it was before any line is shown: what a game changes after its interrupt's work (Guwange's title, Dangun
    // Feveron's stars) came a frame late against the Cave core (docs/CAVE68K.md, "Pictures against the core")
    const uint64_t blank = m_frameStart + blankCycles + CaveVideo::kHeight / 2 * 1024; // (1,024 cycles a line)
    bool drawn = false;
    auto run = [&](uint64_t until, bool exact) {
        if (!drawn && blank <= until) {
            cpu.Run(blank, mode, exact); // (exactly where an interrupt follows: not past it to a block's end)
            if (!noPicture) {
                video.Draw(m_picture.data());
            }
            drawn = true;
        }
        cpu.Run(until, mode, exact);
    };
    // The frame in steps of about a millisecond: at each the sound chip is brought up to the time, and a voice that
    // has ended since raises its interrupt
    for (unsigned step = 1; step <= kSoundSteps; step++) {
        const uint64_t to = m_frameStart + frameCycles * step / kSoundSteps;
        // (a voice of the YMZ280B that ends before then raises its interrupt at its own time, the picture's 2 ms
        // included)
        if (extraIrqAt != ~0ull && m_frameStart + extraIrqAt < to) [[unlikely]] {
            const uint64_t at = m_frameStart + extraIrqAt;
            if (cpu.cycles < at) {
                run(at, true);
            }
            m_vblankIrq = true;
            UpdateIrq(at);
            extraIrqAt = ~0ull;
        }
        for (uint64_t at; !map->oki && (at = sound.NextIrq(to, kCpuHz)) < to;) {
            if (cpu.cycles < at) {
                run(at, true);
            }
            RunSoundTo(at);
            UpdateIrq(at);
        }
        // (to the instruction the next frame's interrupt comes in; and exactly too when a voice ends in the next step:
        // translated code run past `to` to its next block could otherwise pass the interrupt's time, which is only
        // looked for in that step, and take it late, where the interpreter stops at `to` and takes it on time)
        const bool soundSoon = !map->oki && sound.NextIrq(to + frameCycles / kSoundSteps, kCpuHz) < to + frameCycles / kSoundSteps;
        run(to, step == kSoundSteps || soundSoon);
        RunSoundTo(to);
        UpdateIrq(to);
    }
    m_frameStart += frameCycles;
    frame++;
}

// A frame of a board with a Z80 for sound: the same frame, in more steps (the Z80's answers are what the game waits
// for), the things of the frame's beginning each at its own time between them
void Cave68k::RunFrameZ80() {
    constexpr uint64_t kNever = ~0ull;
    if (map->watchdog && cpu.cycles - m_watchdogFed >= kWatchdogCycles) {
        // The watchdog resets the board: both CPUs and the sound chips start again; the memories keep what they hold
        cpu.Reset(true);
        z80.Reset(map->z80Kind, cpu.cycles);
        m_vblankIrq = true;
        m_otherIrq = false;
        UpdateIrq();
        m_watchdogFed = cpu.cycles;
    }
    // (Air Gallet's board resets the 68000 again a millisecond after power on)
    uint64_t resetAt = map->startupReset && frame == 0 ? m_frameStart + kCpuHz / 1000 : kNever;
    uint64_t blankEndAt = map->blankEndIrq ? m_frameStart + blankCycles : kNever;
    uint64_t pictureAt = m_frameStart + map->irqMicroseconds * (kCpuHz / 1000000);
    for (unsigned step = 1; step <= kZ80Steps; step++) {
        const uint64_t to = m_frameStart + frameCycles * step / kZ80Steps;
        for (;;) {
            const uint64_t at = std::min({resetAt, blankEndAt, pictureAt});
            if (at > to) {
                break;
            }
            cpu.Run(at, mode, at == blankEndAt || at == resetAt); // (exact to the interrupt and the reset: where the board has them)
            if (at == resetAt) {
                cpu.Reset(false);
                resetAt = kNever;
            } else if (at == blankEndAt) {
                m_otherIrq = true;
                UpdateIrq(at);
                blankEndAt = kNever;
            } else {
                if (!noPicture) {
                    video.Draw(m_picture.data());
                }
                pictureAt = kNever;
            }
        }
        cpu.Run(to, mode, step == kZ80Steps);
        RunSoundTo(to);
    }
}

uint16_t Cave68k::Fetch16(uint32_t address) {

    if (address + 1 < program.size()) {
        return static_cast<uint16_t>((program[address] << 8) | program[address + 1]);
    }
    return Read16(address);
}

uint16_t Cave68k::Read16(uint32_t address) {
    address &= 0xFFFFFE;
    if (sideRun && m_side.run != sideRuns) {
        m_side = {sideRuns, m_vblankIrq, m_otherIrq, false};
    }
    const uint8_t top = static_cast<uint8_t>(address >> 16);
    const uint32_t low = address & 0xFFFF;
    auto word = [](const std::vector<uint8_t> &memory, uint32_t at) { return static_cast<uint16_t>((memory[at] << 8) | memory[at + 1]); };
    switch (m_parts[top]) {
    case Part::Program: return address + 1 < program.size() ? word(program, address) : 0xFFFF;
    case Part::ExtraRom: {
        const uint32_t at = (static_cast<uint32_t>(m_which[top]) << 16) | low;
        return at + 1 < extraRom.size() ? word(extraRom, at) : 0xFFFF;
    }
    case Part::Ram: return word(ram, low);
    case Part::Sound: {
        if (map->oki) {
            // (the OKI chips' status: a bit for each voice playing; nothing changes by reading it)
            return low == 0 || low == 2 ? 0xFF00 | oki.Read(0, cpu.ReadTime())
                   : low == 0x10 || low == 0x12 ? 0xFF00 | oki.Read(1, cpu.ReadTime()) : 0xFFFF;
        }
        if (sideRun) {
            // (the checker's side run changes nothing: what it has read back already is kept beside)
            const unsigned offset = (low >> 1) & 1;
            const uint8_t value = offset && m_side.statusRead ? 0 : sound.Read(offset, true);
            m_side.statusRead = m_side.statusRead || offset;
            return 0xFF00 | value;
        }
        // (the chip is brought up to the CPU's time at fixed steps of the frame and when written, not when read: what
        // is read is then the same however the code is run)
        const uint8_t value = sound.Read((low >> 1) & 1, false);
        UpdateIrq();
        return 0xFF00 | value;
    }
    case Part::Sprites: return word(video.spriteRam, low);
    case Part::Layer: {
        const int n = m_which[top];
        // (a layer of 8 x 8 tiles only has its 16 KB again and again through the 64 KB; a whole layer's 32 KB end there)
        if (video.kinds[n].only8) {
            return word(video.layerRam[n], 0x4000 + (low & 0x3FFF));
        }
        if (low >= 0x8000) {
            return low == 0x8000 && map->wordsPast[0] != 0xFF ? m_wordsPast[4 + n] : 0xFFFF;
        }
        return word(video.layerRam[n], low);
    }
    case Part::SpriteRegs: {
        // What interrupts are waiting (a 0 bit: that one); reading at 4 takes the vertical blank's back
        if (low < 8) {
            bool &vblank = sideRun ? m_side.vblankIrq : m_vblankIrq, &other = sideRun ? m_side.otherIrq : m_otherIrq;
            uint16_t value = static_cast<uint16_t>(0x0003 ^ (vblank ? 1 : 0) ^ (other ? 2 : 0));
            // (at 0, bit 2 is low for the 2 ms of the blank: DoDonPachi tells the blank's interrupt from the sound
            // chip's by it)
            // (when the bus reads it; an instruction that runs past the frame's end reads in the next frame's blank:
            // ESP Ra.De. waits for the blank with `btst #2,$800001`)
            if (low == 0 && (cpu.ReadTime() - m_frameStart) % frameCycles >= blankCycles) {
                value |= 4;
            }
            if (low == 4) {
                vblank = false;
            } else if (low == 6) {
                other = false;
            }
            if (!sideRun) {
                UpdateIrq();
            }
            return value;
        }
        if (map->z80 && low == 0x6E) {
            // The Z80's oldest answer, taken (none: FF). Nothing is run for it: what is read is the same however the
            // code is run
            if (sideRun) {
                const uint8_t value = z80.Answer(m_side.answersRead);
                m_side.answersRead += z80.AnswerWaits(m_side.answersRead);
                return value;
            }
            const uint8_t value = z80.Answer();
            z80.TakeAnswer();
            return value;
        }
        if (map->z80 && low == 0x6C && map->z80Kind == CaveZ80::Kind::AirGallet) {
            // (bit 1 low: an answer waits)
            return z80.AnswerWaits(sideRun ? m_side.answersRead : 0) ? 0 : 2;
        }
        return 0xFFFF;
    }
    case Part::LayerRegs: return low < 6 ? video.layerRegs[m_which[top]][low >> 1] : 0xFFFF;
    case Part::Palette:
        return low >= map->paletteAt ? word(video.paletteRam, low - map->paletteAt) : map->ramBelowPalette ? word(extraRam, low) : 0xFFFF;
    case Part::ExtraRam: return word(extraRam, low);
    case Part::WordPast: return low == 0 ? m_wordsPast[m_which[top]] : 0xFFFF;
    case Part::Inputs: {
        if (map->guwangeIo) {
            return GuwangeInputs(low);
        }
        if (low == 0) {
            if (onInputRead && !sideRun) {
                onInputRead(onInputReadContext);
            }
            // Player 1's stick, buttons, start and coin; the test switch (0: held)
            const uint32_t system = this->system | systemHeld;
            uint16_t held = static_cast<uint16_t>(inputs[0] & 0x7F);
            held |= (system & kSysStart1) ? 0x0080 : 0;
            held |= (system & kSysCoin1) ? 0x0100 : 0;
            held |= (system & kSysTest) ? 0x0200 : 0;
            return static_cast<uint16_t>(~held);
        }
        if (low == 2) {
            // Player 2's; the service coin; the EEPROM's data line (as it is)
            const uint32_t system = this->system | systemHeld;
            uint16_t held = static_cast<uint16_t>(inputs[1] & 0x7F);
            held |= (system & kSysStart2) ? 0x0080 : 0;
            held |= (system & kSysCoin2) ? 0x0100 : 0;
            held |= (system & kSysService) ? 0x0200 : 0;
            return static_cast<uint16_t>((~held & 0xF7FF) | (eeprom.DataOut() ? 0x0800 : 0));
        }
        return 0xFFFF;
    }
    default: break;
    }
    return 0xFFFF;
}

uint16_t Cave68k::GuwangeInputs(uint32_t low) {
    const uint32_t system = this->system | systemHeld;
    if (low == map->inputsAt) {
        // Both players: start, the stick (up, down, left, right), three buttons; player 2's from bit 8
        if (onInputRead && !sideRun) {
            onInputRead(onInputReadContext);
        }
        uint16_t held = static_cast<uint16_t>(((inputs[0] & 0x7F) << 1) | ((inputs[1] & 0x7F) << 9));
        held |= (system & kSysStart1) ? 0x0001 : 0;
        held |= (system & kSysStart2) ? 0x0100 : 0;
        return static_cast<uint16_t>(~held);
    }
    if (low == map->inputsAt + 2u) {
        // The coins, the test and service switches; the EEPROM's data line (as it is)
        uint16_t held = 0;
        held |= (system & kSysCoin1) ? 0x0001 : 0;
        held |= (system & kSysCoin2) ? 0x0002 : 0;
        held |= (system & kSysTest) ? 0x0004 : 0;
        held |= (system & kSysService) ? 0x0008 : 0;
        return static_cast<uint16_t>((~held & 0xFF7F) | (eeprom.DataOut() ? 0x0080 : 0));
    }
    return 0xFFFF;
}

uint8_t Cave68k::Read8(uint32_t address) {
    address &= 0xFFFFFF;
    const uint8_t top = static_cast<uint8_t>(address >> 16);
    switch (m_parts[top]) {
    case Part::Program: return address < program.size() ? program[address] : 0xFF;
    case Part::Ram: return ram[address & 0xFFFF];
    default: break;
    }
    const uint16_t word = Read16(address);
    return static_cast<uint8_t>((address & 1) ? word : word >> 8);
}

void Cave68k::WriteByteOrWord(uint32_t address, uint16_t value, uint16_t mask) {
    const uint8_t top = static_cast<uint8_t>(address >> 16);
    const uint32_t low = address & 0xFFFE;
    auto put = [&](std::vector<uint8_t> &memory, uint32_t at) {
        if (mask & 0xFF00) {
            memory[at] = static_cast<uint8_t>(value >> 8);
        }
        if (mask & 0x00FF) {
            memory[at + 1] = static_cast<uint8_t>(value);
        }
    };
    auto combine = [&](uint16_t &reg) { reg = static_cast<uint16_t>((reg & ~mask) | (value & mask)); };
    switch (m_parts[top]) {
    case Part::Ram: put(ram, low); break;
    case Part::Sound:
        if (map->oki) {
            // (the first chip at 0, the second at 0x10, the NMK112's eight page registers from 0x20)
            if (mask & 0x00FF) {
                RunSound();
                const uint8_t byte = static_cast<uint8_t>(value);
                if (low < 4) {
                    oki.Write(0, byte, cpu.WriteTime());
                } else if (low >= 0x10 && low < 0x14) {
                    oki.Write(1, byte, cpu.WriteTime());
                } else if (low >= 0x20 && low < 0x30) {
                    oki.SetBank((low - 0x20) >> 1, byte);
                }
            }
            break;
        }
        if (mask & 0x00FF) {
            const uint64_t at = cpu.WriteTime(); // (when the bus makes the write)
            RunSoundTo(at);
            sound.Write((low >> 1) & 1, static_cast<uint8_t>(value));
            UpdateIrq(at);
        }
        break;
    case Part::Sprites:
        put(video.spriteRam, low);
        m_spriteWrites++;
        break;
    case Part::Layer: {
        const int n = m_which[top];
        if (video.kinds[n].only8) {
            put(video.layerRam[n], 0x4000 + (low & 0x3FFF));
        } else if (low < 0x8000) {
            put(video.layerRam[n], low);
        } else if (low == 0x8000) {
            combine(m_wordsPast[4 + n]);
        }
        break;
    }
    case Part::SpriteRegs:
        if (low < 0x80) {
            combine(video.spriteRegs[low >> 1]);
        }
        if (map->watchdog && low == 0x68) {
            m_watchdogFed = cpu.cycles;
        }
        if (map->z80 && low == 0x6E) {
            // A command for the Z80: its side brought up to now, the command given it, and time for it to answer (as
            // long as MAME holds the 68000 for it here; the 68000 is not held: only the Z80's side goes ahead)
            RunSound();
            z80.Command(value & mask);
            RunSoundTo(cpu.cycles + kCommandCycles);
        }
        break;
    case Part::LayerRegs:
        if (low < 6) {
            combine(video.layerRegs[m_which[top]][low >> 1]);
        }
        break;
    case Part::Palette:
        if (low >= map->paletteAt) {
            put(video.paletteRam, low - map->paletteAt);
        } else if (map->ramBelowPalette) {
            put(extraRam, low);
        }
        break;
    case Part::ExtraRam: put(extraRam, low); break;
    case Part::WordPast:
        if (low == 0) {
            combine(m_wordsPast[m_which[top]]);
        }
        break;
    case Part::Inputs:
        // (Guwange's EEPROM, at the low byte of its first input word: the data in, the clock, the select)
        if (map->guwangeIo && low == map->inputsAt && (mask & 0x00FF)) {
            const uint8_t lines = static_cast<uint8_t>(value);
            eeprom.SetLines(lines & 0x20, lines & 0x40, lines & 0x80);
        }
        break;
    case Part::Eeprom:
        // (the high byte: the data in, the clock, the select; the coin counters and lockouts above them)
        if (low == 0 && (mask & 0xFF00)) {
            const uint8_t lines = static_cast<uint8_t>(value >> 8);
            eeprom.SetLines(lines & 2, lines & 4, lines & 8);
            if (map->tileBank) {
                video.tileBank = lines & 1;
            }
        }
        break;
    default: break;
    }
}

void Cave68k::Write16(uint32_t address, uint16_t value) {
    WriteByteOrWord(address & 0xFFFFFE, value, 0xFFFF);
}

void Cave68k::Write8(uint32_t address, uint8_t value) {
    address &= 0xFFFFFF;
    if (address & 1) {
        WriteByteOrWord(address, value, 0x00FF);
    } else {
        WriteByteOrWord(address, static_cast<uint16_t>(value << 8), 0xFF00);
    }
}

uint64_t Cave68k::StateHash() const {
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](const void *p, size_t size) {
        const uint8_t *bytes = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < size; i++) {
            h = (h ^ bytes[i]) * 0x100000001B3ull;
        }
    };
    mix(ram.data(), ram.size());
    mix(cpu.d, sizeof cpu.d);
    mix(cpu.a, sizeof cpu.a);
    mix(&cpu.pc, sizeof cpu.pc);
    const uint16_t sr = cpu.SR();
    mix(&sr, sizeof sr);
    mix(&cpu.cycles, sizeof cpu.cycles);
    mix(video.spriteRam.data(), video.spriteRam.size());
    for (const auto &layer : video.layerRam) {
        mix(layer.data(), layer.size());
    }
    mix(video.paletteRam.data(), video.paletteRam.size());
    mix(video.spriteRegs, sizeof video.spriteRegs);
    mix(video.layerRegs, sizeof video.layerRegs);
    mix(eeprom.words, sizeof eeprom.words);
    mix(extraRam.data(), extraRam.size());
    if (map && map->z80) {
        z80.Hash(mix);
    }
    return h;
}

void Cave68k::Serialize(Archive &ar) {
    ar.Mark(0x43363841);
    cpu.Serialize(ar);
    ar.Vec(ram);
    video.Serialize(ar);
    if (map && map->z80) {
        z80.Serialize(ar);
        ar.Pod(video.tileBank);
        ar.Pod(video.listBefore);
        ar.Pod(m_watchdogFed);
        ar.Pod(m_wordsPast);
    } else if (map && map->oki) {
        oki.Serialize(ar);
    } else {
        sound.Serialize(ar);
    }
    if (!extraRam.empty()) {
        ar.Vec(extraRam);
    }
    eeprom.Serialize(ar);
    ar.Pod(m_vblankIrq);
    ar.Pod(m_otherIrq);
    ar.Pod(m_frameStart);
    ar.Pod(m_bankBefore);
    ar.Pod(m_spriteWrites);
    ar.Pod(frame);
    ar.Pod(pictures);
    ar.Pod(systemHeld);
    ar.Pod(featureWords);
    if (!ar.Saving()) {
        UpdateIrq();
    }
}

void Cave68k::Redraw() {
    video.LatchSprites();
    video.Draw(m_picture.data());
}

void Cave68k::KeepNow(bool exact, int which) {
    m_kept[which].clear();
    Archive ar(m_kept[which], true);
    Serialize(ar);
    // (and the controls as the last frame left them, which a game's features read before the next frame reads its own:
    // free play's Start)
    ar.Pod(inputs);
    ar.Pod(system);
    sound.held = oki.held = z80.held = !exact;
}

void Cave68k::PutBack(int which) {
    sound.held = oki.held = z80.held = false;
    Archive ar(m_kept[which], false);
    Serialize(ar);
    ar.Pod(inputs);
    ar.Pod(system);
}

void Cave68k::SetAside(Aside &to) {
    to.state.clear();
    Archive ar(to.state, true);
    Serialize(ar);
    ar.Pod(inputs);
    ar.Pod(system);
    to.picture = m_picture;
}

void Cave68k::Trade(Aside &with, bool hold) {
    m_traded.clear();
    {
        Archive out(m_traded, true);
        Serialize(out);
        out.Pod(inputs);
        out.Pod(system);
    }
    Archive in(with.state, false);
    Serialize(in);
    in.Pod(inputs);
    in.Pod(system);
    with.state.swap(m_traded);
    m_picture.swap(with.picture);
    sound.held = oki.held = z80.held = hold;
}

} // namespace spelunk
