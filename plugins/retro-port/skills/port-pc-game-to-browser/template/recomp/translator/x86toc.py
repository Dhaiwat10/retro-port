"""Static recompiler: translates the game's 32-bit x86 functions into C.

Each guest function becomes `void f_XXXXXXXX(CPU *c)`. Registers, flags, the x87 stack and XMM
registers live in the CPU struct; guest memory is addressed through R*/W* helpers (see
runtime/cpu.h). Branches inside a function become gotos; calls push a real return address so the
guest stack layout is exact.

Usage:
  x86toc.py <exe> <functions.json> <outdir> [--roots 0x67a97a,0x67aa76 | --all] [--per-file N]
"""
import argparse
import json
import os
import sys
from collections import Counter, defaultdict

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, CS_OP_REG, Cs
from capstone import x86_const as X

GP32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
GP16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
GP8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
GP8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST_ = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}
BITS = {1: 8, 2: 16, 4: 32}

COND = {
    "o": "c->of", "no": "!c->of", "b": "c->cf", "ae": "!c->cf", "e": "c->zf", "ne": "!c->zf",
    "be": "(c->cf || c->zf)", "a": "(!c->cf && !c->zf)", "s": "c->sf", "ns": "!c->sf",
    "p": "PF(c)", "np": "!PF(c)", "l": "(c->sf != c->of)", "ge": "(c->sf == c->of)",
    "le": "(c->zf || c->sf != c->of)", "g": "(!c->zf && c->sf == c->of)",
}


class Unsupported(Exception):
    pass


class FunctionTranslator:
    def __init__(self, prog, func):
        self.p = prog
        self.f = func
        self.entry = func["entry"]
        self.out = []
        self.labels = set()

    # ------------------------------------------------------------ operands

    def reg_read(self, name):
        if name in GP32:
            return f"c->{name}"
        if name in GP16:
            return f"((uint16_t)c->{GP16[name]})"
        if name in GP8L:
            return f"((uint8_t)c->{GP8L[name]})"
        if name in GP8H:
            return f"((uint8_t)(c->{GP8H[name]} >> 8))"
        if name.startswith("xmm"):
            return f"c->xmm[{name[3:]}]"
        raise Unsupported(f"register {name}")

    def reg_write(self, name, val):
        if name in GP32:
            return f"c->{name} = (uint32_t)({val});"
        if name in GP16:
            r = GP16[name]
            return f"c->{r} = (c->{r} & 0xffff0000u) | (uint16_t)({val});"
        if name in GP8L:
            r = GP8L[name]
            return f"c->{r} = (c->{r} & 0xffffff00u) | (uint8_t)({val});"
        if name in GP8H:
            r = GP8H[name]
            return f"c->{r} = (c->{r} & 0xffff00ffu) | ((uint32_t)(uint8_t)({val}) << 8);"
        raise Unsupported(f"register write {name}")

    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(self.reg_read(ins.reg_name(m.base)))
        if m.index:
            idx = self.reg_read(ins.reg_name(m.index))
            parts.append(f"{idx} * {m.scale}" if m.scale != 1 else idx)
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xffffffff:x}u")
        if m.segment and ins.reg_name(m.segment) == "fs":
            parts.append("c->fs_base")
        return "(uint32_t)(" + " + ".join(parts) + ")"

    def read(self, ins, op, size=None):
        size = size or op.size
        if op.type == CS_OP_REG:
            return self.reg_read(ins.reg_name(op.reg))
        if op.type == CS_OP_IMM:
            return f"({UT[size]})0x{op.imm & ((1 << (8 * size)) - 1):x}u"
        if op.type == CS_OP_MEM:
            return f"R{BITS[size]}({self.addr(ins, op)})"
        raise Unsupported("operand")

    def write(self, ins, op, val, size=None):
        size = size or op.size
        if op.type == CS_OP_REG:
            return self.reg_write(ins.reg_name(op.reg), val)
        if op.type == CS_OP_MEM:
            return f"W{BITS[size]}({self.addr(ins, op)}, ({UT[size]})({val}));"
        raise Unsupported("write operand")

    def rmw(self, ins, op, expr_of):
        """Read-modify-write that evaluates a memory address only once."""
        size = op.size
        if op.type == CS_OP_MEM:
            return (f"{{ uint32_t a_ = {self.addr(ins, op)}; "
                    f"W{BITS[size]}(a_, ({UT[size]})({expr_of(f'R{BITS[size]}(a_)')})); }}")
        return self.write(ins, op, expr_of(self.read(ins, op)))

    # ------------------------------------------------------------ control flow helpers

    def target_stmt(self, target, tail=True):
        """Branch to an address: goto inside this function, otherwise a tail call."""
        if target in self.insn_addrs:
            self.labels.add(target)
            return f"goto L_{target:08x};"
        if target in self.p.entries:
            return f"{{ f_{target:08x}(c); return; }}"
        return f"{{ jump_indirect(c, 0x{target:08x}u, 0x{self.cur:08x}u); return; }}"

    def call_stmt(self, ins, target_expr, direct_target=None):
        ret = ins.address + ins.size
        if direct_target is not None:
            if direct_target in self.p.entries:
                return f"PUSH32(c, 0x{ret:08x}u); f_{direct_target:08x}(c);"
            return f"PUSH32(c, 0x{ret:08x}u); call_indirect(c, 0x{direct_target:08x}u);"
        return f"{{ uint32_t t_ = {target_expr}; PUSH32(c, 0x{ret:08x}u); call_indirect(c, t_); }}"

    # ------------------------------------------------------------ translation

    def decode(self):
        """Recursive-descent disassembly from the entry. Ghidra's body ranges are only used as the
        span a branch may stay within; they can split instructions (e.g. around shared bytes)."""
        lo = min(a for a, _ in self.f["ranges"])
        # Ghidra sometimes leaves pieces of a function (branch targets) outside its body ranges, so the
        # span also extends to the next known entry point.
        hi = max(max(b for _, b in self.f["ranges"]), self.p.next_entry(self.entry))
        # Pointer-derived entries can sit mid-function; they don't split it (that would strand branch
        # targets in another fragment), so only Ghidra's function starts stop decoding.
        inside = lambda t: lo <= t < hi and (t == self.entry or t not in self.p.ghidra_entries)
        seen, todo = {}, [self.entry]
        while todo:
            a = todo.pop()
            while a not in seen and inside(a):
                ins = next(self.p.md.disasm(self.p.code(a, a + 16), a, count=1), None)
                if ins is None:
                    break
                seen[a] = ins
                m = ins.mnemonic
                op = ins.operands[0] if ins.operands else None
                if (m == "jmp" or (m.startswith("j") and m[1:] in COND) or m in ("jecxz", "jcxz", "loop")) \
                        and op is not None and op.type == CS_OP_IMM and inside(op.imm):
                    todo.append(op.imm)
                if m == "jmp" and op is not None and op.type != CS_OP_IMM:
                    if not self.p.jumptables.get(a):  # Ghidra lists unresolved jumps with no targets
                        self.p.jumptables[a] = self.recover_table(op, inside)
                    todo.extend(t for t in self.p.jumptables.get(a, []) if inside(t))
                if self.is_terminator(ins):
                    break
                a += ins.size
        return sorted(seen.values(), key=lambda i: i.address)

    def recover_table(self, op, inside):
        """Switch tables Ghidra missed: `jmp [reg*4 + table]` -> read entries while they point into
        this function."""
        if op.type != CS_OP_MEM or op.mem.scale != 4 or op.mem.base or not op.mem.index:
            return []
        table, targets = op.mem.disp & 0xffffffff, []
        for k in range(512):
            ent = table + 4 * k - self.p.base
            if ent < 0 or ent + 4 > len(self.p.img):
                break
            t = int.from_bytes(self.p.img[ent:ent + 4], "little")
            if not inside(t):
                break
            targets.append(t)
        return targets

    @staticmethod
    def is_locked(ins):
        for b in ins.bytes:   # leading prefixes
            if b == 0xF0:
                return True
            if b not in (0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x66, 0x67):
                break
        return ins.mnemonic == "xchg" and any(op.type == CS_OP_MEM for op in ins.operands)

    def translate(self):
        insns = self.decode()
        self.insn_addrs = {i.address for i in insns}
        body = []
        unsupported = []
        for k, ins in enumerate(insns):
            self.cur = ins.address
            try:
                stmt = self.insn(ins)
            except Unsupported as e:
                unsupported.append(f"{ins.mnemonic} ({e})")
                stmt = f'guest_trap(c, 0x{ins.address:08x}u, "unsupported: {ins.mnemonic} {ins.op_str}");'
            if self.is_locked(ins):
                # x86 atomics (lock prefix, xchg with memory): run under the runtime's global guest lock
                # so spinlocks and interlocked counters stay correct with truly parallel threads
                stmt = f"{{ guest_lock(); {stmt} guest_unlock(); }}"
            body.append((ins, stmt))
            nxt = ins.address + ins.size
            following = insns[k + 1].address if k + 1 < len(insns) else None
            if following != nxt and not self.is_terminator(ins):
                body.append((None, self.target_stmt(nxt)))
        lines = [f"void f_{self.entry:08x}(CPU *c) {{ /* {self.f['name']} */", "    PREEMPT_POLL(c);"]
        for ins, stmt in body:
            if ins is not None:
                if ins.address in self.labels:
                    lines.append(f"L_{ins.address:08x}:;")
                if self.p.trace:
                    lines.append(f"    trace_insn(c, 0x{ins.address:08x}u);")
                lines.append(f"    {stmt} /* {ins.mnemonic} {ins.op_str} */")
            else:
                lines.append(f"    {stmt}")
        lines.append("}")
        return "\n".join(lines) + "\n", unsupported, len(insns)

    @staticmethod
    def is_terminator(ins):
        return ins.mnemonic in ("jmp", "ret", "retn", "int3", "hlt", "ud2")

    def insn(self, ins):
        m = ins.mnemonic
        ops = ins.operands
        pre = ins.prefix
        rep = 0xF3 in pre or 0xF2 in pre

        if m.startswith("rep ") or m.startswith("repne ") or m.startswith("repe "):
            return self.string_op(ins, m.split()[1], m.split()[0])
        if m in ("movsb", "movsw", "stosb", "stosw", "stosd", "lodsb", "lodsw", "lodsd",
                 "scasb", "scasw", "scasd", "cmpsb", "cmpsw", "cmpsd") or (
                m == "movsd" and len(ops) == 2 and ops[0].type == CS_OP_MEM and ops[1].type == CS_OP_MEM):
            return self.string_op(ins, m, "rep" if rep else None)

        h = getattr(self, "i_" + m, None)
        if h:
            return h(ins, ops)
        if m.startswith("j") and m[1:] in COND:
            return f"if ({COND[m[1:]]}) {self.target_stmt(ops[0].imm)}"
        if m.startswith("set") and m[3:] in COND:
            return self.write(ins, ops[0], f"{COND[m[3:]]} ? 1 : 0")
        if m.startswith("cmov") and m[4:] in COND:
            return f"if ({COND[m[4:]]}) {self.write(ins, ops[0], self.read(ins, ops[1]))}"
        if m.startswith("f") and m != "femms":
            return self.x87(ins, m, ops)
        v = self.vector_int(ins, m, ops)
        if v is not None:
            return v
        return self.sse(ins, m, ops)

    # --- data movement
    def i_mov(self, ins, o):
        return self.write(ins, o[0], self.read(ins, o[1], o[0].size))

    def i_movzx(self, ins, o):
        return self.write(ins, o[0], f"(uint32_t){self.read(ins, o[1])}")

    def i_movsx(self, ins, o):
        return self.write(ins, o[0], f"(uint32_t)(int32_t)({ST_[o[1].size]}){self.read(ins, o[1])}")

    def i_lea(self, ins, o):
        a = self.addr(ins, o[1]).replace(" + c->fs_base", "")
        return self.write(ins, o[0], a)

    def i_xchg(self, ins, o):
        s = o[0].size
        if o[0].type == CS_OP_REG and o[1].type == CS_OP_REG and o[0].reg == o[1].reg:
            return ";"
        a, b = self.read(ins, o[0]), self.read(ins, o[1])
        return f"{{ {UT[s]} t0_ = {a}, t1_ = {b}; {self.write(ins, o[0], 't1_')} {self.write(ins, o[1], 't0_')} }}"

    def i_push(self, ins, o):
        if o[0].size == 2:
            return f"c->esp -= 2; W16(c->esp, {self.read(ins, o[0])});"
        v = self.read(ins, o[0], 4)
        if o[0].type == CS_OP_IMM:
            v = f"0x{o[0].imm & 0xffffffff:x}u"
        return f"{{ uint32_t v_ = {v}; PUSH32(c, v_); }}"

    def i_pop(self, ins, o):
        if o[0].type == CS_OP_MEM:  # address is computed after ESP is incremented
            return f"{{ uint32_t v_ = R32(c->esp); c->esp += 4; {self.write(ins, o[0], 'v_')} }}"
        return f"{{ uint32_t v_ = POP32(c); {self.write(ins, o[0], 'v_')} }}"

    def i_pushal(self, ins, o):
        return ("{ uint32_t s_ = c->esp; PUSH32(c, c->eax); PUSH32(c, c->ecx); PUSH32(c, c->edx); "
                "PUSH32(c, c->ebx); PUSH32(c, s_); PUSH32(c, c->ebp); PUSH32(c, c->esi); PUSH32(c, c->edi); }")

    def i_popal(self, ins, o):
        return ("c->edi = POP32(c); c->esi = POP32(c); c->ebp = POP32(c); c->esp += 4; "
                "c->ebx = POP32(c); c->edx = POP32(c); c->ecx = POP32(c); c->eax = POP32(c);")

    FLAGS_PACK = ("((uint32_t)c->cf | (PF(c) << 2) | ((uint32_t)c->zf << 6) | ((uint32_t)c->sf << 7) | "
                  "((uint32_t)c->df << 10) | ((uint32_t)c->of << 11) | ((uint32_t)c->idf << 21) | 0x202u)")

    def i_pushfd(self, ins, o):
        return f"PUSH32(c, {self.FLAGS_PACK});"

    def i_popfd(self, ins, o):
        return ("{ uint32_t f_ = POP32(c); c->cf = f_ & 1; c->pf_byte = (f_ >> 2) & 1 ? 0 : 1; "
                "c->zf = (f_ >> 6) & 1; c->sf = (f_ >> 7) & 1; c->df = (f_ >> 10) & 1; c->of = (f_ >> 11) & 1; "
                "c->idf = (f_ >> 21) & 1; }")

    def i_lahf(self, ins, o):
        return self.reg_write("ah", "(c->cf | (PF(c) << 2) | 2 | (c->zf << 6) | (c->sf << 7))")

    def i_sahf(self, ins, o):
        return ("{ uint32_t f_ = (c->eax >> 8) & 0xff; c->cf = f_ & 1; c->pf_byte = (f_ >> 2) & 1 ? 0 : 1; "
                "c->zf = (f_ >> 6) & 1; c->sf = (f_ >> 7) & 1; }")

    def i_cdq(self, ins, o):
        return "c->edx = (uint32_t)((int32_t)c->eax >> 31);"

    def i_cwde(self, ins, o):
        return "c->eax = (uint32_t)(int32_t)(int16_t)c->eax;"

    def i_cbw(self, ins, o):
        return self.reg_write("ax", "(uint16_t)(int16_t)(int8_t)c->eax")

    def i_cwd(self, ins, o):
        return self.reg_write("dx", "((int16_t)c->eax < 0) ? 0xffff : 0")

    def i_bswap(self, ins, o):
        return self.write(ins, o[0], f"__builtin_bswap32({self.read(ins, o[0])})")

    # --- arithmetic / logic
    def arith(self, ins, o, op, store=True):
        s = o[0].size
        b = self.read(ins, o[1], s)
        if not store:
            return f"(void){op}{BITS[s]}(c, {self.read(ins, o[0])}, {b});"
        return self.rmw(ins, o[0], lambda a: f"{op}{BITS[s]}(c, {a}, {b})")

    def i_add(self, ins, o): return self.arith(ins, o, "add")
    def i_adc(self, ins, o): return self.arith(ins, o, "adc")
    def i_sub(self, ins, o): return self.arith(ins, o, "sub")
    def i_sbb(self, ins, o): return self.arith(ins, o, "sbb")
    def i_cmp(self, ins, o): return self.arith(ins, o, "sub", store=False)

    def logic(self, ins, o, cop, store=True):
        s = o[0].size
        if store and cop == "^" and o[0].type == CS_OP_REG and o[1].type == CS_OP_REG and o[0].reg == o[1].reg:
            return self.write(ins, o[0], f"logic{BITS[s]}(c, 0)")
        b = self.read(ins, o[1], s)
        if not store:
            return f"(void)logic{BITS[s]}(c, ({UT[s]})({self.read(ins, o[0])} {cop} {b}));"
        return self.rmw(ins, o[0], lambda a: f"logic{BITS[s]}(c, ({UT[s]})({a} {cop} {b}))")

    def i_and(self, ins, o): return self.logic(ins, o, "&")
    def i_or(self, ins, o): return self.logic(ins, o, "|")
    def i_xor(self, ins, o): return self.logic(ins, o, "^")
    def i_test(self, ins, o): return self.logic(ins, o, "&", store=False)

    def i_not(self, ins, o):
        s = o[0].size
        return self.rmw(ins, o[0], lambda a: f"({UT[s]})~{a}")

    def unary(self, ins, o, fn):
        s = o[0].size
        return self.rmw(ins, o[0], lambda a: f"{fn}{BITS[s]}(c, {a})")

    def i_inc(self, ins, o): return self.unary(ins, o, "inc")
    def i_dec(self, ins, o): return self.unary(ins, o, "dec")
    def i_neg(self, ins, o): return self.unary(ins, o, "neg")

    def shift(self, ins, o, fn):
        s = o[0].size
        n = "1" if len(o) == 1 else (str(o[1].imm) if o[1].type == CS_OP_IMM else "(c->ecx & 0xff)")
        return self.rmw(ins, o[0], lambda a: f"{fn}{BITS[s]}(c, {a}, {n})")

    def i_shl(self, ins, o): return self.shift(ins, o, "shl")
    def i_sal(self, ins, o): return self.shift(ins, o, "shl")
    def i_shr(self, ins, o): return self.shift(ins, o, "shr")
    def i_sar(self, ins, o): return self.shift(ins, o, "sar")
    def i_rol(self, ins, o): return self.shift(ins, o, "rol")
    def i_ror(self, ins, o): return self.shift(ins, o, "ror")
    def i_rcl(self, ins, o): return self.shift(ins, o, "rcl")
    def i_rcr(self, ins, o): return self.shift(ins, o, "rcr")

    def i_xlatb(self, ins, o):
        return self.reg_write("al", "R8(c->ebx + (uint8_t)c->eax)")

    def i_cpuid(self, ins, o):
        return "cpuid(c);"

    def i_rdtsc(self, ins, o):
        return "rdtsc(c);"

    def i_shld(self, ins, o):
        n = str(o[2].imm) if o[2].type == CS_OP_IMM else "(c->ecx & 0xff)"
        src = self.read(ins, o[1])
        return self.rmw(ins, o[0], lambda a: (
            f"({{ unsigned n_ = {n} & 31; uint32_t r_ = n_ ? (({a} << n_) | ({src} >> (32 - n_))) : {a}; "
            f"if (n_) {{ c->cf = ({a} >> (32 - n_)) & 1; c->zf = r_ == 0; c->sf = (int32_t)r_ < 0; c->pf_byte = (uint8_t)r_; }} r_; }})"))

    def i_shrd(self, ins, o):
        n = str(o[2].imm) if o[2].type == CS_OP_IMM else "(c->ecx & 0xff)"
        src = self.read(ins, o[1])
        return self.rmw(ins, o[0], lambda a: (
            f"({{ unsigned n_ = {n} & 31; uint32_t r_ = n_ ? (({a} >> n_) | ({src} << (32 - n_))) : {a}; "
            f"if (n_) {{ c->cf = ({a} >> (n_ - 1)) & 1; c->zf = r_ == 0; c->sf = (int32_t)r_ < 0; c->pf_byte = (uint8_t)r_; }} r_; }})"))

    def i_imul(self, ins, o):
        if len(o) == 1:
            s = o[0].size
            if s == 4:
                return ("{ int64_t r_ = (int64_t)(int32_t)c->eax * (int32_t)" + self.read(ins, o[0]) +
                        "; c->eax = (uint32_t)r_; c->edx = (uint32_t)(r_ >> 32); c->cf = c->of = r_ != (int32_t)r_; }")
            if s == 2:
                return ("{ int32_t r_ = (int16_t)c->eax * (int16_t)" + self.read(ins, o[0]) +
                        "; " + self.reg_write("ax", "r_") + " " + self.reg_write("dx", "r_ >> 16") +
                        " c->cf = c->of = r_ != (int16_t)r_; }")
            return ("{ int16_t r_ = (int8_t)c->eax * (int8_t)" + self.read(ins, o[0]) + "; " +
                    self.reg_write("ax", "r_") + " c->cf = c->of = r_ != (int8_t)r_; }")
        s = o[0].size
        a = self.read(ins, o[0]) if len(o) == 2 else self.read(ins, o[1])
        b = self.read(ins, o[1]) if len(o) == 2 else self.read(ins, o[2], s)
        return ("{ int64_t r_ = (int64_t)(" + ST_[s] + ")" + a + " * (" + ST_[s] + ")" + b + "; " +
                self.write(ins, o[0], "r_") + f" c->cf = c->of = r_ != ({ST_[s]})r_; }}")

    def i_mul(self, ins, o):
        s = o[0].size
        if s == 4:
            return ("{ uint64_t r_ = (uint64_t)c->eax * " + self.read(ins, o[0]) +
                    "; c->eax = (uint32_t)r_; c->edx = (uint32_t)(r_ >> 32); c->cf = c->of = c->edx != 0; }")
        if s == 2:
            return ("{ uint32_t r_ = (uint32_t)(uint16_t)c->eax * " + self.read(ins, o[0]) + "; " +
                    self.reg_write("ax", "r_") + " " + self.reg_write("dx", "r_ >> 16") + " c->cf = c->of = (r_ >> 16) != 0; }")
        return ("{ uint16_t r_ = (uint16_t)((uint8_t)c->eax * " + self.read(ins, o[0]) + "); " +
                self.reg_write("ax", "r_") + " c->cf = c->of = (r_ >> 8) != 0; }")

    def i_div(self, ins, o):
        s = o[0].size
        d = self.read(ins, o[0])
        if s == 4:
            return ("{ uint32_t d_ = " + d + "; uint64_t n_ = ((uint64_t)c->edx << 32) | c->eax; "
                    f"if (!d_) guest_trap(c, 0x{ins.address:08x}u, \"divide by zero\"); "
                    "c->eax = (uint32_t)(n_ / d_); c->edx = (uint32_t)(n_ % d_); }")
        if s == 2:
            return ("{ uint32_t d_ = " + d + "; uint32_t n_ = ((c->edx & 0xffff) << 16) | (c->eax & 0xffff); "
                    f"if (!d_) guest_trap(c, 0x{ins.address:08x}u, \"divide by zero\"); "
                    + self.reg_write("ax", "n_ / d_") + " " + self.reg_write("dx", "n_ % d_") + " }")
        return ("{ uint32_t d_ = " + d + "; uint32_t n_ = c->eax & 0xffff; "
                f"if (!d_) guest_trap(c, 0x{ins.address:08x}u, \"divide by zero\"); "
                + self.reg_write("al", "n_ / d_") + " " + self.reg_write("ah", "n_ % d_") + " }")

    def i_idiv(self, ins, o):
        s = o[0].size
        d = self.read(ins, o[0])
        if s == 4:
            return ("{ int32_t d_ = (int32_t)" + d + "; int64_t n_ = (int64_t)(((uint64_t)c->edx << 32) | c->eax); "
                    f"if (!d_) guest_trap(c, 0x{ins.address:08x}u, \"divide by zero\"); "
                    "c->eax = (uint32_t)(int32_t)(n_ / d_); c->edx = (uint32_t)(int32_t)(n_ % d_); }")
        if s == 2:
            return ("{ int32_t d_ = (int16_t)" + d + "; int32_t n_ = (int32_t)(((c->edx & 0xffff) << 16) | (c->eax & 0xffff)); "
                    f"if (!d_) guest_trap(c, 0x{ins.address:08x}u, \"divide by zero\"); "
                    + self.reg_write("ax", "n_ / d_") + " " + self.reg_write("dx", "n_ % d_") + " }")
        return ("{ int32_t d_ = (int8_t)" + d + "; int32_t n_ = (int16_t)c->eax; "
                f"if (!d_) guest_trap(c, 0x{ins.address:08x}u, \"divide by zero\"); "
                + self.reg_write("al", "n_ / d_") + " " + self.reg_write("ah", "n_ % d_") + " }")

    def bitop(self, ins, o, kind):
        s = o[0].size
        if o[1].type == CS_OP_IMM:
            bit = f"{o[1].imm % BITS[s]}u"
            base = None
        else:
            bit = f"({self.read(ins, o[1])})"
            base = o[0].type == CS_OP_MEM
        if base:  # bt [mem], reg addresses the whole bit string
            a = self.addr(ins, o[0])
            body = (f"{{ int32_t b_ = (int32_t){bit}; uint32_t a_ = {a} + (uint32_t)((b_ >> 3) & ~3); "
                    f"unsigned k_ = b_ & 31; uint32_t v_ = R32(a_); c->cf = (v_ >> k_) & 1;")
            upd = {"bt": "", "bts": " W32(a_, v_ | (1u << k_));", "btr": " W32(a_, v_ & ~(1u << k_));",
                   "btc": " W32(a_, v_ ^ (1u << k_));"}[kind]
            return body + upd + " }"
        k = f"({bit} % {BITS[s]})"
        cf = f"c->cf = ({self.read(ins, o[0])} >> {k}) & 1;"
        if kind == "bt":
            return cf
        expr = {"bts": "|", "btr": "& ~", "btc": "^"}[kind]
        return "{ " + cf + " " + self.rmw(ins, o[0], lambda a: f"({UT[s]})({a} {expr} ((({UT[s]})1) << {k}))") + " }"

    def i_bt(self, ins, o): return self.bitop(ins, o, "bt")
    def i_bts(self, ins, o): return self.bitop(ins, o, "bts")
    def i_btr(self, ins, o): return self.bitop(ins, o, "btr")
    def i_btc(self, ins, o): return self.bitop(ins, o, "btc")

    def i_bsf(self, ins, o):
        v = self.read(ins, o[1])
        return f"{{ uint32_t v_ = {v}; c->zf = v_ == 0; if (v_) {{ {self.write(ins, o[0], '__builtin_ctz(v_)')} }} }}"

    def i_bsr(self, ins, o):
        v = self.read(ins, o[1])
        return f"{{ uint32_t v_ = {v}; c->zf = v_ == 0; if (v_) {{ {self.write(ins, o[0], '31 - __builtin_clz(v_)')} }} }}"

    def i_xadd(self, ins, o):
        s = o[0].size
        return (f"{{ {UT[s]} d_ = {self.read(ins, o[0])}, s_ = {self.read(ins, o[1])}; "
                f"{self.write(ins, o[1], 'd_')} {self.write(ins, o[0], f'add{BITS[s]}(c, d_, s_)')} }}")

    def i_cmpxchg(self, ins, o):
        s = o[0].size
        acc = {1: "al", 2: "ax", 4: "eax"}[s]
        return (f"{{ {UT[s]} d_ = {self.read(ins, o[0])}; (void)sub{BITS[s]}(c, {self.reg_read(acc)}, d_); "
                f"if (c->zf) {{ {self.write(ins, o[0], self.read(ins, o[1]))} }} else {{ {self.reg_write(acc, 'd_')} }} }}")

    # --- flags
    def i_clc(self, ins, o): return "c->cf = 0;"
    def i_stc(self, ins, o): return "c->cf = 1;"
    def i_cmc(self, ins, o): return "c->cf ^= 1;"
    def i_cld(self, ins, o): return "c->df = 0;"
    def i_std(self, ins, o): return "c->df = 1;"
    def i_nop(self, ins, o): return ";"
    def i_fwait(self, ins, o): return ";"
    def i_wait(self, ins, o): return ";"

    def i_int3(self, ins, o):
        return f'guest_trap(c, 0x{ins.address:08x}u, "int3");'

    def i_hlt(self, ins, o):
        return f'guest_trap(c, 0x{ins.address:08x}u, "hlt");'

    def i_ud2(self, ins, o):
        return f'guest_trap(c, 0x{ins.address:08x}u, "ud2");'

    def i_int(self, ins, o):
        return f'guest_trap(c, 0x{ins.address:08x}u, "int {ins.op_str}");'

    # --- control flow
    def i_jmp(self, ins, o):
        if o[0].type == CS_OP_IMM:
            return self.target_stmt(o[0].imm)
        v = self.read(ins, o[0], 4)
        targets = self.p.jumptables.get(ins.address, [])
        cases = " ".join(f"case 0x{t:08x}u: goto L_{t:08x};" for t in sorted(set(targets)) if t in self.insn_addrs)
        for t in targets:
            if t in self.insn_addrs:
                self.labels.add(t)
        return (f"{{ uint32_t t_ = {v}; switch (t_) {{ {cases} default: jump_indirect(c, t_, 0x{ins.address:08x}u); return; }} }}")

    def i_call(self, ins, o):
        if o[0].type == CS_OP_IMM:
            return self.call_stmt(ins, None, o[0].imm)
        return self.call_stmt(ins, self.read(ins, o[0], 4))

    def i_ret(self, ins, o):
        n = 4 + (o[0].imm if o else 0)
        return f"c->esp += {n}; return;"

    i_retn = i_ret

    def i_leave(self, ins, o):
        return "c->esp = c->ebp; c->ebp = POP32(c);"

    def i_jecxz(self, ins, o):
        return f"if (c->ecx == 0) {self.target_stmt(o[0].imm)}"

    def i_jcxz(self, ins, o):
        return f"if ((c->ecx & 0xffff) == 0) {self.target_stmt(o[0].imm)}"

    def i_loop(self, ins, o):
        return f"if (--c->ecx != 0) {self.target_stmt(o[0].imm)}"

    # --- string instructions
    def string_op(self, ins, m, rep):
        size = {"b": 1, "w": 2, "d": 4}[m[-1]]
        base = m[:-1]
        bits, T = BITS[size], UT[size]
        step = f"(c->df ? -{size} : {size})"
        acc = {1: "al", 2: "ax", 4: "eax"}[size]
        if base == "movs":
            one = f"W{bits}(c->edi, R{bits}(c->esi)); c->esi += {step}; c->edi += {step};"
        elif base == "stos":
            one = f"W{bits}(c->edi, ({T}){self.reg_read(acc)}); c->edi += {step};"
        elif base == "lods":
            one = self.reg_write(acc, f"R{bits}(c->esi)") + f" c->esi += {step};"
        elif base == "scas":
            one = f"(void)sub{bits}(c, {self.reg_read(acc)}, R{bits}(c->edi)); c->edi += {step};"
        elif base == "cmps":
            one = f"(void)sub{bits}(c, R{bits}(c->esi), R{bits}(c->edi)); c->esi += {step}; c->edi += {step};"
        else:
            raise Unsupported(m)
        if not rep:
            return one
        if base in ("movs", "stos", "lods"):
            if base == "movs" and size == 4:
                # Fast path only when a forward dword-by-dword copy equals memmove: x86 copies forward,
                # so a destination overlapping just ahead of the source replicates data (memmove wouldn't).
                return ("if (!c->df && (c->edi - c->esi >= c->ecx * 4u || c->edi <= c->esi)) { "
                        "memmove(HP(c->edi), HP(c->esi), (size_t)c->ecx * 4); "
                        "c->esi += c->ecx * 4; c->edi += c->ecx * 4; c->ecx = 0; } "
                        f"else while (c->ecx) {{ {one} c->ecx--; }}")
            return f"while (c->ecx) {{ {one} c->ecx--; }}"
        stop = "!c->zf" if rep in ("rep", "repe", "repz") else "c->zf"
        return f"while (c->ecx) {{ {one} c->ecx--; if ({stop}) break; }}"

    # --- x87
    def st_idx(self, ins, op):
        n = ins.reg_name(op.reg)
        if not n.startswith("st"):
            raise Unsupported(f"x87 reg {n}")
        return int(n[3]) if "(" in n else 0

    def fmem(self, ins, op, integer=False):
        a = self.addr(ins, op)
        if integer:
            return {2: f"(double)(int16_t)R16({a})", 4: f"(double)(int32_t)R32({a})",
                    8: f"(double)(int64_t)R64({a})"}[op.size]
        return {4: f"(double)RF32({a})", 8: f"RF64({a})", 10: f"RF80({a})"}[op.size]

    def x87(self, ins, m, o):
        P = "FPRES(c, {})"
        binops = {"fadd": "{a} + {b}", "fmul": "{a} * {b}", "fsub": "{a} - {b}", "fsubr": "{b} - {a}",
                  "fdiv": "FDIV(c, {a}, {b})", "fdivr": "FDIV(c, {b}, {a})"}
        base = m.rstrip("p") if m not in ("fsubp", "fdivp", "faddp", "fmulp", "fsubrp", "fdivrp") else m[:-1]
        if m in ("fiadd", "fisub", "fisubr", "fimul", "fidiv", "fidivr"):
            b = self.fmem(ins, o[0], integer=True)
            e = binops["f" + m[2:]].format(a="ST(0)", b=b)
            return f"ST(0) = {P.format(e)};"
        if base in binops:
            e = binops[base]
            pop = m.endswith("p") and m not in ("fsubr",) and m != "fsub"
            if len(o) == 0:  # faddp with no operands == faddp st(1), st(0)
                return f"ST(1) = {P.format(e.format(a='ST(1)', b='ST(0)'))}; FPOP(c);"
            if o[0].type == CS_OP_MEM:
                return f"ST(0) = {P.format(e.format(a='ST(0)', b=self.fmem(ins, o[0])))};"
            if len(o) == 1:
                i = self.st_idx(ins, o[0])
                if pop:  # "fsubp st(i)" == FSUBP ST(i), ST(0)
                    return f"ST({i}) = {P.format(e.format(a=f'ST({i})', b='ST(0)'))}; FPOP(c);"
                # "fsub st(i)" == FSUB ST(0), ST(i)
                return f"ST(0) = {P.format(e.format(a='ST(0)', b=f'ST({i})'))};"
            d, s = self.st_idx(ins, o[0]), self.st_idx(ins, o[1])
            # Intel semantics: "fsubp st(i), st(0)" => st(i) = st(i) - st(0); "fsubrp" => st(0) - st(i)
            return f"ST({d}) = {P.format(e.format(a=f'ST({d})', b=f'ST({s})'))};" + (" FPOP(c);" if pop else "")
        if m == "fld":
            if o[0].type == CS_OP_MEM:
                return f"FPUSH(c, {self.fmem(ins, o[0])});"
            return f"{{ double v_ = ST({self.st_idx(ins, o[0])}); FPUSH(c, v_); }}"
        if m == "fild":
            return f"FPUSH(c, {self.fmem(ins, o[0], integer=True)});"
        if m in ("fst", "fstp"):
            pop = " FPOP(c);" if m == "fstp" else ""
            if o[0].type == CS_OP_MEM:
                a = self.addr(ins, o[0])
                w = {4: f"WF32({a}, (float)ST(0));", 8: f"WF64({a}, ST(0));", 10: f"WF80({a}, ST(0));"}[o[0].size]
                return w + pop
            return f"ST({self.st_idx(ins, o[0])}) = ST(0);" + pop
        if m in ("fist", "fistp", "fisttp"):
            a = self.addr(ins, o[0])
            conv = "fpu_to_int64" if o[0].size == 8 else "fpu_to_int32"
            if m == "fisttp":
                conv_e = f"({ST_[o[0].size]})trunc(ST(0))"
            else:
                conv_e = f"{conv}(c, ST(0))"
            w = {2: f"W16({a}, (uint16_t)(int16_t){conv_e});", 4: f"W32({a}, (uint32_t){conv_e});",
                 8: f"W64({a}, (uint64_t){conv_e});"}[o[0].size]
            return w + (" FPOP(c);" if m != "fist" else "")
        if m == "fxch":
            i = self.st_idx(ins, o[-1]) if o else 1  # Capstone: "fxch st(1)" has operands [st(0), st(1)]
            return f"{{ double t_ = ST(0); ST(0) = ST({i}); ST({i}) = t_; }}"
        consts = {"fld1": "1.0", "fldz": "0.0", "fldpi": "M_PI", "fldl2e": "M_LOG2E", "fldln2": "M_LN2",
                  "fldlg2": "0.30102999566398119521", "fldl2t": "3.32192809488736234787"}
        if m in consts:
            return f"FPUSH(c, {consts[m]});"
        unary = {"fchs": "-ST(0)", "fabs": "fabs(ST(0))", "fsqrt": "FPRES(c, sqrt(ST(0)))",
                 "fsin": "FTRIG(c, ST(0), sin)", "fcos": "FTRIG(c, ST(0), cos)",
                 "frndint": "fpu_round(c, ST(0))", "f2xm1": "(exp2(ST(0)) - 1.0)"}
        if m in unary:
            return f"ST(0) = {unary[m]};"
        if m == "fsincos":
            return ("{ double v_ = ST(0); if (fabs(v_) >= 9223372036854775808.0) c->fpu_sw |= 0x0400; "
                    "else { c->fpu_sw &= ~0x0400; ST(0) = sin(v_); FPUSH(c, cos(v_)); } }")
        if m == "fptan":
            return ("{ double v_ = ST(0); if (fabs(v_) >= 9223372036854775808.0) c->fpu_sw |= 0x0400; "
                    "else { c->fpu_sw &= ~0x0400; ST(0) = tan(v_); FPUSH(c, 1.0); } }")
        if m == "fpatan":
            return "ST(1) = atan2(ST(1), ST(0)); FPOP(c);"
        if m == "fyl2x":
            return "ST(1) = ST(1) * log2(ST(0)); FPOP(c);"
        if m == "fscale":
            return "ST(0) = ldexp(ST(0), (int)trunc(ST(1)));"
        if m in ("fprem", "fprem1"):
            fn = "fmod" if m == "fprem" else "remainder"
            return f"ST(0) = {fn}(ST(0), ST(1)); c->fpu_sw &= ~0x0400;"
        if m in ("fcom", "fcomp", "fucom", "fucomp", "ficom", "ficomp"):
            if not o:
                b = "ST(1)"
            elif o[0].type == CS_OP_MEM:
                b = self.fmem(ins, o[0], integer=m.startswith("fi"))
            else:
                b = f"ST({self.st_idx(ins, o[-1])})"
            return f"FCOM(c, ST(0), {b});" + (" FPOP(c);" if m.endswith("p") else "")
        if m in ("fcompp", "fucompp"):
            return "FCOM(c, ST(0), ST(1)); FPOP(c); FPOP(c);"
        if m in ("fcompi", "fucompi"):  # Capstone's spelling of fcomip / fucomip
            m = m[:-2] + "ip"
        if m in ("fcomi", "fucomi", "fcomip", "fucomip"):
            i = self.st_idx(ins, o[-1])
            return f"FCOMI(c, ST(0), ST({i}));" + (" FPOP(c);" if m.endswith("p") else "")
        fcmov = {"fcmovb": "c->cf", "fcmove": "c->zf", "fcmovbe": "(c->cf || c->zf)", "fcmovu": "PF(c)",
                 "fcmovnb": "!c->cf", "fcmovne": "!c->zf", "fcmovnbe": "(!c->cf && !c->zf)", "fcmovnu": "!PF(c)"}
        if m in fcmov:
            return f"if ({fcmov[m]}) ST(0) = ST({self.st_idx(ins, o[-1])});"
        if m == "ftst":
            return "FCOM(c, ST(0), 0.0);"
        if m == "fxam":
            return ("{ double v_ = ST(0); uint16_t sw_ = c->fpu_sw & ~0x4700; if (signbit(v_)) sw_ |= 0x0200; "
                    "if (isnan(v_)) sw_ |= 0x0100; else if (isinf(v_)) sw_ |= 0x0500; else if (v_ == 0) sw_ |= 0x4000; "
                    "else sw_ |= 0x0400; c->fpu_sw = sw_; }")
        if m in ("fnstsw", "fstsw"):
            if o and o[0].type == CS_OP_REG:
                return self.reg_write("ax", "FNSTSW(c)")
            return f"W16({self.addr(ins, o[0])}, FNSTSW(c));"
        if m in ("fnstcw", "fstcw"):
            return f"W16({self.addr(ins, o[0])}, c->fpu_cw);"
        if m == "fldcw":
            return f"c->fpu_cw = R16({self.addr(ins, o[0])}); fpu_sync_rounding(c);"
        if m in ("fnclex", "fclex"):
            return "c->fpu_sw &= 0x7f00;"
        if m in ("fninit", "finit"):
            return "c->fpu_cw = 0x037f; c->fpu_sw = 0; c->top = 0; fpu_sync_rounding(c);"
        if m == "ffree":
            return ";"
        if m == "fincstp":
            return "c->top = (c->top + 1) & 7;"
        if m == "fdecstp":
            return "c->top = (c->top - 1) & 7;"
        raise Unsupported(m)

    # --- packed integer: MMX (mm, 8 bytes) and SSE2 (xmm, 16 bytes) share the helpers in runtime/simd.h
    VBIN = {
        "paddb": "v_add(a_, b_, N, 1, 0)", "paddw": "v_add(a_, b_, N, 2, 0)", "paddd": "v_add(a_, b_, N, 4, 0)",
        "paddq": "v_add(a_, b_, N, 8, 0)", "paddsb": "v_add(a_, b_, N, 1, 1)", "paddsw": "v_add(a_, b_, N, 2, 1)",
        "paddusb": "v_add(a_, b_, N, 1, 2)", "paddusw": "v_add(a_, b_, N, 2, 2)",
        "psubb": "v_sub(a_, b_, N, 1, 0)", "psubw": "v_sub(a_, b_, N, 2, 0)", "psubd": "v_sub(a_, b_, N, 4, 0)",
        "psubq": "v_sub(a_, b_, N, 8, 0)", "psubsb": "v_sub(a_, b_, N, 1, 1)", "psubsw": "v_sub(a_, b_, N, 2, 1)",
        "psubusb": "v_sub(a_, b_, N, 1, 2)", "psubusw": "v_sub(a_, b_, N, 2, 2)",
        "pmullw": "v_mulw(a_, b_, N, 0)", "pmulhw": "v_mulw(a_, b_, N, 1)", "pmulhuw": "v_mulw(a_, b_, N, 2)",
        "pmaddwd": "v_pmaddwd(a_, b_, N)", "pmuludq": "v_pmuludq(a_, b_, N)",
        "pand": "v_logic(a_, b_, N, 0)", "pandn": "v_logic(a_, b_, N, 1)", "por": "v_logic(a_, b_, N, 2)",
        "pxor": "v_logic(a_, b_, N, 3)",
        "punpcklbw": "v_unpack(a_, b_, N, 1, 0)", "punpcklwd": "v_unpack(a_, b_, N, 2, 0)",
        "punpckldq": "v_unpack(a_, b_, N, 4, 0)", "punpcklqdq": "v_unpack(a_, b_, N, 8, 0)",
        "punpckhbw": "v_unpack(a_, b_, N, 1, 1)", "punpckhwd": "v_unpack(a_, b_, N, 2, 1)",
        "punpckhdq": "v_unpack(a_, b_, N, 4, 1)", "punpckhqdq": "v_unpack(a_, b_, N, 8, 1)",
        "packsswb": "v_pack(a_, b_, N, 2, 1)", "packuswb": "v_pack(a_, b_, N, 2, 0)", "packssdw": "v_pack(a_, b_, N, 4, 1)",
        "pcmpeqb": "v_cmp(a_, b_, N, 1, 0)", "pcmpeqw": "v_cmp(a_, b_, N, 2, 0)", "pcmpeqd": "v_cmp(a_, b_, N, 4, 0)",
        "pcmpgtb": "v_cmp(a_, b_, N, 1, 1)", "pcmpgtw": "v_cmp(a_, b_, N, 2, 1)", "pcmpgtd": "v_cmp(a_, b_, N, 4, 1)",
        "pmaxsw": "v_minmax(a_, b_, N, 0)", "pminsw": "v_minmax(a_, b_, N, 1)", "pmaxub": "v_minmax(a_, b_, N, 2)",
        "pminub": "v_minmax(a_, b_, N, 3)", "pavgb": "v_minmax(a_, b_, N, 4)", "pavgw": "v_minmax(a_, b_, N, 5)",
        "psadbw": "v_psadbw(a_, b_, N)",
    }
    VSHIFT = {"psllw": (2, 0), "pslld": (4, 0), "psllq": (8, 0), "psrlw": (2, 1), "psrld": (4, 1), "psrlq": (8, 1),
              "psraw": (2, 2), "psrad": (4, 2)}

    def vreg(self, ins, op):
        if op.type != CS_OP_REG:
            return None
        n = ins.reg_name(op.reg)
        if n.startswith("xmm"):
            return f"c->xmm[{n[3:]}].b", 16
        if n.startswith("mm"):
            return f"&c->mm[{n[2:]}]", 8
        return None

    def vload(self, ins, op, var, n):
        r = self.vreg(ins, op)
        if r:
            return f"memcpy({var}, {r[0]}, {n});"
        return f"memcpy({var}, HP({self.addr(ins, op)}), {n});"

    def vector_int(self, ins, m, o):
        if m in ("emms", "femms"):
            return ";"
        if not o:
            return None
        d = self.vreg(ins, o[0])
        if m in self.VBIN and d and len(o) == 2:
            dst, n = d
            return (f"{{ uint8_t a_[16], b_[16]; memcpy(a_, {dst}, {n}); {self.vload(ins, o[1], 'b_', n)} "
                    f"{self.VBIN[m].replace('N', str(n))}; memcpy({dst}, a_, {n}); }}")
        if m in self.VSHIFT and d:
            dst, n = d
            lane, kind = self.VSHIFT[m]
            if o[1].type == CS_OP_IMM:
                cnt = f"{o[1].imm & 0xff}u"
            else:
                cnt = "cnt_"
            pre = "" if o[1].type == CS_OP_IMM else f"uint8_t c8_[16]; {self.vload(ins, o[1], 'c8_', 8)} uint64_t cnt_; memcpy(&cnt_, c8_, 8); "
            return (f"{{ uint8_t a_[16]; {pre}memcpy(a_, {dst}, {n}); v_shift(a_, {cnt}, {n}, {lane}, {kind}); "
                    f"memcpy({dst}, a_, {n}); }}")
        if m in ("pslldq", "psrldq") and d:
            dst, n = d
            return (f"{{ uint8_t a_[16]; memcpy(a_, {dst}, {n}); v_bshift(a_, {o[1].imm}u, {n}, {int(m == 'pslldq')}); "
                    f"memcpy({dst}, a_, {n}); }}")
        if m in ("pshufw", "pshufd", "pshuflw", "pshufhw") and d:
            dst, n = d
            lane, words, base = {"pshufw": (2, 4, 0), "pshufd": (4, 4, 0), "pshuflw": (2, 4, 0),
                                 "pshufhw": (2, 4, 8)}[m]
            return (f"{{ uint8_t a_[16], b_[16]; {self.vload(ins, o[1], 'b_', n)} memcpy(a_, b_, {n}); "
                    f"v_pshufw(a_, b_, {o[2].imm}u, {lane}, {words}, {base}); memcpy({dst}, a_, {n}); }}")
        if m == "movd":
            if d:
                dst, n = d
                return f"{{ uint32_t v_ = {self.read(ins, o[1], 4)}; memset({dst}, 0, {n}); memcpy({dst}, &v_, 4); }}"
            sreg = self.vreg(ins, o[1])
            if sreg:
                return f"{{ uint32_t v_; memcpy(&v_, {sreg[0]}, 4); {self.write(ins, o[0], 'v_', 4)} }}"
            return None
        if m in ("movq", "movntq", "movq2dq", "movdq2q"):
            s_ = self.vreg(ins, o[1])
            if d:
                dst, n = d
                src = (f"memcpy(&v_, {s_[0]}, 8);" if s_ else f"v_ = R64({self.addr(ins, o[1])});")
                return f"{{ uint64_t v_; {src} memset({dst}, 0, {n}); memcpy({dst}, &v_, 8); }}"
            if s_ and o[0].type == CS_OP_MEM:
                return f"{{ uint64_t v_; memcpy(&v_, {s_[0]}, 8); W64({self.addr(ins, o[0])}, v_); }}"
            return None
        if m == "pextrw":
            s_ = self.vreg(ins, o[1])
            if s_:
                return (f"{{ uint16_t w_; memcpy(&w_, (uint8_t *){s_[0]} + 2 * ({o[2].imm}u % {s_[1] // 2}), 2); "
                        f"{self.write(ins, o[0], '(uint32_t)w_', 4)} }}")
        if m == "pinsrw" and d:
            dst, n = d
            src = self.read(ins, o[1], 4) if o[1].type == CS_OP_REG else f"R16({self.addr(ins, o[1])})"
            return f"{{ uint16_t w_ = (uint16_t)({src}); memcpy((uint8_t *){dst} + 2 * ({o[2].imm}u % {n // 2}), &w_, 2); }}"
        if m == "pmovmskb":
            s_ = self.vreg(ins, o[1])
            if s_:
                return self.write(ins, o[0], f"v_movmskb((const uint8_t *){s_[0]}, {s_[1]})", 4)
        if (d and ins.reg_name(o[0].reg).startswith("mm")) or (len(o) > 1 and self.vreg(ins, o[1]) and
                                                               self.vreg(ins, o[1])[1] == 8):
            raise Unsupported(f"MMX {m}")
        return None

    # --- SSE (the subset this binary's hand-written vector maths uses)
    def xmm(self, ins, op):
        if op.type != CS_OP_REG or not ins.reg_name(op.reg).startswith("xmm"):
            raise Unsupported("MMX/non-XMM operand")
        return f"c->xmm[{ins.reg_name(op.reg)[3:]}]"

    def sse(self, ins, m, o):
        packed = {"addps": "+", "subps": "-", "mulps": "*", "divps": "/"}
        scalar = {"addss": "+", "subss": "-", "mulss": "*", "divss": "/"}
        if m in packed or m in scalar:
            d = self.xmm(ins, o[0])
            n = 4 if m in packed else 1
            opr = (packed if m in packed else scalar)[m]
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                src = lambda k: f"RF32({a} + {4 * k})"
            else:
                s = self.xmm(ins, o[1])
                src = lambda k: f"{s}.f[{k}]"
            body = " ".join(f"{d}.f[{k}] = {d}.f[{k}] {opr} s{k}_;" for k in range(n))
            loads = " ".join(f"float s{k}_ = {src(k)};" for k in range(n))
            return f"{{ {loads} {body} }}"
        if m in ("movaps", "movups", "movdqa", "movdqu", "movapd", "movupd"):
            if o[0].type == CS_OP_MEM:
                return f"memcpy(HP({self.addr(ins, o[0])}), {self.xmm(ins, o[1])}.b, 16);"
            if o[1].type == CS_OP_MEM:
                return f"memcpy({self.xmm(ins, o[0])}.b, HP({self.addr(ins, o[1])}), 16);"
            return f"{self.xmm(ins, o[0])} = {self.xmm(ins, o[1])};"
        if m == "movss":
            if o[0].type == CS_OP_MEM:
                return f"WF32({self.addr(ins, o[0])}, {self.xmm(ins, o[1])}.f[0]);"
            if o[1].type == CS_OP_MEM:
                d = self.xmm(ins, o[0])
                return f"{d}.f[0] = RF32({self.addr(ins, o[1])}); {d}.u[1] = {d}.u[2] = {d}.u[3] = 0;"
            return f"{self.xmm(ins, o[0])}.f[0] = {self.xmm(ins, o[1])}.f[0];"
        if m in ("movlps", "movhps"):
            k = 0 if m == "movlps" else 1
            if o[0].type == CS_OP_MEM:
                return f"W64({self.addr(ins, o[0])}, {self.xmm(ins, o[1])}.q[{k}]);"
            return f"{self.xmm(ins, o[0])}.q[{k}] = R64({self.addr(ins, o[1])});"
        if m == "movhlps":
            return f"{self.xmm(ins, o[0])}.q[0] = {self.xmm(ins, o[1])}.q[1];"
        if m == "movlhps":
            return f"{self.xmm(ins, o[0])}.q[1] = {self.xmm(ins, o[1])}.q[0];"
        if m in ("andps", "andnps", "orps", "xorps"):
            d = self.xmm(ins, o[0])
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                src = lambda k: f"R32({a} + {4 * k})"
            else:
                s = self.xmm(ins, o[1])
                src = lambda k: f"{s}.u[{k}]"
            e = {"andps": "{d} & {s}", "andnps": "~{d} & {s}", "orps": "{d} | {s}", "xorps": "{d} ^ {s}"}[m]
            loads = " ".join(f"uint32_t s{k}_ = {src(k)};" for k in range(4))
            return "{ " + loads + " " + " ".join(
                f"{d}.u[{k}] = " + e.format(d=f"{d}.u[{k}]", s=f"s{k}_") + ";" for k in range(4)) + " }"
        if m == "shufps":
            d, imm = self.xmm(ins, o[0]), o[2].imm
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                s = lambda k: f"RF32({a} + {4 * k})"
            else:
                sx = self.xmm(ins, o[1])
                s = lambda k: f"{sx}.f[{k}]"
            sel = [(imm >> (2 * k)) & 3 for k in range(4)]
            return (f"{{ XMM t_ = {d}; {d}.f[0] = t_.f[{sel[0]}]; {d}.f[1] = t_.f[{sel[1]}]; "
                    f"{d}.f[2] = {s(sel[2])}; {d}.f[3] = {s(sel[3])}; }}")
        if m in ("unpcklps", "unpckhps"):
            d, s = self.xmm(ins, o[0]), self.xmm(ins, o[1])
            b = 0 if m == "unpcklps" else 2
            return (f"{{ XMM a_ = {d}, b_ = {s}; {d}.f[0] = a_.f[{b}]; {d}.f[1] = b_.f[{b}]; "
                    f"{d}.f[2] = a_.f[{b + 1}]; {d}.f[3] = b_.f[{b + 1}]; }}")
        sfn = {"sqrtss": "sqrtf({})", "rsqrtss": "1.0f / sqrtf({})", "rcpss": "1.0f / {}"}
        pfn = {"sqrtps": "sqrtf({})", "rsqrtps": "1.0f / sqrtf({})", "rcpps": "1.0f / {}",
               "minps": None, "maxps": None}
        if m in sfn:
            d = self.xmm(ins, o[0])
            s = f"RF32({self.addr(ins, o[1])})" if o[1].type == CS_OP_MEM else f"{self.xmm(ins, o[1])}.f[0]"
            return f"{d}.f[0] = {sfn[m].format(s)};"
        if m in ("sqrtps", "rsqrtps", "rcpps"):
            d, s = self.xmm(ins, o[0]), self.xmm(ins, o[1])
            return "{ XMM s_ = " + s + "; " + " ".join(f"{d}.f[{k}] = {pfn[m].format(f's_.f[{k}]')};" for k in range(4)) + " }"
        if m in ("minss", "maxss", "minps", "maxps"):
            d = self.xmm(ins, o[0])
            n = 1 if m.endswith("ss") else 4
            cmp = "<" if m.startswith("min") else ">"
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                src = lambda k: f"RF32({a} + {4 * k})"
            else:
                sx = self.xmm(ins, o[1])
                src = lambda k: f"{sx}.f[{k}]"
            return "{ " + " ".join(f"float s{k}_ = {src(k)}; {d}.f[{k}] = {d}.f[{k}] {cmp} s{k}_ ? {d}.f[{k}] : s{k}_;"
                                   for k in range(n)) + " }"
        if m in ("comiss", "ucomiss"):
            s = f"RF32({self.addr(ins, o[1])})" if o[1].type == CS_OP_MEM else f"{self.xmm(ins, o[1])}.f[0]"
            return f"FCOMI(c, {self.xmm(ins, o[0])}.f[0], {s});"
        if m == "cvtsi2ss":
            return f"{self.xmm(ins, o[0])}.f[0] = (float)(int32_t){self.read(ins, o[1], 4)};"
        if m in ("cvttss2si", "cvtss2si"):
            s = f"RF32({self.addr(ins, o[1])})" if o[1].type == CS_OP_MEM else f"{self.xmm(ins, o[1])}.f[0]"
            conv = f"(int32_t){s}" if m == "cvttss2si" else f"(int32_t)nearbyintf({s})"
            return self.write(ins, o[0], f"(uint32_t){conv}")
        if m == "movd":
            if o[0].type == CS_OP_REG and ins.reg_name(o[0].reg).startswith("xmm"):
                d = self.xmm(ins, o[0])
                return f"{d}.u[0] = {self.read(ins, o[1], 4)}; {d}.u[1] = {d}.u[2] = {d}.u[3] = 0;"
            return self.write(ins, o[0], f"{self.xmm(ins, o[1])}.u[0]", 4)
        cmp_pred = {"eq": "a_ == b_", "lt": "a_ < b_", "le": "a_ <= b_", "unord": "(a_ != a_ || b_ != b_)",
                    "neq": "!(a_ == b_)", "nlt": "!(a_ < b_)", "nle": "!(a_ <= b_)", "ord": "(a_ == a_ && b_ == b_)"}
        if m.startswith("cmp") and m[-2:] in ("ps", "ss") and m[3:-2] in cmp_pred:
            d, n = self.xmm(ins, o[0]), (4 if m.endswith("ps") else 1)
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                src = lambda k: f"RF32({a} + {4 * k})"
            else:
                sx = self.xmm(ins, o[1])
                src = lambda k: f"{sx}.f[{k}]"
            pr = cmp_pred[m[3:-2]]
            return "{ " + " ".join(f"{{ float a_ = {d}.f[{k}], b_ = {src(k)}; {d}.u[{k}] = ({pr}) ? 0xffffffffu : 0; }}"
                                   for k in range(n)) + " }"
        if m in ("pxor", "pand", "por", "pandn") and ins.reg_name(o[0].reg).startswith("xmm"):
            d = self.xmm(ins, o[0])
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                src = lambda k: f"R64({a} + {8 * k})"
            else:
                sx = self.xmm(ins, o[1])
                src = lambda k: f"{sx}.q[{k}]"
            e = {"pxor": "{d} ^ {s}", "pand": "{d} & {s}", "por": "{d} | {s}", "pandn": "~{d} & {s}"}[m]
            return "{ " + " ".join(f"uint64_t s{k}_ = {src(k)};" for k in range(2)) + " " + " ".join(
                f"{d}.q[{k}] = " + e.format(d=f"{d}.q[{k}]", s=f"s{k}_") + ";" for k in range(2)) + " }"
        if m == "movq" and all(op.type == CS_OP_MEM or ins.reg_name(op.reg).startswith("xmm") for op in o):
            if o[0].type == CS_OP_MEM:
                return f"W64({self.addr(ins, o[0])}, {self.xmm(ins, o[1])}.q[0]);"
            d = self.xmm(ins, o[0])
            v = f"R64({self.addr(ins, o[1])})" if o[1].type == CS_OP_MEM else f"{self.xmm(ins, o[1])}.q[0]"
            return f"{{ uint64_t v_ = {v}; {d}.q[0] = v_; {d}.q[1] = 0; }}"
        if m == "cvtpi2ps" and o[1].type == CS_OP_MEM:
            d, a = self.xmm(ins, o[0]), self.addr(ins, o[1])
            return f"{d}.f[0] = (float)(int32_t)R32({a}); {d}.f[1] = (float)(int32_t)R32({a} + 4);"
        if m == "cvtps2pd":
            d = self.xmm(ins, o[0])
            if o[1].type == CS_OP_MEM:
                a = self.addr(ins, o[1])
                return f"{{ float x_ = RF32({a}), y_ = RF32({a} + 4); {d}.d[0] = x_; {d}.d[1] = y_; }}"
            sx = self.xmm(ins, o[1])
            return f"{{ float x_ = {sx}.f[0], y_ = {sx}.f[1]; {d}.d[0] = x_; {d}.d[1] = y_; }}"
        if m in ("ldmxcsr",):
            return f"c->mxcsr = R32({self.addr(ins, o[0])});"
        if m in ("stmxcsr",):
            return f"W32({self.addr(ins, o[0])}, c->mxcsr);"
        if m in ("prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "emms", "sfence", "lfence", "mfence"):
            return ";"
        raise Unsupported(m)


class Program:
    def __init__(self, exe, functions_json):
        pe = pefile.PE(exe, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.img = pe.get_memory_mapped_image()
        d = json.load(open(functions_json))
        self.functions = {f["entry"]: f for f in d["functions"] if f["ranges"]}
        self.entries = set(self.functions)
        self.ghidra_entries = set(self.entries)  # real function starts; only these bound decoding
        self.jumptables = {j["at"]: j["targets"] for j in d["jumptables"]}
        self.md = Cs(CS_ARCH_X86, CS_MODE_32)
        self.md.detail = True
        self.trace = False
        self.add_pointer_entries(pe)

    def add_pointer_entries(self, pe):
        """Code reached only through pointers Ghidra never turned into functions (callbacks passed as
        `push offset label`, function tables in .data). Accept a target only if it looks like a function
        start: preceded by padding (int3/nop) or a ret (alignment alone admits round-number constants)."""
        secs = {s.Name.rstrip(b"\0").decode(): (self.base + s.VirtualAddress,
                                                 self.base + s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections}
        tlo, thi = secs[".text"]
        jt = {t for ts in self.jumptables.values() for t in ts}

        def plausible(t):
            if not (tlo <= t < thi) or t in self.entries or t in jt:
                return False
            prev = self.img[t - self.base - 1]
            prev3 = self.img[t - self.base - 3]
            return prev in (0xCC, 0x90, 0xC3) or prev3 == 0xC2

        md = Cs(CS_ARCH_X86, CS_MODE_32)
        md.detail = True
        insn_starts, imms, cross = set(), set(), set()
        ordered_g = sorted(self.entries)
        import bisect
        for f in list(self.functions.values()):
            e = f["entry"]
            k = bisect.bisect_right(ordered_g, e)
            span_end = ordered_g[k] if k < len(ordered_g) else thi
            for a, b in f["ranges"]:
                for ins in md.disasm(self.code(a, b), a):
                    insn_starts.add(ins.address)
                    if ins.mnemonic in ("push", "mov", "lea") and ins.operands and ins.operands[-1].type == CS_OP_IMM:
                        imms.add(ins.operands[-1].imm & 0xffffffff)
                    # direct jumps into the middle of another function (MSVC's strcpy jumps into strcat's
                    # copy loop; unwind funclets jump into their parent): those targets need entries too
                    if (ins.mnemonic == "jmp" or (ins.mnemonic.startswith("j") and ins.mnemonic[1:] in COND)) \
                            and ins.operands and ins.operands[0].type == CS_OP_IMM:
                        t = ins.operands[0].imm & 0xffffffff
                        if tlo <= t < thi and not (e <= t < span_end) and t not in self.entries:
                            cross.add(t)
        # Also accept exact instruction boundaries inside known functions: e.g. the CRT's x87 math
        # dispatcher jumps through a table to handlers in the middle of other functions.
        accept = lambda t: plausible(t) or (t in insn_starts and t not in self.entries and t not in jt)
        # Code no Ghidra function owns (e.g. the CRT's x87 math handlers, reached only via a .data table):
        # accept a data pointer there if it decodes to a few valid instructions.
        import bisect
        spans = sorted((a, b) for f in self.functions.values() for a, b in f["ranges"])
        starts = [a for a, _ in spans]

        def unowned_code(t):
            if not (tlo <= t < thi) or t in self.entries:
                return False
            k = bisect.bisect_right(starts, t) - 1
            if k >= 0 and spans[k][0] <= t < spans[k][1]:
                return False
            return len(list(md.disasm(self.code(t, t + 32), t, count=3))) == 3

        cands = {v for v in imms if accept(v)} | cross
        for name in (".rdata", ".data"):
            a, b = secs[name]
            for va in range(a, b - 3, 4):
                v = int.from_bytes(self.img[va - self.base:va - self.base + 4], "little")
                if accept(v) or unowned_code(v):
                    cands.add(v)
        # Function-pointer tables can also sit inside .text (e.g. right after a switch table); only take
        # targets on known instruction boundaries or that look like function starts.
        rd = lambda va: int.from_bytes(self.img[va - self.base:va - self.base + 4], "little")
        # Code Ghidra never claimed: gaps between known function bodies that decode as code (after
        # skipping int3/nop padding and any switch-table dwords) become functions of their own.
        gaps = 0
        prev_end = tlo
        for a, b in spans + [(thi, thi)]:
            if a > prev_end:
                g = prev_end
                while g < a and self.img[g - self.base] in (0xCC, 0x90):
                    g += 1
                while g + 4 <= a and tlo <= rd(g) < thi:  # table dwords pointing into code
                    g += 4
                while g < a and self.img[g - self.base] in (0xCC, 0x90):
                    g += 1
                if g < a and g not in self.entries:
                    insns = list(md.disasm(self.code(g, a), g, count=4))
                    if len(insns) >= 3 or (insns and insns[-1].mnemonic in ("ret", "jmp")):
                        cands.add(g)
                        gaps += 1
                # Further functions inside the same gap: MSVC pads between functions with int3 and
                # aligns starts to 16, so a 16-aligned address right after int3 padding is a start.
                for x in range((g + 15) & ~15, a, 16):
                    if (self.img[x - 1 - self.base] == 0xCC and self.img[x - self.base] not in (0xCC, 0x90)
                            and x not in self.entries and x not in cands):
                        insns = list(md.disasm(self.code(x, a), x, count=4))
                        if len(insns) >= 3 or (insns and insns[-1].mnemonic in ("ret", "jmp")):
                            cands.add(x)
                            gaps += 1
            prev_end = max(prev_end, b)
        self.gap_entries = gaps
        for va in range(tlo, thi - 3, 4):
            v = rd(va)
            if not (tlo <= v < thi):
                continue
            in_table = (tlo <= rd(va - 4) < thi) or (va + 8 <= thi and tlo <= rd(va + 4) < thi)
            if accept(v) or (in_table and unowned_code(v)):
                cands.add(v)
        # New entries can themselves jump or call into the middle of other functions (e.g. a static
        # initializer thunk `mov ecx, obj; jmp ctor` whose ctor Ghidra merged into a neighbour), so scan
        # them too, until no new targets appear. Direct calls to non-entries count as well.
        ordered_g = sorted(self.entries)
        todo = list(cands)
        seen = set(cands)
        for f in list(self.functions.values()):
            for a, b in f["ranges"]:
                for ins in md.disasm(self.code(a, b), a):
                    if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == CS_OP_IMM:
                        t = ins.operands[0].imm & 0xffffffff
                        if tlo <= t < thi and t not in self.entries and t not in seen:
                            cands.add(t); seen.add(t); todo.append(t)
        while todo:
            e = todo.pop()
            k = bisect.bisect_right(ordered_g, e)
            span_end = ordered_g[k] if k < len(ordered_g) else thi
            for ins in md.disasm(self.code(e, span_end), e):
                m = ins.mnemonic
                if m in ("push", "mov", "lea") and ins.operands and ins.operands[-1].type == CS_OP_IMM:
                    v = ins.operands[-1].imm & 0xffffffff
                    if tlo <= v < thi and v not in self.entries and v not in seen and v not in jt:
                        nxt = next(md.disasm(self.code(v, v + 16), v, count=1), None)
                        if nxt is not None:
                            cands.add(v); seen.add(v); todo.append(v)
                if (m in ("jmp", "call") or (m.startswith("j") and m[1:] in COND)) and ins.operands \
                        and ins.operands[0].type == CS_OP_IMM:
                    t = ins.operands[0].imm & 0xffffffff
                    outside = not (e <= t < span_end) or m == "call"
                    if tlo <= t < thi and outside and t not in self.entries and t not in seen:
                        cands.add(t); seen.add(t); todo.append(t)
                if m in ("ret", "retn", "jmp", "int3"):
                    break
        ordered = sorted(self.entries | cands)
        for t in cands:
            nxt = ordered[ordered.index(t) + 1] if t != ordered[-1] else thi
            self.functions[t] = {"entry": t, "name": f"PTR_{t:08x}", "ranges": [[t, nxt]]}
        self.entries |= cands
        self.pointer_entries = len(cands)

    def code(self, a, b):
        return self.img[a - self.base:b - self.base]

    def next_entry(self, e):
        import bisect
        if getattr(self, "_sorted_entries", None) is None:
            self._sorted_entries = sorted(self.ghidra_entries)
        k = bisect.bisect_right(self._sorted_entries, e)
        return self._sorted_entries[k] if k < len(self._sorted_entries) else e

    def direct_callees(self, f):
        """Direct call/jump targets plus function addresses used as constants (function pointers)."""
        out = set()
        for a, b in f["ranges"]:
            for ins in self.md.disasm(self.code(a, b), a):
                for op in ins.operands:
                    v = op.imm if op.type == CS_OP_IMM else (op.mem.disp & 0xffffffff if op.type == CS_OP_MEM else None)
                    if v in self.entries and v != f["entry"]:
                        out.add(v)
        return out

    def closure(self, roots):
        todo, seen = list(roots), set()
        while todo:
            e = todo.pop()
            if e in seen or e not in self.functions:
                continue
            seen.add(e)
            todo.extend(self.direct_callees(self.functions[e]))
        return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("functions_json")
    ap.add_argument("outdir")
    ap.add_argument("--roots", default="")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--per-file", type=int, default=400)
    ap.add_argument("--trace", action="store_true", help="call trace_insn() before every instruction")
    a = ap.parse_args()

    prog = Program(a.exe, a.functions_json)
    prog.trace = a.trace
    if a.all:
        chosen = set(prog.entries)
    else:
        chosen = prog.closure([int(x, 16) for x in a.roots.split(",") if x])
    os.makedirs(a.outdir, exist_ok=True)
    for old in os.listdir(a.outdir):  # chunk count can shrink between runs
        if old.startswith("recomp_") and old.endswith(".c"):
            os.remove(os.path.join(a.outdir, old))

    stats = Counter()
    unsup = Counter()
    bad_funcs = []
    chunks, cur = [], []
    for e in sorted(chosen):
        try:
            src, uns, n = FunctionTranslator(prog, prog.functions[e]).translate()
        except Exception as ex:  # keep going; record the failure
            src = f"void f_{e:08x}(CPU *c) {{ guest_trap(c, 0x{e:08x}u, \"translator failed: {type(ex).__name__}\"); }}\n"
            uns, n = [f"translator crash: {type(ex).__name__}: {ex}"], 0
        stats["functions"] += 1
        stats["instructions"] += n
        if uns:
            stats["functions_with_unsupported"] += 1
            stats["unsupported_instructions"] += len(uns)
            bad_funcs.append(e)
            for u in uns:
                unsup[u.split(" (")[0]] += 1
        cur.append(src)
        if len(cur) >= a.per_file:
            chunks.append(cur)
            cur = []
    if cur:
        chunks.append(cur)

    hdr = ['#include "runtime.h"', '#include "simd.h"', '#include <math.h>', ""]
    hdr += [f"void f_{e:08x}(CPU *c);" for e in sorted(prog.entries)]
    with open(os.path.join(a.outdir, "recomp.h"), "w") as f:
        f.write("#ifndef RECOMP_H\n#define RECOMP_H\n" + "\n".join(hdr) + "\n#endif\n")
    for i, ch in enumerate(chunks):
        with open(os.path.join(a.outdir, f"recomp_{i:03d}.c"), "w") as f:
            f.write('#include "recomp.h"\n\n' + "\n".join(ch))
    # Functions referenced but not translated get trapping stubs so everything links.
    missing = sorted(prog.entries - chosen)
    with open(os.path.join(a.outdir, "recomp_stubs.c"), "w") as f:
        f.write('#include "recomp.h"\n\n')
        for e in missing:
            f.write(f'void f_{e:08x}(CPU *c) {{ guest_trap(c, 0x{e:08x}u, "function not recompiled"); }}\n')
        f.write("\nvoid register_all_functions(void) {\n")
        for e in sorted(prog.entries):
            f.write(f"    register_function(0x{e:08x}u, f_{e:08x});\n")
        f.write("}\n")

    stats["files"] = len(chunks)
    stats["pointer_entries"] = prog.pointer_entries
    stats["gap_entries"] = prog.gap_entries
    print(json.dumps(dict(stats)))
    if unsup:
        print("unsupported by mnemonic:", unsup.most_common(25))
    with open(os.path.join(a.outdir, "translate_report.json"), "w") as f:
        json.dump({"stats": stats, "unsupported": unsup.most_common(), "functions_with_unsupported":
                   [f"0x{e:08x}" for e in bad_funcs]}, f, indent=1)


if __name__ == "__main__":
    main()
