#!/bin/bash
# watch_counters.sh -- run repeated pipelined loads on ONE server and print the admission-related
# counters after each run.  A counter that grows monotonically is the leak behind
# "the Nth client gets a silently closed connection" (K_REQUEST_INFLIGHT_MAX = 16384).
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
out=build/perf
PORT=${PORT:-9451}
N=${N:-4000}
K=${K:-128}
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-watch
./build/kdbsvr.exe init "disk://D:/kynovo/build/perf/db-watch" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://D:/kynovo/build/perf/db-watch" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/watch-srv.log 2>&1 &
sleep 6
stats(){
  timeout 15 ./build/kdbctl.exe 127.0.0.1:$PORT STATS 2>&1 | tr -d '\r' | tr ' ' '\n' \
    | grep -E "^(client_requests|wal_inflight_count|client_connection_count|request_count|request_bytes|rounds|flush_batches)=" | tr '\n' ' '
}
echo "run0 (before): $(stats)"
for i in $(seq 1 ${RUNS:-6}); do
  res=$(timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET w_$i payload 2>&1 | tr -d '\r' | grep -E "^pipe mode|^pipe: stalled" | head -1)
  echo "run$i: ${res:-<no report>}"
  echo "      after: $(stats)"
done
echo "=== server alive: $(ps -W | grep -c -i kdbsvr) ==="
echo "=== server log (fatal/warning would appear here) ==="
tail -20 $out/watch-srv.log
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
