// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// Blocks of SH-3 code and their translations.
//
// A block starts where the CPU's PC is when the dispatcher looks (after a branch, an interrupt or the previous block)
// and runs to the first of: a branch with its delay slot, an instruction that can change what interrupts are taken
// (ldc sr, rte, trapa, sleep), an undefined instruction, or 128 instructions. The translator (tools/translate.py) and
// the interpreter's block mode (SH3::InterpretBlock) end blocks by the same rule and count the same cycles, and
// interrupts and the board's events come only between blocks, so a translated run and an interpreted one go the same
// way frame by frame (tools/check.sh).
//
// Translations come with the bytes they were made from. Game code lives in RAM, where it can be loaded over: a
// write to a 4 KB page that holds translated code moves that page's epoch on, and a block whose page has moved is
// compared with RAM again before it runs (and interpreted if it differs).
#pragma once

#include <cstdint>
#include <vector>

namespace spelunk {

class SH3;

struct NativeBlock {
    uint32_t address; // (physical: the low 29 bits)
    uint16_t length;  // in bytes
    const uint8_t *bytes;
    void (*run)(SH3 &);
};

// The translations linked in (work/<game>/gen; none in a build without them)
const NativeBlock *NativeBlocks(size_t &count);

enum class BlockEnd : uint8_t { No, Branch, Barrier };
// How an instruction ends a block (the rule both sides share)
BlockEnd EndsBlock(uint16_t opcode);
constexpr unsigned kMaxBlockInstructions = 128;

} // namespace spelunk
