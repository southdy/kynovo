#!/bin/bash
# soak_release.sh -- the SAME repeated deep-pipelined workload against the RELEASE binary
# (debug-heap detection is absent there: what matters is whether the user-visible process survives).
# Liveness is checked after EVERY round, and the server's own log is printed at the end.
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
out=build/perf
RUNS=${RUNS:-24}
N=${N:-4000}
K=${K:-128}
PORT=${PORT:-9481}
mkdir -p $out
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-rel
./build/kdbsvr.exe init "disk://D:/kynovo/build/perf/db-rel" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://D:/kynovo/build/perf/db-rel" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/rel-srv.log 2>&1 &
sleep 6
echo "alive at start: $(ps -W | grep -ci kdbsvr)"
fails=0
for i in $(seq 1 $RUNS); do
  res=$(timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET rl_$i payload 2>&1 | tr -d '\r' | grep -E "^pipe mode|^pipe: stalled|^error" | head -1)
  alive=$(ps -W | grep -ci kdbsvr)
  echo "run$i alive=$alive ${res:-<no report>}"
  case "$res" in *"ok=4000"*) : ;; *) fails=$((fails+1)) ;; esac
  if [ "$alive" = "0" ]; then echo "=== server died during round $i ==="; break; fi
done
echo "rounds_run=$i rounds_without_full_success=$fails"
echo "=== final liveness: $(ps -W | grep -ci kdbsvr) ==="
echo "=== server log ==="
tail -12 $out/rel-srv.log
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
