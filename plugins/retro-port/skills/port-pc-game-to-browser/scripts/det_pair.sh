#!/bin/sh
# Determinism check: two headless native runs side by side, same scripted input, separate fresh saves.
#   sh det_pair.sh <port-folder> <out-dir> <frames> ["PORT_INPUT script"]
# Then: python3 compare_hashes.py <out-dir>/a.hash <out-dir>/b.hash
# Control: run it once with DET=0 in the environment; the runs should then differ from frame 1, which
# proves the comparison can see a difference.
P=$(cd "$1" && pwd); O=$2; N=$3; INPUT=${4:-}
rm -rf "$O"; mkdir -p "$O/ua" "$O/ub"
cd "$P"
for r in a b; do
  if [ "${DET:-1}" = 1 ]; then D="PORT_DETERMINISTIC=1"; else D="PORT_PACE60=1"; fi
  env $D PORT_HEADLESS=1 PORT_NO_AUDIO=1 PORT_USERDATA="$O/u$r" PORT_HASH_LOG="$O/$r.hash" PORT_HASH_ALL=300 \
      PORT_INPUT="$INPUT" recomp/build/game game > "$O/$r.log" 2>&1 &
  eval "pid_$r=$!"
done
while :; do
  na=$(tail -1 "$O/a.hash" 2>/dev/null | awk '{print $1+0}'); nb=$(tail -1 "$O/b.hash" 2>/dev/null | awk '{print $1+0}')
  [ "${na:-0}" -ge "$N" ] && [ "${nb:-0}" -ge "$N" ] && break
  kill -0 $pid_a 2>/dev/null || { echo "run a exited early (see $O/a.log)"; break; }
  kill -0 $pid_b 2>/dev/null || { echo "run b exited early (see $O/b.log)"; break; }
  sleep 2
done
kill $pid_a $pid_b 2>/dev/null
echo "finished: a=$na b=$nb frames"
