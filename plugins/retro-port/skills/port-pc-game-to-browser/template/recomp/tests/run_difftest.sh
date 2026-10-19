#!/bin/sh
# Runs the differential tester in N parallel shards and merges the results.
# Usage: recomp/tests/run_difftest.sh [N] [extra difftest.py args...]
P=$(cd "$(dirname "$0")/../.." && pwd)
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
N=${1:-8}; shift
OUT="$P/recomp/tests/results"; mkdir -p "$OUT"; rm -f "$OUT"/shard_*
for k in $(seq 0 $((N - 1))); do
  "$P/.venv/bin/python" -I "$P/recomp/tests/difftest.py" "$P/game/$EXE" "$P/recomp/data/functions.json" \
    "$P/recomp/build/libportrecomp.dylib" --shard "$k/$N" --report "$OUT/shard_$k.json" "$@" > "$OUT/shard_$k.log" 2>&1 &
done
wait
"$P/.venv/bin/python" -I - "$OUT" <<'PY'
import json, glob, sys, collections
out = sys.argv[1]; tot = collections.defaultdict(list)
for f in glob.glob(f"{out}/shard_*.json"):
    for k, v in json.load(open(f)).items(): tot[k] += v
json.dump(tot, open(f"{out}/merged.json", "w"), indent=1)
print("  ".join(f"{k}={len(v)}" for k, v in tot.items()))
for e, n, why in sorted(tot["fail"]): print("FAIL", e, n, why)
PY
