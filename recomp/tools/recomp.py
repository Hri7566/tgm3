#!/usr/bin/env python3
"""Static recompiler: game.exe (i386 PE) -> C.

Each discovered function becomes `void F_<va>(CPU *c)`. Guest registers and flags are C
locals inside a function (flags never live across calls or returns in compiled code), synced
to the CPU struct around calls. Guest memory is reached through the runtime's RD/WR macros,
so the same C runs on any host with the guest's 4 GiB space at `M`.

usage: recomp.py game.exe outdir
"""
import os
import sys

from capstone import x86 as X

from analyze import JCC, RETS, STOPS, TERM_JMP, Program

R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
MASK = {1: "0xFFu", 2: "0xFFFFu", 4: "0xFFFFFFFFu"}
SIGN = {1: 7, 2: 15, 4: 31}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t"}

COND = {
    X.X86_INS_JO: "of", X.X86_INS_JNO: "!of", X.X86_INS_JB: "cf", X.X86_INS_JAE: "!cf",
    X.X86_INS_JE: "zf", X.X86_INS_JNE: "!zf", X.X86_INS_JBE: "(cf | zf)", X.X86_INS_JA: "!(cf | zf)",
    X.X86_INS_JS: "sf", X.X86_INS_JNS: "!sf", X.X86_INS_JP: "pf", X.X86_INS_JNP: "!pf",
    X.X86_INS_JL: "(sf != of)", X.X86_INS_JGE: "(sf == of)", X.X86_INS_JLE: "(zf | (sf != of))",
    X.X86_INS_JG: "!(zf | (sf != of))",
}
SETCC = {
    X.X86_INS_SETO: "of", X.X86_INS_SETNO: "!of", X.X86_INS_SETB: "cf", X.X86_INS_SETAE: "!cf",
    X.X86_INS_SETE: "zf", X.X86_INS_SETNE: "!zf", X.X86_INS_SETBE: "(cf | zf)", X.X86_INS_SETA: "!(cf | zf)",
    X.X86_INS_SETS: "sf", X.X86_INS_SETNS: "!sf", X.X86_INS_SETP: "pf", X.X86_INS_SETNP: "!pf",
    X.X86_INS_SETL: "(sf != of)", X.X86_INS_SETGE: "(sf == of)", X.X86_INS_SETLE: "(zf | (sf != of))",
    X.X86_INS_SETG: "!(zf | (sf != of))",
}
CMOV = {
    X.X86_INS_CMOVO: "of", X.X86_INS_CMOVNO: "!of", X.X86_INS_CMOVB: "cf", X.X86_INS_CMOVAE: "!cf",
    X.X86_INS_CMOVE: "zf", X.X86_INS_CMOVNE: "!zf", X.X86_INS_CMOVBE: "(cf | zf)", X.X86_INS_CMOVA: "!(cf | zf)",
    X.X86_INS_CMOVS: "sf", X.X86_INS_CMOVNS: "!sf", X.X86_INS_CMOVP: "pf", X.X86_INS_CMOVNP: "!pf",
    X.X86_INS_CMOVL: "(sf != of)", X.X86_INS_CMOVGE: "(sf == of)", X.X86_INS_CMOVLE: "(zf | (sf != of))",
    X.X86_INS_CMOVG: "!(zf | (sf != of))",
}


PREFIXES = (0x66, 0x64, 0x26, 0x2E, 0x36, 0x3E, 0x65, 0xF2, 0xF3, 0x67, 0x9B)


def is_x87(i):
    b = bytes(i.bytes)
    k = 0
    while k < len(b) - 1 and b[k] in PREFIXES:
        k += 1
    return 0xD8 <= b[k] <= 0xDF


class Unsupported(Exception):
    pass


def h(v):
    return "0x%Xu" % (v & 0xFFFFFFFF)


class FuncEmitter:
    def __init__(self, prog, entry):
        self.p = prog
        self.entry = entry
        self.out = []
        self.tmp = 0
        self.warnings = []

    def emit(self, line):
        self.out.append("    " + line)

    # ------------------------------------------------------------------ operands
    def reg_get(self, insn, reg):
        name = insn.reg_name(reg)
        if name in R32:
            return name
        if name in ("ax", "cx", "dx", "bx", "sp", "bp", "si", "di"):
            return "(%s & 0xFFFFu)" % ("e" + name)
        if name in ("al", "cl", "dl", "bl"):
            return "(%s & 0xFFu)" % ("e" + name[0] + "x")
        if name in ("ah", "ch", "dh", "bh"):
            return "((%s >> 8) & 0xFFu)" % ("e" + name[0] + "x")
        raise Unsupported("register " + name)

    def reg_set(self, insn, reg, val):
        name = insn.reg_name(reg)
        if name in R32:
            return "%s = %s;" % (name, val)
        if name in ("ax", "cx", "dx", "bx", "sp", "bp", "si", "di"):
            r = "e" + name
            return "%s = (%s & 0xFFFF0000u) | ((%s) & 0xFFFFu);" % (r, r, val)
        if name in ("al", "cl", "dl", "bl"):
            r = "e" + name[0] + "x"
            return "%s = (%s & 0xFFFFFF00u) | ((%s) & 0xFFu);" % (r, r, val)
        if name in ("ah", "ch", "dh", "bh"):
            r = "e" + name[0] + "x"
            return "%s = (%s & 0xFFFF00FFu) | (((%s) & 0xFFu) << 8);" % (r, r, val)
        raise Unsupported("register " + name)

    def ea(self, insn, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_get(insn, m.base))
        if m.index:
            idx = self.reg_get(insn, m.index)
            parts.append(idx if m.scale == 1 else "%s * %du" % (idx, m.scale))
        if m.disp or not parts:
            parts.append(h(m.disp))
        e = " + ".join(parts)
        if m.segment == X.X86_REG_FS:
            e = "c->fs_base + " + e
        elif m.segment == X.X86_REG_GS:
            raise Unsupported("gs segment")
        return "(uint32_t)(%s)" % e if len(parts) > 1 or m.segment == X.X86_REG_FS else e

    def get(self, insn, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_get(insn, op.reg)
        if op.type == X.X86_OP_IMM:
            return h(op.imm & ((1 << (8 * size)) - 1))
        if op.type == X.X86_OP_MEM:
            return "RD%d(%s)" % (8 * size, self.ea(insn, op))
        raise Unsupported("operand type")

    def set(self, insn, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_set(insn, op.reg, val)
        if op.type == X.X86_OP_MEM:
            return "WR%d(%s, %s);" % (8 * size, self.ea(insn, op), val)
        raise Unsupported("store to operand type")

    def addr_of(self, insn, op):
        """Evaluate a memory operand's address once into a temp (for read-modify-write)."""
        if op.type != X.X86_OP_MEM:
            return None
        t = "ad%d" % self.new_tmp()
        self.emit("uint32_t %s = %s;" % (t, self.ea(insn, op)))
        return t

    def new_tmp(self):
        self.tmp += 1
        return self.tmp

    def rmw(self, insn, op, size):
        """(getter, setter-fn) for a read-modify-write destination."""
        if op.type == X.X86_OP_MEM:
            a = self.addr_of(insn, op)
            return "RD%d(%s)" % (8 * size, a), lambda v: "WR%d(%s, %s);" % (8 * size, a, v)
        return self.get(insn, op, size), lambda v: self.reg_set(insn, op.reg, v)

    # ------------------------------------------------------------------ flags
    def szp(self, r, size):
        self.emit("zf = %s == 0; sf = (%s >> %d) & 1; pf = PARITY(%s);" % (r, r, SIGN[size], r))

    # ------------------------------------------------------------------ control flow
    def goto_or_tail(self, t):
        if self.p.is_tail_target(t, self.entry):
            return "{ SYNC_OUT; F_%X(c); return; }" % t
        if t not in self.p.blocks:
            return "{ SYNC_OUT; rt_trap(c, %s, \"jump to undecoded code\"); return; }" % h(t)
        return "goto L_%X;" % t

    def call_direct(self, insn, t):
        ret = insn.address + insn.size
        self.emit("esp -= 4; WR32(esp, %s);" % h(ret))
        if t == ret:
            return                                  # call $+5 / pop: just pushes eip
        if t not in self.p.functions:
            raise Unsupported("call to non-function %x" % t)
        self.emit("SYNC_OUT; F_%X(c); SYNC_IN; CHECK_RET(%s);" % (t, h(ret)))

    # ------------------------------------------------------------------ main switch
    def insn(self, i):
        iid = i.id
        ops = i.operands
        sz = ops[0].size if ops else 4
        A = self.emit

        if iid == X.X86_INS_MOV:
            if any(o.type == X.X86_OP_REG and i.reg_name(o.reg) in ("cs", "ds", "es", "ss", "fs", "gs") for o in ops):
                raise Unsupported("segment register move")
            A(self.set(i, ops[0], self.get(i, ops[1], sz)))
        elif iid in (X.X86_INS_MOVZX, X.X86_INS_MOVSX):
            src = self.get(i, ops[1])
            if iid == X.X86_INS_MOVSX:
                src = "(uint32_t)(int32_t)(%s)%s" % (ST[ops[1].size], src)
            A(self.set(i, ops[0], src))
        elif iid == X.X86_INS_LEA:
            m = ops[1].mem
            if m.segment:
                raise Unsupported("lea with segment")
            A(self.set(i, ops[0], self.ea(i, ops[1])))
        elif iid == X.X86_INS_XCHG:
            a, b = ops
            if a.type == X.X86_OP_MEM or b.type == X.X86_OP_MEM:
                mem, reg = (a, b) if a.type == X.X86_OP_MEM else (b, a)
                ad = self.addr_of(i, mem)
                t = "t%d" % self.new_tmp()
                A("uint32_t %s = XCHG%d(%s, %s);" % (t, 8 * sz, ad, self.reg_get(i, reg.reg)))
                A(self.reg_set(i, reg.reg, t))
            else:
                t = "t%d" % self.new_tmp()
                A("uint32_t %s = %s;" % (t, self.get(i, a)))
                A(self.set(i, a, self.get(i, b)))
                A(self.set(i, b, t))
        elif iid == X.X86_INS_PUSH:
            v = "t%d" % self.new_tmp()
            if ops[0].type == X.X86_OP_IMM:
                A("uint32_t %s = %s;" % (v, h(ops[0].imm)))
                sz = 4 if ops[0].size != 2 else 2
            else:
                A("uint32_t %s = %s;" % (v, self.get(i, ops[0])))
            A("esp -= %d; WR%d(esp, %s);" % (sz, 8 * sz, v))
        elif iid == X.X86_INS_POP:
            v = "t%d" % self.new_tmp()
            A("uint32_t %s = RD%d(esp); esp += %d;" % (v, 8 * sz, sz))
            if ops[0].type == X.X86_OP_REG and i.reg_name(ops[0].reg) == "esp":
                A("esp = %s;" % v)
            else:
                A(self.set(i, ops[0], v))
        elif iid == X.X86_INS_LEAVE:
            A("esp = ebp; ebp = RD32(esp); esp += 4;")
        elif iid in (X.X86_INS_ADD, X.X86_INS_SUB, X.X86_INS_CMP, X.X86_INS_ADC, X.X86_INS_SBB,
                     X.X86_INS_AND, X.X86_INS_OR, X.X86_INS_XOR, X.X86_INS_TEST):
            self.alu(i, iid, ops, sz)
        elif iid in (X.X86_INS_INC, X.X86_INS_DEC, X.X86_INS_NEG, X.X86_INS_NOT):
            get, put = self.rmw(i, ops[0], sz)
            a, r = "a%d" % self.new_tmp(), "r%d" % self.tmp
            m = MASK[sz]
            A("uint32_t %s = %s, %s;" % (a, get, r))
            if iid == X.X86_INS_INC:
                A("%s = (%s + 1) & %s; of = %s == %s;" % (r, a, m, r, h(1 << SIGN[sz])))
                self.szp(r, sz)
            elif iid == X.X86_INS_DEC:
                A("%s = (%s - 1) & %s; of = %s == %s;" % (r, a, m, a, h(1 << SIGN[sz])))
                self.szp(r, sz)
            elif iid == X.X86_INS_NEG:
                A("%s = (0u - %s) & %s; cf = %s != 0; of = %s == %s;" % (r, a, m, a, a, h(1 << SIGN[sz])))
                self.szp(r, sz)
            else:
                A("%s = ~%s & %s;" % (r, a, m))
            A(put(r))
        elif iid in (X.X86_INS_SHL, X.X86_INS_SAL, X.X86_INS_SHR, X.X86_INS_SAR, X.X86_INS_ROL,
                     X.X86_INS_ROR, X.X86_INS_RCL, X.X86_INS_RCR):
            self.shift(i, iid, ops, sz)
        elif iid == X.X86_INS_IMUL:
            self.imul(i, ops, sz)
        elif iid == X.X86_INS_MUL:
            src = self.get(i, ops[0])
            if sz == 4:
                A("{ uint64_t t = (uint64_t)eax * (%s); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); cf = of = edx != 0; }" % src)
            elif sz == 2:
                A("{ uint32_t t = (eax & 0xFFFFu) * (%s); eax = (eax & 0xFFFF0000u) | (t & 0xFFFFu); "
                  "edx = (edx & 0xFFFF0000u) | (t >> 16); cf = of = (t >> 16) != 0; }" % src)
            else:
                A("{ uint32_t t = (eax & 0xFFu) * (%s); eax = (eax & 0xFFFF0000u) | (t & 0xFFFFu); cf = of = (t >> 8) != 0; }" % src)
        elif iid in (X.X86_INS_DIV, X.X86_INS_IDIV):
            self.div(i, iid, ops, sz)
        elif iid == X.X86_INS_CDQ:
            A("edx = (uint32_t)((int32_t)eax >> 31);")
        elif iid == X.X86_INS_CWDE:
            A("eax = (uint32_t)(int32_t)(int16_t)eax;")
        elif iid == X.X86_INS_CBW:
            A("eax = (eax & 0xFFFF0000u) | ((uint32_t)(int16_t)(int8_t)eax & 0xFFFFu);")
        elif iid == X.X86_INS_CWD:
            A("edx = (edx & 0xFFFF0000u) | (((int16_t)eax < 0) ? 0xFFFFu : 0);")
        elif iid in SETCC:
            A(self.set(i, ops[0], "(uint32_t)(%s)" % SETCC[iid]))
        elif iid in CMOV:
            A("if (%s) { %s }" % (CMOV[iid], self.set(i, ops[0], self.get(i, ops[1]))))
        elif iid == X.X86_INS_NOP:
            pass
        elif iid == X.X86_INS_CLD:
            A("c->df = 0;")
        elif iid == X.X86_INS_STD:
            A("c->df = 1;")
        elif iid == X.X86_INS_CLC:
            A("cf = 0;")
        elif iid == X.X86_INS_STC:
            A("cf = 1;")
        elif iid == X.X86_INS_CMC:
            A("cf ^= 1;")
        elif iid == X.X86_INS_SAHF:
            A("cf = (eax >> 8) & 1; pf = (eax >> 10) & 1; zf = (eax >> 14) & 1; sf = (eax >> 15) & 1;")
        elif iid == X.X86_INS_LAHF:
            A("eax = (eax & 0xFFFF00FFu) | ((cf | 2u | pf << 2 | zf << 6 | sf << 7) << 8);")
        elif iid == X.X86_INS_PUSHFD:
            A("esp -= 4; WR32(esp, cf | 2u | pf << 2 | zf << 6 | sf << 7 | c->df << 10 | of << 11 | 0x200u);")
        elif iid == X.X86_INS_POPFD:
            A("{ uint32_t f = RD32(esp); esp += 4; cf = f & 1; pf = (f >> 2) & 1; zf = (f >> 6) & 1; "
              "sf = (f >> 7) & 1; c->df = (f >> 10) & 1; of = (f >> 11) & 1; }")
        elif iid in (X.X86_INS_MOVSB, X.X86_INS_MOVSW, X.X86_INS_MOVSD, X.X86_INS_STOSB, X.X86_INS_STOSW,
                     X.X86_INS_STOSD, X.X86_INS_LODSB, X.X86_INS_LODSW, X.X86_INS_LODSD, X.X86_INS_CMPSB,
                     X.X86_INS_CMPSW, X.X86_INS_CMPSD, X.X86_INS_SCASB, X.X86_INS_SCASW, X.X86_INS_SCASD):
            self.string(i, iid)
        elif iid == X.X86_INS_WAIT:
            pass
        elif is_x87(i):
            self.x87(i)
        else:
            raise Unsupported(i.mnemonic)

    def alu(self, i, iid, ops, sz):
        A = self.emit
        m = MASK[sz]
        dst, src = ops
        b = self.get(i, src, sz)
        if src.type == X.X86_OP_IMM:
            b = h(src.imm & ((1 << (8 * sz)) - 1))
        # xor r, r / sub r, r
        if iid in (X.X86_INS_XOR, X.X86_INS_SUB) and dst.type == X.X86_OP_REG and src.type == X.X86_OP_REG \
                and dst.reg == src.reg:
            A(self.reg_set(i, dst.reg, "0"))
            A("cf = of = sf = 0; zf = pf = 1;")
            return
        get, put = self.rmw(i, dst, sz)
        n = self.new_tmp()
        a, bb, r = "a%d" % n, "b%d" % n, "r%d" % n
        A("uint32_t %s = %s, %s = %s, %s;" % (a, get, bb, b, r))
        sb = SIGN[sz]
        if iid == X.X86_INS_ADD:
            A("%s = (%s + %s) & %s; cf = %s < %s; of = (((%s ^ %s) & (%s ^ %s)) >> %d) & 1;"
              % (r, a, bb, m, r, a, a, r, bb, r, sb))
        elif iid in (X.X86_INS_SUB, X.X86_INS_CMP):
            A("%s = (%s - %s) & %s; cf = %s < %s; of = (((%s ^ %s) & (%s ^ %s)) >> %d) & 1;"
              % (r, a, bb, m, a, bb, a, bb, a, r, sb))
        elif iid == X.X86_INS_ADC:
            A("{ uint64_t t = (uint64_t)%s + %s + cf; %s = (uint32_t)t & %s; cf = (t >> %d) & 1; }"
              % (a, bb, r, m, 8 * sz))
            A("of = (((%s ^ %s) & (%s ^ %s)) >> %d) & 1;" % (a, r, bb, r, sb))
        elif iid == X.X86_INS_SBB:
            A("{ uint64_t t = (uint64_t)%s - %s - cf; %s = (uint32_t)t & %s; cf = (t >> %d) & 1; }"
              % (a, bb, r, m, 8 * sz))
            A("of = (((%s ^ %s) & (%s ^ %s)) >> %d) & 1;" % (a, bb, a, r, sb))
        else:
            op = {X.X86_INS_AND: "&", X.X86_INS_TEST: "&", X.X86_INS_OR: "|", X.X86_INS_XOR: "^"}[iid]
            A("%s = %s %s %s; cf = of = 0;" % (r, a, op, bb))
        self.szp(r, sz)
        if iid not in (X.X86_INS_CMP, X.X86_INS_TEST):
            A(put(r))

    def shift(self, i, iid, ops, sz):
        A = self.emit
        n = 8 * sz
        m = MASK[sz]
        sb = SIGN[sz]
        if len(ops) == 1:
            cnt = "1u"
            const = 1
        elif ops[1].type == X.X86_OP_IMM:
            const = ops[1].imm & 31
            cnt = "%du" % const
        else:
            const = None
            cnt = "(ecx & 31u)"
        if const == 0:
            return
        get, put = self.rmw(i, ops[0], sz)
        t = self.new_tmp()
        a, k, r = "a%d" % t, "k%d" % t, "r%d" % t
        A("uint32_t %s = %s, %s = %s, %s = %s;" % (a, get, k, cnt, r, a))
        body = []
        if iid in (X.X86_INS_SHL, X.X86_INS_SAL):
            body.append("%s = (uint32_t)((uint64_t)%s << %s) & %s; cf = %s <= %d ? (%s >> (%d - %s)) & 1 : 0; of = ((%s >> %d) & 1) ^ cf;"
                        % (r, a, k, m, k, n, a, n, k, r, sb))
            body.append("zf = %s == 0; sf = (%s >> %d) & 1; pf = PARITY(%s);" % (r, r, sb, r))
        elif iid == X.X86_INS_SHR:
            body.append("%s = (uint32_t)((uint64_t)%s >> %s); cf = (uint32_t)((uint64_t)%s >> (%s - 1)) & 1; of = (%s >> %d) & 1;"
                        % (r, a, k, a, k, a, sb))
            body.append("zf = %s == 0; sf = (%s >> %d) & 1; pf = PARITY(%s);" % (r, r, sb, r))
        elif iid == X.X86_INS_SAR:
            body.append("{ int64_t s = (%s)%s; %s = (uint32_t)(s >> %s) & %s; cf = (uint32_t)(s >> (%s - 1)) & 1; of = 0; }"
                        % (ST[sz], a, r, k, m, k))
            body.append("zf = %s == 0; sf = (%s >> %d) & 1; pf = PARITY(%s);" % (r, r, sb, r))
        elif iid == X.X86_INS_ROL:
            body.append("{ uint32_t q = %s %% %d; %s = q ? ((%s << q) | (%s >> (%d - q))) & %s : %s; } cf = %s & 1; of = ((%s >> %d) & 1) ^ cf;"
                        % (k, n, r, a, a, n, m, a, r, r, sb))
        elif iid == X.X86_INS_ROR:
            body.append("{ uint32_t q = %s %% %d; %s = q ? ((%s >> q) | (%s << (%d - q))) & %s : %s; } cf = (%s >> %d) & 1; of = cf ^ ((%s >> %d) & 1);"
                        % (k, n, r, a, a, n, m, a, r, sb, r, sb - 1))
        elif iid == X.X86_INS_RCL:
            body.append("for (uint32_t q = %s %% %d; q; q--) { uint32_t o = (%s >> %d) & 1; %s = ((%s << 1) | cf) & %s; cf = o; } of = ((%s >> %d) & 1) ^ cf;"
                        % (k, n + 1, r, sb, r, r, m, r, sb))
        elif iid == X.X86_INS_RCR:
            body.append("of = ((%s >> %d) & 1) ^ cf; for (uint32_t q = %s %% %d; q; q--) { uint32_t o = %s & 1; %s = (%s >> 1) | (cf << %d); cf = o; }"
                        % (r, sb, k, n + 1, r, r, r, sb))
        if const is None:
            A("if (%s) { %s %s }" % (k, " ".join(body), put(r)))
        else:
            for b in body:
                A(b)
            A(put(r))

    def imul(self, i, ops, sz):
        A = self.emit
        if len(ops) == 1:
            src = self.get(i, ops[0])
            if sz == 4:
                A("{ int64_t t = (int64_t)(int32_t)eax * (int32_t)(%s); eax = (uint32_t)t; edx = (uint32_t)((uint64_t)t >> 32); "
                  "cf = of = t != (int32_t)t; }" % src)
            elif sz == 2:
                A("{ int32_t t = (int16_t)eax * (int16_t)(%s); eax = (eax & 0xFFFF0000u) | ((uint32_t)t & 0xFFFFu); "
                  "edx = (edx & 0xFFFF0000u) | (((uint32_t)t >> 16) & 0xFFFFu); cf = of = t != (int16_t)t; }" % src)
            else:
                A("{ int32_t t = (int8_t)eax * (int8_t)(%s); eax = (eax & 0xFFFF0000u) | ((uint32_t)t & 0xFFFFu); "
                  "cf = of = t != (int8_t)t; }" % src)
            return
        if len(ops) == 2:
            a, b = self.get(i, ops[0]), self.get(i, ops[1])
        else:
            a, b = self.get(i, ops[1]), h(ops[2].imm)
        if sz == 4:
            A("{ int64_t t = (int64_t)(int32_t)(%s) * (int32_t)(%s); cf = of = t != (int32_t)t; %s }"
              % (a, b, self.set(i, ops[0], "(uint32_t)t")))
        else:
            A("{ int32_t t = (int16_t)(%s) * (int16_t)(%s); cf = of = t != (int16_t)t; %s }"
              % (a, b, self.set(i, ops[0], "(uint32_t)t")))

    def div(self, i, iid, ops, sz):
        A = self.emit
        src = self.get(i, ops[0])
        addr = h(i.address)
        if iid == X.X86_INS_DIV:
            if sz == 4:
                A("{ uint32_t d = %s; if (!d) { SYNC_OUT; rt_div0(c, %s); } uint64_t n = (uint64_t)edx << 32 | eax, q = n / d; "
                  "if (q >> 32) { SYNC_OUT; rt_div0(c, %s); } eax = (uint32_t)q; edx = (uint32_t)(n %% d); }" % (src, addr, addr))
            elif sz == 2:
                A("{ uint32_t d = %s; if (!d) { SYNC_OUT; rt_div0(c, %s); } uint32_t n = (edx & 0xFFFFu) << 16 | (eax & 0xFFFFu); "
                  "eax = (eax & 0xFFFF0000u) | ((n / d) & 0xFFFFu); edx = (edx & 0xFFFF0000u) | (n %% d); }" % (src, addr))
            else:
                A("{ uint32_t d = %s; if (!d) { SYNC_OUT; rt_div0(c, %s); } uint32_t n = eax & 0xFFFFu; "
                  "eax = (eax & 0xFFFF0000u) | ((n %% d) << 8) | ((n / d) & 0xFFu); }" % (src, addr))
        else:
            if sz == 4:
                A("{ int32_t d = (int32_t)%s; if (!d) { SYNC_OUT; rt_div0(c, %s); } int64_t n = (int64_t)((uint64_t)edx << 32 | eax); "
                  "int64_t q = n / d; if (q != (int32_t)q) { SYNC_OUT; rt_div0(c, %s); } eax = (uint32_t)q; edx = (uint32_t)(n %% d); }"
                  % (src, addr, addr))
            elif sz == 2:
                A("{ int32_t d = (int16_t)%s; if (!d) { SYNC_OUT; rt_div0(c, %s); } int32_t n = (int32_t)((edx & 0xFFFFu) << 16 | (eax & 0xFFFFu)); "
                  "eax = (eax & 0xFFFF0000u) | ((uint32_t)(n / d) & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((uint32_t)(n %% d) & 0xFFFFu); }"
                  % (src, addr))
            else:
                A("{ int32_t d = (int8_t)%s; if (!d) { SYNC_OUT; rt_div0(c, %s); } int32_t n = (int16_t)eax; "
                  "eax = (eax & 0xFFFF0000u) | (((uint32_t)(n %% d) & 0xFFu) << 8) | ((uint32_t)(n / d) & 0xFFu); }" % (src, addr))

    def string(self, i, iid):
        A = self.emit
        name = i.mnemonic.split()[-1]
        size = {"b": 1, "w": 2, "d": 4}[name[-1]]
        kind = name[:-1]
        rep = i.prefix[0]         # 0xF3 rep/repe, 0xF2 repne
        n = 8 * size
        step = "(c->df ? -%du : %du)" % (size, size)
        if kind == "movs":
            if rep:
                A("{ SYNC_OUT; rt_rep_movs(c, %d); SYNC_IN; }" % size)
            else:
                A("WR%d(edi, RD%d(esi)); esi += %s; edi += %s;" % (n, n, step, step))
        elif kind == "stos":
            val = {1: "eax & 0xFFu", 2: "eax & 0xFFFFu", 4: "eax"}[size]
            if rep:
                A("{ SYNC_OUT; rt_rep_stos(c, %d); SYNC_IN; }" % size)
            else:
                A("WR%d(edi, %s); edi += %s;" % (n, val, step))
        elif kind == "lods":
            if rep:
                raise Unsupported("rep lods")
            A(self.reg_set(i, {1: X.X86_REG_AL, 2: X.X86_REG_AX, 4: X.X86_REG_EAX}[size], "RD%d(esi)" % n))
            A("esi += %s;" % step)
        elif kind in ("cmps", "scas"):
            m = MASK[size]
            sb = SIGN[size]
            if kind == "cmps":
                one = "a = RD%d(esi); b = RD%d(edi); esi += %s; edi += %s;" % (n, n, step, step)
            else:
                one = "a = eax & %s; b = RD%d(edi); edi += %s;" % (m, n, step)
            flags = ("r = (a - b) & %s; cf = a < b; of = (((a ^ b) & (a ^ r)) >> %d) & 1; zf = r == 0; "
                     "sf = (r >> %d) & 1; pf = PARITY(r);" % (m, sb, sb))
            if not rep:
                A("{ uint32_t a, b, r; %s %s }" % (one, flags))
            else:
                stop = "!zf" if rep == 0xF3 else "zf"
                A("{ uint32_t a, b, r; while (ecx) { %s %s ecx--; if (%s) break; } }" % (one, flags, stop))
        else:
            raise Unsupported(i.mnemonic)

    # ------------------------------------------------------------------ x87
    def x87(self, i):
        A = self.emit
        b = bytes(i.bytes)
        k = 0
        while b[k] in PREFIXES:
            k += 1
        op, modrm = b[k], b[k + 1]
        reg = (modrm >> 3) & 7
        mem = (modrm >> 6) != 3
        sti = modrm & 7
        arith = ["+", "*", None, None, "-", "-r", "/", "/r"]

        def binop(d, s, kind):
            if kind == "-r":
                return "%s = %s - %s;" % (d, s, d)
            if kind == "/r":
                return "%s = %s / %s;" % (d, s, d)
            return "%s = %s %s %s;" % (d, d, kind, s)

        if mem:
            ea = self.ea(i, [o for o in i.operands if o.type == X.X86_OP_MEM][0])
            if op in (0xD8, 0xDC, 0xDA, 0xDE):
                src = {0xD8: "RDF32(%s)", 0xDC: "RDF64(%s)", 0xDA: "(double)(int32_t)RD32(%s)",
                       0xDE: "(double)(int16_t)RD16(%s)"}[op] % ea
                if reg in (2, 3):
                    A("FCOM(ST(0), %s);" % src)
                    if reg == 3:
                        A("FPOP();")
                else:
                    A("{ double s = %s; %s }" % (src, binop("ST(0)", "s", arith[reg])))
            elif op == 0xD9:
                if reg == 0:
                    A("{ double v = RDF32(%s); FPUSH(v); }" % ea)
                elif reg in (2, 3):
                    A("WRF32(%s, ST(0));" % ea)
                    if reg == 3:
                        A("FPOP();")
                elif reg == 5:
                    A("c->fcw = RD16(%s);" % ea)
                elif reg == 7:
                    A("WR16(%s, c->fcw);" % ea)
                else:
                    raise Unsupported("fldenv/fnstenv")
            elif op == 0xDB:
                if reg == 0:
                    A("{ double v = (int32_t)RD32(%s); FPUSH(v); }" % ea)
                elif reg in (2, 3):
                    A("WR32(%s, (uint32_t)FIST32(c, ST(0)));" % ea)
                    if reg == 3:
                        A("FPOP();")
                elif reg == 5:
                    A("{ double v = RDF80(%s); FPUSH(v); }" % ea)
                elif reg == 7:
                    A("WRF80(%s, ST(0)); FPOP();" % ea)
                else:
                    raise Unsupported("x87 DB /%d" % reg)
            elif op == 0xDD:
                if reg == 0:
                    A("{ double v = RDF64(%s); FPUSH(v); }" % ea)
                elif reg in (2, 3):
                    A("WRF64(%s, ST(0));" % ea)
                    if reg == 3:
                        A("FPOP();")
                elif reg == 7:
                    A("WR16(%s, FSW());" % ea)
                else:
                    raise Unsupported("frstor/fnsave")
            elif op == 0xDF:
                if reg == 0:
                    A("{ double v = (int16_t)RD16(%s); FPUSH(v); }" % ea)
                elif reg in (2, 3):
                    A("WR16(%s, (uint16_t)FIST16(c, ST(0)));" % ea)
                    if reg == 3:
                        A("FPOP();")
                elif reg == 5:
                    A("{ double v = (double)(int64_t)RD64(%s); FPUSH(v); }" % ea)
                elif reg == 7:
                    A("WR64(%s, (uint64_t)FIST64(c, ST(0))); FPOP();" % ea)
                else:
                    raise Unsupported("fbld/fbstp")
            else:
                raise Unsupported("x87 %02x" % op)
            return

        s = "ST(%d)" % sti
        if op == 0xD8:
            if reg in (2, 3):
                A("FCOM(ST(0), %s);" % s)
                if reg == 3:
                    A("FPOP();")
            else:
                A(binop("ST(0)", s, arith[reg]))
        elif op in (0xDC, 0xDE):
            # Register forms with st(i) as destination swap the sub/subr and div/divr encodings.
            kinds = ["+", "*", None, None, "-r", "-", "/r", "/"]
            if op == 0xDE and modrm == 0xD9:
                A("FCOM(ST(0), ST(1)); FPOP(); FPOP();")
                return
            if reg in (2, 3):
                A("FCOM(ST(0), %s);" % s)
                if reg == 3:
                    A("FPOP();")
                return
            A(binop(s, "ST(0)", kinds[reg]))
            if op == 0xDE:
                A("FPOP();")
        elif op == 0xD9:
            if reg == 0:
                A("{ double v = %s; FPUSH(v); }" % s)
            elif reg == 1:
                A("{ double t = ST(0); ST(0) = %s; %s = t; }" % (s, s))
            elif modrm == 0xD0:
                pass
            else:
                simple = {
                    0xE0: "ST(0) = -ST(0);",
                    0xE1: "ST(0) = fabs(ST(0));",
                    0xE4: "FCOM(ST(0), 0.0);",
                    0xE5: "FXAM(c);",
                    0xE8: "FPUSH(1.0);",
                    0xE9: "FPUSH(3.321928094887362347870319429489390175864831393);",
                    0xEA: "FPUSH(1.442695040888963407359924681001892137426645954);",
                    0xEB: "FPUSH(3.141592653589793238462643383279502884197169399);",
                    0xEC: "FPUSH(0.301029995663981195213738894724493026768189881);",
                    0xED: "FPUSH(0.693147180559945309417232121458176568075500134);",
                    0xEE: "FPUSH(0.0);",
                    0xF0: "ST(0) = exp2(ST(0)) - 1.0;",
                    0xF1: "ST(1) = ST(1) * log2(ST(0)); FPOP();",
                    0xF2: "ST(0) = tan(ST(0)); FPUSH(1.0);",
                    0xF3: "ST(1) = atan2(ST(1), ST(0)); FPOP();",
                    0xF5: "ST(0) = remainder(ST(0), ST(1));",
                    0xF6: "c->top = (c->top - 1) & 7;",
                    0xF7: "c->top = (c->top + 1) & 7;",
                    0xF8: "ST(0) = fmod(ST(0), ST(1));",
                    0xF9: "ST(1) = ST(1) * log2(ST(0) + 1.0); FPOP();",
                    0xFA: "ST(0) = sqrt(ST(0));",
                    0xFB: "{ double v = ST(0); ST(0) = sin(v); FPUSH(cos(v)); }",
                    0xFC: "ST(0) = FROUND(c, ST(0));",
                    0xFD: "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));",
                    0xFE: "ST(0) = sin(ST(0));",
                    0xFF: "ST(0) = cos(ST(0));",
                }
                if modrm not in simple:
                    raise Unsupported("x87 D9 %02x" % modrm)
                A(simple[modrm])
        elif op == 0xDA and modrm == 0xE9:
            A("FCOM(ST(0), ST(1)); FPOP(); FPOP();")
        elif op == 0xDB and modrm == 0xE2:
            pass                                            # fnclex
        elif op == 0xDB and modrm == 0xE3:
            A("FNINIT(c);")
        elif op == 0xDD:
            if reg == 0:
                pass                                        # ffree
            elif reg in (2, 3):
                A("%s = ST(0);" % s)
                if reg == 3:
                    A("FPOP();")
            elif reg in (4, 5):
                A("FCOM(ST(0), %s);" % s)
                if reg == 5:
                    A("FPOP();")
            else:
                raise Unsupported("x87 DD %02x" % modrm)
        elif op == 0xDF and modrm == 0xE0:
            A("eax = (eax & 0xFFFF0000u) | FSW();")
        elif op == 0xDF and reg == 0:
            A("FPOP();")                                    # ffreep
        else:
            raise Unsupported("x87 %02x %02x" % (op, modrm))

    # ------------------------------------------------------------------ blocks
    def block(self, va):
        insns = self.p.blocks[va]
        self.out.append("L_%X:" % va)
        self.emit("BLK(%s);" % h(va))
        for i in insns:
            if i is None:
                self.emit("SYNC_OUT; rt_trap(c, %s, \"undecodable\"); return;" % h(self.cur_next))
                return
            self.cur_next = i.address + i.size
            self.out.append("    /* %X: %s %s */" % (i.address, i.mnemonic, i.op_str))
            try:
                self.term(i) if self.is_term(i) else self.insn(i)
            except Unsupported as e:
                self.warnings.append("%x: %s (%s %s)" % (i.address, e, i.mnemonic, i.op_str))
                self.emit("SYNC_OUT; rt_trap(c, %s, \"%s\"); return;" % (h(i.address), str(e).replace('"', "'")))
                return
        last = insns[-1]
        if not self.is_term(last) or last.id == X.X86_INS_CALL:
            nxt = last.address + last.size
            self.emit(self.goto_or_tail(nxt) if nxt in self.p.blocks or self.p.is_tail_target(nxt, self.entry)
                      else "SYNC_OUT; rt_trap(c, %s, \"fell off decoded code\"); return;" % h(nxt))

    @staticmethod
    def is_term(i):
        return i.id in JCC or i.id in TERM_JMP or i.id in RETS or i.id in STOPS or i.id == X.X86_INS_CALL

    def term(self, i):
        A = self.emit
        iid = i.id
        nxt = i.address + i.size
        if iid == X.X86_INS_CALL:
            t = self.p.direct_target(i)
            if t is not None:
                self.call_direct(i, t)
            else:
                tgt = self.get(i, i.operands[0])
                A("{ uint32_t t = %s; esp -= 4; WR32(esp, %s); SYNC_OUT; rt_call(c, t); SYNC_IN; CHECK_RET(%s); }"
                  % (tgt, h(nxt), h(nxt)))
        elif iid in JCC:
            t = self.p.direct_target(i)
            if iid in COND:
                cond = COND[iid]
            elif iid == X.X86_INS_JECXZ:
                cond = "ecx == 0"
            elif iid == X.X86_INS_JCXZ:
                cond = "(ecx & 0xFFFFu) == 0"
            elif iid == X.X86_INS_LOOP:
                A("ecx--;")
                cond = "ecx != 0"
            elif iid == X.X86_INS_LOOPE:
                A("ecx--;")
                cond = "ecx != 0 && zf"
            elif iid == X.X86_INS_LOOPNE:
                A("ecx--;")
                cond = "ecx != 0 && !zf"
            else:
                raise Unsupported(i.mnemonic)
            A("if (%s) %s" % (cond, self.goto_or_tail(t)))
            A(self.goto_or_tail(nxt))
        elif iid == X.X86_INS_JMP:
            t = self.p.direct_target(i)
            if t is not None:
                A(self.goto_or_tail(t))
                return
            tgt = self.get(i, i.operands[0])
            table = self.p.jump_tables.get(i.address)
            A("{ uint32_t t = %s;" % tgt)
            if table:
                # every table target of this function, so overlapping/negative-index tables work
                cases = sorted(set(table) | self.all_table_targets)
                A("switch (t) {")
                for c in cases:
                    if c in self.p.blocks and not self.p.is_tail_target(c, self.entry):
                        A("case %s: goto L_%X;" % (h(c), c))
                A("default: break; }")
            A("SYNC_OUT; rt_jmp(c, t); return; }")
        elif iid in RETS:
            if iid != X.X86_INS_RET:
                raise Unsupported(i.mnemonic)
            n = i.operands[0].imm if i.operands else 0
            A("c->ret_to = RD32(esp); esp += %d; SYNC_OUT; return;" % (4 + n))
        elif iid in STOPS:
            A("SYNC_OUT; rt_trap(c, %s, \"%s\"); return;" % (h(i.address), i.mnemonic))

    def function(self):
        blocks = self.p.function_blocks(self.entry)
        self.blockset = set(blocks)
        self.all_table_targets = set()
        for b in blocks:
            last = self.p.blocks[b][-1]
            if last is not None and last.address in self.p.jump_tables:
                self.all_table_targets.update(t for t in self.p.jump_tables[last.address] if t in self.blockset)
        order = [self.entry] + [b for b in blocks if b != self.entry]
        self.out.append("void F_%X(CPU *c)\n{" % self.entry)
        self.out.append("    REGS_DECL;")
        for b in order:
            self.block(b)
        self.out.append("}")
        return "\n".join(self.out) + "\n"


def main():
    exe, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    p = Program(exe)
    p.build()
    funcs = sorted(p.functions)
    print("%d functions, %d blocks, %d jump tables" % (len(funcs), len(p.blocks), len(p.jump_tables)))

    with open(os.path.join(outdir, "funcs.h"), "w") as f:
        f.write("/* generated by recomp.py */\n#pragma once\n#include \"rt.h\"\n")
        for fn in funcs:
            f.write("void F_%X(CPU *c);\n" % fn)

    per_file = 64
    warnings = []
    nfiles = 0
    for k in range(0, len(funcs), per_file):
        chunk = funcs[k:k + per_file]
        path = os.path.join(outdir, "f%03d.c" % nfiles)
        body = ["/* generated by recomp.py from game.exe */", '#include "funcs.h"', ""]
        for fn in chunk:
            e = FuncEmitter(p, fn)
            body.append(e.function())
            warnings += e.warnings
        new = "\n".join(body)
        if not os.path.exists(path) or open(path).read() != new:
            with open(path, "w") as f:
                f.write(new)
        nfiles += 1

    with open(os.path.join(outdir, "table.c"), "w") as f:
        f.write('/* generated by recomp.py */\n#include "funcs.h"\n\n')
        f.write("const struct rc_func rc_funcs[] = {\n")
        for fn in funcs:
            f.write("    { 0x%X, F_%X },\n" % (fn, fn))
        f.write("};\nconst unsigned rc_nfuncs = %d;\n" % len(funcs))
        f.write("const uint32_t rc_entry = 0x%X;\n" % p.img.entry)

    with open(os.path.join(outdir, "files.mk"), "w") as f:
        f.write("GEN_SRCS := " + " ".join("f%03d.c" % n for n in range(nfiles)) + " table.c\n")
    # Remove stale chunk files from a previous run with more functions.
    for name in os.listdir(outdir):
        if name.startswith("f") and name.endswith(".c") and name[1:-2].isdigit() and int(name[1:-2]) >= nfiles:
            os.remove(os.path.join(outdir, name))

    with open(os.path.join(outdir, "warnings.txt"), "w") as f:
        f.write("\n".join(warnings) + "\n")
    print("%d files, %d untranslated instructions (see warnings.txt)" % (nfiles, len(warnings)))


if __name__ == "__main__":
    main()
