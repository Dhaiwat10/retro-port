#!/usr/bin/env python3
"""Disassembles part of the game executable, naming imported functions.

    <port>/.venv/bin/python -I disasm.py <game.exe> <hex address>[:<hex length>] ...

Use it on return addresses from stack dumps, the watchdog or a profile, to see what a loop is doing.
"""
import re
import sys

import capstone
import pefile

pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
imp = {i.address: (i.name or b"#%d" % i.ordinal).decode() for d in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) for i in d.imports}
for spec in sys.argv[2:]:
    a, _, n = spec.partition(":")
    a, n = int(a, 16), int(n or "60", 16)
    print(f"--- {a:08x}")
    for ins in md.disasm(pe.get_data(a - base, n), a):
        s = ins.op_str
        for m in re.findall(r"0x[0-9a-f]+", s):
            if int(m, 16) in imp:
                s += f"   ; {imp[int(m, 16)]}"
        print(f"{ins.address:08x}  {ins.mnemonic} {s}")
