#!/bin/sh
# Usage: re/decomp_in.sh <hexAddr>... — prints Ghidra's decompilation of the functions containing those addresses
P=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME="$P/third_party/jdk/Contents/Home"
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
GHIDRA=$(ls -d "$P"/third_party/ghidra_*_PUBLIC 2>/dev/null | tail -1)
"$GHIDRA/support/analyzeHeadless" "$P/re/ghidra" Port -process "$(basename "$EXE")" \
  -noanalysis -readOnly -scriptPath "$P/re/scripts" -postScript DecompileContaining.java "$@" 2>&1 \
  | sed -n '/DecompileContaining.java> ====/,/REPORT: Post-analysis\|INFO  REPORT/p' \
  | sed 's/^INFO  DecompileContaining.java> //; s/ (GhidraScript)  $//' | grep -v 'INFO  REPORT'
