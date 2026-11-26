#pragma once

// Deterministic fake gameplay driver for Mushihimesama (Cave CV1000-B, SH-3):
// Drives coin insertion, Start, mode/shot selection, aggressive 8-way bullet-hell movement,
// full-auto shot/laser, and periodic bomb bursts across real game scenes (Stage 1..5).
// Uses verified Mushihimesama SH-3 Work RAM addresses (0x0C000000 base, big-endian).
#include "machine/cv1000.h"

#include <cstdint>

namespace spelunk_xbox {

class MushisamFakePlay {
public:
    // Mushihimesama CV1000-B RAM offsets (relative to 0x0C000000, big-endian SH-3 RAM).
    static constexpr uint32_t kCreditsOff    = 0x00056BE7u; // byte: credits
    static constexpr uint32_t kStageOff      = 0x001ECF9Cu; // u32 BE: stage 0..4
    static constexpr uint32_t kShipsOff      = 0x001ED35Bu; // byte: P1 lives
    static constexpr uint32_t kBombsOff      = 0x001ED35Fu; // byte: P1 bombs
    static constexpr uint32_t kShipStateOff  = 0x001ED380u; // u32 BE: 1 = active play, 2 = dying/respawn
    static constexpr uint32_t kSafeFramesOff = 0x001ED3E8u; // u32 BE: invincibility frames

    bool enabled = false;
    bool invincible = true;

    void Reset() {
        tick_ = 0;
        playTicks_ = 0;
    }

    static inline uint32_t ReadRam32(const spelunk::Cv1000 &board, uint32_t off) {
        if (off + 3 >= board.ram.size()) return 0;
        return (static_cast<uint32_t>(board.ram[off]) << 24) |
               (static_cast<uint32_t>(board.ram[off + 1]) << 16) |
               (static_cast<uint32_t>(board.ram[off + 2]) << 8) |
               static_cast<uint32_t>(board.ram[off + 3]);
    }

    static inline void WriteRam32(spelunk::Cv1000 &board, uint32_t off, uint32_t val) {
        if (off + 3 >= board.ram.size()) return;
        board.ram[off]     = static_cast<uint8_t>(val >> 24);
        board.ram[off + 1] = static_cast<uint8_t>(val >> 16);
        board.ram[off + 2] = static_cast<uint8_t>(val >> 8);
        board.ram[off + 3] = static_cast<uint8_t>(val & 0xFF);
    }

    static uint32_t Stage(const spelunk::Cv1000 &board) {
        const uint32_t s = ReadRam32(board, kStageOff);
        return s < 5u ? s : 0u;
    }

    static uint32_t ShipState(const spelunk::Cv1000 &board) {
        return ReadRam32(board, kShipStateOff);
    }

    static bool InPlay(const spelunk::Cv1000 &board) {
        const uint32_t st = ShipState(board);
        return st == 1u || st == 2u;
    }

    void Update(spelunk::Cv1000 &board, uint32_t &outP1, uint32_t &outSys) {
        outP1 = 0;
        outSys = 0;
        if (!enabled) {
            return;
        }

        ++tick_;

        // Keep credits stocked once boot initializes RAM.
        if (tick_ > 300u && kCreditsOff < board.ram.size() && board.ram[kCreditsOff] < 3u) {
            board.ram[kCreditsOff] = 3u;
        }

        if (!InPlay(board)) {
            // Boot / Title / Mode Select / Shot Select / Continue: pulse Coin, Start, and Button 1.
            if ((tick_ % 90u) < 4u) {
                outSys |= spelunk::kSysCoin1;
            }
            if ((tick_ % 45u) >= 10u && (tick_ % 45u) < 16u) {
                outSys |= spelunk::kSysStart1;
            }
            if ((tick_ % 24u) < 6u) {
                outP1 |= spelunk::kInButton1;
            }
            return;
        }

        ++playTicks_;

        // Active gameplay: keep player alive & bombs stocked for sustained bullet-hell stress.
        if (invincible) {
            if (kShipsOff < board.ram.size() && board.ram[kShipsOff] < 3u) {
                board.ram[kShipsOff] = 3u;
            }
            if (kBombsOff < board.ram.size() && board.ram[kBombsOff] < 3u) {
                board.ram[kBombsOff] = 3u;
            }
            if (ReadRam32(board, kSafeFramesOff) < 0x40u) {
                WriteRam32(board, kSafeFramesOff, 0x80u);
            }
        }

        // Deterministic 8-way sweeping pattern across the playfield.
        const uint32_t phase = (playTicks_ / 24u) & 7u;
        switch (phase) {
            case 0: outP1 |= spelunk::kInLeft | spelunk::kInUp; break;
            case 1: outP1 |= spelunk::kInRight; break;
            case 2: outP1 |= spelunk::kInRight | spelunk::kInDown; break;
            case 3: outP1 |= spelunk::kInLeft; break;
            case 4: outP1 |= spelunk::kInLeft | spelunk::kInDown; break;
            case 5: outP1 |= spelunk::kInUp; break;
            case 6: outP1 |= spelunk::kInRight | spelunk::kInUp; break;
            case 7: outP1 |= spelunk::kInDown; break;
        }

        // Alternate rapid full-auto (Button 3) and focused laser (Button 1) + periodic bomb (Button 2).
        if ((playTicks_ & 0x7Fu) < 0x50u) {
            outP1 |= spelunk::kInButton3;
        } else {
            outP1 |= spelunk::kInButton1;
        }
        if ((playTicks_ % 360u) >= 350u && (playTicks_ % 360u) < 355u) {
            outP1 |= spelunk::kInButton2;
        }
    }

private:
    uint32_t tick_ = 0;
    uint32_t playTicks_ = 0;
};

} // namespace spelunk_xbox
