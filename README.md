# retro-port

A Claude Code skill that ports old Windows PC games **you own** to the browser.

Tell Claude which game you want to play, point it at your installed copy, and it does the rest:
checks whether the game can be ported, sets up the tools, recompiles the game's executable for the
web, gets it running and hands you one command to play it in Chrome, Edge or Safari. If the game has
local multiplayer, it can also add **online 1v1**.

> "I have EA Cricket 07 installed in ~/Games/Cricket07. Can you make it run in my browser?"

It was built while porting **EA Sports Cricket 07** and **Rugby 06** to the browser;
both now run there with online 1v1 built in. The skill packages that toolchain and everything learned along the way.

## How it works

Nothing is emulated. The game's x86 machine code is **statically recompiled** into C (one C function
per original function), then compiled to WebAssembly. The Windows functions the game calls, including
Direct3D 8, DirectSound and DirectInput, are reimplemented, with Direct3D 8 translated to WebGPU. The
game's own code runs unchanged on top. For online play, the game runs in a **deterministic** mode (a
virtual clock and a fixed thread schedule), so two players who exchange only controller inputs compute
exactly the same match.

## What it can port

Best fit: **32-bit Windows games from about 1999 to 2008 that use Direct3D 8**, without copy
protection. The skill's survey tells you within seconds whether a game qualifies:

- **Go:** 32-bit, unprotected, Direct3D 8 (or no 3D)
- **Hard:** Direct3D 9, DirectDraw or OpenGL. Possible, but a large extension of the graphics layer comes first.
- **Stop:** copy-protected or packed (SecuROM, SafeDisc, StarForce, Steam DRM, ...) or 64-bit

Porting a new game is still real engineering: expect many hours of Claude's work across several
sessions, and the possibility that a game needs something the toolchain doesn't have yet.

## Install

In Claude Code:

```
/plugin marketplace add Dhaiwat10/retro-port
/plugin install retro-port@retro-port
```

Or copy `plugins/retro-port/skills/port-pc-game-to-browser` into `~/.claude/skills/`.

**Requirements:** macOS (Apple silicon or Intel) with [Homebrew](https://brew.sh) and the Xcode command
line tools. The skill installs everything else (Python packages, wgpu-native, Emscripten, a JDK and
Ghidra) inside the port's folder, about 3 GB in all. Other systems can build for the browser, but the
native debugging build is macOS-only for now.

## Legal

- Use it only with a copy of the game you own.
- The skill **does not remove copy protection**, and stops when it finds any: circumventing it is
  illegal in many countries even for games you own. Look for an official DRM-free edition instead.
- A port contains the game. Keep it private: the port's `.gitignore` keeps game files, analysis and
  builds out of git. Host it only behind a password, for yourself and people who own the game.
- This repository contains only original code: no game files, no code derived from any game.

## What's inside

```
plugins/retro-port/skills/port-pc-game-to-browser/
  SKILL.md          the workflow Claude follows
  references/       deep guides: architecture, bring-up, browser build, determinism, netplay, hosting
  scripts/          survey, new port, macOS setup, determinism and netplay tests, disassembler
  template/         the toolchain: x86→C translator, runtime, Windows layer, WebGPU renderer, page
```

## License

MIT (see `LICENSE`). Games you port remain their publishers' property.

## Credits

Built by [Dhaiwat10](https://github.com/Dhaiwat10) from the Cricket 07 browser port, and by
[Stephan Cilliers](https://github.com/stephancill) from the Rugby 06 port
([rugby06-web](https://github.com/stephancill/rugby06-web)), which contributed the deterministic mode,
lockstep netplay and many fixes. Developed with Claude.

Uses [Ghidra](https://github.com/NationalSecurityAgency/ghidra), [Capstone](https://www.capstone-engine.org),
[wgpu-native](https://github.com/gfx-rs/wgpu-native), [Emscripten](https://emscripten.org) and
[SDL2](https://www.libsdl.org), installed separately under their own licenses.
