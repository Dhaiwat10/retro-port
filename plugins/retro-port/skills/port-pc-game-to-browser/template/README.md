# Browser port

A port of a 2000s Windows PC game to the browser, made with the `port-pc-game-to-browser` skill.
The game is statically recompiled from its x86 executable to C, compiled natively (for debugging) and
to WebAssembly (to play), with the Windows, Direct3D 8, DirectSound and DirectInput calls it makes
reimplemented in `recomp/host/`.

**Play:** `./play.sh`, then use the page that opens (Chrome, Edge or Safari).

**This folder holds your copy of a commercial game** (`game/`) and files derived from it. Keep it
private: never publish it, and never put the `game/`, `re/ghidra/`, `recomp/data/`, `recomp/out_all/`,
`recomp/build*` or `dist/` folders anywhere public. The `.gitignore` keeps them out of git.

| Path | What |
|---|---|
| `port.json` | This game's settings (name, executable, registry values, files, controller, online) |
| `game/` | Your game files |
| `re/` | Ghidra analysis: `re/analyze.sh` exports `recomp/data/functions.json` |
| `recomp/translator/x86toc.py` | x86 → C translator |
| `recomp/runtime/` | CPU state, memory, x87/SSE helpers, interpreter for run-time code |
| `recomp/host/` | Windows API layer, Direct3D 8 → WebGPU, audio, input, determinism, netplay |
| `recomp/web/` | The page, browser storage, local server, packaging, Cloudflare hosting |
| `tools/` | Configure step, executable survey and asset tools |

Build: `recomp/build.sh` (native, macOS: `recomp/build/game game`), then `recomp/build_web.sh` (browser).
Useful run options are listed at the top of `recomp/host/main.c` and in the skill's reference guides.
