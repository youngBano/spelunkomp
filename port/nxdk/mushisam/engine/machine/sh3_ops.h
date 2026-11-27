// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
// What each SH-3 instruction does, written once. The interpreter (sh3.cpp) calls these through its decoding
// table; the translated code calls the same functions with the opcode as a constant, which the compiler folds
// into plain register and memory operations.
//
// Every function takes the CPU and the opcode, and runs with c.PC at the instruction's own address. Branches
// (only the interpreter runs those; the translator writes its own control flow) set c.PC and c.branched.
#pragma once

#include "sh3.h"


namespace spelunk::ops {

#define N_ ((op >> 8) & 15)
#define M_ ((op >> 4) & 15)
#define OPFN(name) inline void name([[maybe_unused]] SH3 &c, [[maybe_unused]] uint16 op)

inline uint32 SignExtend8(uint32 v) {
    return static_cast<uint32>(static_cast<sint32>(static_cast<sint8>(v)));
}
inline uint32 SignExtend16(uint32 v) {
    return static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(v)));
}
inline uint32 SignExtend12(uint32 v) {
    return (v & 0x800) ? (v | 0xFFFFF000u) : v;
}

// --- branches (interpreter only) ---
void RunDelaySlot(SH3 &c, uint32 target); // (sh3.cpp)

// ============================================================================================================
// Data moves

OPFN(MOV_I) { c.R[N_] = SignExtend8(op & 0xFF); }
OPFN(MOVW_PC) { c.R[N_] = SignExtend16(c.Read16(c.PC + 4 + (op & 0xFF) * 2)); }
OPFN(MOVL_PC) { c.R[N_] = c.Read32((c.PC & ~3u) + 4 + (op & 0xFF) * 4); }
OPFN(MOV) { c.R[N_] = c.R[M_]; }
OPFN(MOVB_S) { c.Write8(c.R[N_], static_cast<uint8>(c.R[M_])); }
OPFN(MOVW_S) { c.Write16(c.R[N_], static_cast<uint16>(c.R[M_])); }
OPFN(MOVL_S) { c.Write32(c.R[N_], c.R[M_]); }
OPFN(MOVB_L) { c.R[N_] = SignExtend8(c.Read8(c.R[M_])); }
OPFN(MOVW_L) { c.R[N_] = SignExtend16(c.Read16(c.R[M_])); }
OPFN(MOVL_L) { c.R[N_] = c.Read32(c.R[M_]); }
OPFN(MOVB_M) {
    const uint32 a = c.R[N_] - 1;
    c.Write8(a, static_cast<uint8>(c.R[M_]));
    c.R[N_] = a;
}
OPFN(MOVW_M) {
    const uint32 a = c.R[N_] - 2;
    c.Write16(a, static_cast<uint16>(c.R[M_]));
    c.R[N_] = a;
}
OPFN(MOVL_M) {
    const uint32 a = c.R[N_] - 4;
    c.Write32(a, c.R[M_]);
    c.R[N_] = a;
}
OPFN(MOVB_P) {
    const unsigned n = N_, m = M_;
    const uint32 v = SignExtend8(c.Read8(c.R[m]));
    if (n != m) {
        c.R[m] += 1;
    }
    c.R[n] = v;
}
OPFN(MOVW_P) {
    const unsigned n = N_, m = M_;
    const uint32 v = SignExtend16(c.Read16(c.R[m]));
    if (n != m) {
        c.R[m] += 2;
    }
    c.R[n] = v;
}
OPFN(MOVL_P) {
    const unsigned n = N_, m = M_;
    const uint32 v = c.Read32(c.R[m]);
    if (n != m) {
        c.R[m] += 4;
    }
    c.R[n] = v;
}
OPFN(MOVB_S4) { c.Write8(c.R[M_] + (op & 15), static_cast<uint8>(c.R[0])); }
OPFN(MOVW_S4) { c.Write16(c.R[M_] + (op & 15) * 2, static_cast<uint16>(c.R[0])); }
OPFN(MOVL_S4) { c.Write32(c.R[N_] + (op & 15) * 4, c.R[M_]); }
OPFN(MOVB_L4) { c.R[0] = SignExtend8(c.Read8(c.R[M_] + (op & 15))); }
OPFN(MOVW_L4) { c.R[0] = SignExtend16(c.Read16(c.R[M_] + (op & 15) * 2)); }
OPFN(MOVL_L4) { c.R[N_] = c.Read32(c.R[M_] + (op & 15) * 4); }
OPFN(MOVB_S0) { c.Write8(c.R[N_] + c.R[0], static_cast<uint8>(c.R[M_])); }
OPFN(MOVW_S0) { c.Write16(c.R[N_] + c.R[0], static_cast<uint16>(c.R[M_])); }
OPFN(MOVL_S0) { c.Write32(c.R[N_] + c.R[0], c.R[M_]); }
OPFN(MOVB_L0) { c.R[N_] = SignExtend8(c.Read8(c.R[M_] + c.R[0])); }
OPFN(MOVW_L0) { c.R[N_] = SignExtend16(c.Read16(c.R[M_] + c.R[0])); }
OPFN(MOVL_L0) { c.R[N_] = c.Read32(c.R[M_] + c.R[0]); }
OPFN(MOVB_SG) { c.Write8(c.GBR + (op & 0xFF), static_cast<uint8>(c.R[0])); }
OPFN(MOVW_SG) { c.Write16(c.GBR + (op & 0xFF) * 2, static_cast<uint16>(c.R[0])); }
OPFN(MOVL_SG) { c.Write32(c.GBR + (op & 0xFF) * 4, c.R[0]); }
OPFN(MOVB_LG) { c.R[0] = SignExtend8(c.Read8(c.GBR + (op & 0xFF))); }
OPFN(MOVW_LG) { c.R[0] = SignExtend16(c.Read16(c.GBR + (op & 0xFF) * 2)); }
OPFN(MOVL_LG) { c.R[0] = c.Read32(c.GBR + (op & 0xFF) * 4); }
OPFN(MOVA) { c.R[0] = (c.PC & ~3u) + 4 + (op & 0xFF) * 4; }
OPFN(MOVT) { c.R[N_] = c.T; }
OPFN(SWAPB) {
    const uint32 m = c.R[M_];
    c.R[N_] = (m & 0xFFFF0000u) | ((m & 0xFF) << 8) | ((m >> 8) & 0xFF);
}
OPFN(SWAPW) {
    const uint32 m = c.R[M_];
    c.R[N_] = (m << 16) | (m >> 16);
}
OPFN(XTRCT) { c.R[N_] = (c.R[M_] << 16) | (c.R[N_] >> 16); }

// ============================================================================================================
// Arithmetic

OPFN(ADD) { c.R[N_] += c.R[M_]; }
OPFN(ADD_I) { c.R[N_] += SignExtend8(op & 0xFF); }
OPFN(ADDC) {
    const unsigned n = N_;
    const uint32 tmp0 = c.R[n], tmp1 = tmp0 + c.R[M_];
    c.R[n] = tmp1 + c.T;
    c.T = (tmp0 > tmp1) || (tmp1 > c.R[n]);
}
OPFN(ADDV) {
    const unsigned n = N_;
    const uint32 a = c.R[n], b = c.R[M_], r = a + b;
    c.T = ((~(a ^ b) & (a ^ r)) >> 31) & 1;
    c.R[n] = r;
}
OPFN(SUB) { c.R[N_] -= c.R[M_]; }
OPFN(SUBC) {
    const unsigned n = N_;
    const uint32 tmp0 = c.R[n], tmp1 = tmp0 - c.R[M_];
    c.R[n] = tmp1 - c.T;
    c.T = (tmp0 < tmp1) || (tmp1 < c.R[n]);
}
OPFN(SUBV) {
    const unsigned n = N_;
    const uint32 a = c.R[n], b = c.R[M_], r = a - b;
    c.T = (((a ^ b) & (a ^ r)) >> 31) & 1;
    c.R[n] = r;
}
OPFN(NEG) { c.R[N_] = 0u - c.R[M_]; }
OPFN(NEGC) {
    const uint32 tmp = 0u - c.R[M_];
    const uint32 r = tmp - c.T;
    c.T = (0 < tmp) || (tmp < r);
    c.R[N_] = r;
}
OPFN(DT) {
    const unsigned n = N_;
    c.R[n] -= 1;
    c.T = c.R[n] == 0;
}
OPFN(EXTSB) { c.R[N_] = SignExtend8(c.R[M_]); }
OPFN(EXTSW) { c.R[N_] = SignExtend16(c.R[M_]); }
OPFN(EXTUB) { c.R[N_] = c.R[M_] & 0xFF; }
OPFN(EXTUW) { c.R[N_] = c.R[M_] & 0xFFFF; }
OPFN(MULL) { c.MACL = c.R[N_] * c.R[M_]; }
OPFN(MULSW) {
    c.MACL = static_cast<uint32>(static_cast<sint32>(static_cast<sint16>(c.R[N_])) * static_cast<sint16>(c.R[M_]));
}
OPFN(MULUW) { c.MACL = (c.R[N_] & 0xFFFF) * (c.R[M_] & 0xFFFF); }
OPFN(DMULS) {
    const sint64 r = static_cast<sint64>(static_cast<sint32>(c.R[N_])) * static_cast<sint32>(c.R[M_]);
    c.MACH = static_cast<uint32>(static_cast<uint64>(r) >> 32);
    c.MACL = static_cast<uint32>(r);
}
OPFN(DMULU) {
    const uint64 r = static_cast<uint64>(c.R[N_]) * c.R[M_];
    c.MACH = static_cast<uint32>(r >> 32);
    c.MACL = static_cast<uint32>(r);
}
OPFN(MACL) {
    const unsigned n = N_, m = M_;
    const sint64 a = static_cast<sint32>(c.Read32(c.R[n]));
    c.R[n] += 4;
    const sint64 b = static_cast<sint32>(c.Read32(c.R[m]));
    c.R[m] += 4;
    const sint64 mac = static_cast<sint64>((static_cast<uint64>(c.MACH) << 32) | c.MACL);
    sint64 sum = static_cast<sint64>(static_cast<uint64>(mac) + static_cast<uint64>(a * b));
    if (c.sr & kSrS) {
        // (saturates at 48 bits, portable 32-bit x86 without __int128)
        const sint64 prod = a * b;
        const sint64 high = 0x00007FFFFFFFFFFFll, low = -0x0000800000000000ll;
        const bool ovfPos = (mac > 0 && prod > INT64_MAX - mac);
        const bool ovfNeg = (mac < 0 && prod < INT64_MIN - mac);
        if (ovfPos || (!ovfNeg && sum > high)) {
            sum = high;
        } else if (ovfNeg || sum < low) {
            sum = low;
        }
    }
    c.MACH = static_cast<uint32>(static_cast<uint64>(sum) >> 32);
    c.MACL = static_cast<uint32>(sum);
}
OPFN(MACW) {
    const unsigned n = N_, m = M_;
    const sint32 a = static_cast<sint16>(c.Read16(c.R[n]));
    c.R[n] += 2;
    const sint32 b = static_cast<sint16>(c.Read16(c.R[m]));
    c.R[m] += 2;
    const sint64 product = static_cast<sint64>(a) * b;
    if (c.sr & kSrS) {
        // (the sum saturates at 32 bits, with a mark in MACH)
        const sint64 sum = static_cast<sint64>(static_cast<sint32>(c.MACL)) + product;
        if (sum > INT32_MAX) {
            c.MACL = 0x7FFFFFFFu;
            c.MACH |= 1;
        } else if (sum < INT32_MIN) {
            c.MACL = 0x80000000u;
            c.MACH |= 1;
        } else {
            c.MACL = static_cast<uint32>(sum);
        }
    } else {
        const uint64 mac = ((static_cast<uint64>(c.MACH) << 32) | c.MACL) + static_cast<uint64>(product);
        c.MACH = static_cast<uint32>(mac >> 32);
        c.MACL = static_cast<uint32>(mac);
    }
}
OPFN(CLRMAC) { c.MACH = c.MACL = 0; }
OPFN(DIV0U) {
    c.sr &= ~(kSrQ | kSrM);
    c.T = 0;
}
OPFN(DIV0S) {
    const uint32 q = c.R[N_] >> 31, m = c.R[M_] >> 31;
    c.sr = (c.sr & ~(kSrQ | kSrM)) | (q ? kSrQ : 0) | (m ? kSrM : 0);
    c.T = q ^ m;
}
OPFN(DIV1) {
    // One step of a division (the manual's own description, step for step)
    const unsigned n = N_;
    const bool oldQ = c.sr & kSrQ, M = c.sr & kSrM;
    bool Q = c.R[n] >> 31;
    const uint32 divisor = c.R[M_];
    c.R[n] = (c.R[n] << 1) | c.T;
    const uint32 before = c.R[n];
    if (oldQ == M) {
        c.R[n] -= divisor;
        const bool carry = c.R[n] > before;
        Q = M ? (Q ? carry : !carry) : (Q ? !carry : carry);
    } else {
        c.R[n] += divisor;
        const bool carry = c.R[n] < before;
        Q = M ? (Q ? carry : !carry) : (Q ? !carry : carry);
    }
    c.sr = (c.sr & ~kSrQ) | (Q ? kSrQ : 0);
    c.T = Q == M;
}

// ============================================================================================================
// Compares and logic

OPFN(CMPEQ_I) { c.T = c.R[0] == SignExtend8(op & 0xFF); }
OPFN(CMPEQ) { c.T = c.R[N_] == c.R[M_]; }
OPFN(CMPHS) { c.T = c.R[N_] >= c.R[M_]; }
OPFN(CMPGE) { c.T = static_cast<sint32>(c.R[N_]) >= static_cast<sint32>(c.R[M_]); }
OPFN(CMPHI) { c.T = c.R[N_] > c.R[M_]; }
OPFN(CMPGT) { c.T = static_cast<sint32>(c.R[N_]) > static_cast<sint32>(c.R[M_]); }
OPFN(CMPPZ) { c.T = static_cast<sint32>(c.R[N_]) >= 0; }
OPFN(CMPPL) { c.T = static_cast<sint32>(c.R[N_]) > 0; }
OPFN(CMPSTR) {
    const uint32 x = c.R[N_] ^ c.R[M_];
    c.T = !(x & 0xFF000000u) || !(x & 0x00FF0000u) || !(x & 0x0000FF00u) || !(x & 0x000000FFu);
}
OPFN(TST) { c.T = (c.R[N_] & c.R[M_]) == 0; }
OPFN(TST_I) { c.T = (c.R[0] & (op & 0xFF)) == 0; }
OPFN(TSTB_G) { c.T = (c.Read8(c.GBR + c.R[0]) & (op & 0xFF)) == 0; }
OPFN(AND) { c.R[N_] &= c.R[M_]; }
OPFN(AND_I) { c.R[0] &= op & 0xFF; }
OPFN(ANDB_G) {
    const uint32 a = c.GBR + c.R[0];
    c.Write8(a, c.Read8(a) & (op & 0xFF));
}
OPFN(OR) { c.R[N_] |= c.R[M_]; }
OPFN(OR_I) { c.R[0] |= op & 0xFF; }
OPFN(ORB_G) {
    const uint32 a = c.GBR + c.R[0];
    c.Write8(a, c.Read8(a) | (op & 0xFF));
}
OPFN(XOR) { c.R[N_] ^= c.R[M_]; }
OPFN(XOR_I) { c.R[0] ^= op & 0xFF; }
OPFN(XORB_G) {
    const uint32 a = c.GBR + c.R[0];
    c.Write8(a, c.Read8(a) ^ (op & 0xFF));
}
OPFN(NOT) { c.R[N_] = ~c.R[M_]; }
OPFN(TASB) {
    const uint32 a = c.R[N_];
    const uint8 v = c.Read8(a);
    c.T = v == 0;
    c.Write8(a, v | 0x80);
}

// ============================================================================================================
// Shifts and rotates

OPFN(SHLL) {
    const unsigned n = N_;
    c.T = c.R[n] >> 31;
    c.R[n] <<= 1;
}
OPFN(SHAL) {
    const unsigned n = N_;
    c.T = c.R[n] >> 31;
    c.R[n] <<= 1;
}
OPFN(SHLR) {
    const unsigned n = N_;
    c.T = c.R[n] & 1;
    c.R[n] >>= 1;
}
OPFN(SHAR) {
    const unsigned n = N_;
    c.T = c.R[n] & 1;
    c.R[n] = static_cast<uint32>(static_cast<sint32>(c.R[n]) >> 1);
}
OPFN(ROTL) {
    const unsigned n = N_;
    c.T = c.R[n] >> 31;
    c.R[n] = (c.R[n] << 1) | c.T;
}
OPFN(ROTR) {
    const unsigned n = N_;
    c.T = c.R[n] & 1;
    c.R[n] = (c.R[n] >> 1) | (c.T << 31);
}
OPFN(ROTCL) {
    const unsigned n = N_;
    const uint32 t = c.R[n] >> 31;
    c.R[n] = (c.R[n] << 1) | c.T;
    c.T = t;
}
OPFN(ROTCR) {
    const unsigned n = N_;
    const uint32 t = c.R[n] & 1;
    c.R[n] = (c.R[n] >> 1) | (c.T << 31);
    c.T = t;
}
OPFN(SHLL2) { c.R[N_] <<= 2; }
OPFN(SHLL8) { c.R[N_] <<= 8; }
OPFN(SHLL16) { c.R[N_] <<= 16; }
OPFN(SHLR2) { c.R[N_] >>= 2; }
OPFN(SHLR8) { c.R[N_] >>= 8; }
OPFN(SHLR16) { c.R[N_] >>= 16; }
OPFN(SHAD) {
    const unsigned n = N_;
    const sint32 s = static_cast<sint32>(c.R[M_]);
    if (s >= 0) {
        c.R[n] <<= (s & 31);
    } else if ((s & 31) == 0) {
        c.R[n] = static_cast<sint32>(c.R[n]) < 0 ? 0xFFFFFFFFu : 0;
    } else {
        c.R[n] = static_cast<uint32>(static_cast<sint32>(c.R[n]) >> ((~s & 31) + 1));
    }
}
OPFN(SHLD) {
    const unsigned n = N_;
    const sint32 s = static_cast<sint32>(c.R[M_]);
    if (s >= 0) {
        c.R[n] <<= (s & 31);
    } else if ((s & 31) == 0) {
        c.R[n] = 0;
    } else {
        c.R[n] >>= ((~s & 31) + 1);
    }
}

// ============================================================================================================
// System registers

OPFN(CLRT) { c.T = 0; }
OPFN(SETT) { c.T = 1; }
OPFN(CLRS) { c.sr &= ~kSrS; }
OPFN(SETS) { c.sr |= kSrS; }
OPFN(NOP) {}
OPFN(LDTLB) {}
OPFN(PREF) {}
OPFN(SLEEP) { c.sleeping = true; }

OPFN(LDC_SR) { c.SetSR(c.R[N_]); }
OPFN(LDC_GBR) { c.GBR = c.R[N_]; }
OPFN(LDC_VBR) { c.VBR = c.R[N_]; }
OPFN(LDC_SSR) { c.SSR = c.R[N_]; }
OPFN(LDC_SPC) { c.SPC = c.R[N_]; }
OPFN(LDC_BANK) { c.bank[(op >> 4) & 7] = c.R[N_]; }
OPFN(LDCL_SR) {
    const unsigned n = N_;
    const uint32 v = c.Read32(c.R[n]);
    c.R[n] += 4;
    c.SetSR(v);
}
#define LDCL(name, reg)                                                                                                \
    OPFN(name) {                                                                                                       \
        const unsigned n = N_;                                                                                         \
        c.reg = c.Read32(c.R[n]);                                                                                      \
        c.R[n] += 4;                                                                                                   \
    }
LDCL(LDCL_GBR, GBR)
LDCL(LDCL_VBR, VBR)
LDCL(LDCL_SSR, SSR)
LDCL(LDCL_SPC, SPC)
LDCL(LDSL_MACH, MACH)
LDCL(LDSL_MACL, MACL)
LDCL(LDSL_PR, PR)
#undef LDCL
OPFN(LDCL_BANK) {
    const unsigned n = N_;
    c.bank[(op >> 4) & 7] = c.Read32(c.R[n]);
    c.R[n] += 4;
}
OPFN(STC_SR) { c.R[N_] = c.GetSR(); }
OPFN(STC_GBR) { c.R[N_] = c.GBR; }
OPFN(STC_VBR) { c.R[N_] = c.VBR; }
OPFN(STC_SSR) { c.R[N_] = c.SSR; }
OPFN(STC_SPC) { c.R[N_] = c.SPC; }
OPFN(STC_BANK) { c.R[N_] = c.bank[(op >> 4) & 7]; }
#define STCL(name, expr)                                                                                               \
    OPFN(name) {                                                                                                       \
        const unsigned n = N_;                                                                                         \
        const uint32 v = (expr);                                                                                       \
        const uint32 a = c.R[n] - 4;                                                                                   \
        c.Write32(a, v);                                                                                               \
        c.R[n] = a;                                                                                                    \
    }
STCL(STCL_SR, c.GetSR())
STCL(STCL_GBR, c.GBR)
STCL(STCL_VBR, c.VBR)
STCL(STCL_SSR, c.SSR)
STCL(STCL_SPC, c.SPC)
STCL(STCL_BANK, c.bank[(op >> 4) & 7])
STCL(STSL_MACH, c.MACH)
STCL(STSL_MACL, c.MACL)
STCL(STSL_PR, c.PR)
#undef STCL
OPFN(LDS_MACH) { c.MACH = c.R[N_]; }
OPFN(LDS_MACL) { c.MACL = c.R[N_]; }
OPFN(LDS_PR) { c.PR = c.R[N_]; }
OPFN(STS_MACH) { c.R[N_] = c.MACH; }
OPFN(STS_MACL) { c.R[N_] = c.MACL; }
OPFN(STS_PR) { c.R[N_] = c.PR; }

// ============================================================================================================
// Branches (interpreter only)

OPFN(BRA) { RunDelaySlot(c, c.PC + 4 + SignExtend12(op & 0xFFF) * 2); }
OPFN(BSR) {
    const uint32 target = c.PC + 4 + SignExtend12(op & 0xFFF) * 2;
    c.PR = c.PC + 4;
    RunDelaySlot(c, target);
}
OPFN(BRAF) { RunDelaySlot(c, c.PC + 4 + c.R[N_]); }
OPFN(BSRF) {
    const uint32 target = c.PC + 4 + c.R[N_];
    c.PR = c.PC + 4;
    RunDelaySlot(c, target);
}
OPFN(JMP) { RunDelaySlot(c, c.R[N_]); }
OPFN(JSR) {
    const uint32 target = c.R[N_];
    c.PR = c.PC + 4;
    RunDelaySlot(c, target);
}
OPFN(RTS) { RunDelaySlot(c, c.PR); }
OPFN(RTE) {
    const uint32 target = c.SPC;
    c.SetSR(c.SSR);
    RunDelaySlot(c, target);
}
OPFN(BT) {
    if (c.T) {
        c.PC = c.PC + 4 + SignExtend8(op & 0xFF) * 2;
        c.branched = true;
        c.cycles += 1;
    }
}
OPFN(BF) {
    if (!c.T) {
        c.PC = c.PC + 4 + SignExtend8(op & 0xFF) * 2;
        c.branched = true;
        c.cycles += 1;
    }
}
OPFN(BTS) {
    if (c.T) {
        RunDelaySlot(c, c.PC + 4 + SignExtend8(op & 0xFF) * 2);
    }
}
OPFN(BFS) {
    if (!c.T) {
        RunDelaySlot(c, c.PC + 4 + SignExtend8(op & 0xFF) * 2);
    }
}
OPFN(TRAPA) {
    c.TakeException(0x160, c.PC + 2);
    c.branched = true;
}
void ILLEGAL(SH3 &c, uint16 op); // (sh3.cpp)

#undef OPFN
#undef N_
#undef M_

} // namespace spelunk::ops
