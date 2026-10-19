"""Differential tester: original x86 (under Unicorn) vs recompiled C, on identical random inputs.

For every selected function and several random trials, both sides start from the same image,
registers, stack (8 random arguments + a sentinel return address) and a 64 KiB random scratch
buffer. After the function returns we compare all general registers and every byte of memory
either side could have touched.

Usage: difftest.py <exe> <functions.json> <libportrecomp.dylib> [--max N] [--trials K] [--only 0xADDR]
"""
import argparse
import ctypes
import json
import random
import struct
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, CS_OP_IMM, CS_OP_MEM, Cs
from unicorn import UC_PROT_READ, UC_PROT_WRITE, UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_INSN, UC_HOOK_MEM_FETCH_UNMAPPED, UC_HOOK_MEM_READ_UNMAPPED, \
    UC_HOOK_MEM_WRITE_UNMAPPED, UC_MODE_32, Uc, UcError
from unicorn.x86_const import (UC_X86_INS_CPUID, UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,
                               UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EIP, UC_X86_REG_ESI,
                               UC_X86_REG_ESP, UC_X86_REG_FPCW, UC_X86_REG_EFLAGS, UC_X86_REG_FS,
                               UC_X86_REG_GDTR, UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES)

REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,
        UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]
NAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
PAGE = 0x1000
SENTINEL = 0xDEADBEEF
SCRATCH, SCRATCH_SIZE = 0x20000000, 0x10000
STACK_LO, STACK_HI = 0x002F0000, 0x00300000
FPU_CW = 0x027F  # 53-bit precision: what the recompiled x87 (host doubles) models exactly
TIB = 0x00010000   # GUEST_TIB in runtime.h: fs:[0] is the SEH chain head
GDT = 0x00001000


def gdt_entry(base, limit, access, flags):
    v = (limit & 0xffff) | (base & 0xffffff) << 16 | (access & 0xff) << 40 | ((limit >> 16) & 0xf) << 48 \
        | (flags & 0xf) << 52 | ((base >> 24) & 0xff) << 56
    return struct.pack("<Q", v)


# GDT page: [2] flat 32-bit data segment for SS/DS/ES (once a GDT is loaded, Unicorn otherwise treats SS as
# 16-bit), [3] fs -> thread information block. Ring 0, like Unicorn itself. The same bytes are written into
# the recompiled side's memory, since code following a random small pointer can read this page.
GDT_PAGE = (bytes(16) + gdt_entry(0, 0xfffff, 0x80 | 0x10 | 0x2 | 0x1, 0xc)  # accessed bit pre-set: loading SS sets it
            + gdt_entry(TIB, PAGE - 1, 0x80 | 0x10 | 0x2 | 0x1, 0x4)).ljust(PAGE, b"\0")


def pick_functions(prog_img, base, funcs, jumptables, allow_calls=False):
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    out = []
    for f in funcs:
        ok, n = True, 0
        for a, b in f["ranges"]:
            for ins in md.disasm(prog_img[a - base:b - base], a):
                n += 1
                m = ins.mnemonic
                if (m == "call" and not allow_calls) or m.startswith(("int", "in ", "out", "hlt", "cpuid", "rdtsc")):
                    ok = False
                if m == "jmp" and ins.operands and ins.operands[0].type != CS_OP_IMM and ins.address not in jumptables:
                    ok = False
                if m == "jmp" and not allow_calls and ins.operands and ins.operands[0].type == CS_OP_IMM and not any(
                        x <= ins.operands[0].imm < y for x, y in f["ranges"]):
                    ok = False  # tail call
                if any(ins.reg_name(r).startswith("mm") for r in ins.regs_access()[0] + ins.regs_access()[1]):
                    ok = False
            if not ok:
                break
        if ok and n >= 3:
            out.append(f)
    return out


class Tester:
    def __init__(self, exe, lib):
        self.lib = ctypes.CDLL(lib)
        self.lib.dt_mem.restype = ctypes.c_void_p
        self.lib.dt_trap_msg.restype = ctypes.c_char_p
        self.lib.dt_run.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint16]
        if self.lib.dt_init(exe.encode()) != 0:
            raise SystemExit("dt_init failed")
        self.mem = self.lib.dt_mem()
        pe = pefile.PE(exe, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = (pe.OPTIONAL_HEADER.SizeOfImage + PAGE - 1) & ~(PAGE - 1)
        # Snapshot the loaded image from the C side (it includes the patched import table).
        self.image = ctypes.string_at(self.mem + self.base, self.size)
        self.tib = ctypes.string_at(self.mem + TIB, PAGE)
        # rdtsc makes a run nondeterministic (the game seeds its RNG with it): find every occurrence
        # (byte pattern 0F 31; a false match only turns a test into a skip)
        text = self.image[0x1000:0x385000]
        self.rdtsc_sites, k = [], text.find(b"\x0f\x31")
        while k >= 0:
            self.rdtsc_sites.append(self.base + 0x1000 + k)
            k = text.find(b"\x0f\x31", k + 1)

    def cwrite(self, addr, data):
        ctypes.memmove(self.mem + addr, data, len(data))

    def cread(self, addr, n):
        return ctypes.string_at(self.mem + addr, n)

    def setup_state(self, rng, sane=False):
        if sane:  # every word is a moderate float (|x| in [2^-10, 2^10]); as doubles the high words are moderate too
            words = [(rng.getrandbits(1) << 31) | ((127 + rng.randrange(-10, 11)) << 23) | rng.getrandbits(23)
                     for _ in range(SCRATCH_SIZE // 4)]
            scratch = b"".join(w.to_bytes(4, "little") for w in words)
        else:
            scratch = rng.randbytes(SCRATCH_SIZE)
        ptr = lambda: SCRATCH + rng.randrange(0x100, SCRATCH_SIZE - 0x1000) & ~3
        small = lambda: rng.choice([0, 1, 2, 3, 4, 7, 8, 16, 31, 100, 255, 0x7fffffff, rng.randrange(1 << 16)])
        args = [ptr() if rng.random() < 0.5 else small() for _ in range(8)]
        esp = STACK_HI - 0x800
        stack = SENTINEL.to_bytes(4, "little") + b"".join(a.to_bytes(4, "little") for a in args)
        regs = [small(), ptr(), ptr(), small(), esp, ptr(), ptr(), ptr()]
        return scratch, stack, regs

    def run_unicorn(self, entry, scratch, stack, regs, code_hook=None):
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        if code_hook:
            uc.hook_add(UC_HOOK_CODE, code_hook)
        uc.mem_map(self.base, self.size)
        uc.mem_write(self.base, self.image)
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_write(SCRATCH, scratch)
        uc.mem_map(STACK_LO, STACK_HI - STACK_LO)
        uc.mem_write(regs[4], stack)
        # fs: -> thread information block, through a GDT entry (Unicorn has no fs base in 32-bit mode)
        uc.mem_map(GDT, PAGE)
        uc.mem_write(GDT, GDT_PAGE)
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, PAGE - 1, 0))
        for seg in (UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES):
            uc.reg_write(seg, 2 << 3)
        uc.reg_write(UC_X86_REG_FS, 3 << 3)
        uc.mem_map(TIB, PAGE)
        uc.mem_write(TIB, self.tib)
        demand = []

        def on_unmapped(uc_, access, addr, size, value, data):
            if access == UC_HOOK_MEM_FETCH_UNMAPPED or addr == SENTINEL:
                return False
            page = addr & ~(PAGE - 1)
            uc_.mem_map(page, PAGE, UC_PROT_READ | UC_PROT_WRITE)  # data only: calling into it is a crash
            demand.append(page)
            if (addr + size - 1) & ~(PAGE - 1) != page:
                uc_.mem_map(page + PAGE, PAGE, UC_PROT_READ | UC_PROT_WRITE)
                demand.append(page + PAGE)
            return True

        uc.hook_add(UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED, on_unmapped)
        # Page 0 is hit constantly through small random "pointers". Map it up front: QEMU can drop the
        # rest of a `rep movs` whose destination page gets mapped on demand mid-instruction.
        uc.mem_map(0, PAGE, UC_PROT_READ | UC_PROT_WRITE)
        demand.append(0)
        nondet = []
        for a in self.rdtsc_sites:
            uc.hook_add(UC_HOOK_CODE, lambda uc_, addr, size, d: (nondet.append(addr), uc_.emu_stop()), begin=a, end=a)
        uc.hook_add(UC_HOOK_INSN, cpuid_hook, None, 1, 0, UC_X86_INS_CPUID)
        for r, v in zip(REGS, regs):
            uc.reg_write(r, v)
        uc.reg_write(UC_X86_REG_FPCW, FPU_CW)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)  # user-mode Windows: IF set
        try:
            uc.emu_start(entry, SENTINEL, timeout=2_000_000, count=2_000_000)
        except UcError as e:
            if uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
                return None, f"unicorn: {e} at eip={uc.reg_read(UC_X86_REG_EIP):08x}", demand
        if nondet:
            return None, "unicorn: nondeterministic (rdtsc)", demand
        if uc.reg_read(UC_X86_REG_EIP) != SENTINEL:
            return None, "unicorn: instruction limit", demand
        return uc, None, demand

    def run(self, f, trials, seed):
        status, why = self.run_mode(f, trials, seed, sane=False)
        if status == "fpu_flags":
            return status, why
        if status == "fail" and self.uses_fp(f) and "trap" not in why and "watchdog" not in why:
            s2, _ = self.run_mode(f, trials, seed, sane=True)
            if s2 in ("pass", "nan"):
                return "fp_range", why  # differs only for magnitudes beyond the double exponent range
        return status, why

    def uses_fp(self, f):
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        for a, b in f["ranges"]:
            for ins in md.disasm(self.image[a - self.base:b - self.base], a):
                if ins.mnemonic.startswith("f") or ins.mnemonic.endswith(("ss", "ps")):
                    return True
        return False

    def run_mode(self, f, trials, seed, sane):
        entry = f["entry"]
        nan_only = False
        for t in range(trials):
            rng = random.Random(seed * 1000003 + entry * 31 + t + (7777 if sane else 0))
            scratch, stack, regs = self.setup_state(rng, sane)
            uc, err, demand = self.run_unicorn(entry, scratch, stack, regs)
            if uc is None:
                return "skip", err
            # C side from the identical state
            self.cwrite(self.base, self.image)
            self.cwrite(SCRATCH, scratch)
            self.cwrite(STACK_LO, bytes(STACK_HI - STACK_LO))
            self.cwrite(TIB, self.tib)
            self.cwrite(GDT, GDT_PAGE)
            self.cwrite(regs[4], stack)
            for p in demand:
                self.cwrite(p, bytes(PAGE))
            cregs = (ctypes.c_uint32 * 8)(*regs)
            rc = self.lib.dt_run(entry, cregs, FPU_CW)
            if rc == 1:
                return "fail", f"trap {self.lib.dt_trap_msg().decode()}"
            if rc == 2:
                return "skip", "not recompiled"
            if rc == 3:
                return "fail", f"trial {t}: recompiled code did not return (watchdog)"
            for i, r in enumerate(REGS):
                u = uc.reg_read(r)
                if u != cregs[i] and nan_word(u.to_bytes(4, "little"), cregs[i].to_bytes(4, "little"), 0):
                    nan_only = True
                elif u != cregs[i] and (u ^ cregs[i]) & ~0x3f == 0 and NAMES[i] == "eax" and self.uses_fp(f):
                    # x87 exception bits read back with FNSTSW. QEMU (inside Unicorn) raises ZE whenever the
                    # divisor is zero, even for 0/0, where hardware raises only IE as the recompiled code does.
                    return "fpu_flags", f"trial {t}: {NAMES[i]} unicorn={u:08x} recomp={cregs[i]:08x}"
                elif u != cregs[i]:
                    return "fail", f"trial {t}: {NAMES[i]} unicorn={u:08x} recomp={cregs[i]:08x}"
            regions = [(self.base, self.size), (SCRATCH, SCRATCH_SIZE), (STACK_LO, STACK_HI - STACK_LO), (TIB, PAGE), (GDT, PAGE)]
            regions += [(p, PAGE) for p in demand]
            for a, n in regions:
                um, cm = bytes(uc.mem_read(a, n)), self.cread(a, n)
                if um != cm:
                    diffs = [i for i in range(n) if um[i] != cm[i]]
                    if all(nan_word(um, cm, i) for i in diffs):
                        nan_only = True
                        continue
                    k = next(i for i in diffs if not nan_word(um, cm, i))
                    return "fail", f"trial {t}: memory differs at {a + k:08x} (unicorn {um[k]:02x}, recomp {cm[k]:02x})"
        return ("nan", "only NaN bit patterns differ") if nan_only else ("pass", None)


def cpuid_hook(uc, data):
    """Same answers as runtime.c cpuid(): a plain Pentium-class CPU without MMX/SSE/3DNow!."""
    leaf = uc.reg_read(UC_X86_REG_EAX)
    vals = {0: (1, 0x756e6547, 0x6c65746e, 0x49656e69), 1: (0x683, 0, 0, 0x8111),
            0x80000000: (0x80000000, 0, 0, 0)}.get(leaf, (0, 0, 0, 0))
    for r, v in zip((UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX), vals):
        uc.reg_write(r, v)
    return True


def nan_word(um, cm, i):
    """True if byte i lies inside a float32/float64 (any alignment) that is NaN on both sides."""
    for size, mask_e, mask_m in ((4, 0x7f800000, 0x007fffff), (8, 0x7ff0000000000000, 0x000fffffffffffff)):
        for j in range(max(0, i - size + 1), i + 1):
            if j + size > len(um):
                continue
            a, b = int.from_bytes(um[j:j + size], "little"), int.from_bytes(cm[j:j + size], "little")
            if (a & mask_e) == mask_e and (a & mask_m) and (b & mask_e) == mask_e and (b & mask_m):
                return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("functions_json")
    ap.add_argument("lib")
    ap.add_argument("--max", type=int, default=0)
    ap.add_argument("--trials", type=int, default=5)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", default="")
    ap.add_argument("--report", default="")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--shard", default="", help="k/n: test every n-th function starting at k")
    ap.add_argument("--allow-calls", action="store_true", help="also test functions that call others (whole call trees run on both sides)")
    a = ap.parse_args()

    tester = Tester(a.exe, a.lib)
    d = json.load(open(a.functions_json))
    jumptables = {j["at"] for j in d["jumptables"]}
    funcs = [f for f in d["functions"] if f["ranges"]]
    if a.only:
        funcs = [f for f in funcs if f["entry"] == int(a.only, 16)]
    else:
        funcs = pick_functions(tester.image, tester.base, funcs, jumptables, a.allow_calls)
    if a.max:
        funcs = funcs[:a.max]
    if a.shard:
        k, n = map(int, a.shard.split("/"))
        funcs = funcs[k::n]
    print(f"testing {len(funcs)} functions x {a.trials} trials", flush=True)
    results = {"pass": [], "nan": [], "fp_range": [], "fpu_flags": [], "fail": [], "skip": []}
    for i, f in enumerate(funcs):
        if a.verbose: print(f"  run 0x{f['entry']:08x}", flush=True)
        status, why = tester.run(f, a.trials, a.seed)
        results[status].append((f"0x{f['entry']:08x}", f["name"], why))
        if status == "fail":
            print(f"FAIL 0x{f['entry']:08x} {f['name']}: {why}", flush=True)
        if (i + 1) % 500 == 0:
            print(f"  ... {i + 1}: pass={len(results['pass'])} fail={len(results['fail'])} skip={len(results['skip'])}", flush=True)
    print("\n" + " ".join(f"{k}={len(v)}" for k, v in results.items()))
    if a.report:
        json.dump(results, open(a.report, "w"), indent=1)


if __name__ == "__main__":
    main()
