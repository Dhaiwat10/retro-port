#!/usr/bin/env python3
"""Assembles a folder you can host privately: the page, the WebAssembly build and your game files.

    python3 recomp/web/make_dist.py --game game [--out dist]

Upload the resulting folder to any static host. On the first visit from a device the page downloads
the game files (~1.1 GB) into that browser's private storage; later visits start straight away.

The folder contains EA's game files and code derived from them. Keep it behind real access control
(password / login), not just an unguessable URL, and never commit it.

Hosting needs two response headers on every file, or threads (SharedArrayBuffer) are disabled:
    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
A `_headers` file for Cloudflare Pages / Netlify is included; for nginx or Caddy add them yourself.
HTTP range requests (supported by most hosts) let an interrupted download resume mid-file.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from gamefiles import game_files, manifest_for  # noqa: E402

BUILD = os.path.join(os.path.dirname(HERE), "build_web", "www")
PAGE = ["index.html", "port.js", "port-config.js", "netplay.js", "audio-worklet.js"]

HEADERS = """/*
  Cross-Origin-Opener-Policy: same-origin
  Cross-Origin-Embedder-Policy: require-corp
  Cross-Origin-Resource-Policy: same-origin
  X-Robots-Tag: noindex

/build/*
  Cache-Control: no-cache

/game/*
  Cache-Control: no-cache
"""


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.exists(dst) and os.path.getsize(dst) == os.path.getsize(src) and \
            os.path.getmtime(dst) >= os.path.getmtime(src):
        return
    try:   # APFS clone: instant, no extra disk space
        subprocess.run(["cp", "-c", src, dst], check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="your own copy of the game's folder")
    ap.add_argument("--out", default="dist")
    a = ap.parse_args()
    for f in ("game.js", "game.wasm"):
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit(f"missing {BUILD}/{f}: run recomp/build_web.sh first")
    out = os.path.abspath(a.out)
    for f in PAGE:
        copy(os.path.join(HERE, f), os.path.join(out, f))
    for f in ("game.js", "game.wasm"):
        copy(os.path.join(BUILD, f), os.path.join(out, "build", f))
    files = game_files(os.path.abspath(a.game))
    for rel, src in files.items():
        copy(src, os.path.join(out, "game", rel))
    manifest = manifest_for(files)
    with open(os.path.join(out, "game", "manifest.json"), "w") as fh:
        json.dump(manifest, fh)
    with open(os.path.join(out, "_headers"), "w") as fh:
        fh.write(HEADERS)
    with open(os.path.join(out, "robots.txt"), "w") as fh:
        fh.write("User-agent: *\nDisallow: /\n")
    total = sum(e["size"] for e in manifest["files"])
    print(f"{out}: page + build + {len(files)} game files ({total / 1e6:.0f} MB), manifest version {manifest['version']}")


if __name__ == "__main__":
    main()
