#!/bin/sh
# Usage: re/decomp_at.sh <hex-start> [hex-end] — prints Ghidra's decompilation of functions starting in [start, end)
P=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME="$P/third_party/jdk/Contents/Home"
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
GHIDRA=$(ls -d "$P"/third_party/ghidra_*_PUBLIC 2>/dev/null | tail -1)
s=$1; e=${2:-$(printf '%x' $((0x$1 + 1)))}
out=$(mktemp -t portdecomp)
"$GHIDRA/support/analyzeHeadless" "$P/re/ghidra" Port -process "$(basename "$EXE")" \
  -noanalysis -readOnly -scriptPath "$P/re/scripts" -postScript DecompileRange.java "$s" "$e" "$out" > /dev/null 2>&1
cat "$out"; rm -f "$out"
