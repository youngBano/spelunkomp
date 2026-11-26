// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// U10: the RTC-9701, a real-time clock with a 256-word serial EEPROM (the games' settings and scores), on three
// lines: chip select, clock and data. A command is four bits: 0000 clock write, 1000 clock read (4 address bits, 8
// data), 0010 EEPROM write, 1010 EEPROM read (12 address bits, 16 data), 0110 write enable.
// The protocol follows MAME's rtc9701.cpp (BSD-3-Clause, Angelo Salese and David Haywood): see THIRD_PARTY_NOTICES.md.
#pragma once

#include "../runtime/state.h"

#include <array>
#include <cstdint>

namespace spelunk {

class Rtc9701 {
public:
    std::array<uint16_t, 256> eeprom;
    bool eepromChanged = false;
    // The clock's registers, in BCD: seconds, minutes, hours, weekday (one bit), day, month, year. The port sets them
    // from the host's clock; a run that must repeat (the checks, movies) leaves the fixed date they start with.
    std::array<uint8_t, 7> clock = {0x00, 0x00, 0x12, 0x02, 0x12, 0x10, 0x04}; // 2004-10-12 12:00:00, a Tuesday

    Rtc9701() {
        eeprom.fill(0xFFFF);
    }

    void SetSelect(bool selected) {
        if (!selected) {
            m_state = State::Command;
            m_position = 0;
            m_command = 0;
            m_address = 0;
            m_data = 0;
            m_dataPosition = 0;
        }
        m_selected = selected;
    }
    void SetData(bool bit) {
        m_latch = bit;
    }
    void SetClock(bool high) {
        const bool rising = high && !m_clock;
        m_clock = high;
        if (!rising || !m_selected) {
            return;
        }
        switch (m_state) {
        case State::Command:
            m_command = static_cast<uint8_t>((m_command << 1) | m_latch);
            if (++m_position == 4) {
                m_position = 0;
                m_address = 0;
                m_data = 0;
                m_dataPosition = 0;
                switch (m_command) {
                case 0x0: m_state = State::ClockWrite; break;
                case 0x2: m_state = State::EepromWrite; break;
                case 0x6: m_state = State::WriteEnable; break;
                case 0x8: m_state = State::ClockRead; break;
                case 0xA: m_state = State::EepromRead; break;
                default: break;
                }
                m_command = 0;
            }
            break;
        case State::WriteEnable:
            if (++m_position == 12) {
                m_position = 0;
                m_state = State::Command;
            }
            break;
        case State::ClockWrite:
            m_position++;
            if (m_position <= 4) {
                m_address = (m_address << 1) | m_latch;
            } else {
                m_data = (m_data << 1) | m_latch;
            }
            if (m_position == 12) {
                m_position = 0;
                if (m_address < 7) {
                    clock[m_address] = static_cast<uint8_t>(m_data);
                }
                m_state = State::Command;
            }
            break;
        case State::ClockRead:
            m_position++;
            if (m_position <= 4) {
                m_address = (m_address << 1) | m_latch;
                if (m_position == 4) {
                    m_data = m_address < 7 ? clock[m_address] : 0x20;
                    m_dataPosition = 8;
                }
            } else {
                m_dataPosition--;
            }
            if (m_position == 12) {
                m_position = 0;
            }
            break;
        case State::EepromRead:
            m_position++;
            if (m_position <= 12) {
                m_address = (m_address << 1) | m_latch;
                if (m_position == 12) {
                    m_data = eeprom[(m_address >> 1) & 0xFF];
                    m_dataPosition = 16;
                }
            } else {
                m_dataPosition--;
            }
            if (m_position == 28) {
                m_position = 0;
            }
            break;
        case State::EepromWrite:
            m_position++;
            if (m_position <= 12) {
                m_address = (m_address << 1) | m_latch;
            } else {
                m_data = (m_data << 1) | m_latch;
            }
            if (m_position == 28) {
                m_position = 0;
                eeprom[(m_address >> 1) & 0xFF] = static_cast<uint16_t>(m_data);
                eepromChanged = true;
                m_state = State::Command;
            }
            break;
        }
    }
    bool ReadData() const {
        if ((m_state == State::ClockRead || m_state == State::EepromRead) && m_dataPosition > 0) {
            return (m_data >> (m_dataPosition - 1)) & 1;
        }
        return false;
    }

    void Serialize(Archive &ar) {
        ar.Mark(0x52544339);
        ar.Pod(eeprom);
        ar.Pod(clock);
        ar.Pod(m_state);
        ar.Pod(m_selected);
        ar.Pod(m_clock);
        ar.Pod(m_latch);
        ar.Pod(m_command);
        ar.Pod(m_position);
        ar.Pod(m_address);
        ar.Pod(m_data);
        ar.Pod(m_dataPosition);
    }

private:
    enum class State : uint8_t { Command, WriteEnable, ClockWrite, ClockRead, EepromRead, EepromWrite };
    State m_state = State::Command;
    bool m_selected = false, m_clock = false, m_latch = false;
    uint8_t m_command = 0;
    uint32_t m_position = 0, m_address = 0, m_data = 0, m_dataPosition = 0;
};

} // namespace spelunk
