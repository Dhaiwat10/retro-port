#!/bin/sh
# Retranslates the whole executable and rebuilds build/libportrecomp.dylib (or, with TRACE=1, build/libporttrace.dylib).
# Only generated files whose content changed are recompiled.
set -e
R=$(cd "$(dirname "$0")" && pwd)
P=$(dirname "$R")
PY="$P/.venv/bin/python"
OPT=${OPT:--O1}
SDL_PREFIX=${SDL_PREFIX:-$(brew --prefix)}  # brew --prefix sdl2 now points at sdl2-compat
FPFLAGS="-frounding-math"  # the x87 rounding mode is mirrored into the host FPU at runtime
# TRACE=1 builds a separate tracing variant (every instruction calls trace_insn) for debugging
if [ "${TRACE:-0}" = 1 ]; then OUT=out_trace; OBJ=obj_trace; LIB=libporttrace.dylib; TFLAG=--trace
else OUT=out_all; OBJ=obj_all; LIB=libportrecomp.dylib; TFLAG=; fi
export OUT OBJ  # used inside the per-file compile command below

EXE=$("$PY" -I "$P/tools/configure.py" --print-exe)   # also regenerates host/port_config.h and web/port-config.js
[ -f "$R/data/functions.json" ] || { echo "recomp/data/functions.json is missing: run the Ghidra analysis first (see README)"; exit 1; }
"$PY" -I "$R/translator/x86toc.py" "$P/game/$EXE" "$R/data/functions.json" "$R/$OUT" --all --per-file 300 $TFLAG | head -1
mkdir -p "$R/build/$OBJ"
cd "$R"
ls $OUT/*.c | while read f; do
    b=$(basename "$f" .c)
    h=$( (cat "$f"; echo "$OPT $FPFLAGS") | shasum | cut -c1-40)  # flags are part of the cache key
    if [ ! -f "build/$OBJ/$b.o" ] || [ "$(cat "build/$OBJ/$b.hash" 2>/dev/null)" != "$h" ]; then
        echo "$f $b $h"
    fi
done | xargs -P 10 -L 1 sh -c 'clang '"$OPT $FPFLAGS"' -w -c -Iruntime -I$OUT "$0" -o "build/$OBJ/$1.o" && echo "$2" > "build/$OBJ/$1.hash" && echo "  compiled $1"'
for o in build/$OBJ/*.o; do  # drop objects whose source no longer exists
    [ -f "$OUT/$(basename "$o" .o).c" ] || rm -f "$o" "${o%.o}.hash"
done
clang $OPT $FPFLAGS -w -c -Iruntime runtime/runtime.c -o build/runtime.o
clang $OPT $FPFLAGS -w -c -Iruntime runtime/interp.c -o build/interp.o
clang $OPT -w -c -Iruntime tests/difftest_api.c -o build/difftest_api.o
# link to a temp file and rename, so running test processes keep their mapped copy intact
clang -dynamiclib -o build/$LIB.tmp build/runtime.o build/interp.o build/difftest_api.o build/$OBJ/*.o -lm
mv -f build/$LIB.tmp build/$LIB
echo "built build/$LIB"

# Native launcher (normal build only): the Windows API layer in host/ + the recompiled game
if [ "${TRACE:-0}" != 1 ]; then
    mkdir -p build/host
    for f in host/*.c; do
        clang $OPT -Wall -Wno-unused-function -Wno-misleading-indentation -c -Iruntime -Ihost \
            -I"$P/third_party/wgpu/include" -I"$SDL_PREFIX/include" "$f" -o "build/host/$(basename "$f" .c).o" || exit 1
    done
    clang -o build/game.tmp build/runtime.o build/interp.o build/host/*.o build/$OBJ/*.o -lm -framework AudioToolbox \
        "$P/third_party/wgpu/lib/libwgpu_native.a" -L"$SDL_PREFIX/lib" -lSDL2 \
        -lz -framework Metal -framework QuartzCore -framework Cocoa -framework IOKit -framework CoreFoundation
    mv -f build/game.tmp build/game
    echo "built build/game"
fi
