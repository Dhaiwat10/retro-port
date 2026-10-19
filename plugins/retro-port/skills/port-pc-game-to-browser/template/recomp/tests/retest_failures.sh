#!/bin/sh
# Re-runs only the functions that failed in results/merged.json with the current tester,
# so new classifications or fixes can be checked without a full run.
P=$(cd "$(dirname "$0")/../.." && pwd)
EXE=$(python3 -I "$P/tools/configure.py" --print-exe)
"$P/.venv/bin/python" -I -c "import json; [print(e) for e,_,_ in json.load(open('$P/recomp/tests/results/merged.json'))['fail']]" |
while read e; do
  "$P/.venv/bin/python" -I "$P/recomp/tests/difftest.py" "$P/game/$EXE" "$P/recomp/data/functions.json" \
    "$P/recomp/build/libportrecomp.dylib" --only "$e" --trials 3 --allow-calls --report /tmp/retest_one.json > /dev/null 2>&1
  "$P/.venv/bin/python" -I -c "import json; r=json.load(open('/tmp/retest_one.json')); [print(k, e, n, w) for k, v in r.items() for e, n, w in v]"
done
