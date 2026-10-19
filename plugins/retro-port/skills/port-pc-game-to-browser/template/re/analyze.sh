#!/bin/sh
# Full Ghidra pass over the game executable: import, auto-analysis, name functions from assert strings,
# recover C++ classes from RTTI, and export the function list the recompiler needs.
#   re/analyze.sh            (takes a few minutes; writes re/ghidra/, re/*.csv, recomp/data/functions.json)
# Everything it writes is derived from the game: keep it out of anything you share.
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
export JAVA_HOME="$P/third_party/jdk/Contents/Home"
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
GHIDRA=$(ls -d "$P"/third_party/ghidra_*_PUBLIC 2>/dev/null | tail -1)
[ -n "$GHIDRA" ] || { echo "Ghidra is not in third_party/: run the skill's setup script first"; exit 1; }
mkdir -p "$P/re/ghidra" "$P/recomp/data"
"$GHIDRA/support/analyzeHeadless" "$P/re/ghidra" Port -import "$P/game/$EXE" -overwrite \
  -scriptPath "$P/re/scripts" \
  -postScript NameFromAsserts.java "$P/re/assert_names.csv" \
  -postScript RecoverRttiClasses.java "$P/re/rtti_classes.csv" \
  -postScript ExportFunctions.java "$P/recomp/data/functions.json" > "$P/re/analysis.log" 2>&1 \
  || { tail -30 "$P/re/analysis.log"; exit 1; }
python3 -I -c "import json,sys; f=json.load(open(sys.argv[1])); n=len(f if isinstance(f,list) else f['functions']); print(f'exported {n} functions')" "$P/recomp/data/functions.json"
