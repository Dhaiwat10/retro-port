# Determinism

Contents: why · what deterministic mode does · checking it · when a check fails · spin-loop traps ·
other sources of divergence · checklist

## Why

Lockstep online play (netplay.md) only works if two machines given the same inputs compute exactly
the same game. Normally they don't: the game reads the real clock, and its threads (loader, audio,
timers) run in whatever order the OS picks, so two runs of the same game differ from the first frame.
Deterministic mode makes a run a pure function of its inputs. It also enables replays, regression
tests ("same input, same hashes") and rebuilding a session from its input log.

## What deterministic mode does (`PORT_DETERMINISTIC=1`)

- **Virtual clock** (`host/core.c`): every time source the game can see reads one virtual clock:
  `timeGetTime`, `GetTickCount`, `QueryPerformanceCounter`, `GetSystemTimeAsFileTime` (from 2006-01-01),
  `rdtsc`, message and DirectInput timestamps. Each read adds 1 µs, so loops that poll the clock still
  progress. `Present` waits for the next 1/60 s tick (online: for the host's measured frame time).
  Time zone is fixed (UTC) and dates format to a fixed string.
- **Deterministic scheduler** (`host/sched.c`): exactly one participant runs at a time (game threads,
  the multimedia-timer thread, the sound mixer). It runs until it blocks (Sleep, a wait, a contended
  critical section, `Present`, `GetMessage`); the next is picked in creation order. When everyone is
  blocked, the clock jumps to the earliest deadline. Waits whose deadline has passed wake even while
  another thread could run.
- **Deterministic preemption**: every recompiled function entry counts toward a quantum
  (`PREEMPT_QUANTUM`, 100,000 entries); then the thread yields. This lets threads that spin on memory
  another thread writes make progress, at points that are the same on every run.
- **Time passes when threads work**: a zero-length sleep (a yield) advances the clock by
  `PORT_DET_YIELD_US` (default 1000 µs), and a preempted thread by `PORT_DET_PREEMPT_US` (default 1000 µs).
  Without these, the two loops described under "spin-loop traps" either crawled or froze.
- The mixer runs in game time and feeds the page's audio ring (natively it's silent in this mode).

## Checking it

```bash
sh <skill>/scripts/det_pair.sh . /tmp/det 6000 "2000:pad=1,2400:pad=1,2800:pad=1,3200:pad=2000"
python3 <skill>/scripts/compare_hashes.py /tmp/det/a.hash /tmp/det/b.hash
```

Two headless runs side by side, same scripted input, separate fresh saves folders. `PORT_HASH_LOG`
writes a hash of the game's global data (its writable sections, from the image) every frame, and with
`PORT_HASH_ALL=300` the heap and VirtualAlloc memory every 300 frames. Pass = 0 differences.

Also run the two controls, or a pass proves nothing:
- `DET=0 sh det_pair.sh ...`: without deterministic mode the runs must differ (from frame 1, typically,
  with different RNG seeds in the log). This shows the comparison can see a difference.
- One run with no input against the scripted one: identical until the first press, different after.
  This shows the input reaches the game.

Go further than the menus before trusting it: script input into actual gameplay (a match, a race, a
level) for several thousand frames, because gameplay code is where most game logic lives.

Compare across builds and machines too: native and wasm differ from each other (different memory
windows, different libm), so all players must run the same build; wasm on V8 and JavaScriptCore was
identical for Rugby 06.

## When a check fails

1. `compare_hashes.py` gives the first differing frame. Re-run both with `PORT_SNAPSHOT=<that frame>`
   (+`PORT_SNAPSHOT_HEAP=1`, `PORT_SNAPSHOT_DIR`) and diff the dumps: the differing addresses point at
   the data (and, via `re/xrefs.sh`, the code) that diverged.
2. Usual suspects: a time source not routed through the virtual clock; a Windows facility that answers
   from host state (directory listing order, file timestamps, memory status, cursor position, window
   size); uninitialised memory a handler hands back; a thread created or woken outside the scheduler
   (a host callback thread the scheduler doesn't know about).
3. The hash log also counts the game thread's sleeps, waits and clock reads per frame; where those
   start to differ is often where the cause is.

## Spin-loop traps (found in Cricket 07)

Symptoms show up as **very slow** or **frozen** deterministic runs while normal runs are fine. Find
them with `PORT_PROFILE_WAITS=1` (busiest call sites), `PORT_WATCHDOG=5` (whose clock reads and
sleeps stop moving) and `sample`.

1. **Yield spin**: the intro-movie player waited for its next frame with `SleepEx(0)` in a loop.
   Each yield was a full scheduler hand-off and advanced the clock only by the 1 µs of a clock read, so a
   40 ms video frame took ~6,600 hand-offs: ~2 fps. Fix: a yield advances the clock (`PORT_DET_YIELD_US`).
2. **Memory spin**: the loading screen polled its async loader's tree without any Windows call; every
   other thread was blocked waiting for a timer callback. Preemption yielded, but the clock never moved
   (only blocked-everyone advances it), so the timer never came due: frozen at the same frame forever.
   Fix: preemption advances the clock, and expired deadlines wake waiters even while someone can run.

Both fixes are generic and deterministic. A new game may have its own variant: same tools, same idea
(make the passage of time depend on the work done, deterministically).

## Other sources of divergence to keep in mind

- **Directory listings** come back in host file-system order (`readdir`). Two machines with different
  file systems could list a folder differently. If a game enumerates directories, sort the results in
  `FindFirstFileA`/`FindNextFileA`.
- **Floating point**: wasm is IEEE-deterministic except NaN bit patterns, which can differ between CPU
  architectures and engines. A game that branches on NaN bits would diverge (rare).
- **GPU readbacks**: different GPUs can return slightly different pixels. Cricket 07's light map only
  affected lighting, but if gameplay reads a readback, those values must be made identical (e.g. compute
  them on the CPU in deterministic mode).
- **Keyboard and mouse** go straight to the game in solo play; in online play only pad input travels
  (the keyboard is mapped onto the player's pad) so one player's keys can't desync the other.

## Checklist before netplay

- [ ] Deterministic runs reach gameplay at real speed (not just menus).
- [ ] Two runs with scripted input are identical for thousands of frames, globals and heap.
- [ ] The DET=0 control differs; the no-input control diverges exactly at the first press.
- [ ] The wasm (Node) build passes the same check.
