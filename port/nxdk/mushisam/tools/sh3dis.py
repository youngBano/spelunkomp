#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
"""SH-3 disassembler (big-endian), for reading the game's code and for the translator.

    sh4dis.py FILE BASE START [COUNT]     disassemble FILE (loaded at BASE) from START
"""
import struct
import sys

# (mask, match, format): r = Rn (bits 8-11), m = Rm (bits 4-7), i8/d4/d8/d12 immediates
PATTERNS = [
    (0xF0FF, 0x0002, "stc sr,r{n}"), (0xF0FF, 0x0012, "stc gbr,r{n}"), (0xF0FF, 0x0022, "stc vbr,r{n}"),
    (0xF0FF, 0x0032, "stc ssr,r{n}"), (0xF0FF, 0x0042, "stc spc,r{n}"), (0xF08F, 0x0082, "stc r{b}_bank,r{n}"),
    (0xF0FF, 0x003A, "stc sgr,r{n}"), (0xF0FF, 0x00FA, "stc dbr,r{n}"), (0xF0FF, 0x0003, "bsrf r{n}"),
    (0xF0FF, 0x0023, "braf r{n}"), (0xF0FF, 0x0083, "pref @r{n}"), (0xF0FF, 0x0093, "ocbi @r{n}"),
    (0xF0FF, 0x00A3, "ocbp @r{n}"), (0xF0FF, 0x00B3, "ocbwb @r{n}"), (0xF0FF, 0x00C3, "movca.l r0,@r{n}"),
    (0xF00F, 0x0004, "mov.b r{m},@(r0,r{n})"), (0xF00F, 0x0005, "mov.w r{m},@(r0,r{n})"),
    (0xF00F, 0x0006, "mov.l r{m},@(r0,r{n})"), (0xF00F, 0x0007, "mul.l r{m},r{n}"), (0xFFFF, 0x0008, "clrt"),
    (0xFFFF, 0x0018, "sett"), (0xFFFF, 0x0028, "clrmac"), (0xFFFF, 0x0038, "ldtlb"), (0xFFFF, 0x0048, "clrs"),
    (0xFFFF, 0x0058, "sets"), (0xFFFF, 0x0009, "nop"), (0xFFFF, 0x0019, "div0u"), (0xF0FF, 0x0029, "movt r{n}"),
    (0xF0FF, 0x000A, "sts mach,r{n}"), (0xF0FF, 0x001A, "sts macl,r{n}"), (0xF0FF, 0x002A, "sts pr,r{n}"),
    (0xF0FF, 0x005A, "sts fpul,r{n}"), (0xF0FF, 0x006A, "sts fpscr,r{n}"), (0xFFFF, 0x000B, "rts"),
    (0xFFFF, 0x001B, "sleep"), (0xFFFF, 0x002B, "rte"), (0xF00F, 0x000C, "mov.b @(r0,r{m}),r{n}"),
    (0xF00F, 0x000D, "mov.w @(r0,r{m}),r{n}"), (0xF00F, 0x000E, "mov.l @(r0,r{m}),r{n}"),
    (0xF00F, 0x000F, "mac.l @r{m}+,@r{n}+"),
    (0xF000, 0x1000, "mov.l r{m},@({d4x4},r{n})"), (0xF000, 0x5000, "mov.l @({d4x4},r{m}),r{n}"),
    (0xF00F, 0x2000, "mov.b r{m},@r{n}"), (0xF00F, 0x2001, "mov.w r{m},@r{n}"), (0xF00F, 0x2002, "mov.l r{m},@r{n}"),
    (0xF00F, 0x2004, "mov.b r{m},@-r{n}"), (0xF00F, 0x2005, "mov.w r{m},@-r{n}"), (0xF00F, 0x2006, "mov.l r{m},@-r{n}"),
    (0xF00F, 0x2007, "div0s r{m},r{n}"), (0xF00F, 0x2008, "tst r{m},r{n}"), (0xF00F, 0x2009, "and r{m},r{n}"),
    (0xF00F, 0x200A, "xor r{m},r{n}"), (0xF00F, 0x200B, "or r{m},r{n}"), (0xF00F, 0x200C, "cmp/str r{m},r{n}"),
    (0xF00F, 0x200D, "xtrct r{m},r{n}"), (0xF00F, 0x200E, "mulu.w r{m},r{n}"), (0xF00F, 0x200F, "muls.w r{m},r{n}"),
    (0xF00F, 0x3000, "cmp/eq r{m},r{n}"), (0xF00F, 0x3002, "cmp/hs r{m},r{n}"), (0xF00F, 0x3003, "cmp/ge r{m},r{n}"),
    (0xF00F, 0x3004, "div1 r{m},r{n}"), (0xF00F, 0x3005, "dmulu.l r{m},r{n}"), (0xF00F, 0x3006, "cmp/hi r{m},r{n}"),
    (0xF00F, 0x3007, "cmp/gt r{m},r{n}"), (0xF00F, 0x3008, "sub r{m},r{n}"), (0xF00F, 0x300A, "subc r{m},r{n}"),
    (0xF00F, 0x300B, "subv r{m},r{n}"), (0xF00F, 0x300C, "add r{m},r{n}"), (0xF00F, 0x300D, "dmuls.l r{m},r{n}"),
    (0xF00F, 0x300E, "addc r{m},r{n}"), (0xF00F, 0x300F, "addv r{m},r{n}"),
    (0xF0FF, 0x4000, "shll r{n}"), (0xF0FF, 0x4001, "shlr r{n}"), (0xF0FF, 0x4002, "sts.l mach,@-r{n}"),
    (0xF0FF, 0x4003, "stc.l sr,@-r{n}"), (0xF0FF, 0x4004, "rotl r{n}"), (0xF0FF, 0x4005, "rotr r{n}"),
    (0xF0FF, 0x4006, "lds.l @r{n}+,mach"), (0xF0FF, 0x4007, "ldc.l @r{n}+,sr"), (0xF0FF, 0x4008, "shll2 r{n}"),
    (0xF0FF, 0x4009, "shlr2 r{n}"), (0xF0FF, 0x400A, "lds r{n},mach"), (0xF0FF, 0x400B, "jsr @r{n}"),
    (0xF0FF, 0x400E, "ldc r{n},sr"), (0xF0FF, 0x4010, "dt r{n}"), (0xF0FF, 0x4011, "cmp/pz r{n}"),
    (0xF0FF, 0x4012, "sts.l macl,@-r{n}"), (0xF0FF, 0x4013, "stc.l gbr,@-r{n}"), (0xF0FF, 0x4015, "cmp/pl r{n}"),
    (0xF0FF, 0x4016, "lds.l @r{n}+,macl"), (0xF0FF, 0x4017, "ldc.l @r{n}+,gbr"), (0xF0FF, 0x4018, "shll8 r{n}"),
    (0xF0FF, 0x4019, "shlr8 r{n}"), (0xF0FF, 0x401A, "lds r{n},macl"), (0xF0FF, 0x401B, "tas.b @r{n}"),
    (0xF0FF, 0x401E, "ldc r{n},gbr"), (0xF0FF, 0x4020, "shal r{n}"), (0xF0FF, 0x4021, "shar r{n}"),
    (0xF0FF, 0x4022, "sts.l pr,@-r{n}"), (0xF0FF, 0x4023, "stc.l vbr,@-r{n}"), (0xF0FF, 0x4024, "rotcl r{n}"),
    (0xF0FF, 0x4025, "rotcr r{n}"), (0xF0FF, 0x4026, "lds.l @r{n}+,pr"), (0xF0FF, 0x4027, "ldc.l @r{n}+,vbr"),
    (0xF0FF, 0x4028, "shll16 r{n}"), (0xF0FF, 0x4029, "shlr16 r{n}"), (0xF0FF, 0x402A, "lds r{n},pr"),
    (0xF0FF, 0x402B, "jmp @r{n}"), (0xF0FF, 0x402E, "ldc r{n},vbr"), (0xF0FF, 0x4032, "stc.l sgr,@-r{n}"),
    (0xF0FF, 0x4033, "stc.l ssr,@-r{n}"), (0xF0FF, 0x4037, "ldc.l @r{n}+,ssr"), (0xF0FF, 0x403E, "ldc r{n},ssr"),
    (0xF0FF, 0x4043, "stc.l spc,@-r{n}"), (0xF0FF, 0x4047, "ldc.l @r{n}+,spc"), (0xF0FF, 0x404E, "ldc r{n},spc"),
    (0xF0FF, 0x4052, "sts.l fpul,@-r{n}"), (0xF0FF, 0x4056, "lds.l @r{n}+,fpul"), (0xF0FF, 0x405A, "lds r{n},fpul"),
    (0xF0FF, 0x4062, "sts.l fpscr,@-r{n}"), (0xF0FF, 0x4066, "lds.l @r{n}+,fpscr"), (0xF0FF, 0x406A, "lds r{n},fpscr"),
    (0xF0FF, 0x40F2, "stc.l dbr,@-r{n}"), (0xF0FF, 0x40F6, "ldc.l @r{n}+,dbr"), (0xF0FF, 0x40FA, "ldc r{n},dbr"),
    (0xF08F, 0x4083, "stc.l r{b}_bank,@-r{n}"), (0xF08F, 0x4087, "ldc.l @r{n}+,r{b}_bank"),
    (0xF08F, 0x408E, "ldc r{n},r{b}_bank"), (0xF00F, 0x400C, "shad r{m},r{n}"), (0xF00F, 0x400D, "shld r{m},r{n}"),
    (0xF00F, 0x400F, "mac.w @r{m}+,@r{n}+"),
    (0xF00F, 0x6000, "mov.b @r{m},r{n}"), (0xF00F, 0x6001, "mov.w @r{m},r{n}"), (0xF00F, 0x6002, "mov.l @r{m},r{n}"),
    (0xF00F, 0x6003, "mov r{m},r{n}"), (0xF00F, 0x6004, "mov.b @r{m}+,r{n}"), (0xF00F, 0x6005, "mov.w @r{m}+,r{n}"),
    (0xF00F, 0x6006, "mov.l @r{m}+,r{n}"), (0xF00F, 0x6007, "not r{m},r{n}"), (0xF00F, 0x6008, "swap.b r{m},r{n}"),
    (0xF00F, 0x6009, "swap.w r{m},r{n}"), (0xF00F, 0x600A, "negc r{m},r{n}"), (0xF00F, 0x600B, "neg r{m},r{n}"),
    (0xF00F, 0x600C, "extu.b r{m},r{n}"), (0xF00F, 0x600D, "extu.w r{m},r{n}"), (0xF00F, 0x600E, "exts.b r{m},r{n}"),
    (0xF00F, 0x600F, "exts.w r{m},r{n}"), (0xF000, 0x7000, "add #{i8},r{n}"),
    (0xFF00, 0x8000, "mov.b r0,@({d4},r{m})"), (0xFF00, 0x8100, "mov.w r0,@({d4x2},r{m})"),
    (0xFF00, 0x8400, "mov.b @({d4},r{m}),r0"), (0xFF00, 0x8500, "mov.w @({d4x2},r{m}),r0"),
    (0xFF00, 0x8800, "cmp/eq #{i8},r0"), (0xFF00, 0x8900, "bt {t8}"), (0xFF00, 0x8B00, "bf {t8}"),
    (0xFF00, 0x8D00, "bt/s {t8}"), (0xFF00, 0x8F00, "bf/s {t8}"),
    (0xF000, 0x9000, "mov.w @({pcw}),r{n}"), (0xF000, 0xA000, "bra {t12}"), (0xF000, 0xB000, "bsr {t12}"),
    (0xFF00, 0xC000, "mov.b r0,@({d8},gbr)"), (0xFF00, 0xC100, "mov.w r0,@({d8x2},gbr)"),
    (0xFF00, 0xC200, "mov.l r0,@({d8x4},gbr)"), (0xFF00, 0xC300, "trapa #{u8}"),
    (0xFF00, 0xC400, "mov.b @({d8},gbr),r0"), (0xFF00, 0xC500, "mov.w @({d8x2},gbr),r0"),
    (0xFF00, 0xC600, "mov.l @({d8x4},gbr),r0"), (0xFF00, 0xC700, "mova {pcl},r0"), (0xFF00, 0xC800, "tst #{u8},r0"),
    (0xFF00, 0xC900, "and #{u8},r0"), (0xFF00, 0xCA00, "xor #{u8},r0"), (0xFF00, 0xCB00, "or #{u8},r0"),
    (0xFF00, 0xCC00, "tst.b #{u8},@(r0,gbr)"), (0xFF00, 0xCD00, "and.b #{u8},@(r0,gbr)"),
    (0xFF00, 0xCE00, "xor.b #{u8},@(r0,gbr)"), (0xFF00, 0xCF00, "or.b #{u8},@(r0,gbr)"),
    (0xF000, 0xD000, "mov.l @({pcl}),r{n}"), (0xF000, 0xE000, "mov #{i8},r{n}"),
    (0xF00F, 0xF000, "fadd fr{m},fr{n}"), (0xF00F, 0xF001, "fsub fr{m},fr{n}"), (0xF00F, 0xF002, "fmul fr{m},fr{n}"),
    (0xF00F, 0xF003, "fdiv fr{m},fr{n}"), (0xF00F, 0xF004, "fcmp/eq fr{m},fr{n}"), (0xF00F, 0xF005, "fcmp/gt fr{m},fr{n}"),
    (0xF00F, 0xF006, "fmov.s @(r0,r{m}),fr{n}"), (0xF00F, 0xF007, "fmov.s fr{m},@(r0,r{n})"),
    (0xF00F, 0xF008, "fmov.s @r{m},fr{n}"), (0xF00F, 0xF009, "fmov.s @r{m}+,fr{n}"), (0xF00F, 0xF00A, "fmov.s fr{m},@r{n}"),
    (0xF00F, 0xF00B, "fmov.s fr{m},@-r{n}"), (0xF00F, 0xF00C, "fmov fr{m},fr{n}"), (0xF00F, 0xF00E, "fmac fr0,fr{m},fr{n}"),
    (0xF0FF, 0xF00D, "fsts fpul,fr{n}"), (0xF0FF, 0xF01D, "flds fr{n},fpul"), (0xF0FF, 0xF02D, "float fpul,fr{n}"),
    (0xF0FF, 0xF03D, "ftrc fr{n},fpul"), (0xF0FF, 0xF04D, "fneg fr{n}"), (0xF0FF, 0xF05D, "fabs fr{n}"),
    (0xF0FF, 0xF06D, "fsqrt fr{n}"), (0xF0FF, 0xF07D, "fsrra fr{n}"), (0xF0FF, 0xF08D, "fldi0 fr{n}"),
    (0xF0FF, 0xF09D, "fldi1 fr{n}"), (0xF1FF, 0xF0AD, "fcnvsd fpul,dr{n}"), (0xF1FF, 0xF0BD, "fcnvds dr{n},fpul"),
    (0xF0FF, 0xF0ED, "fipr fv{fm},fv{fn}"), (0xF3FF, 0xF1FD, "ftrv xmtrx,fv{fn}"), (0xF1FF, 0xF0FD, "fsca fpul,dr{n}"),
    (0xFFFF, 0xFBFD, "frchg"), (0xFFFF, 0xF3FD, "fschg"),
]

BRANCHES = {"bra", "bsr", "braf", "bsrf", "jmp", "jsr", "rts", "rte", "bt", "bf", "bt/s", "bf/s"}
DELAYED = {"bra", "bsr", "braf", "bsrf", "jmp", "jsr", "rts", "rte", "bt/s", "bf/s"}


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s12(v):
    return v - 0x1000 if v & 0x800 else v


def decode(op, pc):
    """(mnemonic text, pattern format) or ('.word', None)"""
    for mask, match, fmt in PATTERNS:
        if op & mask == match:
            n, m = (op >> 8) & 15, (op >> 4) & 15
            fields = {
                "n": n, "m": m, "b": m & 7, "i8": s8(op & 0xFF), "u8": "0x%02X" % (op & 0xFF),
                "d4": op & 15, "d4x2": (op & 15) * 2, "d4x4": (op & 15) * 4,
                "d8": op & 0xFF, "d8x2": (op & 0xFF) * 2, "d8x4": (op & 0xFF) * 4,
                "t8": "0x%08X" % ((pc + 4 + s8(op & 0xFF) * 2) & 0xFFFFFFFF),
                "t12": "0x%08X" % ((pc + 4 + s12(op & 0xFFF) * 2) & 0xFFFFFFFF),
                "pcw": "0x%08X" % ((pc + 4 + (op & 0xFF) * 2) & 0xFFFFFFFF),
                "pcl": "0x%08X" % (((pc & ~3) + 4 + (op & 0xFF) * 4) & 0xFFFFFFFF),
                "fn": (op >> 10) & 3, "fm": (op >> 8) & 3,
            }
            return fmt.format(**fields), fmt
    return ".word 0x%04X" % op, None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    base, start = int(sys.argv[2], 0), int(sys.argv[3], 0)
    count = int(sys.argv[4], 0) if len(sys.argv) > 4 else 32
    pc = start
    for _ in range(count):
        off = (pc - base) & 0x1FFFFFFF if base >= 0x80000000 else pc - base
        off = pc - base
        if off < 0 or off + 2 > len(data):
            break
        op = struct.unpack_from(">H", data, off)[0]
        text, _ = decode(op, pc)
        extra = ""
        if "@(0x" in text and ",r" in text and text.startswith("mov.l @(0x"):
            target = int(text.split("@(")[1].split(")")[0], 16)
            t = target - base
            if 0 <= t + 4 <= len(data):
                extra = "   ; =0x%08X" % struct.unpack_from(">I", data, t)[0]
        print("%08X  %04X  %s%s" % (pc, op, text, extra))
        pc += 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
