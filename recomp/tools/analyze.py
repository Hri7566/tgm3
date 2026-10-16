"""Code discovery for game.exe: functions, basic blocks, jump tables.

Function entries come from the PE entry point, direct call targets, and every dword in the
image (data or immediate operands) that points at decodable code in .text. A function's body
is every block reachable from its entry without following calls; a jmp to another function's
entry is a tail call. Blocks can belong to several functions (they are just duplicated).
"""
import struct

import capstone
from capstone import x86 as X

from pe import Image

TERM_JMP = {X.X86_INS_JMP}
JCC = {
    X.X86_INS_JO, X.X86_INS_JNO, X.X86_INS_JB, X.X86_INS_JAE, X.X86_INS_JE, X.X86_INS_JNE,
    X.X86_INS_JBE, X.X86_INS_JA, X.X86_INS_JS, X.X86_INS_JNS, X.X86_INS_JP, X.X86_INS_JNP,
    X.X86_INS_JL, X.X86_INS_JGE, X.X86_INS_JLE, X.X86_INS_JG, X.X86_INS_JCXZ, X.X86_INS_JECXZ,
    X.X86_INS_LOOP, X.X86_INS_LOOPE, X.X86_INS_LOOPNE,
}
RETS = {X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRET, X.X86_INS_IRETD}
STOPS = {X.X86_INS_INT3, X.X86_INS_HLT, X.X86_INS_UD2}
# Never emitted by MSVC for user-mode C/C++: seeing one means "this is data, not code".
BAD = {getattr(X, "X86_INS_" + n) for n in (
    "IN", "OUT", "INSB", "INSD", "INSW", "OUTSB", "OUTSD", "OUTSW", "CLI", "STI", "HLT", "ARPL", "BOUND",
    "LES", "LDS", "LSS", "LFS", "LGS", "RETF", "INT1", "INTO", "IRET", "IRETD", "LJMP", "LCALL", "SALC",
    "DAA", "DAS", "AAA", "AAS", "AAM", "AAD", "ENTER", "INT", "PUSHAL", "POPAL", "PUSHAW", "POPAW",
    "LAHF", "CMC", "WAIT", "ICEBP", "XLATB", "SCASW", "LODSW", "LODSD") if hasattr(X, "X86_INS_" + n)}


class Program:
    def __init__(self, path):
        self.img = Image(path)
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        self.insns = {}          # va -> capstone insn (None if undecodable)
        self.functions = set()
        self.jump_tables = {}    # jmp insn va -> list of targets
        self._jt_cache = {}
        self.blocks = {}         # block va -> list of insns (ends at terminator or next block start)

    def decode(self, va):
        if va in self.insns:
            return self.insns[va]
        insn = None
        if self.img.in_text(va):
            code = self.img.bytes(va, 16)
            for i in self.md.disasm(code, va, 1):
                insn = i
        self.insns[va] = insn
        return insn

    # ---- operand helpers ----
    @staticmethod
    def direct_target(insn):
        op = insn.operands[0] if insn.operands else None
        if op is not None and op.type == X.X86_OP_IMM:
            return op.imm & 0xFFFFFFFF
        return None

    def linear_prev(self, va, n=8):
        """Instructions decoded immediately before va (same linear run), nearest first."""
        out = []
        cur = va
        for _ in range(n):
            p = self.prev.get(cur)
            if p is None:
                break
            out.append(self.insns[p])
            cur = p
        return out

    def find_bound(self, va, reg):
        """Look back from va for `cmp reg, imm` guarding a table index; returns count or None."""
        for i in self.linear_prev(va, 10):
            if i is None:
                break
            if i.id in (X.X86_INS_CMP, X.X86_INS_AND) and len(i.operands) == 2 and i.operands[0].type == X.X86_OP_REG \
                    and i.operands[0].reg == reg and i.operands[1].type == X.X86_OP_IMM:
                return (i.operands[1].imm & 0xFFFFFFFF) + 1
            if reg in i.regs_access()[1]:
                return None
        return None

    def jump_table(self, insn):
        """Targets of `jmp [disp + idx*4]`, or None if not that shape."""
        if insn.address not in self._jt_cache:
            self._jt_cache[insn.address] = self._jump_table(insn)
        return self._jt_cache[insn.address]

    def _jump_table(self, insn):
        op = insn.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.base != 0 or op.mem.scale != 4 or op.mem.index == 0:
            return None
        table = op.mem.disp & 0xFFFFFFFF
        idx = op.mem.index
        count = self.find_bound(insn.address, idx)
        if count is None:
            # Two-level table: movzx idx, byte [r + tbl1] just before the jmp.
            for i in self.linear_prev(insn.address, 4):
                if i is None:
                    break
                if i.id == X.X86_INS_MOVZX and i.operands[0].reg == idx and i.operands[1].type == X.X86_OP_MEM \
                        and i.operands[1].size == 1:
                    m = i.operands[1].mem
                    n1 = self.find_bound(i.address, m.base or m.index)
                    if n1:
                        tbl1 = m.disp & 0xFFFFFFFF
                        count = max(self.img.u8(tbl1 + k) for k in range(n1)) + 1
                    break
        targets = []
        k = 0
        while True:
            if count is not None and k >= count:
                break
            if count is None and k >= 512:
                break
            if not self.img.in_image(table + 4 * k):
                break
            t = self.img.u32(table + 4 * k)
            k += 1
            if not self.img.in_text(t) or (count is None and not self.trial(t)):
                # memcpy's tables have unused slots overlapping code; skip those
                if count is None and targets:
                    break
                continue
            targets.append(t)
        if count is None:
            print("warning: unbounded jump table at %x, guessed %d entries" % (insn.address, len(targets)))
        return targets

    # ---- block discovery ----
    def explore(self):
        """Decode everything reachable from function entries; fills self.functions/self.block_starts."""
        self.prev = {}
        self.block_starts = set()
        work = list(self.functions)
        seen = set()
        while work:
            va = work.pop()
            if va in seen:
                continue
            seen.add(va)
            self.block_starts.add(va)
            cur = va
            while True:
                insn = self.decode(cur)
                if insn is None:
                    break
                nxt = cur + insn.size
                iid = insn.id
                if iid == X.X86_INS_CALL:
                    t = self.direct_target(insn)
                    if t is not None and self.img.in_text(t):
                        if t not in self.functions:
                            self.functions.add(t)
                            work.append(t)
                elif iid in JCC:
                    t = self.direct_target(insn)
                    work.append(t)
                    work.append(nxt)
                    self.prev.setdefault(nxt, cur)
                    break
                elif iid in TERM_JMP:
                    t = self.direct_target(insn)
                    if t is not None:
                        work.append(t)
                    else:
                        tbl = self.jump_table(insn)
                        if tbl is not None:
                            self.jump_tables[cur] = tbl
                            work.extend(tbl)
                    break
                elif iid in RETS or iid in STOPS:
                    break
                self.prev.setdefault(nxt, cur)
                cur = nxt

    def data_pointers(self):
        """Aligned dwords in the data sections that point into .text."""
        out = set()
        img = self.img
        for name, lo, size, _ in img.sections:
            if name == ".text":
                continue
            for va in range(lo, lo + size - 3, 4):
                v = img.u32(va)
                if img.in_text(v):
                    out.add(v)
        return out

    def trial(self, va):
        """Decode the intra-procedural code reachable from va; False if it hits anything that
        compiled C wouldn't contain (privileged/legacy instructions, undecodable bytes)."""
        seen = set()
        work = [va]
        n = 0
        while work:
            cur = work.pop()
            while cur not in seen:
                seen.add(cur)
                insn = self.decode(cur)
                if insn is None or insn.id in BAD:
                    return False
                n += 1
                if n > 20000:
                    return True
                nxt = cur + insn.size
                if insn.id in JCC:
                    t = self.direct_target(insn)
                    if not self.img.in_text(t):
                        return False
                    work.append(t)
                elif insn.id in TERM_JMP:
                    t = self.direct_target(insn)
                    if t is not None:
                        if not self.img.in_text(t):
                            return False
                        work.append(t)
                    break
                elif insn.id in RETS or insn.id in STOPS:
                    break
                elif insn.id == X.X86_INS_CALL:
                    t = self.direct_target(insn)
                    if t is not None and not self.img.in_text(t):
                        return False
                cur = nxt
        return True

    def code_bytes(self):
        """Byte addresses covered by instructions reachable from current functions."""
        covered = set()
        starts = set()
        for b in self.block_starts:
            cur = b
            while True:
                insn = self.insns.get(cur)
                if insn is None or cur in starts:
                    break
                starts.add(cur)
                covered.update(range(cur, cur + insn.size))
                if insn.id in JCC or insn.id in TERM_JMP or insn.id in RETS or insn.id in STOPS:
                    break
                cur += insn.size
        return covered, starts

    def build(self):
        img = self.img
        self.functions = {img.entry}
        self.explore()
        self.call_derived = set(self.functions)
        self.trusted_blocks = set(self.block_starts)
        data_ptrs = self.data_pointers()
        rejected = set()
        self.pointer_entries = 0
        while True:
            covered, starts = self.code_bytes()
            cands = set(data_ptrs)
            for va in starts:
                i = self.insns[va]
                if i.id in JCC or i.id in (X.X86_INS_JMP, X.X86_INS_CALL):
                    continue
                for op in i.operands:
                    if op.type == X.X86_OP_IMM and img.in_text(op.imm & 0xFFFFFFFF):
                        cands.add(op.imm & 0xFFFFFFFF)
            table_targets = {t for ts in self.jump_tables.values() for t in ts}
            new = []
            self.mid_refs = set()
            for c in sorted(cands - self.functions - rejected - table_targets):
                if c in covered:
                    # Inside known code: SEH filters/handlers (scope tables) or labels; never a
                    # call target unless proven otherwise at runtime.
                    if c in starts:
                        self.mid_refs.add(c)
                    continue
                if not self.trial(c):
                    rejected.add(c)
                    continue
                new.append(c)
            if not new:
                break
            self.functions.update(new)
            self.pointer_entries += len(new)
            self.explore()
        # Drop pointer entries that sit inside instructions of code reached through calls alone
        # (trusted), then rediscover from scratch with the surviving set.
        pointer = set(self.functions) - self.call_derived
        interior = set()
        for b in self.trusted_blocks:
            for i in self.blocks_insns(b):
                interior.update(range(i.address + 1, i.address + i.size))
        bad = {f for f in pointer if f in interior}
        if bad:
            rejected |= bad
            self.insns.clear()
            self._jt_cache.clear()
            self.jump_tables.clear()
            self.functions -= bad
            self.explore()
        self.pointer_entries = len(pointer & self.functions)
        self.rejected = rejected
        self.split_blocks()

    def blocks_insns(self, va):
        """Instructions of the linear run starting at va (up to its terminator)."""
        cur = va
        while True:
            i = self.insns.get(cur)
            if i is None:
                return
            yield i
            if i.id in JCC or i.id in TERM_JMP or i.id in RETS or i.id in STOPS:
                return
            cur += i.size

    def split_blocks(self):
        """Cut decoded runs into blocks at every block start."""
        starts = self.block_starts
        self.blocks = {}
        for va in sorted(starts):
            out = []
            cur = va
            while True:
                insn = self.insns.get(cur) or self.decode(cur)
                if insn is None:
                    out.append(None)      # undecodable: trap
                    break
                out.append(insn)
                nxt = cur + insn.size
                if insn.id in JCC or insn.id in TERM_JMP or insn.id in RETS or insn.id in STOPS:
                    break
                if nxt in starts:
                    break
                cur = nxt
            self.blocks[va] = out

    def successors(self, va):
        insns = self.blocks[va]
        last = insns[-1]
        if last is None:
            return []
        nxt = last.address + last.size
        if last.id in JCC:
            return [self.direct_target(last), nxt]
        if last.id in TERM_JMP:
            t = self.direct_target(last)
            if t is not None:
                return [t]
            return list(self.jump_tables.get(last.address, []))
        if last.id in RETS or last.id in STOPS:
            return []
        return [nxt]

    def is_tail_target(self, t, entry):
        """A jump to t leaves the current function (tail call) rather than staying inside it."""
        if t == entry or t not in self.functions:
            return False
        if t in self.call_derived:
            return True
        # Pointer-only entries count as functions when they look like one (after padding or a
        # ret); otherwise a jump there is more likely a label that happens to equal some data.
        prev = self.img.u8(t - 1)
        return prev in (0xCC, 0x90, 0xC3) or self.img.u8(t - 3) == 0xC2

    def function_blocks(self, entry):
        """Blocks of a function: reachable from entry, not entering other function entries."""
        out = []
        seen = {entry}
        work = [entry]
        while work:
            b = work.pop()
            out.append(b)
            for s in self.successors(b):
                if self.is_tail_target(s, entry):
                    continue        # tail call
                if s not in seen:
                    seen.add(s)
                    work.append(s)
        return sorted(out)


if __name__ == "__main__":
    import collections
    import sys
    p = Program(sys.argv[1])
    p.build()
    n_ins = sum(1 for i in p.insns.values() if i)
    print("functions", len(p.functions), "(pointer entries %d)" % p.pointer_entries)
    print("blocks", len(p.blocks), "insns", n_ins, "jump tables", len(p.jump_tables))
    hist = collections.Counter()
    for i in p.insns.values():
        if i:
            hist[i.mnemonic] += 1
    for m, c in sorted(hist.items(), key=lambda x: -x[1]):
        print("%6d %s" % (c, m))
    covered = sum(i.size for i in p.insns.values() if i)
    print("text bytes covered %d / %d" % (covered, p.img.text_hi - p.img.text_lo))
    print("mid-function refs:", sorted(hex(x) for x in p.mid_refs))
    bad = [hex(b) for b, ins in p.blocks.items() if ins[-1] is None]
    print("blocks ending in undecodable:", len(bad), bad[:20])
