// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp; from cps2recomp: https://github.com/tmcleroy/cps2recomp
//
// A 93C46 serial EEPROM, 64 words of 16 bits (or wired as 128 bytes: eightBit), a board's settings memory,
// which the game talks to one bit at a time over three lines (select, clock, data in)
// and one back (data out).
#pragma once

#include <cstdint>

namespace spelunk {

class Archive;

class Eeprom93c46 {
public:
    uint16_t words[64];   // (with eightBit, byte n is the nth byte of this as it is in memory)
    bool changed = false; // (something was written since this was last cleared)
    bool eightBit = false; // (set by the machine for its board: not part of a save state)

    Eeprom93c46();
    void SetLines(bool select, bool clock, bool dataIn);
    bool DataOut() const {
        return m_out;
    }
    void Serialize(Archive &ar);

private:
    int Bits() const {
        return eightBit ? 8 : 16;
    }
    int AddressMask() const {
        return eightBit ? 127 : 63;
    }
    uint32_t Get(int at) const;
    void Set(int at, uint32_t value);
    bool m_select = false, m_clock = false;
    bool m_out = true;
    bool m_writable = false;
    bool m_started = false;   // the start bit has come
    uint32_t m_shift = 0;     // bits received since the start bit
    int m_count = 0;
    int m_reading = -1;       // bits left to send, or -1
    uint32_t m_sending = 0;
    int m_address = 0;
    int m_writing = 0; // 0: no; 1: one word; 2: every word
};

} // namespace spelunk
