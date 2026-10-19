#!/bin/sh
# Plays the port in your browser: builds it if needed, starts a local server and opens the page.
#   ./play.sh            (Ctrl-C to stop)
# The server only listens on this computer. Use Chrome, Edge or Safari (the game needs WebGPU).
set -e
P=$(cd "$(dirname "$0")" && pwd)
cd "$P"
if [ ! -f recomp/build_web/www/game.wasm ]; then
  [ -d recomp/out_all ] || sh recomp/build.sh
  . third_party/emsdk/emsdk_env.sh >/dev/null 2>&1
  sh recomp/build_web.sh
fi
python3 -I tools/configure.py
PORT=${PORT:-8007}
( sleep 1.5; open "http://localhost:$PORT/" ) &
exec python3 -I recomp/web/serve.py --port "$PORT" --game game
