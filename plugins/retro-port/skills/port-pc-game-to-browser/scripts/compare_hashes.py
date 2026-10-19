#!/usr/bin/env python3
"""Compares two PORT_HASH_LOG files from runs that should be identical.

    python3 compare_hashes.py a.hash b.hash

Prints how many frames were compared, how many differ and the first differing frame (globals every
frame; heap + VirtualAlloc memory at the PORT_HASH_ALL checkpoints). Exit status 1 if they differ.
"""
import sys


def load(p):
    g, m = {}, {}
    for line in open(p):
        f = line.split()
        if len(f) > 5 and f[1] == "heap":
            m[int(f[0])] = (f[2], f[5])
        elif len(f) >= 2:
            g[int(f[0])] = line.split(" t=")[0]
    return g, m


(ga, ma), (gb, mb) = load(sys.argv[1]), load(sys.argv[2])
common = sorted(set(ga) & set(gb))
diff = [f for f in common if ga[f] != gb[f]]
mc = sorted(set(ma) & set(mb))
mdiff = [f for f in mc if ma[f] != mb[f]]
print(f"frames compared: {len(common)} (1..{common[-1] if common else 0}); globals differ: {len(diff)}"
      + (f", first at frame {diff[0]}" if diff else ""))
print(f"heap+VirtualAlloc checkpoints: {len(mc)}; differ: {len(mdiff)}" + (f", first at frame {mdiff[0]}" if mdiff else ""))
if diff:
    print(" a:", ga[diff[0]])
    print(" b:", gb[diff[0]])
sys.exit(1 if diff or mdiff or not common else 0)
