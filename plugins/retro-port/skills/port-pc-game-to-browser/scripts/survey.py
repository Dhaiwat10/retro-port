#!/usr/bin/env python3
"""First look at a game before porting it: is this executable something the template can take on?

    python3 survey.py <game-folder-or-exe> [--template <skill>/template] [--json]

No dependencies (plain Python 3). Reports, for every Windows .exe in the folder (or the one given):
  - architecture: the recompiler handles 32-bit x86 (PE32) only
  - copy protection / packers: known section and import signatures, plus code entropy. A protected
    executable can't be ported with this skill (and removing protection is illegal in many places)
  - graphics API: which Direct3D/DirectDraw/OpenGL DLL it imports (the template speaks Direct3D 8)
  - Windows calls: how many imports the template's host layer already implements, and which not
  - multiplayer hints: strings suggesting local 2-player support (needed for online play)
and a verdict line per executable.
"""
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TEMPLATE = os.path.join(os.path.dirname(HERE), "template")

PROTECTION_SECTIONS = {
    ".securom": "SecuROM", ".cms_t": "SecuROM 7", ".cms_d": "SecuROM 7", "stxt774": "SafeDisc", "stxt371": "SafeDisc",
    ".txt2": "SafeDisc (possible)", ".sforce": "StarForce", ".sforce3": "StarForce", ".tages": "TAGES",
    "upx0": "UPX packer", "upx1": "UPX packer", ".aspack": "ASPack packer", ".adata": "ASPack/ASProtect",
    ".themida": "Themida", ".winlice": "WinLicense", ".vmp0": "VMProtect", ".vmp1": "VMProtect",
    ".bind": "Steam DRM (SteamStub)", ".enigma1": "Enigma Protector", ".petite": "Petite packer",
    "pec2": "PECompact", "pec1": "PECompact", ".nsp0": "NsPack", ".mpress1": "MPRESS",
}
PROTECTION_IMPORTS = {
    "drm.dll": "SecuROM", "dplayerx.dll": "SafeDisc", "protect.dll": "StarForce", "steam_api.dll": "Steam (API, often with DRM)",
    "tages.dll": "TAGES", "lgcore.dll": "LaserLock",
}
GRAPHICS = [("d3d8.dll", "Direct3D 8", "supported by the template"),
            ("d3d9.dll", "Direct3D 9", "not yet: the host layer needs a Direct3D 9 implementation"),
            ("d3d10.dll", "Direct3D 10", "not supported"), ("d3d11.dll", "Direct3D 11", "not supported"),
            ("ddraw.dll", "DirectDraw / Direct3D 7 or earlier", "not yet: needs a DirectDraw/D3D7 layer"),
            ("opengl32.dll", "OpenGL", "not yet: needs an OpenGL layer"),
            ("glide2x.dll", "3dfx Glide", "not supported")]
MULTI = re.compile(rb"(player ?2|2 ?players|two players|multi ?player|split ?screen|hot ?seat|"
                   rb"controller ?select|versus|head ?to ?head|1 ?v ?1|vs\.? cpu)", re.I)


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]


def entropy(data):
    if not data:
        return 0.0
    counts = [0] * 256
    for x in data:
        counts[x] += 1
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def parse_pe(path):
    b = open(path, "rb").read()
    if b[:2] != b"MZ":
        return None
    pe = u32(b, 0x3C)
    if b[pe:pe + 4] != b"PE\0\0":
        return None
    machine, nsec = u16(b, pe + 4), u16(b, pe + 6)
    optsz = u16(b, pe + 20)
    opt = pe + 24
    magic = u16(b, opt)
    info = {"path": path, "size": len(b), "machine": machine, "pe32plus": magic == 0x20B, "sections": [], "imports": {}}
    if magic != 0x10B:
        return info
    info["image_base"] = u32(b, opt + 28)
    imp_rva = u32(b, opt + 96 + 8)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        s = so + 40 * i
        name = b[s:s + 8].split(b"\0")[0].decode("latin-1")
        vsize, va, rawsz, rawptr, flags = u32(b, s + 8), u32(b, s + 12), u32(b, s + 16), u32(b, s + 20), u32(b, s + 36)
        secs.append({"name": name, "va": va, "vsize": vsize, "rawptr": rawptr, "rawsz": rawsz, "flags": flags,
                     "entropy": round(entropy(b[rawptr:rawptr + min(rawsz, 4 << 20)]), 2)})
    info["sections"] = secs

    def off(rva):
        for s in secs:
            if s["va"] <= rva < s["va"] + max(s["vsize"], s["rawsz"]):
                return s["rawptr"] + rva - s["va"]
        return None

    def cstr(o):
        e = b.index(b"\0", o)
        return b[o:e].decode("latin-1")

    d = off(imp_rva) if imp_rva else None
    while d is not None and d + 20 <= len(b) and u32(b, d + 12):
        dll = cstr(off(u32(b, d + 12))).lower()
        ilt = u32(b, d) or u32(b, d + 16)
        names = []
        t = off(ilt)
        while t is not None and t + 4 <= len(b) and u32(b, t):
            v = u32(b, t)
            names.append(f"#{v & 0xFFFF}" if v & 0x80000000 else cstr(off(v) + 2))
            t += 4
        info["imports"].setdefault(dll, []).extend(names)
        d += 20
    info["multiplayer_hints"] = sorted({m.decode("latin-1").lower() for m in MULTI.findall(b)})[:12]
    return info


def implemented(template):
    names = set()
    host = os.path.join(template, "recomp", "host")
    for f in os.listdir(host) if os.path.isdir(host) else []:
        if f.endswith(".c"):
            src = open(os.path.join(host, f), errors="replace").read()
            names.update(re.findall(r"^W\((\w+)\)", src, re.M))             # W(GetTickCount) { ... }
            names.update(re.findall(r"^void w_(\w+)\(CPU", src, re.M))      # void w_Direct3DCreate8(CPU *c)
            names.update(re.findall(r'"[A-Z0-9_]+\.DLL!(\w+)"', src))       # { "D3D8.DLL!Direct3DCreate8", ... }
    return names


def judge(info, impl):
    out = {"exe": info["path"]}
    if info["machine"] != 0x14C or info["pe32plus"]:
        out["verdict"] = "NO: not a 32-bit x86 Windows executable (the recompiler handles PE32/i386 only)"
        return out
    prot = []
    for s in info["sections"]:
        hit = PROTECTION_SECTIONS.get(s["name"].lower())
        if hit:
            prot.append(f"{hit} (section {s['name']})")
    for dll in info["imports"]:
        if dll in PROTECTION_IMPORTS:
            prot.append(f"{PROTECTION_IMPORTS[dll]} (imports {dll})")
    code = [s for s in info["sections"] if s["flags"] & 0x20000000]
    if code and max(s["entropy"] for s in code) > 7.3:
        prot.append(f"packed or encrypted code (entropy {max(s['entropy'] for s in code)})")
    nimp = sum(len(v) for v in info["imports"].values())
    if nimp < 15:
        prot.append(f"very few imports ({nimp}): typical of a packed executable")
    out["protection"] = prot
    gfx = [(api, note) for dll, api, note in GRAPHICS if dll in info["imports"]]
    out["graphics"] = [f"{api}: {note}" for api, note in gfx] or ["no Direct3D/OpenGL import found (software rendering, or loaded at run time)"]
    allnames = [(dll, n) for dll, ns in info["imports"].items() for n in ns]
    missing = sorted(f"{dll}!{n}" for dll, n in allnames if not n.startswith("#") and n not in impl)
    out["imports"] = {"total": len(allnames), "implemented": len(allnames) - len(missing), "missing": missing}
    out["dlls"] = sorted(info["imports"])
    out["multiplayer_hints"] = info["multiplayer_hints"]
    if prot:
        out["verdict"] = ("STOP: this executable looks protected or packed. The skill can't port it, and removing copy "
                          "protection is illegal in many countries. Look for an official DRM-free release or patch.")
    elif gfx and gfx[0][0] != "Direct3D 8":
        out["verdict"] = f"HARD: uses {gfx[0][0]}; the template only speaks Direct3D 8 today ({gfx[0][1]})"
    else:
        out["verdict"] = (f"GO: 32-bit, no known protection, {len(missing)} Windows functions still to implement"
                          + ("" if out["multiplayer_hints"] else "; no local-multiplayer strings found, so plan for solo play"))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        sys.exit(__doc__)
    template = DEFAULT_TEMPLATE
    if "--template" in sys.argv:
        template = sys.argv[sys.argv.index("--template") + 1]
        args = [a for a in args if a != template]
    target = args[0]
    exes = [target] if os.path.isfile(target) else [
        os.path.join(b, n) for b, _, ns in os.walk(target) for n in ns if n.lower().endswith(".exe")]
    impl = implemented(template)
    results = []
    for e in sorted(exes, key=lambda p: -os.path.getsize(p)):
        info = parse_pe(e)
        if info and not any(k in os.path.basename(e).lower() for k in ("unins", "setup", "dxsetup", "vcredist")):
            results.append(judge(info, impl))
    if "--json" in sys.argv:
        print(json.dumps(results, indent=2))
        return
    for r in results:
        print(f"== {os.path.relpath(r['exe'], target) if os.path.isdir(target) else r['exe']}")
        print(f"   verdict: {r['verdict']}")
        for k in ("protection", "graphics", "dlls", "multiplayer_hints"):
            if r.get(k):
                print(f"   {k}: {', '.join(r[k])}")
        if "imports" in r:
            i = r["imports"]
            print(f"   windows calls: {i['implemented']}/{i['total']} already implemented")
            if i["missing"]:
                print("   missing: " + ", ".join(i["missing"][:40]) + (" ..." if len(i["missing"]) > 40 else ""))
    if not results:
        print("no Windows executables found")


if __name__ == "__main__":
    main()
