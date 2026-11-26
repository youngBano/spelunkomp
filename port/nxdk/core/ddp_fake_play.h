#pragma once

// Deterministic fake gameplay driver for DoDonPachi (ddonpach):
// Drives coin insertion, start, ship/style selection, aggressive 8-way bullet-hell movement,
// full-auto shot/laser, and periodic bomb bursts across real game scenes (Stage 1..6).
// Uses verified DoDonPachi 68000 Work RAM addresses (0x100000 base) to keep the soak test alive
// and optionally cycle through all 6 stages for whole-game timing coverage.
#include "machine/cave68k.h"

#include <cstdint>

namespace spelunk_xbox {

class DdpFakePlay {
public:
    // DoDonPachi Work RAM offsets (relative to 0x100000, big-endian 68000 RAM).
    static constexpr uint32_t kRamBase = 0x100000u;
    static constexpr uint32_t kCreditsOff = 0x0013ABu;
    static constexpr uint32_t kModeOff = 0x0017A6u;      // 16-bit BE: 7 = play, 8 = continue
    static constexpr uint32_t kShipsOff = 0x001965u;     // byte: P1 lives
    static constexpr uint32_t kStageOff = 0x001978u;     // 16-bit BE: 0..5
    static constexpr uint32_t kLoopOff = 0x00197Au;      // 16-bit BE: 0..1
    static constexpr uint32_t kBombsOff = 0x002CB0u;     // byte: P1 bombs
    static constexpr uint32_t kSafeFramesOff = 0x002CCAu; // byte: invincibility countdown
    static constexpr uint32_t kStageEndTaskOff = 0x000002u + 0x3Au * 0x40u;

    bool enabled = false;
    bool invincible = true;
    uint32_t stageHopFrames = 0; // 0 = play naturally; >0 = force advance to next stage every N play frames

    void Reset() {
        tick_ = 0;
        playTicksInStage_ = 0;
        lastStage_ = 0xFFFFu;
    }

    static inline uint16_t ReadRam16(const spelunk::Cave68k &board, uint32_t off) {
        if (off + 1 >= board.ram.size()) return 0;
        return static_cast<uint16_t>((board.ram[off] << 8) | board.ram[off + 1]);
    }

    static inline void WriteRam16(spelunk::Cave68k &board, uint32_t off, uint16_t val) {
        if (off + 1 >= board.ram.size()) return;
        board.ram[off] = static_cast<uint8_t>(val >> 8);
        board.ram[off + 1] = static_cast<uint8_t>(val & 0xFF);
    }

    static uint32_t Mode(const spelunk::Cave68k &board) { return ReadRam16(board, kModeOff); }
    static uint32_t Stage(const spelunk::Cave68k &board) { return ReadRam16(board, kStageOff); }
    static uint32_t Loop(const spelunk::Cave68k &board) { return ReadRam16(board, kLoopOff); }

    void Update(spelunk::Cave68k &board, uint32_t &outP1, uint32_t &outSys) {
        outP1 = 0;
        outSys = 0;
        if (!enabled) {
            return;
        }

        ++tick_;
        const uint32_t mode = Mode(board);
        const uint32_t stage = Stage(board);

        // Keep credits available so Start always begins gameplay.
        if (board.ram[kCreditsOff] < 3) {
            board.ram[kCreditsOff] = 3;
        }

        if (mode != 7u) {
            // Attract / Title / Ship Select / Continue: pulse Coin, Start, and Shot to enter gameplay fast.
            if ((tick_ % 60u) < 4u) {
                outSys |= spelunk::kSysCoin1;
            }
            if ((tick_ % 30u) >= 8u && (tick_ % 30u) < 14u) {
                outSys |= spelunk::kSysStart1;
            }
            if ((tick_ % 16u) < 6u) {
                outP1 |= spelunk::kInButton1; // Confirm ship & Shot/Laser style
            }
            // Cycle ship selection slightly on even ticks
            if ((tick_ % 90u) == 20u) {
                outP1 |= spelunk::kInRight;
            }
            playTicksInStage_ = 0;
            return;
        }

        // Active gameplay (mode == 7): keep ship alive & bombs stocked for sustained bullet-hell stress.
        if (invincible) {
            if (board.ram[kShipsOff] < 3) board.ram[kShipsOff] = 3;
            if (board.ram[kSafeFramesOff] < 0x40) board.ram[kSafeFramesOff] = 0x80;
            if (board.ram[kBombsOff] < 3) board.ram[kBombsOff] = 3;
        }

        if (stage != lastStage_) {
            lastStage_ = stage;
            playTicksInStage_ = 0;
        }
        ++playTicksInStage_;

        // Optional whole-game stage hopper: trigger stage-end task to benchmark every stage 1..6.
        if (stageHopFrames > 0 && playTicksInStage_ >= stageHopFrames) {
            WriteRam16(board, kStageEndTaskOff, 0x803Au);
            board.ram[kStageEndTaskOff + 3] |= 0x01u;
            WriteRam16(board, kStageEndTaskOff + 0x3Cu, 1u);
            playTicksInStage_ = 0;
        }

        // Deterministic 8-way sweeping pattern across the playfield to trigger maximum enemy/bullet/sprite activity.
        const uint32_t phase = (tick_ / 24u) & 7u;
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

        // Alternate rapid full-auto shot (button 3) and laser hold (button 1) + periodic bomb (button 2).
        if ((tick_ & 0x7Fu) < 0x50u) {
            outP1 |= spelunk::kInButton3; // Full-auto wide shot (heavy player bullet + hit spark load)
        } else {
            outP1 |= spelunk::kInButton1; // Focused laser beam
        }
        if ((tick_ % 360u) >= 350u && (tick_ % 360u) < 355u) {
            outP1 |= spelunk::kInButton2; // Bomb burst (heavy sprite/explosion stress)
        }
    }

private:
    uint32_t tick_ = 0;
    uint32_t playTicksInStage_ = 0;
    uint32_t lastStage_ = 0xFFFFu;
};

} // namespace spelunk_xbox
