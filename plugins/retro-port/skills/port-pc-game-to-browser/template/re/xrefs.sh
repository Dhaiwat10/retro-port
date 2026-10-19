#!/bin/sh
# Usage: re/xrefs.sh <hexAddr>... — lists references to those addresses (from Ghidra's analysis)
P=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME="$P/third_party/jdk/Contents/Home"
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
GHIDRA=$(ls -d "$P"/third_party/ghidra_*_PUBLIC 2>/dev/null | tail -1)
"$GHIDRA/support/analyzeHeadless" "$P/re/ghidra" Port -process "$(basename "$EXE")" \
  -noanalysis -readOnly -scriptPath "$P/re/scripts" -postScript XrefsTo.java "$@" 2>&1 \
  | grep 'XREF' | sed 's/^INFO  XrefsTo.java> //; s/ (GhidraScript)  $//'
