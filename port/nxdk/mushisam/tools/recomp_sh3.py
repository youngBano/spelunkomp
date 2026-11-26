#!/usr/bin/env python3
"""Whole-Function AOT Static SH-3 -> Pentium III C++20 Recompiler for Mushihimesama (CV1000-B).

Upgrades spelunkomp's basic-block translator (translate.py) for Original Xbox:
  1. Whole-function CFG lifting: merges intra-function basic blocks (BT/BF/BTS/BFS/BRA/fallthrough)
     into single C++ functions with direct `goto` branches (eliminating ~85-90% of Lookup(PC) dispatches).
  2. Direct C++ calls for static BSR and constant-folded JSR @Rn targets (using Pentium III RSB).
  3. Local register promotion: loads only referenced R0..R15, T, and cycles into local variables
     so Clang/GCC allocates x86 GPRs across loops instead of reloading c.R[] from memory.
  4. Compile-time PC-relative literal pool constant folding (MOV.L/W @(disp,PC)) verified against
     post-boot RAM snapshots (--ram / --ram-end).
  5. Cycle-accurate block-level accumulation with loop back-edge checks only on backward branches.
  6. Strips dead `c.at = ...`, `if (c.timing)`, and ~1 MB of `k_XXXXXXXX[]` verification arrays.
  7. Profile-guided hot/cold TU splitting (--hot) for Pentium III 16 KB L1i / 128 KB L2 cache.

Usage:
  recomp_sh3.py [--ram RAM600.bin] [--ram-end RAM4000.bin] [--hot HOT.txt] OUTDIR BLOCKS.txt
"""
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

BRANCHES = {"BRA", "BSR", "BRAF", "BSRF", "JMP", "JSR", "RTS", "RTE", "BT", "BF", "BTS", "BFS"}
BARRIERS = {"TRAPA", "ILLEGAL", "SLEEP", "LDC_SR", "LDCL_SR"}
NOT_DELAYED = {"BT", "BF"}
MAX_FUNC_BLOCKS = 48
FUNCS_PER_FILE = 120


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
    v &= 0xFF
    return f"0x{u32(v - 0x100 if v & 0x80 else v):08X}u"


def is_ram(a):
    return (a & 0x1F000000) == 0x0C000000 and a < 0xE0000000


class RamOracle:
    """Provides verified compile-time constant folding for immutable PC-relative literal pools."""
    def __init__(self, ram_path=None, ram_end_path=None, code_end=0x0C164000):
        self.ram = open(ram_path, "rb").read() if ram_path and os.path.exists(ram_path) else None
        self.ram_end = open(ram_end_path, "rb").read() if ram_end_path and os.path.exists(ram_end_path) else None
        self.code_end = code_end
        self.folded = 0

    def can_fold(self, addr, size):
        if self.ram is None:
            return False
        if not is_ram(addr):
            return False
        phys = addr & 0x007FFFFF
        if phys < 0x00002000 or phys + size > (self.code_end & 0x007FFFFF):
            return False
        if self.ram_end is not None:
            if self.ram[phys:phys + size] != self.ram_end[phys:phys + size]:
                return False
        return True

    def read32(self, addr):
        if not self.can_fold(addr, 4):
            return None
        phys = addr & 0x007FFFFF
        self.folded += 1
        return struct.unpack_from(">I", self.ram, phys)[0]

    def read16s(self, addr):
        if not self.can_fold(addr, 2):
            return None
        phys = addr & 0x007FFFFF
        self.folded += 1
        val = struct.unpack_from(">h", self.ram, phys)[0]
        return u32(val)


def parse_block(table, pc, code, oracle):
    """Parse a recorded block into instructions, static control-flow edges, and folded constants."""
    ops = [(code[i] << 8) | code[i + 1] for i in range(0, len(code) - 1, 2)]
    a = pc
    i = 0
    insns = []
    edges_intra = []   # (target_pc) for BT/BF/BTS/BFS/BRA/fallthrough
    edges_call = []    # (call_target_pc, ret_pc) for BSR / constant JSR
    const_regs = {}    # reg -> known 32-bit constant within this block

    def track_const(name, op, addr):
        n = (op >> 8) & 15
        m = (op >> 4) & 15
        if name == "MOVL_PC":
            lit = u32((addr & ~3) + 4 + (op & 0xFF) * 4)
            v = oracle.read32(lit)
            if v is not None:
                const_regs[n] = v
            else:
                const_regs.pop(n, None)
        elif name == "MOVW_PC":
            lit = u32(addr + 4 + (op & 0xFF) * 2)
            v = oracle.read16s(lit)
            if v is not None:
                const_regs[n] = v
            else:
                const_regs.pop(n, None)
        elif name == "MOV_I":
            const_regs[n] = u32(s8(op & 0xFF))
        elif name == "MOV":
            if m in const_regs:
                const_regs[n] = const_regs[m]
            else:
                const_regs.pop(n, None)
        elif name == "MOVA":
            const_regs[0] = u32((addr & ~3) + 4 + (op & 0xFF) * 4)
        elif name == "NOP":
            pass
        else:
            # Conservative: clobber destination reg (or all if complex)
            if name in ("MOVB_L4", "MOVW_L4", "MOVB_LG", "MOVW_LG", "MOVL_LG", "AND_I", "OR_I", "XOR_I"):
                const_regs.pop(0, None)
            elif name.startswith(("MOVB_S", "MOVW_S", "MOVL_S", "CMP", "TST", "CLRT", "SETT", "CLRS", "SETS", "PREF")):
                pass
            else:
                const_regs.pop(n, None)
                if name in ("MOVB_P", "MOVW_P", "MOVL_P", "MACL", "MACW"):
                    const_regs.pop(m, None)

    term = None
    while i < len(ops):
        op = ops[i]
        name, cyc = table[op]
        if name in BRANCHES:
            slot = ops[i + 1] if i + 1 < len(ops) else None
            if name in ("BT", "BF"):
                target = u32(a + 4 + s8(op & 0xFF) * 2)
                fall = u32(a + 2)
                edges_intra.extend([target, fall])
                term = (name, op, a, cyc, None, target, fall)
            elif name in ("BTS", "BFS"):
                target = u32(a + 4 + s8(op & 0xFF) * 2)
                fall = u32(a + 2)
                edges_intra.extend([target, fall])
                term = (name, op, a, cyc, slot, target, fall)
            elif name == "BRA":
                target = u32(a + 4 + s12(op & 0xFFF) * 2)
                edges_intra.append(target)
                term = (name, op, a, cyc, slot, target, None)
            elif name == "BSR":
                target = u32(a + 4 + s12(op & 0xFFF) * 2)
                ret_pc = u32(a + 4)
                edges_call.append((target, ret_pc))
                edges_intra.append(ret_pc)
                term = (name, op, a, cyc, slot, target, ret_pc)
            elif name == "JSR":
                reg = (op >> 8) & 15
                known = const_regs.get(reg)
                ret_pc = u32(a + 4)
                if known is not None:
                    edges_call.append((known, ret_pc))
                edges_intra.append(ret_pc)
                term = (name, op, a, cyc, slot, known, ret_pc)
            elif name == "JMP":
                reg = (op >> 8) & 15
                known = const_regs.get(reg)
                if known is not None:
                    edges_intra.append(known)
                term = (name, op, a, cyc, slot, known, None)
            else:
                term = (name, op, a, cyc, slot, None, None)
            break
        if name in BARRIERS:
            fall = u32(a + 2)
            if name != "ILLEGAL":
                edges_intra.append(fall)
            term = (name, op, a, cyc, None, None, fall)
            break
        insns.append((name, op, a, cyc))
        track_const(name, op, a)
        a = u32(a + 2)
        i += 1
    else:
        edges_intra.append(a)
        term = ("FALL", 0, a, 0, None, a, None)

    return {
        "pc": pc,
        "code": code,
        "insns": insns,
        "term": term,
        "edges_intra": edges_intra,
        "edges_call": edges_call,
    }


def partition_functions(blocks_by_pc):
    """Group basic blocks into whole C++ functions (CFGs) with bounded size."""
    all_pcs = set(blocks_by_pc.keys())
    call_targets = set()
    intra_preds = collections.defaultdict(set)

    for pc, blk in blocks_by_pc.items():
        for ct, _ in blk["edges_call"]:
            if ct in all_pcs:
                call_targets.add(ct)
        for t in blk["edges_intra"]:
            if t in all_pcs:
                intra_preds[t].add(pc)

    # Roots: call targets, interrupt/exception vectors, boot vector, and blocks with no intra-function preds
    roots = set(call_targets)
    for special in (0xA0000000, 0x8C000100, 0x8C000600, 0x0C0023D4):
        if special in all_pcs:
            roots.add(special)
    for pc in all_pcs:
        if not intra_preds[pc]:
            roots.add(pc)

    # Order roots deterministically
    sorted_roots = sorted(roots)
    owner = {}
    functions = []  # list of (root_pc, [member_pcs])

    def build_func(root_pc):
        members = []
        q = collections.deque([root_pc])
        seen = {root_pc}
        while q and len(members) < MAX_FUNC_BLOCKS:
            cur = q.popleft()
            if cur in owner:
                continue
            owner[cur] = root_pc
            members.append(cur)
            blk = blocks_by_pc[cur]
            for nxt in blk["edges_intra"]:
                if nxt in all_pcs and nxt not in owner and nxt not in seen:
                    # Don't swallow another function's entry root
                    if nxt in roots and nxt != root_pc:
                        continue
                    # Keep same memory segment (flash vs RAM)
                    if is_ram(nxt) != is_ram(root_pc):
                        continue
                    seen.add(nxt)
                    q.append(nxt)
        return members

    for r in sorted_roots:
        if r not in owner:
            m = build_func(r)
            if m:
                functions.append((r, m))

    # Any remaining cyclic components with no explicit root
    for pc in sorted(all_pcs):
        if pc not in owner:
            m = build_func(pc)
            if m:
                functions.append((pc, m))

    return functions, owner


def emit_insn_direct(name, op, a, oracle, used_regs, use_t_ref):
    """Emit plain C++ using promoted local regs r0..r15 and T, or return None for sh3_ops fallback."""
    n, m, i4, i8 = (op >> 8) & 15, (op >> 4) & 15, op & 15, op & 0xFF
    R = lambda k: (used_regs.add(k), f"r{k}")[1]
    se8 = lambda e: f"static_cast<uint32>(static_cast<sint32>(static_cast<sint8>({e})))"
    se16 = lambda e: f"static_cast<uint32>(static_cast<sint32>(static_cast<sint16>({e})))"

    def set_t(expr):
        use_t_ref[0] = True
        return f"T = ({expr});"

    if name == "MOVL_PC":
        lit = u32((a & ~3) + 4 + i8 * 4)
        v = oracle.read32(lit)
        if v is not None:
            return [f"{R(n)} = 0x{v:08X}u;"]
        return [f"{R(n)} = recomp::R32(c, 0x{lit:08X}u, cyc);"]
    if name == "MOVW_PC":
        lit = u32(a + 4 + i8 * 2)
        v = oracle.read16s(lit)
        if v is not None:
            return [f"{R(n)} = 0x{v:08X}u;"]
        return [f"{R(n)} = {se16(f'recomp::R16(c, 0x{lit:08X}u, cyc)')};"]
    if name == "MOVA":
        return [f"{R(0)} = 0x{u32((a & ~3) + 4 + i8 * 4):08X}u;"]

    # Direct ALU, shifts, moves, memory, multiply, compare
    if name == "MOV_I": return [f"{R(n)} = {sx8(i8)};"]
    if name == "MOV": return [f"{R(n)} = {R(m)};"]
    if name == "ADD_I": return [f"{R(n)} += {sx8(i8)};"]
    if name == "ADD": return [f"{R(n)} += {R(m)};"]
    if name == "SUB": return [f"{R(n)} -= {R(m)};"]
    if name == "AND": return [f"{R(n)} &= {R(m)};"]
    if name == "AND_I": return [f"{R(0)} &= 0x{i8:02X}u;"]
    if name == "OR": return [f"{R(n)} |= {R(m)};"]
    if name == "OR_I": return [f"{R(0)} |= 0x{i8:02X}u;"]
    if name == "XOR": return [f"{R(n)} ^= {R(m)};"]
    if name == "XOR_I": return [f"{R(0)} ^= 0x{i8:02X}u;"]
    if name == "NOT": return [f"{R(n)} = ~{R(m)};"]
    if name == "NEG": return [f"{R(n)} = 0u - {R(m)};"]
    if name == "NOP" or name == "PREF": return []
    if name == "CLRT": return [set_t("0u")]
    if name == "SETT": return [set_t("1u")]
    if name == "MOVT":
        use_t_ref[0] = True
        return [f"{R(n)} = T;"]
    if name == "TST": return [set_t(f"({R(n)} & {R(m)}) == 0u")]
    if name == "TST_I": return [set_t(f"({R(0)} & 0x{i8:02X}u) == 0u")]
    if name == "CMPEQ_I": return [set_t(f"{R(0)} == {sx8(i8)}")]
    if name == "CMPEQ": return [set_t(f"{R(n)} == {R(m)}")]
    if name == "CMPHS": return [set_t(f"{R(n)} >= {R(m)}")]
    if name == "CMPHI": return [set_t(f"{R(n)} > {R(m)}")]
    if name == "CMPGE": return [set_t(f"static_cast<sint32>({R(n)}) >= static_cast<sint32>({R(m)})")]
    if name == "CMPGT": return [set_t(f"static_cast<sint32>({R(n)}) > static_cast<sint32>({R(m)})")]
    if name == "CMPPZ": return [set_t(f"static_cast<sint32>({R(n)}) >= 0")]
    if name == "CMPPL": return [set_t(f"static_cast<sint32>({R(n)}) > 0")]
    if name == "DT":
        rn = R(n)
        return [f"{rn} -= 1u;", set_t(f"{rn} == 0u")]
    if name == "EXTUB": return [f"{R(n)} = {R(m)} & 0xFFu;"]
    if name == "EXTUW": return [f"{R(n)} = {R(m)} & 0xFFFFu;"]
    if name == "EXTSB": return [f"{R(n)} = {se8(R(m))};"]
    if name == "EXTSW": return [f"{R(n)} = {se16(R(m))};"]
    if name == "SWAPB":
        rm = R(m)
        return [f"{{ const uint32 _v = {rm}; {R(n)} = (_v & 0xFFFF0000u) | ((_v & 0xFFu) << 8) | ((_v >> 8) & 0xFFu); }}"]
    if name == "SWAPW":
        rm = R(m)
        return [f"{{ const uint32 _v = {rm}; {R(n)} = (_v << 16) | (_v >> 16); }}"]
    if name == "XTRCT":
        return [f"{R(n)} = ({R(m)} << 16) | ({R(n)} >> 16);"]
    if name == "SHLL2": return [f"{R(n)} <<= 2;"]
    if name == "SHLL8": return [f"{R(n)} <<= 8;"]
    if name == "SHLL16": return [f"{R(n)} <<= 16;"]
    if name == "SHLR2": return [f"{R(n)} >>= 2;"]
    if name == "SHLR8": return [f"{R(n)} >>= 8;"]
    if name == "SHLR16": return [f"{R(n)} >>= 16;"]
    if name == "SHLL" or name == "SHAL":
        rn = R(n)
        return [set_t(f"{rn} >> 31"), f"{rn} <<= 1;"]
    if name == "SHLR":
        rn = R(n)
        return [set_t(f"{rn} & 1u"), f"{rn} >>= 1;"]
    if name == "SHAR":
        rn = R(n)
        return [set_t(f"{rn} & 1u"), f"{rn} = static_cast<uint32>(static_cast<sint32>({rn}) >> 1);"]
    if name == "SHAD":
        rn, rm = R(n), R(m)
        return [f"{{ const sint32 _s = static_cast<sint32>({rm}); if (_s >= 0) {rn} <<= (_s & 31); else if ((_s & 31) == 0) {rn} = static_cast<sint32>({rn}) < 0 ? 0xFFFFFFFFu : 0u; else {rn} = static_cast<uint32>(static_cast<sint32>({rn}) >> ((~_s & 31) + 1)); }}"]
    if name == "SHLD":
        rn, rm = R(n), R(m)
        return [f"{{ const sint32 _s = static_cast<sint32>({rm}); if (_s >= 0) {rn} <<= (_s & 31); else if ((_s & 31) == 0) {rn} = 0u; else {rn} >>= ((~_s & 31) + 1); }}"]
    if name == "MULL": return [f"c.MACL = {R(n)} * {R(m)};"]
    if name == "MULSW": return [f"c.MACL = static_cast<uint32>(static_cast<sint32>(static_cast<sint16>({R(n)})) * static_cast<sint16>({R(m)}));"]
    if name == "MULUW": return [f"c.MACL = ({R(n)} & 0xFFFFu) * ({R(m)} & 0xFFFFu);"]
    if name == "DMULS":
        return [f"{{ const sint64 _p = static_cast<sint64>(static_cast<sint32>({R(n)})) * static_cast<sint32>({R(m)}); c.MACH = static_cast<uint32>(static_cast<uint64>(_p) >> 32); c.MACL = static_cast<uint32>(_p); }}"]
    if name == "DMULU":
        return [f"{{ const uint64 _p = static_cast<uint64>({R(n)}) * {R(m)}; c.MACH = static_cast<uint32>(_p >> 32); c.MACL = static_cast<uint32>(_p); }}"]
    if name == "STS_MACH": return [f"{R(n)} = c.MACH;"]
    if name == "STS_MACL": return [f"{R(n)} = c.MACL;"]
    if name == "LDS_MACH": return [f"c.MACH = {R(n)};"]
    if name == "LDS_MACL": return [f"c.MACL = {R(n)};"]
    if name == "STS_PR": return [f"{R(n)} = c.PR;"]
    if name == "LDS_PR": return [f"c.PR = {R(n)};"]
    if name == "STSL_PR":
        rn = R(n)
        return [f"{{ const uint32 _a = {rn} - 4u; recomp::W32(c, _a, c.PR, cyc); {rn} = _a; }}"]
    if name == "LDSL_PR":
        rn = R(n)
        return [f"c.PR = recomp::R32(c, {rn}, cyc);", f"{rn} += 4u;"]
    if name == "STSL_MACL":
        rn = R(n)
        return [f"{{ const uint32 _a = {rn} - 4u; recomp::W32(c, _a, c.MACL, cyc); {rn} = _a; }}"]
    if name == "LDSL_MACL":
        rn = R(n)
        return [f"c.MACL = recomp::R32(c, {rn}, cyc);", f"{rn} += 4u;"]
    if name == "STSL_MACH":
        rn = R(n)
        return [f"{{ const uint32 _a = {rn} - 4u; recomp::W32(c, _a, c.MACH, cyc); {rn} = _a; }}"]
    if name == "LDSL_MACH":
        rn = R(n)
        return [f"c.MACH = recomp::R32(c, {rn}, cyc);", f"{rn} += 4u;"]
    if name == "MOVL_L": return [f"{R(n)} = recomp::R32(c, {R(m)}, cyc);"]
    if name == "MOVL_L4": return [f"{R(n)} = recomp::R32(c, {R(m)} + {i4 * 4}u, cyc);"]
    if name == "MOVL_L0": return [f"{R(n)} = recomp::R32(c, {R(m)} + {R(0)}, cyc);"]
    if name == "MOVL_S": return [f"recomp::W32(c, {R(n)}, {R(m)}, cyc);"]
    if name == "MOVL_S4": return [f"recomp::W32(c, {R(n)} + {i4 * 4}u, {R(m)}, cyc);"]
    if name == "MOVL_S0": return [f"recomp::W32(c, {R(n)} + {R(0)}, {R(m)}, cyc);"]
    if name == "MOVL_P":
        return [f"{R(n)} = recomp::R32(c, {R(m)}, cyc);"] if n == m else \
               [f"{{ const uint32 _v = recomp::R32(c, {R(m)}, cyc); {R(m)} += 4u; {R(n)} = _v; }}"]
    if name == "MOVL_M":
        rn = R(n)
        return [f"{{ const uint32 _a = {rn} - 4u; recomp::W32(c, _a, {R(m)}, cyc); {rn} = _a; }}"]
    if name == "MOVB_L": return [f"{R(n)} = {se8(f'recomp::R8(c, {R(m)}, cyc)')};"]
    if name == "MOVB_L0": return [f"{R(n)} = {se8(f'recomp::R8(c, {R(m)} + {R(0)}, cyc)')};"]
    if name == "MOVB_L4": return [f"{R(0)} = {se8(f'recomp::R8(c, {R(m)} + {i4}u, cyc)')};"]
    if name == "MOVB_P":
        return [f"{R(n)} = {se8(f'recomp::R8(c, {R(m)}, cyc)')};"] if n == m else \
               [f"{{ const uint32 _v = {se8(f'recomp::R8(c, {R(m)}, cyc)')}; {R(m)} += 1u; {R(n)} = _v; }}"]
    if name == "MOVB_M":
        rn = R(n)
        return [f"{{ const uint32 _a = {rn} - 1u; recomp::W8(c, _a, static_cast<uint8>({R(m)}), cyc); {rn} = _a; }}"]
    if name == "MOVB_S": return [f"recomp::W8(c, {R(n)}, static_cast<uint8>({R(m)}), cyc);"]
    if name == "MOVB_S0": return [f"recomp::W8(c, {R(n)} + {R(0)}, static_cast<uint8>({R(m)}), cyc);"]
    if name == "MOVB_S4": return [f"recomp::W8(c, {R(m)} + {i4}u, static_cast<uint8>({R(0)}), cyc);"]
    if name == "MOVW_L": return [f"{R(n)} = {se16(f'recomp::R16(c, {R(m)}, cyc)')};"]
    if name == "MOVW_L0": return [f"{R(n)} = {se16(f'recomp::R16(c, {R(m)} + {R(0)}, cyc)')};"]
    if name == "MOVW_L4": return [f"{R(0)} = {se16(f'recomp::R16(c, {R(m)} + {i4 * 2}u, cyc)')};"]
    if name == "MOVW_P":
        return [f"{R(n)} = {se16(f'recomp::R16(c, {R(m)}, cyc)')};"] if n == m else \
               [f"{{ const uint32 _v = {se16(f'recomp::R16(c, {R(m)}, cyc)')}; {R(m)} += 2u; {R(n)} = _v; }}"]
    if name == "MOVW_M":
        rn = R(n)
        return [f"{{ const uint32 _a = {rn} - 2u; recomp::W16(c, _a, static_cast<uint16>({R(m)}), cyc); {rn} = _a; }}"]
    if name == "MOVW_S": return [f"recomp::W16(c, {R(n)}, static_cast<uint16>({R(m)}), cyc);"]
    if name == "MOVW_S0": return [f"recomp::W16(c, {R(n)} + {R(0)}, static_cast<uint16>({R(m)}), cyc);"]
    if name == "MOVW_S4": return [f"recomp::W16(c, {R(m)} + {i4 * 2}u, static_cast<uint16>({R(0)}), cyc);"]
    if name == "MOVL_LG": return [f"{R(0)} = recomp::R32(c, c.GBR + {i8 * 4}u, cyc);"]
    if name == "MOVW_LG": return [f"{R(0)} = {se16(f'recomp::R16(c, c.GBR + {i8 * 2}u, cyc)')};"]
    if name == "MOVB_LG": return [f"{R(0)} = {se8(f'recomp::R8(c, c.GBR + {i8}u, cyc)')};"]
    if name == "MOVL_SG": return [f"recomp::W32(c, c.GBR + {i8 * 4}u, {R(0)}, cyc);"]
    if name == "MOVW_SG": return [f"recomp::W16(c, c.GBR + {i8 * 2}u, static_cast<uint16>({R(0)}), cyc);"]
    if name == "MOVB_SG": return [f"recomp::W8(c, c.GBR + {i8}u, static_cast<uint8>({R(0)}), cyc);"]
    return None


def emit_function(root_pc, members, blocks_by_pc, table, oracle, func_roots_set):
    """Emit a whole-function C++20 routine F_<root_pc>(SH3 &c)."""
    member_set = set(members)
    used_regs = set()
    use_t = [False]
    body_blocks = []  # list of (pc, lines_list)

    # Pre-pass: emit body lines while collecting which R[k] and T are used.
    # For fallback sh3_ops calls, we emit sentinel markers __FLUSH__ and __RELOAD__
    # and expand them once the full used_regs set for the function is known!
    def emit_one(name, op, a, indent):
        pad = " " * indent
        plain = emit_insn_direct(name, op, a, oracle, used_regs, use_t)
        if plain is not None:
            return [pad + ln for ln in plain]
        # Fallback to sh3_ops.h (must flush & reload local regs around call)
        return [f"{pad}__FLUSH__", f"{pad}{name}(c, 0x{op:04X}u);", f"{pad}__RELOAD__"]

    def emit_edge(target_pc, branch_pc, indent):
        pad = " " * indent
        if target_pc in member_set:
            if target_pc <= branch_pc:
                # Backward edge (loop): check blockStop / interruptReady before continuing!
                return [
                    f"{pad}if (cyc < c.blockStop && !c.interruptReady) goto L_{target_pc:08X};",
                    f"{pad}c.PC = 0x{target_pc:08X}u;",
                    f"{pad}__FLUSH_RETURN__",
                ]
            return [f"{pad}goto L_{target_pc:08X};"]
        # Cross-function tail-call chaining when target is a known compiled function root!
        if target_pc in func_roots_set and target_pc != root_pc:
            return [
                f"{pad}c.PC = 0x{target_pc:08X}u;",
                f"{pad}__FLUSH__",
                f"{pad}if (cyc < c.blockStop && !c.interruptReady) {{ F_{target_pc:08X}(c); return; }}",
                f"{pad}return;",
            ]
        return [f"{pad}c.PC = 0x{target_pc:08X}u;", f"{pad}__FLUSH_RETURN__"]

    for pc in members:
        blk = blocks_by_pc[pc]
        blines = []
        # Fast-forward & native HLE intrinsics for Mushihimesama's hottest SH-3 loops
        if pc == 0x0C04A2AA:
            blines.extend([
                "    __FLUSH__",
                "    c.FastForwardTmuPoll();",
                "    if (c.cycles >= c.blockStop || c.interruptReady) return;",
                "    __RELOAD__",
            ])
        elif pc in (0x0C0566F4, 0x0C05670C, 0x0C05671E, 0x0C05673E, 0x0C056756, 0x0C056768):
            blines.extend([
                "    __FLUSH__",
                f"    c.FastForwardDelayLoop(0x{pc:08X}u);",
                "    if (c.cycles >= c.blockStop || c.interruptReady) return;",
                "    __RELOAD__",
            ])
        elif pc == 0x0C04F37C:
            blines.extend([
                "    __FLUSH__",
                "    if (game_native::FastMemcpyByteLoop(c)) return;",
            ])
        elif pc == 0x0C04F370:
            blines.extend([
                "    __FLUSH__",
                "    if (game_native::FastMemsetByteLoop(c)) return;",
            ])

        pending_cyc = 0
        for name, op, a, cyc in blk["insns"]:
            # If the instruction might touch MMIO (Read/Write), flush pending_cyc first
            # so SyncTimer / Blitter sees exact cycle count!
            if name.startswith(("MOVB_", "MOVW_", "MOVL_", "STSL_", "LDSL_", "STCL_", "LDCL_", "MAC", "TASB", "ANDB_G", "ORB_G", "XORB_G", "TSTB_G")):
                if pending_cyc:
                    blines.append(f"    cyc += {pending_cyc}u;")
                    pending_cyc = 0
            blines.extend(emit_one(name, op, a, 4))
            pending_cyc += cyc

        tname, top, ta, tcyc, tslot, ttgt, tfall = blk["term"]
        if tname == "FALL":
            if pending_cyc:
                blines.append(f"    cyc += {pending_cyc}u;")
            blines.extend(emit_edge(ttgt, ta, 4))
        elif tname in ("BT", "BF"):
            use_t[0] = True
            cond = "T" if tname == "BT" else "!T"
            blines.append(f"    if ({cond}) {{")
            blines.append(f"        cyc += {pending_cyc + tcyc + 1}u;")
            blines.extend(emit_edge(ttgt, ta, 8))
            blines.append("    } else {")
            blines.append(f"        cyc += {pending_cyc + tcyc}u;")
            blines.extend(emit_edge(tfall, ta, 8))
            blines.append("    }")
        elif tname in ("BTS", "BFS"):
            use_t[0] = True
            sname, scyc = table[tslot]
            cond = "T" if tname == "BTS" else "!T"
            blines.append(f"    if ({cond}) {{")
            if pending_cyc:
                blines.append(f"        cyc += {pending_cyc}u;")
            blines.extend(emit_one(sname, tslot, u32(ta + 2), 8))
            blines.append(f"        cyc += {scyc + tcyc}u;")
            blines.extend(emit_edge(ttgt, ta, 8))
            blines.append("    } else {")
            blines.append(f"        cyc += {pending_cyc + tcyc}u;")
            blines.extend(emit_edge(tfall, ta, 8))
            blines.append("    }")
        elif tname == "BRA":
            sname, scyc = table[tslot]
            if pending_cyc:
                blines.append(f"    cyc += {pending_cyc}u;")
            blines.extend(emit_one(sname, tslot, u32(ta + 2), 4))
            blines.append(f"    cyc += {scyc + tcyc}u;")
            blines.extend(emit_edge(ttgt, ta, 4))
        elif tname in ("BSR", "JSR"):
            sname, scyc = table[tslot]
            if tname == "JSR":
                rn = (top >> 8) & 15
                used_regs.add(rn)
                blines.append(f"    _tgt = r{rn};")
            if pending_cyc:
                blines.append(f"    cyc += {pending_cyc}u;")
            blines.append(f"    c.PR = 0x{tfall:08X}u;")
            blines.extend(emit_one(sname, tslot, u32(ta + 2), 4))
            blines.append(f"    cyc += {scyc + tcyc}u;")
            # Direct C++ call if target is a known compiled function root!
            if ttgt is not None and ttgt in func_roots_set:
                blines.append(f"    c.PC = 0x{ttgt:08X}u;")
                blines.append("    __FLUSH__")
                blines.append(f"    if (cyc < c.blockStop && !c.interruptReady) {{")
                blines.append(f"        F_{ttgt:08X}(c);")
                blines.append("        if (c.PC == 0x%08Xu && c.cycles < c.blockStop && !c.interruptReady) {" % tfall)
                if tfall in member_set:
                    blines.append("            __RELOAD__")
                    blines.append(f"            goto L_{tfall:08X};")
                else:
                    blines.append("            return;")
                blines.append("        }")
                blines.append("    }")
                blines.append("    return;")
            else:
                tgt_expr = f"0x{ttgt:08X}u" if tname == "BSR" else "_tgt"
                blines.append(f"    c.PC = {tgt_expr};")
                blines.append("    __FLUSH_RETURN__")
        elif tname in ("BRAF", "BSRF", "JMP", "RTS", "RTE"):
            sname, scyc = table[tslot]
            if tname in ("BRAF", "BSRF"):
                rn = (top >> 8) & 15
                used_regs.add(rn)
                blines.append(f"    _tgt = 0x{u32(ta + 4):08X}u + r{rn};")
                if tname == "BSRF":
                    blines.append(f"    c.PR = 0x{u32(ta + 4):08X}u;")
            elif tname == "JMP":
                rn = (top >> 8) & 15
                used_regs.add(rn)
                blines.append(f"    _tgt = r{rn};")
            elif tname == "RTS":
                blines.append("    _tgt = c.PR;")
            else:  # RTE
                blines.append("    _tgt = c.SPC;")
                blines.append("    __FLUSH__")
                blines.append("    c.SetSR(c.SSR);")
                blines.append("    __RELOAD__")
            if pending_cyc:
                blines.append(f"    cyc += {pending_cyc}u;")
            blines.extend(emit_one(sname, tslot, u32(ta + 2), 4))
            blines.append(f"    cyc += {scyc + tcyc}u;")
            blines.append("    c.PC = _tgt;")
            blines.append("    __FLUSH_RETURN__")
        elif tname in BARRIERS:
            if pending_cyc:
                blines.append(f"    cyc += {pending_cyc}u;")
            blines.append(f"    c.PC = 0x{ta:08X}u;")
            blines.append("    c.branched = false;")
            blines.append("    __FLUSH__")
            blines.append(f"    {tname}(c, 0x{top:04X}u);")
            blines.append(f"    c.cycles += {tcyc}u;")
            blines.append(f"    if (!c.branched) c.PC = 0x{tfall:08X}u;")
            blines.append("    return;")

        body_blocks.append((pc, blines))

    sorted_regs = sorted(used_regs)

    def make_load(pad):
        ls = [f"{pad}uint64 cyc = c.cycles;"]
        for k in sorted_regs:
            ls.append(f"{pad}uint32 r{k} = c.R[{k}];")
        if use_t[0]:
            ls.append(f"{pad}uint32 T = c.T;")
        return ls

    def make_reload(pad):
        ls = [f"{pad}cyc = c.cycles;"]
        for k in sorted_regs:
            ls.append(f"{pad}r{k} = c.R[{k}];")
        if use_t[0]:
            ls.append(f"{pad}T = c.T;")
        return ls

    def make_flush(pad):
        ls = [f"{pad}c.cycles = cyc;"]
        for k in sorted_regs:
            ls.append(f"{pad}c.R[{k}] = r{k};")
        if use_t[0]:
            ls.append(f"{pad}c.T = T;")
        return ls

    out = [f"void F_{root_pc:08X}(SH3 &c) {{", "    [[maybe_unused]] uint32 _tgt = 0;"]
    out.extend(make_load("    "))
    if len(members) > 1:
        out.append(f"    if (c.PC != 0x{root_pc:08X}u) [[unlikely]] {{")
        out.append("        switch (c.PC) {")
        for pc in members[1:]:
            out.append(f"        case 0x{pc:08X}u: goto L_{pc:08X};")
        out.append("        default: return;")
        out.append("        }")
        out.append("    }")

    for pc, blines in body_blocks:
        out.append(f"L_{pc:08X}:")
        for ln in blines:
            s = ln.strip()
            pad = ln[:len(ln) - len(ln.lstrip())]
            if s == "__FLUSH__":
                out.extend(make_flush(pad))
            elif s == "__RELOAD__":
                out.extend(make_reload(pad))
            elif s == "__FLUSH_RETURN__":
                out.extend(make_flush(pad))
                out.append(f"{pad}return;")
            else:
                out.append(ln)
    out.append("}")
    return out


def main():
    args = sys.argv[1:]
    ram_path = None
    ram_end_path = None
    hot_path = None
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--ram" and i + 1 < len(args):
            ram_path = args[i + 1]; i += 2
        elif args[i] == "--ram-end" and i + 1 < len(args):
            ram_end_path = args[i + 1]; i += 2
        elif args[i] == "--hot" and i + 1 < len(args):
            hot_path = args[i + 1]; i += 2
        else:
            pos.append(args[i]); i += 1

    if len(pos) < 2:
        print(__doc__)
        return 2
    outdir = pos[0]
    block_files = pos[1:]

    table = load_table()
    oracle = RamOracle(ram_path, ram_end_path)

    raw_blocks = {}
    for path in block_files:
        for line in open(path):
            parts = line.split()
            if len(parts) >= 2:
                pc = int(parts[0], 16)
                code = bytes.fromhex(parts[1])
                raw_blocks[pc] = code

    blocks_by_pc = {pc: parse_block(table, pc, code, oracle) for pc, code in raw_blocks.items()}
    functions, owner = partition_functions(blocks_by_pc)
    func_roots_set = {r for r, _ in functions}

    hot_pcs = set()
    if hot_path and os.path.exists(hot_path):
        for line in open(hot_path):
            p = line.split()
            if p:
                hot_pcs.add(int(p[0], 16))

    # Order functions: hot functions first (into blocks_hot.cpp), then cold chunks
    hot_funcs = []
    cold_funcs = []
    for r, members in functions:
        if hot_pcs and any(m in hot_pcs for m in members):
            hot_funcs.append((r, members))
        else:
            cold_funcs.append((r, members))

    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith(".cpp") or f.endswith(".h"):
            os.remove(os.path.join(outdir, f))

    # Emit shared forward declarations header
    decl_lines = [
        "// Generated by tools/recomp_sh3.py: whole-function AOT declarations.",
        "#pragma once",
        "#include \"sh3.h\"",
        "namespace spelunk {",
    ]
    for r, _ in functions:
        decl_lines.append(f"void F_{r:08X}(SH3 &c);")
    decl_lines.append("} // namespace spelunk")
    with open(os.path.join(outdir, "recomp_decls.h"), "w") as f:
        f.write("\n".join(decl_lines) + "\n")

    header = [
        "// Generated by tools/recomp_sh3.py: whole-function AOT SH-3 -> Pentium III C++20.",
        "#include \"blocks.h\"",
        "#include \"recomp_rt.h\"",
        "#include \"recomp_decls.h\"",
        "#include \"game_native/mushisam_native.h\"",
        "#include \"sh3_ops.h\"",
        "",
        "namespace spelunk {",
        "using namespace ops;",
        "",
    ]

    tu_groups = []
    if hot_funcs:
        tu_groups.append(("blocks_hot.cpp", hot_funcs))
    for idx in range(0, len(cold_funcs), FUNCS_PER_FILE):
        tu_groups.append((f"blocks_{idx // FUNCS_PER_FILE:03d}.cpp", cold_funcs[idx:idx + FUNCS_PER_FILE]))

    for tu_idx, (fname, fgroup) in enumerate(tu_groups):
        lines = list(header)
        for r, members in fgroup:
            lines.extend(emit_function(r, members, blocks_by_pc, table, oracle, func_roots_set))
            lines.append("")
        lines.append(f"const NativeBlock *Blocks{tu_idx}(size_t &count);")
        lines.append(f"static const NativeBlock kBlocks{tu_idx}[] = {{")
        for r, members in fgroup:
            for pc in members:
                ln_bytes = len(blocks_by_pc[pc]["code"])
                lines.append(f"    {{0x{pc:08X}u, {ln_bytes}u, nullptr, F_{r:08X}}},")
        lines.append("};")
        lines.append(f"const NativeBlock *Blocks{tu_idx}(size_t &count) {{")
        lines.append(f"    count = sizeof(kBlocks{tu_idx}) / sizeof(kBlocks{tu_idx}[0]);")
        lines.append(f"    return kBlocks{tu_idx};")
        lines.append("}")
        lines.append("} // namespace spelunk")
        with open(os.path.join(outdir, fname), "w") as f:
            f.write("\n".join(lines) + "\n")

    # Emit registry.cpp
    reg = [
        "// Generated by tools/recomp_sh3.py",
        "#include \"blocks.h\"",
        "#include <vector>",
        "namespace spelunk {",
    ]
    for k in range(len(tu_groups)):
        reg.append(f"const NativeBlock *Blocks{k}(size_t &count);")
    reg.append("const NativeBlock *NativeBlocks(size_t &count) {")
    reg.append("    static std::vector<NativeBlock> all = [] {")
    reg.append("        std::vector<NativeBlock> v;")
    reg.append("        size_t n = 0;")
    for k in range(len(tu_groups)):
        reg.append(f"        {{ const NativeBlock *b = Blocks{k}(n); v.insert(v.end(), b, b + n); }}")
    reg.append("        return v;")
    reg.append("    }();")
    reg.append("    count = all.size();")
    reg.append("    return all.data();")
    reg.append("}")
    reg.append("} // namespace spelunk")
    with open(os.path.join(outdir, "registry.cpp"), "w") as f:
        f.write("\n".join(reg) + "\n")

    print(f"recomp_sh3: {len(raw_blocks)} blocks -> {len(functions)} whole functions in {len(tu_groups)} TUs "
          f"(hot={len(hot_funcs)}, folded_literals={oracle.folded})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
