"""Finds the first instruction where the recompiled code diverges from the original.

Runs one function/trial of the differential tester twice: the original under Unicorn with a
per-instruction hook, and the tracing build (TRACE=1 recomp/build.sh) recording the same registers
before every instruction. Prints the first step where the instruction address or a general register
differs, with the preceding instructions for context.

Usage: trace_compare.py <0xFUNC> [trial] [--sane] [--max N]
"""
import argparse
import ctypes
import os
import random
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import difftest as D  # noqa: E402

P = os.path.dirname(os.path.dirname(HERE))


class TraceRec(ctypes.Structure):
    _fields_ = [("addr", ctypes.c_uint32), ("regs", ctypes.c_uint32 * 8), ("st0", ctypes.c_double),
                ("top", ctypes.c_uint32)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("func")
    ap.add_argument("trial", nargs="?", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--sane", action="store_true")
    ap.add_argument("--max", type=int, default=2_000_000)
    a = ap.parse_args()
    entry = int(a.func, 16)

    t = D.Tester(f"{P}/game/" + __import__("json").load(open(f"{P}/port.json"))["exe"], f"{P}/recomp/build/libporttrace.dylib")
    t.lib.dt_trace_buf.restype = ctypes.POINTER(TraceRec)
    rng = random.Random(a.seed * 1000003 + entry * 31 + a.trial + (7777 if a.sane else 0))
    scratch, stack, regs = t.setup_state(rng, a.sane)

    utrace = []

    from unicorn.x86_const import UC_X86_REG_FP0, UC_X86_REG_FPSW

    def st0(uc):
        top = (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7  # Unicorn's ST0 constant is physical register 0
        raw = uc.reg_read(UC_X86_REG_FP0 + top)  # 80-bit value: 64-bit mantissa, then 16-bit sign+exponent
        mant, exp = raw if isinstance(raw, tuple) else (raw & ((1 << 64) - 1), raw >> 64)
        e = exp & 0x7fff
        if e == 0 and mant == 0:
            v = 0.0
        elif e == 0x7fff:
            v = float("nan") if (mant << 1) & ((1 << 64) - 1) else float("inf")
        else:
            v = float(mant) * 2.0 ** (e - 16383 - 63)
        return -v if exp & 0x8000 else v

    def rec(uc, addr, size, data):
        # a rep-prefixed instruction shows up once per iteration; keep only its first step
        if utrace and utrace[-1][0] == addr and data is None and rep_insn(addr):
            return
        if len(utrace) < a.max:
            utrace.append((addr, [uc.reg_read(r) for r in D.REGS], st0(uc)))

    md0 = Cs(CS_ARCH_X86, CS_MODE_32)
    rep_cache = {}

    def rep_insn(addr):
        if addr not in rep_cache:
            ins = next(md0.disasm(t.image[addr - t.base:addr - t.base + 16], addr, count=1), None)
            rep_cache[addr] = bool(ins) and ins.mnemonic.startswith(("rep", "repne", "repe"))
        return rep_cache[addr]

    uc, err, demand = t.run_unicorn(entry, scratch, stack, regs, code_hook=rec)
    print("unicorn:", err or "returned", f"({len(utrace)} instructions)")

    t.cwrite(t.base, t.image)
    t.cwrite(D.SCRATCH, scratch)
    t.cwrite(D.STACK_LO, bytes(D.STACK_HI - D.STACK_LO))
    t.cwrite(D.TIB, t.tib)
    t.cwrite(D.GDT, D.GDT_PAGE)
    t.cwrite(regs[4], stack)
    for p_ in demand:
        t.cwrite(p_, bytes(D.PAGE))
    t.lib.dt_trace_start(a.max)
    cregs = (ctypes.c_uint32 * 8)(*regs)
    rc = t.lib.dt_run(entry, cregs, D.FPU_CW)
    n = t.lib.dt_trace_len()
    buf = t.lib.dt_trace_buf()
    ctrace = [(buf[i].addr, list(buf[i].regs), buf[i].st0) for i in range(n)]
    print("recomp: rc", rc, f"({n} instructions)", t.lib.dt_trap_msg().decode() if rc == 1 else "")

    md = Cs(CS_ARCH_X86, CS_MODE_32)

    def dis(addr):
        ins = next(md.disasm(t.image[addr - t.base:addr - t.base + 16], addr, count=1), None)
        return f"{ins.mnemonic} {ins.op_str}" if ins else "?"

    for i in range(min(len(utrace), len(ctrace))):
        ua, ur, us = utrace[i]
        ca, cr, cs = ctrace[i]
        same_st0 = us == cs or (us != us and cs != cs) or (abs(us - cs) <= abs(us) * 1e-15)
        if ua != ca or ur != cr or (i > 0 and not same_st0 and not rep_insn(ua)):
            print(f"\nfirst divergence at step {i}:")
            for j in range(max(0, i - 6), i + 1):
                print(f"  step {j}: {utrace[j][0]:08x}  {dis(utrace[j][0])}")
            print(f"  -> unicorn at {ua:08x}, recomp at {ca:08x}")
            for k, name in enumerate(D.NAMES):
                if ur[k] != cr[k]:
                    print(f"     {name}: unicorn {ur[k]:08x}  recomp {cr[k]:08x}")
            if not same_st0:
                print(f"     st0: unicorn {us!r}  recomp {cs!r}")
            print(f"  (registers are captured before each step, so the culprit is step {i - 1})")
            return
    print("no divergence in the common prefix" + ("" if len(utrace) == len(ctrace) else
          f"; lengths differ ({len(utrace)} vs {len(ctrace)})"))


if __name__ == "__main__":
    main()
