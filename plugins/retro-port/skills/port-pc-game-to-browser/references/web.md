# The browser build

Contents: building · playing locally · what had to change for WebAssembly · memory · storage ·
threads and headers · rendering and browsers · input and audio in the page · checking it · packaging

## Building

```bash
. third_party/emsdk/emsdk_env.sh
sh recomp/build_web.sh        # needs recomp/out_all from recomp/build.sh; incremental
```

Outputs `recomp/build_web/www/game.{js,wasm}` (browser) and `recomp/build_web/node/game.js` (headless
Node with the real file system, for tests and netplay peers). The script also patches Emscripten's
WebGPU bindings so Firefox accepts `setBindGroup` dynamic offsets (it warns if a newer emsdk changed the
pattern).

## Playing locally

`./play.sh` builds if needed, runs `recomp/web/serve.py --game game` on http://localhost:8007 and opens
it. The page downloads the game files into the browser's storage on first visit (from the local
server, so it's fast) and starts from there afterwards. `serve.py` only listens on localhost; use
`--password` if it ever has to be reachable from elsewhere.

## What had to change for WebAssembly

These are already in the template; know them when something breaks:
- **Compact guest memory**: wasm32 has a single 4 GiB memory shared with everything, so the guest
  gets a 1.25 GiB window: heap 0x10000000-0x20000000, VirtualAlloc 0x20000000-0x50000000
  (`runtime/runtime.h`). Cricket 07 peaks at ~125 MB heap and ~580 MB VirtualAlloc (mostly mapped
  archives). A game that needs more must have its ranges retuned, and the absolute ceiling is
  below 4 GiB total (Memory64 isn't usable here yet).
- **Big thread stacks**: wasm stacks have no guard page (an overflow silently corrupts memory) and
  recompiled frames are large: game threads get 64 MB host stacks, the main game thread 256 MB.
- **Clock near zero**: `host_seconds()` counts from process start (+60 s). Epoch-sized tick counts
  made one engine's timer lose precision.
- `lock`/`xchg [mem]` run under a global lock; file reads loop on short reads.
- Not available: `fesetround` (round-to-nearest only) and write watchpoints.

## Storage

- Game files live in the Origin Private File System (OPFS), mounted at `/opfs` through WASMFS
  (`host/main.c`). The page asks the browser to keep them (`navigator.storage.persist`).
- First visit: every file in `game/manifest.json` is downloaded with resume (HTTP range requests);
  the manifest `version` changes when any file's size or name changes, and then only differing files
  are fetched. Without a manifest next to the page, a folder picker imports the game from disk.
- Saves go to OPFS too, per browser. Online sessions use a fresh saves folder per player slot
  (`<id>-netplay-<slot>`) so both players start from identical state.

## Threads and the headers they need

The build uses pthreads (`-sPROXY_TO_PTHREAD`), which need `SharedArrayBuffer`, which browsers only
enable on **cross-origin isolated** pages: every response must carry
`Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.
`serve.py`, the Worker and `dist/_headers` set them. They also require a **secure context**: `https://`,
or `localhost` on the same machine. A plain `http://192.168.x.x` address from another computer won't run
the game; use HTTPS (hosting.md) for anything beyond one machine.

## Rendering and browsers

- The game thread records commands; the page's main thread replays them with WebGPU each animation
  frame. GPU readbacks block the game thread until mapped (answered mid-frame too, so it doesn't wait
  for vsync).
- **Chrome / Edge**: the reference. **Safari**: WebGPU works on current versions; memory is tighter (the
  Rugby port trimmed memory use for iPhone/iPad). **Firefox**: WebGPU depends on GPU and driver
  (`about:support`, "WebGPU"); it rejects typed arrays over 2 GB in `setBindGroup` (patched at build).
- `navigator.gpu` can exist without a usable adapter (blocklisted GPU, hardware acceleration off,
  Remote Desktop, VMs): the page checks and explains instead of showing a black screen; it tries the
  default power preference and then the software fallback adapter.
- Textures: DXT1/3/5 upload as BC1/2/3 when the device has `texture-compression-bc` (most desktop GPUs),
  otherwise convert to RGBA8. Freed textures are destroyed right after the frame is submitted, and draw
  bind groups are cached: on Windows/D3D12 browsers, creating one per draw ran out of descriptor memory.
- The 4:3 picture is letterboxed into whatever size the canvas has; fullscreen (button, Alt+Enter)
  just grows the canvas.

## Input and audio in the page

- Keyboard: `KeyboardEvent.code` → USB HID usages → DirectInput. Quick taps count as held for at least
  50 ms (solo) or one frame (online) so they aren't lost between samples.
- Mouse: relative motion, pointer lock when allowed.
- Gamepads: the Gamepad API, polled every animation frame; browsers only expose a pad after a button
  press on it. Pads without the `standard` mapping are read in HID order like DirectInput would.
- Audio: an AudioWorklet reads the mixer's ring from shared memory; browsers need a click before audio
  starts, which the Play button provides. `?mute` turns it off.

## Checking it

- `node recomp/build_web/node/game.js game` with `PORT_HASH_LOG`, `PORT_INPUT`: proves the wasm build
  runs the game logic (no picture).
- The browser pane or Chrome: `?fps` logs the frame rate, `?log` the host log, the console shows errors.
- A full play-through of the first minutes on the page before telling the user it's ready.

## Packaging

`python3 recomp/web/make_dist.py --game game` writes `dist/`: the page, the build, the game files and
`manifest.json`, `_headers` for static hosts that read it, and a `robots.txt` that disallows indexing.
`dist/` contains the game: host it only behind a password (`hosting.md`), never publicly.
