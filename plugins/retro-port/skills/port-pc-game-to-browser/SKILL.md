---
name: port-pc-game-to-browser
description: Port an old 32-bit Windows PC game (roughly 1999-2008, Direct3D 8 era) that the user owns so it runs in a web browser, by statically recompiling its executable to C and WebAssembly with a bundled Windows / Direct3D 8 / DirectSound / DirectInput layer, and optionally add online 1v1 play through deterministic lockstep netplay. Use this skill whenever someone wants to play an old Windows game in the browser, run an old PC game on a Mac or anywhere without Windows via the web, "port", "recompile" or "decompile" a game's .exe, bring a 2000s game back to life, or add online multiplayer to an old game with local multiplayer, even if they never mention recompilation or WebAssembly. Also use it to continue or debug a port started with it (a folder with port.json and recomp/).
---

# Port a PC game to the browser

You turn someone's own copy of an old Windows game into something they can play in a browser tab,
using a proven toolchain: a static x86-to-C recompiler, a reimplementation of the Windows calls the
game makes, Direct3D 8 translated to WebGPU, and an Emscripten build. It was built porting EA's
**Cricket 07** and **Rugby 06** (HB Studios) to the browser, where both now run with online 1v1. Everything in
`template/` is that toolchain with the game-specific parts moved into `port.json`.

The person asking is often not a programmer. They want to say "here's my game" and later play it.
Your job is to do the engineering, keep them informed in plain language, and only ask them for the
things only they can do (pointing at their game folder, trying the game, approving anything public).

## Before you start: set expectations

Tell the user, briefly and honestly:
- **It takes a long time.** A port is many hours of your work, often across several sessions, mostly
  spent implementing Windows features the game uses and fixing what doesn't render or behave right.
  Games close to Cricket 07 / Rugby 06 (same era, Direct3D 8, EA tech) go fastest.
- **It may not work out.** Some games use things the template doesn't have yet (Direct3D 9, DirectDraw,
  OpenGL, unusual threading, copy protection). The survey in step 1 tells you early; say so if it does.
- **It's for them.** The port uses their files and stays on their machine (or behind a password if
  they host it). It's not something to publish or share with people who don't own the game.

## Ground rules (why they matter)

- **Only the user's own, legitimately obtained copy.** The skill never downloads games or game files.
- **No copy-protection removal.** If the survey finds SecuROM, SafeDisc, StarForce, Steam DRM or a
  packer, stop and explain: bypassing protection is illegal in many countries even for a game you own,
  and this skill won't do it. Suggest an official DRM-free version (a GOG release, an official patch
  or a later edition) and survey that instead. Don't fabricate CD keys or serials either; registry
  values come from the user's own installation.
- **Never publish the game or anything derived from it**: `game/`, Ghidra projects, `functions.json`,
  generated C (`recomp/out_all/`), builds, `dist/`. The template's `.gitignore` keeps them out of git;
  keep it that way. The port's *own* code (translator, host layer, page) is fine to share.
- **Hosting only behind real access control** (a password), for the user and people who own the game.

## Requirements

The full workflow (native debugging build, Ghidra analysis) runs on **macOS** (Apple silicon or
Intel) with Homebrew. `scripts/setup_macos.sh` installs everything else. On Linux or Windows the
browser build and the headless Node build still work, but the native build (SDL2 + Metal + AudioToolbox)
would need porting first; say so and offer to continue in browser/Node-only mode
(`references/bringup.md`, "Without the native build").

Disk: the game size twice over is plenty (APFS clones the game folder for free), plus ~3 GB of tools.

## The workflow

Keep a `PORTING_LOG.md` in the port folder: what works, what you changed and why, what's next, and
the commands to reproduce problems. A port spans many sessions and this file is how the next one
picks up. Commit to the port's own git repository after each meaningful step (it ignores the game).

### 1. Find the game and survey it

Ask for the folder where the game is installed (or an extracted copy of it). If they only have a disc
image, help them install or extract it first; an installer-only folder isn't enough.

```bash
python3 <skill>/scripts/survey.py "<game folder>"
```

It needs no installs. For each .exe it reports architecture, copy protection or packing, the graphics
API, how many of its Windows calls the template already implements, and local-multiplayer hints, with
a verdict:
- **GO**: 32-bit, unprotected, Direct3D 8 (or no 3D). Continue.
- **HARD**: unprotected but Direct3D 9 / DirectDraw / OpenGL. Possible, but the graphics layer must be
  extended first (`references/bringup.md`, "Other graphics APIs"). Tell the user it's a much bigger job.
- **STOP**: protected, packed or 64-bit. Explain why (ground rules) and stop unless they find a clean edition.

Tell the user the verdict in a sentence or two, including whether online play is a possibility
(it needs the game's own local multiplayer: `references/netplay.md`).

### 2. Create the port and install the tools

```bash
python3 <skill>/scripts/new_port.py --game "<game folder>" --out ~/ports/<id> --name "<Game Name>"
sh <skill>/scripts/setup_macos.sh ~/ports/<id>
```

Before running setup, tell the user what it installs: Homebrew packages if missing (SDL2, uv, Node;
nothing already installed is upgraded), and inside the port folder Python packages, wgpu-native,
Emscripten, a JDK and Ghidra (~3 GB in all). Homebrew itself and the Xcode command line tools must
already exist or be installed by the user (both are one-step installs they approve).

`new_port.py` copies `template/`, clones the game into `game/`, picks the executable and writes
`port.json`. Read `port.json` and fill in what you can now: `guest_exe` (the executable's original
name, e.g. `Game.exe`), `files.include`/`required` if only some folders matter, `controls`. Registry
values come later, when the game asks for them (step 4 shows which keys it reads). Setup takes a while
(Ghidra download and a decompiler build); run it in the background.

### 3. Analyse the executable (once)

```bash
cd ~/ports/<id> && sh re/analyze.sh
```

Ghidra finds every function and switch table and exports `recomp/data/functions.json`, which the
translator needs. A few minutes. `re/decomp.sh`, `re/decomp_at.sh`, `re/xrefs.sh` and
`re/decomp_in.sh` later show decompiled code for any function or address, which is how you understand
what the game is doing when it misbehaves.

### 4. Bring it up natively

```bash
sh recomp/build.sh            # translate + compile; incremental after the first run
recomp/build/game game        # window; PORT_LOG=1 / PORT_LOG_IMPORTS=1 for what the game does
```

This is where most of the work is. The loop: run, see where it stops (a trap naming an unimplemented
Windows function, a crash with a stack dump, a hang, wrong rendering), fix it in `recomp/host/` (or the
translator for an unsupported instruction), rebuild, repeat. Work through it in this order of goals:
boots to the first screen, menus navigate, a game session starts, it plays properly, sound works,
controllers work. Read `references/bringup.md` before starting this phase; it has the playbook for each
kind of failure, the debugging tools, and the traps found in the two games ported so far.

Use scripted input (`PORT_INPUT`) and screenshots (`PORT_SCREENSHOTS`) to check progress yourself
instead of asking the user to test every change. When it's playable, ask the user to try it natively
if they'd like; their feedback on controls and feel is worth having.

### 5. Build for the browser and hand it over

```bash
. third_party/emsdk/emsdk_env.sh && sh recomp/build_web.sh
./play.sh                     # local server + opens the page
```

Read `references/web.md` first: browser memory limits, threads and the headers they need, storage of
the game files, WebGPU differences between browsers. Check the page yourself in the browser pane or a
headless Node run (`node recomp/build_web/node/game.js game`) before telling the user it's ready.

**Done** means: the user runs `./play.sh` (or opens their hosted URL), the launcher shows their game,
**Play Solo** starts it, and it plays like the native build. Tell them exactly that command, which
browsers work (current Chrome, Edge or Safari; Firefox depends on the GPU), the controls, and
Alt+Enter for fullscreen.

### 6. Optional: online 1v1

Only if the game has local multiplayer (two controllers on one PC) and the user wants it. Online play
works by making player 2's controller live on another computer, with both machines running the same
deterministic simulation.

1. **Determinism first** (`references/determinism.md`): two side-by-side runs with the same input must
   stay identical (`scripts/det_pair.sh` + `scripts/compare_hashes.py`). Expect a game-specific spin loop
   or two to fix; the reference lists the ones found so far and how to find new ones.
2. **Netplay** (`references/netplay.md`): set `"online": true`, rebuild, test two headless players in a
   local room (`scripts/netplay_pair.sh`), then two browser tabs.
3. **Hosting** (`references/hosting.md`): the user's own Cloudflare account (free tier), password
   protected, deployed with their approval. Online play between two homes needs a hosted copy.

Publishing anything is the user's decision: confirm before deploying, and never deploy without a password.

## Talking to the user

- Report progress in plain words: "it now reaches the main menu", "sound works, controllers next",
  not instruction-level detail. Offer to explain more if they're curious.
- When something will take a long time or might not be solvable, say so early and offer options.
- At the end of each session, update `PORTING_LOG.md` and tell them where things stand and how to play
  what works so far.

## Reference guides

Read the one for the phase you're in; they're long because they hold the hard-won details.

| File | Read when |
|---|---|
| `references/architecture.md` | First time in a port: how translator, runtime, host layer and page fit together; every `PORT_*` switch |
| `references/bringup.md` | Phase 4: failure playbook, debugging tools, Windows API patterns, graphics/audio/input, known gaps |
| `references/web.md` | Phase 5: Emscripten build, memory layout, storage, browser quirks, packaging |
| `references/determinism.md` | Phase 6: virtual clock, scheduler, preemption, checks, spin-loop traps |
| `references/netplay.md` | Phase 6: lockstep design, inputs, desync detection, transport, testing |
| `references/hosting.md` | Phase 6 or private hosting: Cloudflare Worker + R2, password, uploads, TURN |

## Scripts

| Script | What |
|---|---|
| `scripts/survey.py` | Go/no-go check of an executable (no installs needed) |
| `scripts/new_port.py` | New port folder from the template with the user's game files |
| `scripts/setup_macos.sh` | All tools, into the port folder (idempotent) |
| `scripts/det_pair.sh`, `scripts/compare_hashes.py` | Determinism check and its comparison |
| `scripts/netplay_pair.sh` | Two headless online players in one room |
| `scripts/disasm.py` | Disassemble around addresses from stack dumps or profiles |
