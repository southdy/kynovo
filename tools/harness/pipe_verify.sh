#!/bin/bash
# pipe_verify.sh -- pipelined load through the REAL client (kdbctl PIPE) + its verification gates:
#   gate 1: PIPE SET  <K> <n> SET <prefix> <value>   -> ok == n        (accepted)
#   gate 2: PIPE GET  <K> <n> GET <prefix>           -> ok == n, not_found == 0  (actually stored)
#   gate 3: PIPE GET  <K> 8   GET <never-written>    -> ok == 0, not_found == 8  (negative control:
#                                                        the check can fail, so gate 2 means something)
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
N=${N:-4000}
PORT=${PORT:-9401}
BASE=${BASE:-$ROOT/build/perf/db-pipe}
rm -rf $out/db-pipe
./build/kdbsvr.exe init "disk://$BASE" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://$BASE" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/pipe-srv.log 2>&1 &
sleep 6
echo "=== gate 1/2/3 @ K=${1:-128} ==="
K=${1:-128}
./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET pipeval_ payload 2>&1 | grep -E "^pipe|^error|^warning"
./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N GET pipeval_ 2>&1 | grep -E "^pipe|^error|^warning"
./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K 8 GET zzz_never_ 2>&1 | grep -E "^pipe|^error|^warning"
echo "=== 吞吐/延时前沿（真实客户端）==="
for k in 32 64 128 512; do
  ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $k $N SET sweep${k}_ payload 2>&1 | grep -E "^pipe mode" | sed "s/^/  /"
done
./build/kdbctl.exe 127.0.0.1:$PORT STATS 2>&1 | tr ' ' '\n' | grep -E "^(rounds|client_requests|flush_batches|flush_by_target|sync_us_ewma|window_ms|wal_records)=" | tr '\n' ' '
echo
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
