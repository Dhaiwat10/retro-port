"""Which files of the game's install the browser build needs, and the manifest the page reads.

The files come from port.json: the executable plus everything under files.include (the whole folder
when that list is empty); files.required are checked for so a wrong folder is caught early.
The page (port.js) downloads every file in manifest.json into the browser's private storage on the
first visit; `version` changes whenever a file's size or name changes, which makes the page re-check.
"""
import hashlib
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CFG = json.load(open(os.path.join(ROOT, "port.json")))
EXE = CFG["exe"]
INCLUDE = [p.lower() for p in CFG.get("files", {}).get("include", [])]
REQUIRED = CFG.get("files", {}).get("required", [])
# never needed in the browser: installers, uninstallers, setup leftovers
SKIP = ("uninst", "setup.exe", "autorun.inf", "unins000", "directx/", "redist/", "_setup/")


def game_files(root):
    """published path -> source path."""
    out = {}
    for base, _, names in os.walk(root):
        for n in names:
            full = os.path.join(base, n)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            low = rel.lower()
            if low == EXE.lower():
                out[EXE] = full
            elif any(s in low for s in SKIP) or n.startswith("."):
                continue
            elif not INCLUDE or any(low.startswith(p) for p in INCLUDE):
                out[rel] = full
    if EXE not in out:
        raise SystemExit(f"{root}: no {EXE} (port.json 'exe')")
    for r in REQUIRED:
        if not any(k.lower() == r.lower() for k in out):
            raise SystemExit(f"{root}: no {r} (port.json files.required)")
    return out


def manifest_for(files):
    entries = sorted(({"path": p, "size": os.path.getsize(src)} for p, src in files.items()), key=lambda e: e["path"])
    version = hashlib.sha1("".join(f'{e["path"]}:{e["size"]};' for e in entries).encode()).hexdigest()[:12]
    return {"version": version, "files": entries}
