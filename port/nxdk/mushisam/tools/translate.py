#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Tommy McLeroy. Part of spelunkomp.
"""Translates recorded SH-3 blocks to C++.

    translate.py [--found] OUTDIR BLOCKS.txt [BLOCKS.txt...]

Each BLOCKS line is `PC BYTES` (hex), as the headless runner's --record writes them: the block's code as it was
in RAM when it ran (each version of the code at an address, where code is loaded over other code: a translation is
used only while RAM holds the bytes it was made from). Lines ending in `found` are blocks the recorder found by
following the code from the ones that ran (--record-static), not seen running: translated only with --found. Blocks are cut by the rule in runtime/blocks.h; the instructions themselves are the
functions of machine/sh3_ops.h, called with the opcode as a constant (the decoding table is read from machine/sh3.cpp,
so the two cannot disagree). Cycles are counted as the interpreter counts them, instruction by instruction.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BRANCHES = {"BRA", "BSR", "BRAF", "BSRF", "JMP", "JSR", "RTS", "RTE", "BT", "BF", "BTS", "BFS"}
BARRIERS = {"TRAPA", "ILLEGAL", "SLEEP", "LDC_SR", "LDCL_SR"}
# A branch taken, for the board's bus timing (machine/bus_timing.h): where the interpreter calls it, after the branch's
# and its slot's cycles
TIME_BRANCH = "if (c.timing) [[unlikely]] {{ c.TimeBranch(0x{a:08X}u, {target}); }}"
NOT_DELAYED = {"BT", "BF"}
MAX_INSTRUCTIONS = 128
PER_FILE = 600


def load_table():
    src = open(os.path.join(ROOT, "engine", "machine", "sh3.cpp")).read()
    body = src[src.index("const Pattern kPatterns[]"):src.index("bool g_built")]
    pats = [(int(m, 16), int(v, 16), name, int(cyc))
            for m, v, name, cyc in re.findall(r"\{(0x[0-9A-F]+), (0x[0-9A-F]+), (\w+), (\d+)\}", body)]
    table = []
    for op in range(0x10000):
        for mask, match, name, cyc in pats:
            if op & mask == match:
                table.append((name, cyc))
                break
        else:
            table.append(("ILLEGAL", 1))
    return table


def s8(v):
    return v - 0x100 if v & 0x80 else v


def s12(v):
    return v - 0x1000 if v & 0x800 else v


def u32(v):
    return v & 0xFFFFFFFF


def sx8(v):
    """the value as the CPU sign-extends a byte, as C++"""
    v &= 0xFF
    return f"0x{u32(v - 0x100 if v & 0x80 else v):08X}u"


# The instructions the code is mostly made of, as plain C++ with the registers and constants worked out (the same
# as sh3_ops.h does them; the rest call sh3_ops.h). R(n) is c.R[n]
def direct(name, op):
    n, m, i4, i8 = (op >> 8) & 15, (op >> 4) & 15, op & 15, op & 0xFF
    R = lambda k: f"c.R[{k}]"
    se8 = lambda e: f"static_cast<uint32>(static_cast<sint32>(static_cast<sint8>({e})))"
    se16 = lambda e: f"static_cast<uint32>(static_cast<sint32>(static_cast<sint16>({e})))"
    return {
        "MOV_I": lambda: [f"{R(n)} = {sx8(i8)};"],
        "MOV": lambda: [f"{R(n)} = {R(m)};"],
        "ADD_I": lambda: [f"{R(n)} += {sx8(i8)};"],
        "ADD": lambda: [f"{R(n)} += {R(m)};"],
        "SUB": lambda: [f"{R(n)} -= {R(m)};"],
        "AND": lambda: [f"{R(n)} &= {R(m)};"],
        "OR": lambda: [f"{R(n)} |= {R(m)};"],
        "XOR": lambda: [f"{R(n)} ^= {R(m)};"],
        "NOT": lambda: [f"{R(n)} = ~{R(m)};"],
        "NEG": lambda: [f"{R(n)} = 0u - {R(m)};"],
        "NOP": lambda: [],
        "TST": lambda: [f"c.T = ({R(n)} & {R(m)}) == 0;"],
        "CMPEQ_I": lambda: [f"c.T = {R(0)} == {sx8(i8)};"],
        "CMPEQ": lambda: [f"c.T = {R(n)} == {R(m)};"],
        "CMPHS": lambda: [f"c.T = {R(n)} >= {R(m)};"],
        "CMPHI": lambda: [f"c.T = {R(n)} > {R(m)};"],
        "CMPGE": lambda: [f"c.T = static_cast<sint32>({R(n)}) >= static_cast<sint32>({R(m)});"],
        "CMPGT": lambda: [f"c.T = static_cast<sint32>({R(n)}) > static_cast<sint32>({R(m)});"],
        "CMPPZ": lambda: [f"c.T = static_cast<sint32>({R(n)}) >= 0;"],
        "CMPPL": lambda: [f"c.T = static_cast<sint32>({R(n)}) > 0;"],
        "DT": lambda: [f"{R(n)} -= 1;", f"c.T = {R(n)} == 0;"],
        "EXTUB": lambda: [f"{R(n)} = {R(m)} & 0xFFu;"],
        "EXTUH": lambda: [f"{R(n)} = {R(m)} & 0xFFFFu;"],
        "EXTSB": lambda: [f"{R(n)} = {se8(R(m))};"],
        "EXTSW": lambda: [f"{R(n)} = {se16(R(m))};"],
        "SHLL2": lambda: [f"{R(n)} <<= 2;"],
        "SHLL8": lambda: [f"{R(n)} <<= 8;"],
        "SHLL16": lambda: [f"{R(n)} <<= 16;"],
        "SHLR2": lambda: [f"{R(n)} >>= 2;"],
        "SHLR8": lambda: [f"{R(n)} >>= 8;"],
        "SHLR16": lambda: [f"{R(n)} >>= 16;"],
        "SHLL": lambda: [f"c.T = {R(n)} >> 31;", f"{R(n)} <<= 1;"],
        "SHLR": lambda: [f"c.T = {R(n)} & 1;", f"{R(n)} >>= 1;"],
        "SHAR": lambda: [f"c.T = {R(n)} & 1;", f"{R(n)} = static_cast<uint32>(static_cast<sint32>({R(n)}) >> 1);"],
        "MOVL_L": lambda: [f"{R(n)} = c.Read32({R(m)});"],
        "MOVL_L4": lambda: [f"{R(n)} = c.Read32({R(m)} + {i4 * 4}u);"],
        "MOVL_L0": lambda: [f"{R(n)} = c.Read32({R(m)} + {R(0)});"],
        "MOVL_S": lambda: [f"c.Write32({R(n)}, {R(m)});"],
        "MOVL_S4": lambda: [f"c.Write32({R(n)} + {i4 * 4}u, {R(m)});"],
        "MOVL_S0": lambda: [f"c.Write32({R(n)} + {R(0)}, {R(m)});"],
        "MOVL_P": lambda: ([f"{R(n)} = c.Read32({R(m)});"] if n == m else
                           [f"{{ const uint32 v = c.Read32({R(m)}); {R(m)} += 4; {R(n)} = v; }}"]),
        "MOVL_M": lambda: [f"{{ const uint32 a = {R(n)} - 4; c.Write32(a, {R(m)}); {R(n)} = a; }}"],
        "MOVB_L": lambda: [f"{R(n)} = {se8(f'c.Read8({R(m)})')};"],
        "MOVB_L0": lambda: [f"{R(n)} = {se8(f'c.Read8({R(m)} + {R(0)})')};"],
        "MOVB_L4": lambda: [f"{R(0)} = {se8(f'c.Read8({R(m)} + {i4}u)')};"],
        "MOVB_S": lambda: [f"c.Write8({R(n)}, static_cast<uint8>({R(m)}));"],
        "MOVB_S0": lambda: [f"c.Write8({R(n)} + {R(0)}, static_cast<uint8>({R(m)}));"],
        "MOVB_S4": lambda: [f"c.Write8({R(m)} + {i4}u, static_cast<uint8>({R(0)}));"],
        "MOVW_L": lambda: [f"{R(n)} = {se16(f'c.Read16({R(m)})')};"],
        "MOVW_L0": lambda: [f"{R(n)} = {se16(f'c.Read16({R(m)} + {R(0)})')};"],
        "MOVW_L4": lambda: [f"{R(0)} = {se16(f'c.Read16({R(m)} + {i4 * 2}u)')};"],
        "MOVW_S": lambda: [f"c.Write16({R(n)}, static_cast<uint16>({R(m)}));"],
        "MOVW_S0": lambda: [f"c.Write16({R(n)} + {R(0)}, static_cast<uint16>({R(m)}));"],
        "MOVW_S4": lambda: [f"c.Write16({R(m)} + {i4 * 2}u, static_cast<uint16>({R(0)}));"],
        "STS_PR": lambda: [f"{R(n)} = c.PR;"],
        "LDS_PR": lambda: [f"c.PR = {R(n)};"],
        "STSL_PR": lambda: [f"{{ const uint32 a = {R(n)} - 4; c.Write32(a, c.PR); {R(n)} = a; }}"],
        "LDSL_PR": lambda: [f"c.PR = c.Read32({R(n)});", f"{R(n)} += 4;"],
    }.get(name, lambda: None)()


def instruction(table, op, a, lines, indent):
    """C++ for one instruction that does not branch, at address a. One that touches memory says where it is first
    (c.at: the board's bus timing looks its own cache line up, machine/bus_timing.h)"""
    own = []
    instruction_lines(table, op, a, own, indent)
    if any("Read" in line or "Write" in line or "(c, 0x" in line for line in own):
        lines.append(f"{' ' * indent}c.at = 0x{u32(a):08X}u;")
    lines.extend(own)


def instruction_lines(table, op, a, lines, indent):
    name, cyc = table[op]
    n = (op >> 8) & 15
    pad = " " * indent
    plain = direct(name, op)
    if plain is not None:
        lines.extend(pad + line for line in plain)
    elif name == "MOVW_PC":
        lines.append(f"{pad}c.R[{n}] = SignExtend16(c.Read16(0x{u32(a + 4 + (op & 0xFF) * 2):08X}u));")
    elif name == "MOVL_PC":
        lines.append(f"{pad}c.R[{n}] = c.Read32(0x{u32((a & ~3) + 4 + (op & 0xFF) * 4):08X}u);")
    elif name == "MOVA":
        lines.append(f"{pad}c.R[0] = 0x{u32((a & ~3) + 4 + (op & 0xFF) * 4):08X}u;")
    else:
        lines.append(f"{pad}{name}(c, 0x{op:04X});")
    lines.append(f"{pad}c.cycles += {cyc};")


def block_name(pc, version):
    """the C++ name of a block's version (the first plain, as before versions were kept)"""
    return f"{pc:08X}" if version == 0 else f"{pc:08X}_{version}"


def block_function(table, pc, code, name):
    ops = [(code[i] << 8) | code[i + 1] for i in range(0, len(code) - 1, 2)]  # (big-endian)
    lines = [f"static void B_{name}(SH3 &c) {{", "    for (;;) {"]
    a = pc
    i = 0
    count = 0
    while True:
        op = ops[i]
        name, cyc = table[op]
        if name in BRANCHES:
            slot = ops[i + 1] if i + 1 < len(ops) else None
            if name in ("BT", "BF"):
                target = u32(a + 4 + s8(op & 0xFF) * 2)
                cond = "c.T" if name == "BT" else "!c.T"
                lines.append(f"    if ({cond}) {{")
                lines.append(f"        c.cycles += {cyc + 1};")
                lines.append(f"        {TIME_BRANCH.format(a=u32(a), target=f'0x{target:08X}u')}")
                lines.append(f"        c.PC = 0x{target:08X}u;")
                lines.append("    } else {")
                lines.append(f"        c.cycles += {cyc};")
                lines.append(f"        c.PC = 0x{u32(a + 2):08X}u;")
                lines.append("    }")
                break
            if slot is None:
                raise ValueError("block %08X: delayed branch without its slot" % pc)
            sname, scyc = table[slot]
            if name in ("BTS", "BFS"):
                target = u32(a + 4 + s8(op & 0xFF) * 2)
                cond = "c.T" if name == "BTS" else "!c.T"
                lines.append(f"    if ({cond}) {{")
                lines.append("        c.inSlot = true;")
                instruction(table, slot, a + 2, lines, 8)
                lines.append("        c.inSlot = false;")
                lines.append(f"        c.cycles += {cyc};")
                lines.append(f"        {TIME_BRANCH.format(a=u32(a), target=f'0x{target:08X}u')}")
                lines.append(f"        c.PC = 0x{target:08X}u;")
                lines.append("    } else {")
                lines.append(f"        c.cycles += {cyc};")
                lines.append(f"        c.PC = 0x{u32(a + 2):08X}u;")
                lines.append("    }")
                break
            if name in ("BRA", "BSR"):
                target = f"0x{u32(a + 4 + s12(op & 0xFFF) * 2):08X}u"
            elif name in ("BRAF", "BSRF"):
                lines.append(f"    const uint32 target = 0x{u32(a + 4):08X}u + c.R[{(op >> 8) & 15}];")
                target = "target"
            elif name in ("JMP", "JSR"):
                lines.append(f"    const uint32 target = c.R[{(op >> 8) & 15}];")
                target = "target"
            elif name == "RTS":
                lines.append("    const uint32 target = c.PR;")
                target = "target"
            else:  # RTE
                lines.append("    const uint32 target = c.SPC;")
                lines.append("    c.SetSR(c.SSR);")
                target = "target"
            if name in ("BSR", "BSRF", "JSR"):
                lines.append(f"    c.PR = 0x{u32(a + 4):08X}u;")
            instruction(table, slot, a + 2, lines, 4)
            lines.append(f"    c.cycles += {cyc};")
            lines.append(f"    {TIME_BRANCH.format(a=u32(a), target=target)}")
            lines.append(f"    c.PC = {target};")
            break
        if name in BARRIERS:
            lines.append(f"    c.PC = 0x{a:08X}u; // (the instruction's own address, which it reads)")
            lines.append(f"    c.at = 0x{a:08X}u;")
            lines.append("    c.branched = false;")
            lines.append(f"    {name}(c, 0x{op:04X});")
            lines.append(f"    c.cycles += {cyc};")
            lines.append("    if (!c.branched) {")
            lines.append(f"        c.PC = 0x{u32(a + 2):08X}u;")
            lines.append("    } else if (c.timing) [[unlikely]] {")
            lines.append(f"        c.TimeBranch(0x{a:08X}u, c.PC);")
            lines.append("    }")
            break
        instruction(table, op, a, lines, 4)
        a += 2
        i += 1
        count += 1
        if count == MAX_INSTRUCTIONS or i >= len(ops):
            lines.append(f"    c.PC = 0x{u32(a):08X}u;")
            break
    lines.append("    return;")
    lines.append("    }")
    lines.append("}")
    # (consecutive cycle counts as one)
    merged = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("c.cycles += ") and merged and merged[-1].strip().startswith("c.cycles += ") and \
                len(line) - len(line.lstrip()) == len(merged[-1]) - len(merged[-1].lstrip()):
            pad = line[:len(line) - len(line.lstrip())]
            total = int(merged[-1].strip()[12:-1]) + int(stripped[12:-1])
            merged[-1] = f"{pad}c.cycles += {total};"
        else:
            merged.append(line)
    lines = merged
    # A branch back to the block's own start runs the block again here when the dispatcher would (the same checks)
    again = f"c.PC = 0x{pc:08X}u;"
    out = []
    for line in lines:
        out.append(line)
        if line.strip() == again:
            pad = line[:len(line) - len(line.lstrip())]
            out.append(f"{pad}if (c.cycles < c.blockStop && !c.interruptReady) {{")
            out.append(f"{pad}    continue;")
            out.append(f"{pad}}}")
            out.append(f"{pad}return;")
    return out


def main():
    args = sys.argv[1:]
    found = "--found" in args
    args = [a for a in args if a != "--found"]
    if len(args) < 2:
        print(__doc__)
        return 2
    out = args[0]
    ran, seen = set(), set()
    for path in args[1:]:
        for line in open(path):
            parts = line.split()
            if len(parts) >= 2:
                key = (int(parts[0], 16), bytes.fromhex(parts[1]))
                seen.add(key)
                if len(parts) == 2:
                    ran.add(key)
    keys = sorted(seen if found else ran)
    # (each version of an address's code, in the order of its bytes: named B_<PC> for the first, B_<PC>_<n> after)
    blocks, names = {}, {}
    for pc, code in keys:
        version = names.get(pc, 0)
        names[pc] = version + 1
        blocks[(pc, code)] = block_name(pc, version)
    table = load_table()
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith(".cpp"):
            os.remove(os.path.join(out, f))
    order = keys
    header = ['// Generated by tools/translate.py from the player\'s game: never committed.',
              '#include "blocks.h"', '#include "sh3_ops.h"', '', 'namespace spelunk {', 'using namespace ops;', '']
    for chunk in range(0, len(order), PER_FILE):
        lines = list(header)
        for pc, code in order[chunk:chunk + PER_FILE]:
            lines += block_function(table, pc, code, blocks[(pc, code)])
            lines.append("")
        lines.append("const NativeBlock *Blocks%d(size_t &count);" % (chunk // PER_FILE))
        lines.append("")
        lines.append("const NativeBlock kBlocks%d[] = {" % (chunk // PER_FILE))
        for pc, code in order[chunk:chunk + PER_FILE]:
            name = blocks[(pc, code)]
            lines.append(f"    {{0x{pc:08X}u, {len(code)}, k_{name}, B_{name}}},")
        lines.append("};")
        lines.append("const NativeBlock *Blocks%d(size_t &count) {" % (chunk // PER_FILE))
        lines.append("    count = sizeof(kBlocks%d) / sizeof(kBlocks%d[0]);" % (chunk // PER_FILE, chunk // PER_FILE))
        lines.append("    return kBlocks%d;" % (chunk // PER_FILE))
        lines.append("}")
        lines.append("")
        lines.append("} // namespace spelunk")
        # the bytes each block was made from, ahead of the table
        data = []
        for pc, code in order[chunk:chunk + PER_FILE]:
            data.append("static const uint8_t k_%s[] = {%s};" % (blocks[(pc, code)], ",".join(str(b) for b in code)))
        at = lines.index("namespace spelunk {") + 3
        lines[at:at] = data + [""]
        with open(os.path.join(out, "blocks_%03d.cpp" % (chunk // PER_FILE)), "w") as f:
            f.write("\n".join(lines) + "\n")
    files = (len(order) + PER_FILE - 1) // PER_FILE
    lines = list(header)
    for k in range(files):
        lines.append("const NativeBlock *Blocks%d(size_t &count);" % k)
    lines.append("")
    lines.append("const NativeBlock *NativeBlocks(size_t &count) {")
    lines.append("    static std::vector<NativeBlock> all = [] {")
    lines.append("        std::vector<NativeBlock> v;")
    lines.append("        size_t n = 0;")
    for k in range(files):
        lines.append("        { const NativeBlock *b = Blocks%d(n); v.insert(v.end(), b, b + n); }" % k)
    lines.append("        return v;")
    lines.append("    }();")
    lines.append("    count = all.size();")
    lines.append("    return all.data();")
    lines.append("}")
    lines.append("")
    lines.append("} // namespace spelunk")
    with open(os.path.join(out, "registry.cpp"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("%d blocks (%d addresses) in %d files" % (len(order), len(names), files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
