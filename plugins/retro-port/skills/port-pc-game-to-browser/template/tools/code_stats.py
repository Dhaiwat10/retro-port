import sys, pefile, struct, collections
from capstone import Cs, CS_ARCH_X86, CS_MODE_32, CS_GRP_JUMP
pe = pefile.PE(sys.argv[1]); base = pe.OPTIONAL_HEADER.ImageBase
t = [s for s in pe.sections if s.Name.startswith(b'.text')][0]
code = t.get_data(); tva = base + t.VirtualAddress; tend = tva + t.Misc_VirtualSize
# direct call targets via linear sweep
md = Cs(CS_ARCH_X86, CS_MODE_32); md.skipdata = True
targets = set(); mn = collections.Counter(); total = 0; indirect_jmp = 0; indirect_call = 0
for ins in md.disasm(code, tva):
    total += 1
    m = ins.mnemonic; mn[m] += 1
    if m == 'call':
        op = ins.op_str
        if op.startswith('0x'):
            v = int(op, 16)
            if tva <= v < tend: targets.add(v)
        else: indirect_call += 1
    elif m == 'jmp' and not ins.op_str.startswith('0x'): indirect_jmp += 1
x87 = sum(v for k, v in mn.items() if k.startswith('f'))
sse = sum(v for k, v in mn.items() if k.endswith(('ps', 'ss', 'pd', 'sd')) and not k.startswith('f') and k not in ('movsd',))
mmx = sum(v for k, v in mn.items() if k.startswith('p') and k not in ('push','pop','pushal','popal','pushfd','popfd','pause'))
print(f".text: {t.Misc_VirtualSize/1e6:.2f} MB, ~{total:,} instructions (linear sweep)")
print(f"Unique direct call targets (≈ functions, lower bound): {len(targets):,}")
print(f"Indirect calls: {indirect_call:,}  Indirect jumps (switch tables/tail calls): {indirect_jmp:,}")
print(f"x87 FPU instructions: {x87:,}   SSE scalar/packed: {sse:,}   MMX/SSE int (p*): {mmx:,}")
print("Top FPU mnemonics:", [(k, v) for k, v in mn.most_common(400) if k.startswith('f')][:12])
print("Top SSE mnemonics:", [(k, v) for k, v in mn.most_common(400) if k.endswith(('ps','ss')) and not k.startswith('f')][:10])
