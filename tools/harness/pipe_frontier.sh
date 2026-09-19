#!/bin/bash
# pipe_frontier.sh -- throughput/latency frontier through the REAL client (kdbctl PIPE), with the
# three verification gates.  Latency is per-response (the app-layer clock is refreshed on every
# inbound event and on every send).
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
N=${N:-4000}
PORT=${PORT:-9411}
rm -rf $out/db-front
./build/kdbsvr.exe init "disk://$ROOT/build/perf/db-front" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://$ROOT/build/perf/db-front" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/front-srv.log 2>&1 &
sleep 6
echo "=== disk frontier (SET, N=$N) ==="
for k in 32 64 128 256 512 1024; do
  timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $k $N SET fr_ payload 2>&1 | grep -E "^pipe" | sed "s/^/K=$k /"
done
echo "=== gates @ K=128 ==="
timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE 128 $N SET gate_ payload 2>&1 | grep -E "^pipe mode"
timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE 128 $N GET gate_ 2>&1 | grep -E "^pipe mode"
timeout 20 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE 128 8 GET zzz_never_ 2>&1 | grep -E "^pipe mode"
echo "=== GET frontier (same keys) ==="
for k in 128 512; do
  timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $k $N GET fr_ 2>&1 | grep -E "^pipe mode" | sed "s/^/K=$k /"
done
echo "=== stats ==="
./build/kdbctl.exe 127.0.0.1:$PORT STATS 2>&1 | tr ' ' '\n' | grep -E "^(rounds|client_requests|flush_batches|flush_by_target|sync_us_ewma|window_ms|wal_records)=" | tr '\n' ' '
echo
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
