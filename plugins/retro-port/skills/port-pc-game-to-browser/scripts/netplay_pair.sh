#!/bin/sh
# Online-play check without a browser: two headless Node players join one room on a running Worker
# (local: `npx wrangler dev --config recomp/web/cloudflare/wrangler.toml --port 8799`, with PASSWORD in
# recomp/web/cloudflare/.dev.vars) and play in lockstep; each can run a scripted input.
#   sh netplay_pair.sh <port-folder> <out-dir> <frames> [host PORT_INPUT] [guest PORT_INPUT]
# Then compare_hashes.py on the two logs. PORT_NP_LOG_INPUT=1 is on, so each log lists what was sent.
P=$(cd "$1" && pwd); O=$2; N=$3; BASE=${WORKER:-http://127.0.0.1:8799}
rm -rf "$O"; mkdir -p "$O/u0" "$O/u1"
cd "$P"
PW=$(sed -n 's/^PASSWORD=//p' recomp/web/cloudflare/.dev.vars)
T=$(curl -s -u "x:$PW" "$BASE/_netplay_ticket" | python3 -c 'import sys,json; print(json.load(sys.stdin)["ticket"])') || exit 1
ROOM=T$(date +%s | tail -c 6)
URL="$(echo "$BASE" | sed 's/^http/ws/')/netplay/$ROOM?ticket=$T"
PORT_NP_LOG_INPUT=1 PORT_NP_URL="$URL&create=1" PORT_NP_HOST=2 PORT_USERDATA="$O/u0" PORT_NO_AUDIO=1 PORT_HASH_LOG="$O/a.hash" \
  PORT_INPUT="${4:-}" node recomp/build_web/node/game.js game > "$O/a.log" 2>&1 &
PA=$!
sleep 3
PORT_NP_LOG_INPUT=1 PORT_NP_URL="$URL" PORT_USERDATA="$O/u1" PORT_NO_AUDIO=1 PORT_HASH_LOG="$O/b.hash" \
  PORT_INPUT="${5:-}" node recomp/build_web/node/game.js game > "$O/b.log" 2>&1 &
PB=$!
while :; do
  na=$(tail -1 "$O/a.hash" 2>/dev/null | awk '{print $1+0}'); nb=$(tail -1 "$O/b.hash" 2>/dev/null | awk '{print $1+0}')
  [ "${na:-0}" -ge "$N" ] && [ "${nb:-0}" -ge "$N" ] && break
  kill -0 $PA 2>/dev/null || { echo "player a exited early (see $O/a.log)"; break; }
  kill -0 $PB 2>/dev/null || { echo "player b exited early (see $O/b.log)"; break; }
  sleep 2
done
kill $PA $PB 2>/dev/null
echo "finished: a=$na b=$nb frames"
