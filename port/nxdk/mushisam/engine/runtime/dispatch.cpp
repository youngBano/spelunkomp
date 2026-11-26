// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// The block dispatcher: translations where there are any (and the RAM still holds what they were made from), the
// interpreter a block at a time where not. runtime/blocks.h says how blocks are cut.
#include "blocks.h"

#include "../machine/sh3.h"
#include "../machine/sh3_ops.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace spelunk {

namespace {
    BlockEnd g_ends[0x10000];
    bool g_ready = false;
    void BuildEnds() {
        using namespace ops;
        for (uint32 op = 0; op < 0x10000; op++) {
            const OpFn f = SH3::s_ops[op];
            BlockEnd e = BlockEnd::No;
            if (f == BRA || f == BSR || f == BRAF || f == BSRF || f == JMP || f == JSR || f == RTS || f == RTE || f == BT ||
                f == BF || f == BTS || f == BFS) {
                e = BlockEnd::Branch;
            } else if (f == TRAPA || f == ILLEGAL || f == SLEEP || f == LDC_SR || f == LDCL_SR) {
                e = BlockEnd::Barrier;
            }
            g_ends[op] = e;
        }
        g_ready = true;
    }
    bool Delayed(uint16 op) {
        using namespace ops;
        const OpFn f = SH3::s_ops[op];
        return f != BT && f != BF && f != TRAPA && f != ILLEGAL && f != SLEEP && f != LDC_SR && f != LDCL_SR;
    }
} // namespace

BlockEnd EndsBlock(uint16_t opcode) {
    if (!g_ready) [[unlikely]] {
        BuildEnds();
    }
    return g_ends[opcode];
}

bool SH3::RecordBlock(uint32 pc, bool found) {
    const uint32 phys = pc & (kRamSize - 1);
    std::vector<uint32> &versions = m_recordedAt[pc];
    if (!IsRam(pc) && !versions.empty()) {
        // (the flash: never another version)
        if (!found) {
            m_recorded[versions[0]].found = false;
        }
        return false;
    }
    // (the same bytes as a version already recorded: the same block, since where it ends is read from them)
    for (uint32 i : versions) {
        Recorded &r = m_recorded[i];
        if (phys + r.bytes.size() <= kRamSize && std::memcmp(ram + phys, r.bytes.data(), r.bytes.size()) == 0) {
            if (!found) {
                r.found = false; // (found by following, and now run)
            }
            return false;
        }
    }
    // (the block's bytes as they are now, by the shared rule)
    std::vector<uint8> bytes;
    uint32 a = pc;
    for (unsigned n = 0; n < kMaxBlockInstructions; n++) {
        const uint16 op = Fetch(a);
        bytes.push_back(static_cast<uint8>(op >> 8)); // (as RAM holds them: big-endian)
        bytes.push_back(static_cast<uint8>(op));
        const BlockEnd end = EndsBlock(op);
        if (end != BlockEnd::No) {
            if (end == BlockEnd::Branch && Delayed(op)) {
                const uint16 slotOp = Fetch(a + 2);
                bytes.push_back(static_cast<uint8>(slotOp >> 8));
                bytes.push_back(static_cast<uint8>(slotOp));
            }
            break;
        }
        a += 2;
    }
    versions.push_back(static_cast<uint32>(m_recorded.size()));
    m_recorded.push_back({pc, found, std::move(bytes)});
    return true;
}

void SH3::FollowBlock(uint32 start, unsigned steps) {
    using namespace ops;
    std::vector<std::pair<uint32, unsigned>> work{{start, steps}};
    while (!work.empty()) {
        const auto [pc, left] = work.back();
        work.pop_back();
        uint32 a = pc;
        uint16 op = 0;
        BlockEnd end = BlockEnd::No;
        for (unsigned n = 0; n < kMaxBlockInstructions; n++, a += 2) {
            op = Fetch(a);
            end = EndsBlock(op);
            if (end != BlockEnd::No) {
                break;
            }
        }
        // Where the CPU can go next from this block's end, as far as the code says (not through a register)
        uint32 next[2];
        int count = 0;
        const OpFn f = SH3::s_ops[op];
        const uint32 near = a + 4 + static_cast<uint32>(static_cast<sint32>(static_cast<sint8>(op & 0xFF)) * 2);
        const uint32 far = a + 4 + static_cast<uint32>((static_cast<sint32>((op & 0xFFF) << 20) >> 20) * 2);
        if (end == BlockEnd::No) {
            next[count++] = a; // (128 instructions: the next block goes on from there)
        } else if (f == BT || f == BF) {
            next[count++] = near;
            next[count++] = a + 2;
        } else if (f == BTS || f == BFS) {
            next[count++] = near;
            next[count++] = a + 4;
        } else if (f == BRA) {
            next[count++] = far;
        } else if (f == BSR) {
            next[count++] = far;
            next[count++] = a + 4; // (where the call returns to)
        } else if (f == BSRF || f == JSR) {
            next[count++] = a + 4;
        } else if (f == TRAPA || f == SLEEP || f == LDC_SR || f == LDCL_SR) {
            next[count++] = a + 2;
        }
        for (int i = 0; i < count; i++) {
            if ((IsRam(next[i]) || IsFlash(next[i])) && !(next[i] & 1) && RecordBlock(next[i], true) && left > 1) {
                work.push_back({next[i], left - 1});
            }
        }
    }
}

void SH3::InterpretBlock() {
    if (recording && (IsRam(PC) || IsFlash(PC))) [[unlikely]] {
        if (RecordBlock(PC, false) && recordStatic) {
            FollowBlock(PC, recordStatic);
        }
    }
    for (unsigned n = 0; n < kMaxBlockInstructions; n++) {
        const uint32 pc = PC;
        const uint16 op = Fetch(pc);
        branched = false;
        at = pc;
        s_ops[op](*this, op);
        cycles += s_cycles[op];
        if (!branched) {
            PC += 2;
        } else if (timing) [[unlikely]] {
            TimeBranch(pc, PC);
        }
        if (EndsBlock(op) != BlockEnd::No) {
            return;
        }
    }
}

SH3::Translation *SH3::Lookup(uint32 pc) {
    if (!IsRam(pc)) [[unlikely]] {
        if (m_flash.empty()) {
            return nullptr;
        }
        const auto it = m_flash.find(pc);
        if (it == m_flash.end()) {
            return nullptr;
        }
        Translation *t = it->second;
        if (t->epoch == UINT32_MAX) {
            // (looked at once: the flash is the game's own, as the translation was made from it)
            t->epoch = 0;
            t->valid = true;
            if (t->block->bytes) {
                for (uint32 i = 0; i < t->block->length; i += 2) {
                    const uint16 op = Fetch(pc + i);
                    t->valid = t->valid && t->block->bytes[i] == static_cast<uint8>(op >> 8) && t->block->bytes[i + 1] == static_cast<uint8>(op);
                }
            }
        }
        return t->valid ? t : nullptr;
    }
    const uint32 phys = pc & (kRamSize - 1), page = phys >> 12;
    Translation **slots = m_pagePtrs[page];
    if (!slots) [[unlikely]] {
        return nullptr;
    }
    // (an address can have more than one translation: code loaded over other code, each version recorded)
    const uint32 pEpoch = pageEpoch[page];
    for (Translation *t = slots[(phys & 0xFFF) >> 1]; t; t = t->next) {
        if (t->address != pc) [[unlikely]] {
            continue;
        }
        const uint32 lastEpoch = t->lastPage < kPages ? pageEpoch[t->lastPage] : 0;
        if (t->epoch != pEpoch || t->epoch2 != lastEpoch) [[unlikely]] {
            t->epoch = pEpoch;
            t->epoch2 = lastEpoch;
            t->valid = !t->block->bytes ||
                       (phys + t->block->length <= kRamSize && std::memcmp(ram + phys, t->block->bytes, t->block->length) == 0);
        }
        if (t->valid) [[likely]] {
            return t;
        }
    }
    return nullptr;
}

#ifdef XBOX_FAST_CPU
namespace {
    void (*g_origTmuPoll)(SH3 &) = nullptr;
    void (*g_origDelay1)(SH3 &) = nullptr;
    void (*g_origDelay2)(SH3 &) = nullptr;

    void FastTmuPollWrapper(SH3 &c) {
        c.FastForwardTmuPoll();
        if (c.cycles < c.blockStop && !c.interruptReady) {
            g_origTmuPoll(c);
        }
    }
    void FastDelay1Wrapper(SH3 &c) {
        c.FastForwardDelayLoop(0x0C056756u);
        if (c.cycles < c.blockStop && !c.interruptReady) {
            g_origDelay1(c);
        }
    }
    void FastDelay2Wrapper(SH3 &c) {
        c.FastForwardDelayLoop(0x0C056768u);
        if (c.cycles < c.blockStop && !c.interruptReady) {
            g_origDelay2(c);
        }
    }
} // namespace
#endif

#ifdef MUSHISAM_PROFILE_HOT
#include <unordered_map>
extern std::unordered_map<uint32, uint64> g_hotPcCounts;
extern std::unordered_map<uint32, uint64> g_hotPcCycles;
#endif

void SH3::RunBlocks(uint64 stop) {
    blockStop = stop;
    if (mode == Mode::Native) {
        uint32 localNative = 0;
        while (cycles < blockStop && !interruptReady && !sleeping) {
#ifndef XBOX_FAST_CPU
            if (stopPc != 0xFFFFFFFFu) [[unlikely]] {
                if ((PC & 0x1FFFFFFFu) == (stopPc & 0x1FFFFFFFu)) {
                    stopPc = 0xFFFFFFFFu;
                    if (onStopPc) {
                        onStopPc();
                    }
                }
            }
            if (RunNativeCall()) [[unlikely]] {
                continue;
            }
#endif
            if ((PC & 0xFF800000u) == 0x0C000000u) [[likely]] {
                const uint32 phys = PC & (kRamSize - 1);
                if (BlockFn *fnSlots = m_pageFnPtrs[phys >> 12]) [[likely]] {
                    if (BlockFn fn = fnSlots[(phys & 0xFFF) >> 1]) [[likely]] {
#ifdef MUSHISAM_PROFILE_HOT
                        const uint32 entryPc = PC;
                        const uint64 c0 = cycles;
#endif
                        fn(*this);
#ifdef MUSHISAM_PROFILE_HOT
                        g_hotPcCounts[entryPc]++;
                        g_hotPcCycles[entryPc] += (cycles - c0);
#endif
                        ++localNative;
                        continue;
                    }
                }
            }
            if (Translation *t = Lookup(PC)) [[likely]] {
                t->run(*this);
                ++localNative;
            } else {
                if (countInterpreted) [[unlikely]] {
                    m_interpretedAt[PC]++;
                }
                lastInterpretedPc = PC;
                InterpretBlock();
                interpretedBlocks++;
            }
        }
        nativeBlocks += localNative;
        return;
    }
    while (cycles < blockStop && !interruptReady && !sleeping) {
        if (stopPc != 0xFFFFFFFFu && (PC & 0x1FFFFFFFu) == (stopPc & 0x1FFFFFFFu)) [[unlikely]] {
            stopPc = 0xFFFFFFFFu;
            if (onStopPc) {
                onStopPc();
            }
        }
        if (RunNativeCall()) [[unlikely]] {
            continue;
        }
        InterpretBlock();
        interpretedBlocks++;
    }
}

void SH3::RunOneBlock(bool native) {
    if (native && RunNativeCall()) {
        return;
    }
    if (mode == Mode::Native) {
        if (Translation *t = Lookup(PC)) {
            t->block->run(*this);
            nativeBlocks++;
            return;
        }
    }
    InterpretBlock();
    interpretedBlocks++;
}

void SH3::InstallTranslations() {
    size_t count = 0;
    const NativeBlock *blocks = NativeBlocks(count);
    m_translations.assign(count, Translation{});
    m_flash.clear();
    codeAddrEnd = 0;
    std::memset(m_pagePtrs, 0, sizeof(m_pagePtrs));
    std::memset(m_pageFnPtrs, 0, sizeof(m_pageFnPtrs));
    for (size_t i = 0; i < count; i++) {
        Translation &t = m_translations[i];
        t.block = &blocks[i];
        t.run = blocks[i].run;
        t.address = blocks[i].address;
        if (!IsRam(blocks[i].address)) {
            m_flash[blocks[i].address] = &t;
            continue;
        }
        const uint32 phys = blocks[i].address & (kRamSize - 1);
        const uint32 first = phys >> 12, last = (phys + blocks[i].length - 1) >> 12;
        t.lastPage = last;
        for (uint32 p = first; p <= last && p < kPages; p++) {
            codeMark[p] = 1;
            const uint32 pageEnd = (p + 1u) << 12;
            if (pageEnd > codeAddrEnd) {
                codeAddrEnd = pageEnd;
            }
        }
        // Populate zero-indirection direct function-pointer table for canonical 0x0Cxxxxxx RAM blocks.
        if (!blocks[i].bytes && (blocks[i].address & 0xFF800000u) == 0x0C000000u) {
            std::vector<BlockFn> &fnSlots = m_fnPages[first];
            if (fnSlots.empty()) {
                fnSlots.assign(2048, nullptr);
                m_pageFnPtrs[first] = fnSlots.data();
            }
            fnSlots[(phys & 0xFFF) >> 1] = blocks[i].run;
#ifdef XBOX_FAST_CPU
            // On Xbox, canonical 0x0Cxxxxxx blocks always hit m_pageFnPtrs directly; skip duplicate m_pages (~1.8 MB saved!).
            continue;
#endif
        }
        std::vector<Translation *> &slots = m_pages[first];
        if (slots.empty()) {
            slots.assign(2048, nullptr);
            m_pagePtrs[first] = slots.data();
        }
        Translation *&head = slots[(phys & 0xFFF) >> 1];
        t.next = head;
        head = &t;
    }
}

void SH3::NoteRamWrite(uint32 a, uint32 length) {
    if (!length) {
        return;
    }
    const uint32 first = (a & (kRamSize - 1)) >> 12, last = ((a & (kRamSize - 1)) + length - 1) >> 12;
    for (uint32 p = first; p <= last && p < kPages; p++) {
        if (codeMark[p]) {
            ++pageEpoch[p];
        }
    }
}

void SH3::SaveRecording(const char *path) const {
    FILE *out = fopen(path, "w");
    if (!out) {
        return;
    }
    for (const Recorded &r : m_recorded) {
        fprintf(out, "%08X ", r.pc);
        for (uint8 b : r.bytes) {
            fprintf(out, "%02X", b);
        }
        fputs(r.found ? " found\n" : "\n", out);
    }
    fclose(out);
}

void SH3::SaveInterpreted(const char *path) {
    FILE *out = fopen(path, "w");
    if (!out) {
        return;
    }
    std::vector<std::pair<uint64, uint32>> order;
    for (const auto &[pc, n] : m_interpretedAt) {
        order.push_back({n, pc});
    }
    std::sort(order.begin(), order.end(), [](const auto &x, const auto &y) { return x.first != y.first ? x.first > y.first : x.second < y.second; });
    for (const auto &[n, pc] : order) {
        // (stale: a translation of another version of the code there; none: never recorded)
        bool translated = false;
        if (IsRam(pc)) {
            const uint32 phys = pc & (kRamSize - 1);
            const std::vector<Translation *> &slots = m_pages[phys >> 12];
            for (Translation *t = slots.empty() ? nullptr : slots[(phys & 0xFFF) >> 1]; t; t = t->next) {
                translated |= t->block->address == pc;
            }
        }
        fprintf(out, "%08X %llu %s\n", pc, static_cast<unsigned long long>(n), translated ? "stale" : "none");
    }
    fclose(out);
}

} // namespace spelunk
