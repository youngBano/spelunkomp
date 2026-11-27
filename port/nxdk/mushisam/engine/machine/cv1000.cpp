// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
#include "cv1000.h"

#include <cstdio>
#include <cstdlib>

namespace spelunk {

namespace {
    FILE *const g_blitLog = getenv("SPELUNK_BLIT_LOG") ? fopen(getenv("SPELUNK_BLIT_LOG"), "w") : nullptr;

    inline uint64_t ReadTsc() {
#if defined(__i386__) || defined(__x86_64__)
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return 0;
#endif
    }
}

Cv1000::Cv1000()
    : ram(SH3::kRamSize, 0)
#ifdef XBOX_CV1000_B
    , program(0x200000, 0xFF)
#else
    , program(0x400000, 0xFF)
#endif
#ifdef NXDK
    , soundRom()
#else
    , soundRom(0x800000, 0xFF)
#endif
{
    blitter.keepStats = g_blitLog != nullptr;
    cpu.ram = ram.data();
    cpu.bus = this;
    blitter.ram = ram.data();
}

void Cv1000::Reset() {
    blitter.ramMask = boardD ? 0x00FFFFFFu : 0x007FFFFFu;
    cpu.Reset();
    cpu.cycles = 0;
    if (!soundRom.empty()) {
        sound.rom = soundRom.data();
        sound.romSize = static_cast<uint32_t>(soundRom.size());
    }
    sound.Reset();
    m_blitterDone = 0;
    m_busyPollCount = 0;
    m_frameEnd = 0;
    frame = 0;
    cpu.eventAt = ~0ull;
}

void Cv1000::RunFrame() {
    profSh3Tsc = 0;
    profAudioTsc = 0;
    profBlitterTsc = 0;
    // The vertical sync pulse, on the CPU's IRQ2 pin
    cpu.RaiseIrq(2);
    m_frameEnd += kFrameCycles;
    const uint64_t tCpu0 = ReadTsc();
    cpu.Run(m_frameEnd);
    const uint64_t tCpu1 = ReadTsc();
    const uint64_t cpuTotal = tCpu1 - tCpu0;
    const uint64_t subTsc = profBlitterTsc + profAudioTsc;
    profSh3Tsc = (cpuTotal > subTsc) ? (cpuTotal - subTsc) : 0;
#ifndef XBOX_AUDIO_OFF
    if (!onSoundWrite) {
        const uint64_t tAud0 = ReadTsc();
        sound.RunTo(cpu.cycles, kCpuHz);
        profAudioTsc += ReadTsc() - tAud0;
    }
#endif
    frame++;
#ifdef SPELUNK_SHADOW_TIMING
    cpu.ShadowCheckWhole("the frame's end");
#endif
}

// The blitter has run its list (Write: the CPU's eventAt is m_blitterDone only while a list runs, with the feature on)
void Cv1000::Event() {
    if (cpu.timing) {
        cpu.RaiseIrq(1);
    }
}

// After a state is loaded (and when one is kept: the same): the IRQ1 of a list that was still running then. One that was
// due by then has been raised already (SH3::Run raises a due event before it returns), so the list's end alone says it
// (not state of its own)
void Cv1000::ScheduleIrq1() {
    cpu.eventAt = cpu.timing && m_blitterDone > cpu.cycles ? m_blitterDone : ~0ull;
}

uint16 Cv1000::Fetch(uint32 address) {
    if (address < 0x400000 && !program.empty()) {
        const uint32 mask = static_cast<uint32>(program.size() - 1);
        return static_cast<uint16>((program[address & mask] << 8) | program[(address + 1) & mask]);
    }
    static int reported = 0;
    if (reported++ < 4) {
        fprintf(stderr, "cv1000: fetch from %08X\n", address);
    }
    return 0x0009; // (nop)
}

bool Cv1000::ReadBulk(uint32 address, uint8 *dst, uint32 count) {
    if (address == 0x10000000u) {
        return nand.ReadBlock(dst, count);
    }
    return false;
}

uint32 Cv1000::Read(uint32 address, unsigned size) {
    if (address < 0x400000) {
        if (program.empty()) return 0xFFFFFFFFu;
        const uint32 mask = static_cast<uint32>(program.size() - 1);
        uint32 v = 0;
        for (unsigned i = 0; i < size; i++) {
            v = (v << 8) | program[(address + i) & mask];
        }
        return v;
    }
    if (address >= 0x18000000u && address < 0x18000058u) {
        const uint32 offset = (address - 0x18000000u) & ~3u;
        if (offset == 0x10 && blitter.busy && g_blitLog && !m_pollSeen) {
            m_pollSeen = true;
            m_firstPoll = cpu.cycles;
        }
        if (offset == 0x10 && blitter.busy) {
            if (cpu.cycles >= m_blitterDone) {
                blitter.busy = false;
                m_busyPollCount = 0;
            } else if (++m_busyPollCount >= 2u && !cpu.interruptReady) {
                // O(1) blitter busy-wait fast-forward: skips tight SH-3 poll spinloop while
                // preserving exact CV1000 slowdown timing (stops at min(m_blitterDone, blockStop)).
                const uint64_t target = (m_blitterDone < cpu.blockStop) ? m_blitterDone : cpu.blockStop;
                if (target > cpu.cycles) {
                    cpu.cycles = target;
                }
                if (cpu.cycles >= m_blitterDone) {
                    blitter.busy = false;
                    m_busyPollCount = 0;
                }
            }
        }
        const uint32 v = blitter.Read(offset);
        // (a narrower read takes its bytes from the 32-bit register, high byte first)
        const unsigned shift = (4 - size - (address & 3)) * 8;
        return size == 4 ? v : (v >> shift) & ((1u << (size * 8)) - 1);
    }
    if (address >= 0x10000000u && address < 0x10000008u) {
        return address == 0x10000000u ? nand.Read() : 0xFF;
    }
    if (address >= 0x10C00000u && address < 0x10C00008u) {
        return address == 0x10C00001u ? (0xFE | (rtc.ReadData() ? 1 : 0)) : 0;
    }
    if (getenv("SPELUNK_TRACE_BUS")) {
        fprintf(stderr, "cv1000: read %08X (%u) pc %08X\n", address, size, cpu.PC);
    }
    return 0;
}

void Cv1000::Write(uint32 address, uint32 value, unsigned size) {
    if (address >= 0x18000000u && address < 0x18000058u) {
        const uint32_t regOff = (address - 0x18000000u) & ~3u;
        blitter.started = false;
        const bool starting = (regOff == 0x04u) && ((value & 1u) != 0);
        if (starting) {
            blitter.simulateSlowdown = !noSlowdown;
        }
        const uint64_t tBlit0 = starting ? ReadTsc() : 0;
        blitter.Write(regOff, value);
        if (starting) {
            profBlitterTsc += ReadTsc() - tBlit0;
        }
        if (blitter.started) {
            m_busyPollCount = 0;
            // (its uploads took the bus from the CPU; it is busy for the time the list takes)
            const uint64_t prevDone = m_blitterDone;
            if (noSlowdown) {
                m_blitterDone = cpu.cycles;
                blitter.busy = false;
            } else {
                cpu.cycles += blitter.lastStolenCpuCycles;
                const uint64_t busyNs = blitter.lastBusyNs + (cpu.timing ? Blitter::kListStartNs : 0);
                m_blitterDone = cpu.cycles + busyNs * kCpuHz / 1000000000ull;
            }
            // (the blitter raises IRQ1 when it has run the list's end: with "Slowdown as the board" on, as the board
            // does; docs/SLOWDOWN.md, "The blitter's interrupt")
            if (cpu.timing) {
                cpu.Schedule(m_blitterDone);
            }
            if (g_blitLog) {
                // frame, list start (cycles into the frame), busy cycles, stolen, previous list's first wait poll and its
                // end (cycles into this frame; -1 none), then the list's make-up
                const int64_t frameStart = static_cast<int64_t>(m_frameEnd - kFrameCycles);
                const Blitter::ListStats &st = blitter.stats;
                fprintf(g_blitLog, "%llu %lld %llu %llu %lld %lld %u %u %u %u %u %u %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
                        (unsigned long long)frame, (long long)cpu.cycles - frameStart, (unsigned long long)(m_blitterDone - cpu.cycles),
                        (unsigned long long)blitter.lastStolenCpuCycles, m_pollSeen ? (long long)m_firstPoll - frameStart : -1LL,
                        (long long)prevDone - frameStart, st.draws, st.drawn, st.blended, st.overLeftTop, st.overRightBottom, st.big,
                        (unsigned long long)st.pixels, (unsigned long long)st.pixelClocks, (unsigned long long)st.rowClocks,
                        (unsigned long long)st.fixedClocks, (unsigned long long)st.blendClocks, (unsigned long long)st.overLeftTopClocks,
                        (unsigned long long)st.overRightBottomClocks, (unsigned long long)st.bigClocks, (unsigned long long)st.offscreenPixels,
                        (unsigned long long)st.uploadNs, (unsigned long long)st.idleNs, (unsigned long long)st.lineFetchNs);
                m_pollSeen = false;
            }
        }
        return;
    }
    if (address >= 0x10000000u && address < 0x10000008u) {
        switch (address & 7) {
        case 0: nand.Write(static_cast<uint8_t>(value)); break;
        case 1: nand.Command(static_cast<uint8_t>(value)); break;
        case 2: nand.Address(static_cast<uint8_t>(value)); break;
        default: break;
        }
        return;
    }
    if (address >= 0x10400000u && address < 0x10400008u) {
        // (the register's number, then its value; the chip is brought up to now first, so a write lands on its sample)
        if ((address & 1) == 0) {
            m_soundRegister = static_cast<uint8_t>(value);
        } else {
            if (onSoundWrite) {
                onSoundWrite(onSoundWriteContext, m_soundRegister, static_cast<uint8_t>(value));
            } else if (!sound.held) {
#ifndef XBOX_AUDIO_OFF
                const uint64_t tAud0 = ReadTsc();
                sound.RunTo(cpu.cycles, kCpuHz);
                profAudioTsc += ReadTsc() - tAud0;
#endif
                sound.WriteRegister(m_soundRegister, static_cast<uint8_t>(value));
            }
        }
        return;
    }
    if (address >= 0x10C00000u && address < 0x10C00008u) {
        if (address == 0x10C00001u) {
            // (data, clock, chip select)
            rtc.SetSelect(value & 4);
            rtc.SetData(value & 1);
            rtc.SetClock(value & 2);
        }
        return; // (0x10C00003: the NAND's chip enable)
    }
    if (address < 0x400000) {
        return;
    }
    if (getenv("SPELUNK_TRACE_BUS")) {
        fprintf(stderr, "cv1000: write %08X = %X (%u) pc %08X\n", address, value, size, cpu.PC);
    }
}

uint8 Cv1000::PortRead(unsigned port) {
    switch (port) {
    case 2: // C: coins, starts, service (low while held)
        if (onInputRead) {
            onInputRead(onInputReadContext);
        }
        return static_cast<uint8>(~system & 0xFF);
    case 3: // D: player 1
        if (onInputRead) {
            onInputRead(onInputReadContext);
        }
        return static_cast<uint8>(~inputs[0] & 0xFF);
    case 4: // E: the NAND's ready line (bit 5); it is always ready here
        return 0xFF;
    case 5: // F: the test push button (bit 1)
        return (system & kSysTestButton) ? 0xFD : 0xFF;
    case 10: // L: player 2
        return static_cast<uint8>(~inputs[1] & 0xFF);
    default:
        return 0xFF;
    }
}

void Cv1000::PortWrite(unsigned port, uint8 value) {
    if (port == 8) {
        // J: the FPGA's configuration, a bit at a time (chip enable 0x08, clock 0x10, data 0x20). The blitter here is
        // not made from it; the bits are only counted.
        if ((value & 0x08) && !(m_fpgaPort & 0x10) && (value & 0x10)) {
            m_fpgaBits++;
        }
        m_fpgaPort = value;
    }
}

uint64_t Cv1000::StateHash() const {
    // FNV-1a over main RAM, 8 bytes at a time, then the registers
    uint64_t h = 0xCBF29CE484222325ull;
    const size_t size = boardD ? 0x1000000 : 0x800000;
    for (size_t i = 0; i < size; i += 8) {
        uint64_t v;
        std::memcpy(&v, &ram[i], 8);
        h = (h ^ v) * 0x100000001B3ull;
    }
    for (uint32 r : cpu.R) {
        h = (h ^ r) * 0x100000001B3ull;
    }
    h = (h ^ cpu.PC) * 0x100000001B3ull;
    h = (h ^ cpu.GetSR()) * 0x100000001B3ull;
    h = (h ^ cpu.cycles) * 0x100000001B3ull;
    for (uint16_t w : rtc.eeprom) {
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

// Everything of the board but main RAM, VRAM's pixels and the sound chip
void Cv1000::SerializeSmall(Archive &ar) {
    cpu.Serialize(ar);
    blitter.SerializeRegisters(ar);
    nand.Serialize(ar);
    rtc.Serialize(ar);
    ar.Pod(inputs);
    ar.Pod(system);
    ar.Pod(frame);
    ar.Pod(m_soundRegister);
    ar.Pod(m_blitterDone);
    ar.Pod(m_frameEnd);
    ar.Pod(m_fpgaPort);
    ar.Pod(m_fpgaBits);
    ScheduleIrq1();
}

void Cv1000::KeepNow(bool exact, int which) {
    const size_t ramSize = boardD ? 0x1000000 : 0x800000;
    m_keptRam[which].resize(ramSize);
    std::memcpy(m_keptRam[which].data(), ram.data(), ramSize);
    m_kept[which].clear();
    Archive ar(m_kept[which], true);
    SerializeSmall(ar);
    if (exact) {
        sound.Serialize(ar);
        sound.KeepDecoders(which);
    }
    m_keptExact[which] = exact;
#ifndef XBOX_FAST_CPU
    m_keptPathsClock[which] = cpu.timingPaths.clock;
    m_keptPathsGeneration[which] = cpu.timingPaths.generation;
#endif
    blitter.BeginJournal(which);
    sound.held = !exact;
}

void Cv1000::PutBack(int which, bool again) {
    std::vector<uint8_t> &keptRam = m_keptRam[which];
    const size_t ramSize = keptRam.size();
    // (a page of translated code that was written meanwhile is compared with its translations again)
    for (size_t page = 0; page < ramSize >> 12; page++) {
        if (cpu.codeMark[page] && std::memcmp(&ram[page << 12], &keptRam[page << 12], 0x1000) != 0) {
            ++cpu.pageEpoch[page];
        }
    }
    if (again || keptRam.size() != ram.size()) {
        std::memcpy(ram.data(), keptRam.data(), ramSize);
    } else {
        // (the two change places: what the frames run on wrote is overwritten by the next KeepNow)
        ram.swap(keptRam);
        cpu.ram = ram.data();
        blitter.ram = ram.data();
    }
    Archive ar(m_kept[which], false);
    cpu.keepEpochs = true;
    cpu.keepPaths = true; // (the timing model's paths are the code's, not the state's: kept, but those over RAM written since)
    SerializeSmall(ar);
    cpu.keepEpochs = false;
    cpu.keepPaths = false;
#ifndef XBOX_FAST_CPU
    cpu.timingPaths.PutBack(m_keptPathsClock[which], m_keptPathsGeneration[which]);
#endif
    if (m_keptExact[which]) {
        sound.Serialize(ar);
        sound.PutBackDecoders(which);
    }
    blitter.Rollback(which);
    sound.held = false;
}

void Cv1000::Drop(int which) {
    blitter.EndJournal(which);
}

void Cv1000::SetAside(Aside &to) {
    to.ram.assign(ram.begin(), ram.begin() + static_cast<std::ptrdiff_t>(RamSize()));
    to.small.clear();
    Archive ar(to.small, true);
    SerializeSmall(ar);
    if (blitter.rasterize) {
        to.vram = blitter.vram;
    } else {
        to.vram.clear();
    }
}

void Cv1000::Trade(Aside &with, bool hold) {
    const size_t ramSize = with.ram.size();
    // (a page of translated code that differs between the two is compared with its translations again; the RAM
    // changes places a part at a time, through a small buffer)
    uint8_t part[0x1000];
    for (size_t page = 0; page < ramSize >> 12; page++) {
        uint8_t *here = &ram[page << 12], *there = &with.ram[page << 12];
        if (cpu.codeMark[page] && std::memcmp(here, there, 0x1000) != 0) {
            ++cpu.pageEpoch[page];
        }
        std::memcpy(part, here, 0x1000);
        std::memcpy(here, there, 0x1000);
        std::memcpy(there, part, 0x1000);
    }
    m_traded.clear();
    {
        Archive out(m_traded, true);
        SerializeSmall(out);
    }
    Archive in(with.small, false);
    cpu.keepEpochs = true;
    SerializeSmall(in);
    cpu.keepEpochs = false;
    with.small.swap(m_traded);
    if (!with.vram.empty()) {
        blitter.vram.swap(with.vram);
    }
    sound.held = hold;
}

void Cv1000::KeepMoment(std::vector<uint8_t> &out) {
    out.clear();
    Archive ar(out, true);
    SerializeSmall(ar);
    sound.Serialize(ar);
}

void Cv1000::SetMoment(std::vector<uint8_t> &moment, const uint8_t *ramThen) {
    const size_t ramSize = RamSize();
    // (a page of translated code that differs is compared with its translations again)
    for (size_t page = 0; page < ramSize >> 12; page++) {
        if (std::memcmp(&ram[page << 12], &ramThen[page << 12], 0x1000) != 0) {
            if (cpu.codeMark[page]) {
                ++cpu.pageEpoch[page];
            }
            std::memcpy(&ram[page << 12], &ramThen[page << 12], 0x1000);
        }
    }
    Archive ar(moment, false);
    cpu.keepEpochs = true;
    SerializeSmall(ar);
    cpu.keepEpochs = false;
    sound.Serialize(ar);
}

void Cv1000::Serialize(Archive &ar) {
    ar.Mark(0x43563130);
    cpu.Serialize(ar);
    ar.Bytes(ram.data(), boardD ? 0x1000000 : 0x800000);
    blitter.Serialize(ar);
    nand.Serialize(ar);
    rtc.Serialize(ar);
    sound.Serialize(ar);
    ar.Pod(inputs);
    ar.Pod(system);
    ar.Pod(frame);
    ar.Pod(m_soundRegister);
    ar.Pod(m_blitterDone);
    ar.Pod(m_frameEnd);
    ar.Pod(m_fpgaPort);
    ar.Pod(m_fpgaBits);
    ScheduleIrq1();
}

} // namespace spelunk
