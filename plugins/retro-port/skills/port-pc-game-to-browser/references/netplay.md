# Online play (lockstep netplay)

Contents: the idea and its requirement · the frame loop · inputs · frame time · desync detection ·
rooms and transport · input delay · turning it on · testing · limits

## The idea and its requirement

Both players run the whole game. Only controller input travels between them, and the game is
deterministic (determinism.md), so both compute the same session. To the game it's local two-player:
player 2's controller just happens to be on another computer.

**Requirement:** the game must support local multiplayer (two controllers on one PC). If it doesn't,
lockstep has nothing to attach to. Check during the survey (multiplayer hints) and in the game's menus
(a controller-select or "player 2" screen).

## The frame loop (`host/netplay.c`)

Frames are `Present` calls. At Present f:
1. The local pad's state becomes this player's input for frame **f + D** (D = input delay) and goes to
   the outbox.
2. The game thread waits (wall time only; the virtual clock doesn't move) until every player's input
   for frame f + 1 has arrived.
3. All pads are applied at once for frame f + 1.

Frames 1..D are neutral for everyone. Physical input never reaches the game directly: the page (or
the Node harness) only updates the local sample. A late input makes the game pause briefly; it never
guesses.

## Inputs

- One `NpInput` per player per frame: buttons, 4 axes, and (from the host) the frame time.
- **Quick taps**: a button pressed and released between two samples still counts as held for one frame
  (`g_local_pressed` in netplay.c, `netKeyPressed` in the page). Without it, taps were silently dropped,
  more often the slower the game ran.
- The keyboard is mapped onto the player's pad in online mode (the `NET_KEYS` table in `port.js`);
  the mouse is off. Raw keys would reach only one machine and desync the other.
- **Keep that map complete.** A key the game uses in solo play but missing from `NET_KEYS` silently does
  nothing online. Cricket 07's lofted shots are Shift + a shot key, and Shift wasn't mapped, so players
  couldn't hit sixes online. To find the pad button for a keyboard action, read the game's own help:
  its tutorial and control texts usually show button icons (Cricket 07: icon codes 0xC0-0xCF in the
  string table, in controller order, so "0xC4 + front foot" = L1 + shot = Shift + S on the keyboard),
  and its controller table (e.g. `controltypes.csv`) gives the button order. Ask a playtester to list
  every key they use and check each one online.
- **Pause** is the pad's Start button, so map the game's keyboard pause key (usually Esc) to Start, not
  to B. One static map can't give a key two meanings (Esc = back in menus, pause in a match), so keep a
  separate back key (Backspace). In fullscreen the browser takes Esc to leave fullscreen.

## Frame time

The host measures the real time between its frames (clamped to 4-50 ms) and sends it with its input;
every player advances the virtual clock by exactly that for that frame. The game runs at real speed
(choppier, not slower, when someone is slow) and every player still sees the same times.

## Desync detection

Every 60 frames each player sends a hash of the game's global data; a mismatch shows "Out of sync since
frame N" on the page. It's detection only: there is no state transfer or rollback, so a desync ends the
session. (Replaying the input log on the desynced player would be the natural way to add recovery.)

## Rooms and transport

- **Rooms** (`web/cloudflare/worker.js`, a Durable Object per room code): lobby (names, build versions,
  round trips), start (player count, delay), relay of inputs and hashes, WebRTC signalling, latency
  reports. The first player hosts. WebSocket upgrades need a short-lived ticket from `/_netplay_ticket`,
  signed with the site password, because browsers don't send basic auth on upgrades.
- **Room location**: fixed by the first request, so only the host's creating connection sends a location
  hint: the region picked in the lobby, or the nearest by the host's location. Durable Objects don't run
  in every region (none in Africa or the Middle East): hints land in the nearest one.
- **Direct channels** (`web/netplay.js`): browsers also open WebRTC data channels to each other
  (unordered, no retransmits) and each packet repeats the last 8 inputs, so a lost packet is covered by
  the next. Duplicates are harmless (inputs are stored by frame), so the faster path wins and the room
  relay is the fallback. With Cloudflare TURN credentials configured, pairs that can't connect directly
  are relayed through the data centres nearest each player instead of the room.
- **Reconnects**: a player whose room connection drops reconnects with its slot and resends its last
  600 input batches; the others are told it left only after 15 s.
- Builds must match: the lobby compares the manifest version plus the wasm's ETag.

## Input delay

Auto delay covers the slowest pair: `ceil((one-way ms × 1.25 + 20) / 16.7)` frames, minimum 2.
Same Wi-Fi with a direct channel: 2-3 frames (33-50 ms). Through a room far away: 10-15 frames. The
delay applies to each player's own presses too, which matters for timing-heavy actions (batting,
shooting). The host can override it in the lobby.

## Turning it on

1. Determinism checklist passed (determinism.md).
2. `"online": true` in `port.json`; rebuild the web build (the launcher then offers Play Multiplayer).
3. Number of virtual pads = players (set automatically from the session).
4. In the game, each player picks their team/side at its controller-select screen, as in local play.

## Testing

```bash
# terminal 1: the Worker locally (rooms + storage on this machine)
printf 'PASSWORD=%s\nUPLOAD_TOKEN=%s\n' "$(openssl rand -hex 8)" "$(openssl rand -hex 24)" > recomp/web/cloudflare/.dev.vars
npx wrangler dev --config recomp/web/cloudflare/wrangler.toml --ip 127.0.0.1 --port 8799
# terminal 2: two headless players, each tapping buttons; then compare
sh <skill>/scripts/netplay_pair.sh . /tmp/np 3000 "1500:pad=1/0,1700:pad=1" "1600:pad=1/0"
python3 <skill>/scripts/compare_hashes.py /tmp/np/a.hash /tmp/np/b.hash
grep "sent buttons" /tmp/np/a.log /tmp/np/b.log     # every tap was sent
```

Then two browser tabs on the local Worker (upload `dist/` to it first: hosting.md), then two machines
over HTTPS. `PORT_NP_PERTURB=<frame>` flips a byte on one player to confirm desync detection works.

## Limits

- No late joining, spectating or recovery after a crash or desync (all would be possible by replaying
  the input log at full speed, since the session is deterministic).
- Input delay is felt; removing it needs rollback (save/restore the whole game state every frame and
  re-simulate), which is hard here: threads keep execution state on the host stack.
- Different machines are less tested than one machine: sort directory listings, test across
  CPU architectures and browsers, and watch for desyncs before relying on it.
