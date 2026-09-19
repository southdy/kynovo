#!/bin/bash
# soak_release.sh -- the SAME repeated deep-pipelined workload against the RELEASE binary
# (debug-heap detection is absent there: what matters is whether the user-visible process survives).
# Liveness is checked after EVERY round, and the server's own log is printed at the end.
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
RUNS=${RUNS:-24}
N=${N:-4000}
K=${K:-128}
PORT=${PORT:-9481}
mkdir -p $out
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-rel
# Do not swallow the init failure: on CI a bad store path (once: a hard-coded D:/kynovo) made the
# server die at once and the only visible message was the app's own "failed to start server 1".
if ! ./build/kdbsvr.exe init "disk://$ROOT/build/perf/db-rel" > $out/rel-init.log 2>&1; then
  echo "FATAL: store init failed; log:"; tail -5 $out/rel-init.log
  echo "rounds_run=0 rounds_without_full_success=$RUNS"; exit 1
fi
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://$ROOT/build/perf/db-rel" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/rel-srv.log 2>&1 &
srvpid=$!
# Certify readiness instead of trusting a fixed sleep: poll STATS and abort loudly.
ready=0
for w in 1 2 3 4 5 6 7 8 9 10; do
  if timeout 5 ./build/kdbctl.exe 127.0.0.1:$PORT STATS >/dev/null 2>&1; then ready=1; break; fi
  sleep 1
done
if [ "$ready" != 1 ]; then
  echo "FATAL: server not answering after 10s; server log:"; tail -12 $out/rel-srv.log
  echo "rounds_run=0 rounds_without_full_success=$RUNS"; exit 1
fi
echo "alive at start: $(ps -W | grep -ci kdbsvr)"
fails=0
for i in $(seq 1 $RUNS); do
  res=$(timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET rl_$i payload 2>&1 | tr -d '\r' | grep -E "^pipe mode|^pipe: stalled|^error" | head -1)
  alive=$(ps -W | grep -ci kdbsvr)
  echo "run$i alive=$alive ${res:-<no report>}"
  # Judge against the CONFIGURED N, not a hard-coded 4000: with N overridden the old pattern
  # matched nothing and every round was counted as a failure.
  case "$res" in *"ok=$N"*) : ;; *) fails=$((fails+1)) ;; esac
  if [ "$alive" = "0" ]; then echo "=== server died during round $i ==="; break; fi
done
echo "rounds_run=$i rounds_without_full_success=$fails"
echo "=== final liveness: $(ps -W | grep -ci kdbsvr) ==="
echo "=== server log ==="
tail -12 $out/rel-srv.log
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
