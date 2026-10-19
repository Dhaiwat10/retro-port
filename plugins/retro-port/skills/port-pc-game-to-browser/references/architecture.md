# How a port works

Contents: the idea · translator · runtime · host layer · rendering · audio · input · files and
registry · the page · port.json · build outputs · every PORT_* switch

## The idea

The game's x86 machine code is translated **ahead of time** into C, one C function per x86
function, and compiled like any program: natively (macOS, for debugging with real tools) and to
WebAssembly (to play). Nothing is emulated at run time. The Windows functions the game imports
(kernel32, user32, Direct3D 8, DirectSound, DirectInput, winmm, ...) are reimplemented in C in
`recomp/host/`. The game's own code, unchanged, runs on top.

Two properties make this work for games of this era:
- **The image stays at its original address.** Guest memory is one flat reservation (`g_mem`) and the
  PE image is loaded where it expects to be (normally 0x400000), so every pointer, vtable and jump
  table in the original is still valid.
- **The guest stack is real.** `call` pushes the real return address and `ret N` pops it, so code that
  reads arguments or return addresses through ESP behaves exactly as on Windows.

## Translator (`recomp/translator/x86toc.py`)

- Input: the executable + `recomp/data/functions.json` (Ghidra's function entries, ranges and switch
  tables, from `re/analyze.sh`). Output: `recomp/out_all/*.c` (300 functions per file).
- Each guest function becomes `void f_XXXXXXXX(CPU *c)`. Registers, flags, the x87 stack and XMM
  registers live in the `CPU` struct (`runtime/cpu.h`). Branches inside a function become `goto`s;
  switch tables become C `switch`es; other indirect calls and jumps go through an address → function
  table at run time.
- Disassembly is recursive descent from each entry, using Ghidra's bounds only as limits (its body
  ranges can split instructions). Extra entry points are found from code addresses stored in data,
  immediates that point at code, packed CRT tables, and direct calls/jumps to non-entries; those are
  translated as functions of their own until no new targets appear.
- Every function starts with `PREEMPT_POLL(c)` (free unless deterministic mode is on).
- CPUID reports a plain Pentium-class CPU (no MMX/SSE/3DNow!), so games take their x87 paths.
  Unsupported: MMX, 3DNow!, SSE2 integer (games normally only reach them through CPU-feature dispatch,
  which CPUID steers away from). An unsupported instruction traps with its address when reached.
- `TRACE=1 sh recomp/build.sh` builds a variant that records every instruction (for the differential
  tester in `recomp/tests/`, which runs original code under Unicorn and compares).

## Runtime (`recomp/runtime/`)

- `cpu.h`: CPU state, memory accessors (`R32`, `W32`, `HP(addr)` = host pointer), flag helpers.
- `runtime.c`: guest memory, PE loader (`load_image`, which also records the code and writable-data
  ranges in `g_code_lo/hi`, `g_data_lo/hi`), import dispatch (each import slot gets a magic address
  that calls a host handler; an unimplemented import traps with its name), traps with stack dumps.
- `interp.c`: a small x86 interpreter for code the game **creates at run time** (EA's AEMS sound system
  ships x86 modules inside sound banks and calls them). Regions are registered when the game marks
  memory executable (`VirtualProtect`/`VirtualAlloc` with execute rights).
- Memory layout (`runtime.h`): TIB at 0x10000, main stack below 0x300000, image from 0x400000, guest
  heap from 0x10000000, VirtualAlloc above. Natively the guest gets 4 GiB; in WebAssembly a compacted
  1.25 GiB window (see `web.md`).

## Host layer (`recomp/host/`)

| File | What |
|---|---|
| `win32.h` | Handler macros (`W(name)`, `ARG(i)`, `RET(nargs, value)`), shared declarations, `GUEST_ROOT` |
| `kernel32.c` | Memory, files (mapped onto `game/` case-insensitively), threads on real host threads, events/mutexes/semaphores/critical sections, TLS, time, locale, Sleep |
| `user32.c` | A virtual window and message queue that calls the game's window procedure; cursor, keyboard state |
| `misc.c` | GDI stubs, the virtual registry (`port.json` registry values), winmm timers, DirectX entry points, the import tables |
| `core.c` | Guest heap allocator, handle table, path mapping, the clocks (real and virtual), thread setup |
| `com.c/.h` | Minimal COM object model for the DirectX interfaces |
| `d3d8.c/.h` | Direct3D 8 device: resources, states, draws, Present; frame hashing for determinism checks |
| `render_rec.c` | CPU side of rendering: primitive expansion, vertex shaders (CPU), texture conversion, uniforms |
| `render_wgpu.c` | GPU side: every WebGPU object; executes the command stream; bind-group cache; BC textures |
| `vshader.c`, `pshader.c` | vs.1.1 vertex shaders on the CPU; ps.1.x pixel shaders translated to WGSL |
| `texconv.c` | DXT and palettised textures to RGBA where the GPU can't take them as-is |
| `dsound.c` | DirectSound buffers and a mixer thread; audio out through AudioToolbox or the page's worklet |
| `dinput.c`, `input.c` | DirectInput keyboard/mouse/joystick devices; virtual gamepads; scripted input |
| `window.c` | Native window (SDL2), events, controllers (macOS only) |
| `sched.c` | The deterministic scheduler (determinism.md) |
| `netplay.c` | Lockstep netplay core (netplay.md) |
| `main.c` | Launcher, watchdog; `web.c` is the browser entry glue |
| `watch.c` | Hardware write watchpoints for debugging (native only) |

Handler pattern:

```c
W(GetTickCount) { RET(0, (uint32_t)(guest_seconds() * 1000)); }   /* stdcall: RET pops nargs */
```

`ARG(i)` reads the i-th stack argument; strings come in through `GSTR(addr)`; output buffers are
written with `W32` or through `HP(addr)`. Register a new handler in its DLL's table (e.g. the `K(...)`
list at the end of `kernel32.c`, the `U(...)` list in `user32.c`, the tables in `misc.c`).

## Rendering

Split in two (`render_cmd.h`): `render_rec.c` runs on the game thread and turns Direct3D calls into
self-contained commands; `render_wgpu.c` executes them. Natively each command runs at once. In the
browser, the page thread replays queued chunks once per animation frame, because WebGPU only presents
from a thread that returns to its event loop. GPU readbacks (a game reading a render target back, as
Cricket 07's stadium light map does) pause the game thread until the copy is mapped.

## Audio

The mixer thread mixes DirectSound buffers paced by the clock and writes into a ring: natively played
through AudioToolbox, in the browser by `web/audio-worklet.js` from shared memory. Nothing waits on the
audio device, so audio can never stall the game.

## Input

- Keyboard and mouse go to DirectInput devices and the window's message queue (`input.c`).
- Up to 4 **virtual gamepads** always exist (`PORT_PADS`, page `?pads=N`) because games enumerate
  devices once at startup. Each presents the identity in `port.json` `pad` (default Logitech Dual
  Action USB, 046D:C216): pick one the game's own controller table recognises so it maps buttons right.
- Physical controllers come in through `input_gamepad()` in the W3C "standard gamepad" layout: SDL2
  natively, the Gamepad API in the page. Slots are handed out in connection order.
- `PORT_INPUT` scripts input by frame for automated testing (below).

## Files and registry

- Paths under `port.json` `guest_root` map to `game/`, matching components case-insensitively. Other
  paths the game writes (saves, settings) go to `userdata/` (or `PORT_USERDATA`).
- Drive D: reports as a CD-ROM labelled `volume_label` (some games check for their disc's label;
  if a game insists on actual disc contents, that's a copy-protection check: see SKILL.md ground rules).
- The registry is virtual: `misc.c` serves "My Documents" plus the `port.json` registry values. Run with
  `PORT_LOG=1` to see which keys and values the game asks for, then add the ones from the user's own
  installation (on Windows: `regedit`, `HKEY_LOCAL_MACHINE\SOFTWARE\<publisher>\<game>` or the
  `WOW6432Node` equivalent).

## The page (`recomp/web/`)

`index.html` + `port.js`: a launcher (Play Solo; Play Multiplayer when `online` is on, with host/join
lobby), first-visit download of the game files into the browser's private storage (OPFS) with resume,
a folder picker when no files are hosted, WebGPU replay of render commands, keyboard/mouse/gamepad
input, audio worklet, fullscreen (button and Alt+Enter), online status overlay. `port-config.js`
(generated) supplies the name, executable and file rules. `serve.py` serves it locally with the right
headers; `make_dist.py` packages it for hosting; `cloudflare/` is the Worker for private hosting and
netplay rooms.

## port.json

Edited by you; everything else derives from it (`tools/configure.py` runs at every build).

| Field | Meaning |
|---|---|
| `name`, `id` | Display name; lower-case slug for files, storage and the Worker name |
| `exe` | The executable to recompile, relative to `game/` (unprotected) |
| `guest_exe`, `guest_root` | The name and install folder the game believes it has |
| `volume_label` | Label of the virtual CD drive |
| `files.include`, `files.required` | Which folders the browser needs (empty = all) and sanity checks |
| `registry` | Values the game reads: `{"key", "value", "sz"}` or `{..., "dword"}` |
| `pad` | Controller identity shown to the game |
| `online`, `controls` | Offer online 1v1; one line of control hints for the launcher |

## Build outputs

| Path | What | Shareable |
|---|---|---|
| `recomp/out_all/` | Generated C | No (derived from the game) |
| `recomp/build/game` | Native build | No |
| `recomp/build_web/www/game.{js,wasm}` | Browser build | No |
| `recomp/build_web/node/game.js` | Headless Node build (tests, netplay peers) | No |
| `dist/` | Page + build + game files, for hosting | No |

## PORT_* switches (environment variables)

Logging: `PORT_LOG=1` (files, registry, threads, window), `PORT_LOG_IMPORTS=1` (every Windows call),
`PORT_LOG_D3D=1|2` (Direct3D objects | every call), `PORT_LOG_FPS=1`.

Running: `PORT_HEADLESS=1` (no window, 60 Hz pacing), `PORT_NO_AUDIO=1`, `PORT_FULLSCREEN=1`,
`PORT_PADS=N` (1-4), `PORT_USERDATA=<dir>` (saves folder), `PORT_PACE60=1` (fixed 60 Hz so scripted
input is repeatable without deterministic mode), `PORT_NO_BC=1` (no compressed GPU textures).

Scripted input: `PORT_INPUT="900:click,1150:key=1f,1400:pad=1,1500:pad=2000/10,1600:stick=0:-32768,2000:pad=1/0"`
by frame: `click`, `move=dx:dy`, `key=<DIK hex>` (1f = S, 1c = Enter, c8/d0/cb/cd = arrows),
`pad=<standard-gamepad bitmask hex>[/hold frames]` on pad 0 (1 = A, 2 = B, 4 = X, 8 = Y, 100 = Back,
200 = Start, 1000/2000/4000/8000 = d-pad up/down/left/right; `/0` = released within the same frame),
`stick=lx:ly` (left stick, -32768..32767).

Seeing: `PORT_SCREENSHOTS=1100,1700` (PNG to `/tmp/port_frame_N.png`), `PORT_CAPTURE=N` (log every draw
of frame N and save the image after each to `/tmp/port_cap/`; `touch /tmp/port_snap` does the same on
demand), `PORT_DUMP="800:a3a670:40"` (hex-dump guest memory at a frame), `PORT_DUMP_VS=1`/`PORT_DUMP_PS=1`
(shaders), `PORT_TRACE_VS=1`.

Debugging: `PORT_WATCHDOG=secs` (every N s: each thread's guest return addresses and, in deterministic
mode, the virtual clock and game-thread counters), `PORT_PROFILE_WAITS=1` (busiest Sleep/Wait call sites
every 3 s), `PORT_TRACE_CALL=<hex addr>` (log indirect calls to it), `PORT_WATCH_VSC=n`/`PORT_WATCH_ARM=1`
(who writes vertex-shader constant n, via a write watchpoint).

Determinism: `PORT_DETERMINISTIC=1`, `PORT_HASH_LOG=<file>`, `PORT_HASH_ALL=N`, `PORT_SNAPSHOT=<frames>`
(+`PORT_SNAPSHOT_HEAP=1`, `PORT_SNAPSHOT_DIR`), `PORT_DET_YIELD_US`, `PORT_DET_PREEMPT_US`.

Netplay: `PORT_NETPLAY=1`, `PORT_NP_PLAYERS`, `PORT_NP_SLOT`, `PORT_NP_DELAY`, `PORT_NP_LOG_INPUT=1`,
`PORT_NP_PERTURB=<frame>` (test desync detection); Node harness: `PORT_NP_URL`, `PORT_NP_HOST=<n>`,
`PORT_NP_NAME`, `PORT_NP_VERSION`, `PORT_NP_DROP_AT=<s>`.

Page URL options: `?mute`, `?fps`, `?log`, `?autoplay`, `?pads=N`, `?game=<url>`, `?join=<code>`,
`?turn=only`.
