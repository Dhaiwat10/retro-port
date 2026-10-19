#!/usr/bin/env python3
"""Starts a new port: copies the template, brings in the user's own game files, writes port.json.

    python3 new_port.py --game "<folder with the installed game>" --out <new port folder> --name "Game Name"
                        [--exe <path of the executable inside the game folder>] [--id slug]

- The template goes to <out> (which must not exist yet, or be empty) and becomes its own git repository.
- The game folder is copied to <out>/game. On APFS the copy is a clone: instant, no extra disk space.
  The .gitignore keeps game/ and everything derived from it out of git.
- Without --exe, the largest 32-bit executable that survey.py finds no protection on is chosen.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(os.path.dirname(HERE), "template")
sys.path.insert(0, HERE)
import survey  # noqa: E402


def pick_exe(game):
    impl = survey.implemented(TEMPLATE)
    best = None
    for base, _, names in os.walk(game):
        for n in names:
            if not n.lower().endswith(".exe") or any(k in n.lower() for k in ("unins", "setup", "dxsetup", "vcredist")):
                continue
            p = os.path.join(base, n)
            info = survey.parse_pe(p)
            if not info:
                continue
            r = survey.judge(info, impl)
            if r["verdict"].startswith(("GO", "HARD")) and (not best or os.path.getsize(p) > os.path.getsize(best)):
                best = p
    return best


def clone_tree(src, dst):
    try:
        subprocess.run(["cp", "-Rc", src, dst], check=True, capture_output=True)   # APFS clone
    except (subprocess.CalledProcessError, FileNotFoundError):
        shutil.copytree(src, dst, symlinks=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--exe")
    ap.add_argument("--id")
    a = ap.parse_args()
    game = os.path.abspath(os.path.expanduser(a.game))
    out = os.path.abspath(os.path.expanduser(a.out))
    if not os.path.isdir(game):
        sys.exit(f"{game} is not a folder")
    if os.path.exists(out) and os.listdir(out):
        sys.exit(f"{out} already exists and is not empty")
    exe = a.exe
    if exe:
        if not os.path.isfile(os.path.join(game, exe)):
            sys.exit(f"{exe} is not in {game}")
    else:
        found = pick_exe(game)
        if not found:
            sys.exit("no usable executable: run survey.py on the folder to see why (protected, 64-bit, ...)")
        exe = os.path.relpath(found, game)
    exe = exe.replace(os.sep, "/")
    slug = a.id or re.sub(r"[^a-z0-9]+", "-", a.name.lower()).strip("-")

    shutil.copytree(TEMPLATE, out, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "port_config.h", "port-config.js"))
    clone_tree(game, os.path.join(out, "game"))
    cfg = json.load(open(os.path.join(TEMPLATE, "port.example.json")))
    cfg.update({"name": a.name, "id": slug, "exe": exe, "guest_exe": os.path.basename(exe),
                "guest_root": "C:\\Games\\" + re.sub(r"[^A-Za-z0-9]+", "", a.name), "volume_label": slug.upper().replace("-", "")[:11],
                "registry": [], "online": False})
    with open(os.path.join(out, "port.json"), "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    subprocess.run(["git", "init", "-q"], cwd=out, check=False)
    subprocess.run(["git", "add", "-A"], cwd=out, check=False)
    subprocess.run(["git", "commit", "-q", "-m", f"Start the {a.name} port from the template"], cwd=out, check=False)
    print(f"port folder: {out}")
    print(f"executable:  game/{exe}")
    print("next: setup_macos.sh, then re/analyze.sh, then recomp/build.sh (see the skill's SKILL.md)")


if __name__ == "__main__":
    main()
