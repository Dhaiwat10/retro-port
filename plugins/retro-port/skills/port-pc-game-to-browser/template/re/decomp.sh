#!/bin/sh
# Usage: re/decomp.sh <name-substring> [max]  — prints Ghidra's decompilation of matching functions
P=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME="$P/third_party/jdk/Contents/Home"
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
GHIDRA=$(ls -d "$P"/third_party/ghidra_*_PUBLIC 2>/dev/null | tail -1)
"$GHIDRA/support/analyzeHeadless" "$P/re/ghidra" Port -process "$(basename "$EXE")" \
  -noanalysis -readOnly -scriptPath "$P/re/scripts" -postScript DecompileMatching.java "$1" "${2:-1}" 2>&1 \
  | awk '/==== /,/DECOMPILED/' | sed 's/^INFO  DecompileMatching.java> //; s/ (GhidraScript)  $//' | grep -v '^DECOMPILED'
