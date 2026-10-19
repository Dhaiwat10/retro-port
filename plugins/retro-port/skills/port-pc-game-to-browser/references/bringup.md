# Bringing a game up natively

Contents: the loop · reading a failure · unimplemented Windows functions · COM interfaces · crashes ·
hangs and slowness · early exits · rendering · audio · input · tools · known gaps · other graphics
APIs · without the native build · what the first two games needed

## The loop

```bash
sh recomp/build.sh                         # first run translates everything (~1-2 min), then incremental
PORT_LOG=1 recomp/build/game game 2>&1 | tee run.log
```

Fix one thing, rebuild, rerun. `HOST_ONLY`-style shortcuts aren't needed: `build.sh` only recompiles
generated files whose content changed, and host files are fast. Milestones, in order:

1. Reaches `WinMain` and creates its window and Direct3D device (`PORT_LOG_D3D=1` shows it).
2. Shows the first screen (logos, intro video). Use `PORT_SCREENSHOTS=300,600,900` to look without
   watching.
3. Menus work with the keyboard and a controller (`PORT_INPUT` to script it).
4. A game session starts (loading, then gameplay rendered).
5. Plays correctly: physics, AI, timing, saves.
6. Sound (music, effects, speech), then controllers in gameplay, then options/saves/profiles.

Record each milestone and the commands that reproduce problems in `PORTING_LOG.md`.

## Reading a failure

The game stops in one of four ways. Identify which before changing anything:

| What you see | Meaning | Go to |
|---|---|---|
| `unimplemented import KERNEL32.DLL!Foo` | The game called a Windows function the layer lacks | Unimplemented Windows functions |
| `trap: ...` with registers and `stack:` addresses | Bad memory access, unsupported instruction, or a handler returned garbage earlier | Crashes |
| Nothing happens, CPU busy or idle | Hang: deadlock, a wait that never ends, or a spin loop | Hangs and slowness |
| `ExitProcess(...)` with a stack and strings, or a `[MessageBox]` line | The game decided to quit (missing file, failed check) | Early exits |

Addresses in stack dumps are guest return addresses. Turn them into understanding with:

```bash
.venv/bin/python -I <skill>/scripts/disasm.py game/<exe> 5d43c8:40   # instructions there
sh re/decomp_in.sh 5d43c8                                           # Ghidra's C for the containing function
sh re/xrefs.sh 9f33fc                                               # who reads/writes an address
```

`re/assert_names.csv` names functions after the source files in assert strings (e.g.
`VideoPlayer_cpp_142`), and `re/rtti_classes.csv` gives C++ class names and vtables: both are often
the fastest way to know which subsystem you're in.

## Unimplemented Windows functions

1. Look up the function's documented behaviour (Microsoft's Win32 docs). Note the argument count
   (stdcall pops them: `RET(nargs, value)`), what it writes through pointers, and the return value.
2. Implement the smallest *faithful* behaviour: correct outputs and return values for what this game
   needs, logged under `PORT_LOG` so the next surprise is visible. A stub that returns success but
   leaves an output buffer uninitialised causes crashes much later that are hard to trace back.
3. Put it with its family (`kernel32.c`, `user32.c`, `misc.c` for gdi/advapi/winmm, the DirectX files)
   and register it in that DLL's table. Imports by ordinal (DSOUND `#1`) are registered as
   `"DSOUND.DLL!#1"`.
4. When a game probes for optional features (`GetProcAddress` for newer functions), returning 0 is
   usually right: the game falls back.

Common families games of this era need beyond what's there: `GetFileVersionInfo*` (version.dll),
`CoInitialize`/`CoCreateInstance` (ole32: often for DirectShow video or DirectMusic, frequently best
answered with failure so the game skips the feature), `timeBeginPeriod`, `GlobalMemoryStatus`,
`GetDiskFreeSpace`, `GetVersionEx` (report Windows XP), `RaiseException` (C++ exceptions: not
implemented yet; needs SEH unwinding through the guest's exception registration chain).

## COM interfaces (DirectX objects)

`com.h` makes guest-visible COM objects whose vtable slots dispatch to host handlers:

```c
static const ComMethod device_methods[] = {
    { "QueryInterface", 2, com_QueryInterface }, { "AddRef", 0, com_AddRef }, { "Release", 0, com_Release },
    { "GetCaps", 1, dev_GetCaps }, ...   /* in vtable order; nargs excludes `this` */
};
```

Methods with `impl = NULL` log once and return S_OK, which is how you find which ones a game uses
(`PORT_LOG_D3D=1`). Vtable order must match the real interface exactly (use the DirectX SDK headers'
order). `THIS` is the object; `com_data(THIS)` is its host state.

## Crashes

- **Unsupported instruction** trap: names the instruction. Add it to `x86toc.py` (look at how a
  similar instruction is emitted), or, if it's MMX/SSE2-integer reached through CPU dispatch, check why
  the game chose that path (CPUID reports no SIMD on purpose).
- **Memory fault at a small address** (NULL + offset): usually a handler that returned success without
  filling in an object or output pointer. Find the last Windows calls before the crash with
  `PORT_LOG_IMPORTS=1 ... | tail -50`.
- **Fault inside a vtable call**: a COM method in the wrong slot, or the game calling an interface you
  returned for an unknown IID from QueryInterface. Return `E_NOINTERFACE` for IIDs you don't implement.
- **Fault in code created at run time**: the game generated or loaded x86 code and jumped to it.
  `runtime/interp.c` interprets such regions once registered (`dyncode_register`, done automatically
  when memory is made executable); extend the interpreter's instruction set if it traps there.
- Natively, `lldb recomp/build/game -- game` works: recompiled functions are named `f_<address>`, so
  a native backtrace maps straight back to guest functions.

## Hangs and slowness

Look before guessing:
- `PORT_WATCHDOG=5` prints every thread's guest return addresses every 5 s: the game thread's top
  frames tell you where it's stuck.
- `sample <pid> 3 -file s.txt` (macOS) shows host stacks: is a thread waiting in `pthread_cond_wait`
  (a Windows wait or critical section), in guest code (`f_00xxxxxx`), or in the GPU?
- `PORT_PROFILE_WAITS=1` prints the busiest Sleep/Wait call sites: a polling loop shows up as one call
  site with a huge count. Disassemble its return addresses.

Typical causes:
- **A wait that's never satisfied**: an event the game expects some Windows facility to signal
  (overlapped I/O completion, a timer, a window message, a DirectSound notification position).
  Find what normally signals it on Windows and do that.
- **Starvation**: a thread that locks and unlocks in a tight loop never lets a waiter in.
  `ReleaseMutex` hands the mutex to a waiting thread for this reason (EA sound code needed it).
- **Spin loops on the clock or on memory**: harmless natively, but they matter for determinism
  (`determinism.md`).
- **Slow**: `PORT_LOG_FPS=1`; profile with `sample` or Instruments. Common costs: per-draw GPU object
  creation (cache them; the bind-group cache in `render_wgpu.c` exists because of this), texture
  conversion every frame (games that recreate textures every frame need freed textures destroyed
  promptly), and huge logs (turn logging off).

## Early exits

The game decided to quit. The `ExitProcess` dump prints strings still on the stack, which often name
the reason. Then:
- **Missing file**: `PORT_LOG=1` logs every file open; the path tells you whether it's a mapping
  problem (wrong `guest_root`, a CD path on D:, a file in "My Documents") or a real missing file.
- **Registry**: `PORT_LOG=1` logs every registry query. Add the values from the user's own install to
  `port.json`. Install paths should point at `guest_root`. Many DirectX 8 games read
  `HKLM\SOFTWARE\Microsoft\DirectX\Version` (DirectX 8.1 = `4.08.01.0810`) and refuse to run without it.
- **Disc check**: the game looks for its CD on D:. If it only checks the volume label, set
  `volume_label` to the real disc's label. If it reads protected sectors or demands the disc's files,
  that's copy protection: stop (SKILL.md ground rules).
- **Hardware checks**: video memory, card names, CPU speed, free disk space. Report plausible
  mid-2000s values (the virtual registry already reports a 2 GHz CPU so games skip speed calibration).

## Rendering

- `PORT_CAPTURE=N` logs every draw of frame N and saves the picture after each one to `/tmp/port_cap/`:
  the draw where it goes wrong points at the render state, texture or shader involved.
- Vertex shaders (vs.1.1) run on the CPU (`vshader.c`); pixel shaders (ps.1.x) become WGSL
  (`pshader.c`): `PORT_DUMP_VS=1` / `PORT_DUMP_PS=1` print them.
- Render targets and readbacks work (a game rendering a texture and reading it back stalls until the
  copy is mapped).
- Not implemented yet: **fixed-function lighting, fog, cube-map pixel-shader ops, GDI text** (games
  that draw text with GDI onto surfaces show white boxes). Games that rely on them need them added in
  `render_rec.c` / `render_wgpu.c`. Fixed-function lighting is the most likely to be needed next.

## Audio

`dsound.c` implements DirectSound buffers, a mixer thread, streaming (position notifications) and
capture stubs. If music plays but effects don't (or the reverse), log buffer creation and Play calls
(`PORT_LOG=1`). EA's AEMS uses x86 modules inside sound banks: that's what `interp.c` is for. Other
engines may use DirectMusic or DirectShow (ole32 COM objects): answering `CoCreateInstance` with
failure often makes the game fall back to plain DirectSound.

## Input

- Keyboard: DirectInput keyboard state by DIK code, plus window messages; key names come from
  `GetKeyNameText`-style calls if the game shows them.
- Mouse: relative motion; the page uses pointer lock when it can.
- Controllers: virtual DirectInput joysticks with the `port.json` `pad` identity. Look in the game's
  data for a controller table (Cricket 07 and Rugby 06 have `Data/controltypes.csv`) and pick a pad it
  knows so buttons map correctly; the default mapping is in `dinput.c` ("W3C standard gamepad buttons
  -> the Dual Action's DirectInput buttons"). Some games read digital directions from the stick rather
  than the POV hat for a given pad type: the d-pad also deflects the left stick for that reason.
- Games enumerate devices once at startup, so all virtual pads exist from the start.

## Tools summary

| Tool | Use |
|---|---|
| `PORT_LOG`, `PORT_LOG_IMPORTS`, `PORT_LOG_D3D` | What the game asks of Windows |
| `PORT_SCREENSHOTS`, `PORT_CAPTURE` | Look at frames without watching |
| `PORT_INPUT` + `PORT_PACE60` | Repeatable scripted sessions |
| `PORT_WATCHDOG`, `PORT_PROFILE_WAITS`, `sample` | Where it's stuck or busy |
| `disasm.py`, `re/decomp_*.sh`, `re/xrefs.sh` | What code at an address does |
| `PORT_DUMP`, `PORT_WATCH_*`, lldb | Memory, who writes it, native backtraces |
| `recomp/tests/run_difftest.sh` | Translator bugs: compares recompiled functions with the original under Unicorn |

## Known gaps in the template

MMX/3DNow!/SSE2-integer instructions; fixed-function lighting and fog; cube-map pixel-shader ops; GDI
text rendering; C++ exceptions (`RaiseException` and SEH unwinding); DirectDraw beyond what Direct3D 8
games touch; force feedback; NaN bit patterns (differ from x87; games don't depend on them).
`fesetround` isn't available in WebAssembly (only round-to-nearest), which hasn't mattered so far.

## Other graphics APIs

- **Direct3D 9**: the closest step. The device model is similar to D3D8 (same draw/state concepts,
  vertex declarations instead of FVF-only, sampler states split from texture stages, shader model 2/3).
  Approach: a `d3d9.c` beside `d3d8.c` reusing `render_rec.c`/`render_wgpu.c`, extending the shader
  translators to vs/ps 2.0 and 3.0 (most games of the era use 2.0). Plan it as its own large milestone.
- **DirectDraw / Direct3D 7**: 2D surfaces and blits become texture uploads and quads; D3D7 is
  fixed-function only (so lighting/fog must exist first).
- **OpenGL 1.x**: an immediate-mode-to-WebGPU layer (like the D3D8 path's command stream). Large.

Tell the user honestly when a game needs one of these: it's weeks of work, not hours.

## Without the native build (Linux / Windows hosts)

The browser build doesn't need SDL2, Metal or AudioToolbox. On other systems:
`recomp/build.sh` needs porting (its native link step is macOS-specific: replace the framework flags
and use the platform's wgpu-native and audio), or skip native and iterate with the **Node build**
(`node recomp/build_web/node/game.js game`: headless, real file system, logs and `PORT_INPUT` work,
no picture) plus the page in a browser for anything visual. It's slower going; tell the user.

## What the first two games needed

Useful as a guide to what a similar game will hit:
- **Cricket 07**: the protected retail executable had to be replaced by an unprotected one; ~240
  Windows functions; vs.1.1 on the CPU and ps.1.1 to WGSL; a stadium light map rendered and read
  back at load; EA AEMS x86 sound modules (the interpreter); a fair ReleaseMutex for EA's sound
  locking; a clock starting near zero (huge GetTickCount values broke EA's timer in WebAssembly); in
  deterministic mode, two spin loops (intro video, loading screen: `determinism.md`).
- **Rugby 06** (same engine): unprotected retail executable; section names with junk after the NUL;
  code reached only from immediates and packed CRT tables (translator fixes); ~270 missed direct
  call/jump targets translated as extra functions; a COM leak (texture surfaces never released)
  that exhausted objects at team select; `DirectSoundCreate` imported by name, not ordinal; a
  generic USB pad hidden by SDL's MFi backend on macOS (`SDL_HINT_JOYSTICK_MFI=0`).
