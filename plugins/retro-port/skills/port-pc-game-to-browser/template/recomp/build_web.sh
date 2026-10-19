#!/bin/sh
# Browser build: compiles the recompiled game (translator output from build.sh), the runtime and the
# Windows API layer to WebAssembly with Emscripten. Only changed files are recompiled.
#   recomp/build_web.sh  -> build_web/www/game.js   the browser build (serve with recomp/web/serve.py)
#                        -> build_web/node/game.js  headless under Node with the real file system:
#                           node recomp/build_web/node/game.js game
set -e
R=$(cd "$(dirname "$0")" && pwd)
P=$(dirname "$R")
OPT=${OPT:--O1}
CFLAGS="$OPT -pthread -frounding-math -w $EXTRA_CFLAGS"
OUT=out_all
B="$R/build_web"
mkdir -p "$B/obj" "$B/host" "$B/node" "$B/www"
cd "$R"
python3 -I "$P/tools/configure.py"   # host/port_config.h, web/port-config.js
[ -d "$OUT" ] || { echo "run build.sh first (it generates $OUT/)"; exit 1; }

# generated code: one object per translator output file, rebuilt when the source or flags change
ls $OUT/*.c | while read f; do
    b=$(basename "$f" .c)
    h=$( (cat "$f"; echo "$CFLAGS") | shasum | cut -c1-40)
    if [ ! -f "$B/obj/$b.o" ] || [ "$(cat "$B/obj/$b.hash" 2>/dev/null)" != "$h" ]; then echo "$f $b $h"; fi
done | xargs -P 10 -L 1 sh -c 'emcc '"$CFLAGS"' -Iruntime -I'"$OUT"' "$0" -c -o "'"$B"'/obj/$1.o" && echo "$2" > "'"$B"'/obj/$1.hash" && echo "  compiled $1"'
for o in "$B"/obj/*.o; do [ -f "$OUT/$(basename "$o" .o).c" ] || rm -f "$o" "${o%.o}.hash"; done

# runtime + Windows layer (the macOS-only window, renderer and watchpoint are left out)
emcc $CFLAGS -Iruntime -c runtime/runtime.c -o "$B/runtime.o"
emcc $CFLAGS -Iruntime -c runtime/interp.c -o "$B/interp.o"
for f in host/*.c; do
    case "$(basename "$f")" in window.c|watch.c) continue ;; esac   # SDL window / signals: native only
    emcc $CFLAGS --use-port=emdawnwebgpu -Iruntime -Ihost -c "$f" -o "$B/host/$(basename "$f" .c).o"
done
emcc $CFLAGS -DPORT_NODE_BUILD -Iruntime -Ihost -c host/main.c -o "$B/main_node.o"
HOST_WEB=$(ls "$B"/host/*.o)
emcc $CFLAGS -c web/node_stubs.c -o "$B/node_stubs.o"
HOST_NODE="$(ls "$B"/host/*.o | grep -v '/main.o$' | grep -v '/render_wgpu.o$') $B/node_stubs.o"

# Browser build: page thread renders (WebGPU), game files from OPFS, modular loader for port.js
emcc -pthread ${LINKOPT:-$OPT} --profiling-funcs -o "$B/www/game.js" "$B"/runtime.o "$B"/interp.o $HOST_WEB "$B"/obj/*.o \
    --use-port=emdawnwebgpu -sWASMFS -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=16 -sALLOW_MEMORY_GROWTH \
    -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=32MB -sENVIRONMENT=web,worker \
    -sMODULARIZE -sEXPORT_NAME=createPort -sEXPORTED_RUNTIME_METHODS=ENV,wasmMemory -sEXIT_RUNTIME=0 $EXTRA_LINK
# Firefox rejects any typed array over 2 GB as setBindGroup's dynamic offsets, and the bindings pass a view of
# the whole wasm memory (with start/length): hand it a copy of just the offsets instead.
perl -0pi -e 's/\(growMemViews\(\), HEAPU32\), \(\(dynamicOffsetsPtr\) >>> 2\), dynamicOffsetCount\)/(growMemViews(), HEAPU32.slice(dynamicOffsetsPtr >>> 2, (dynamicOffsetsPtr >>> 2) + dynamicOffsetCount)))/g' "$B/www/game.js"
grep -q 'HEAPU32.slice(dynamicOffsetsPtr' "$B/www/game.js" || echo "warning: setBindGroup offsets patch did not apply (emdawnwebgpu changed?)" >&2
echo "built build_web/www/game.js"

# Node build: headless, real file system, PORT_* environment passed through
emcc -pthread ${LINKOPT:-$OPT} --profiling-funcs -o "$B/node/game.js" "$B"/runtime.o "$B"/interp.o "$B"/main_node.o $HOST_NODE "$B"/obj/*.o \
    -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=12 -sALLOW_MEMORY_GROWTH -sMAXIMUM_MEMORY=4GB \
    -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=32MB -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1 \
    --pre-js web/node_env.js $EXTRA_LINK
# Node gives each worker a 4 MB engine stack; recompiled code nests one engine frame per guest call
perl -0pi -e 's/("?name"?:\s*"em-pthread")/$1, resourceLimits: { stackSizeMb: 256 }/' "$B/node/game.js"
echo "built build_web/node/game.js"
