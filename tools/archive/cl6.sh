#!/bin/bash
# cold vs warm on ONE 3-node cluster with the SAME leader and the SAME workload shape.
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl6 && mkdir -p build/cl6
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://$ROOT/build/cl6/n$i" >/dev/null || echo "init $i FAILED"; done
SPEC="1@127.0.0.1:8131:8231,2@127.0.0.1:8132:8232,3@127.0.0.1:8133:8233"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://$ROOT/build/cl6/n$1" "$SPEC" > build/cl6/s$1.log 2>&1 & }
start_node 1 8131 8231
start_node 2 8132 8232
start_node 3 8133 8233
sleep 8
LEADER=""
for p in 8131 8132 8133; do
  if timeout 60 $B 127.0.0.1:$p 1 1 1 __e$p 0 2>/dev/null | grep -q PHASE; then LEADER=$p; break; fi
done
echo "LEADER=$LEADER alive=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
[ -z "$LEADER" ] && { echo NO_LEADER; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL6_DONE; exit 1; }
phase(){
  timeout 240 $B 127.0.0.1:$LEADER 500 8 37 "$2" 1 > build/cl6/$1.txt 2>&1
  printf "%-6s %s | %s\n" "$1" "$(grep -E '^PHASE' build/cl6/$1.txt | sed 's/PHASE|//')" "$(grep -E '^LAT' build/cl6/$1.txt | sed 's/LAT|//')"
  grep -m1 '^STATS_AFTER' build/cl6/$1.txt | tr ' ' '\n' | grep -E '^(snapshot=|wal_records=|flush_batches=|flush_by_window=|flush_by_target=|flush_by_drain=|rounds=)' | tr '\n' ' '
  echo
  sleep 3
}
echo "--- same leader, same shape (K=8, 37B, 500 distinct keys), four phases ---"
phase cold  __a__
phase warm2 __b__
phase warm3 __c__
phase warm4 __d__
echo "--- leader STATS at the end ---"
timeout 60 $B 127.0.0.1:$LEADER 1 1 1 __z 0 2>&1 | grep -m1 '^STATS_BEFORE' | tr ' ' '\n' | grep -E '^(id=|state=|leader=|term=|commit=|applied=|snapshot=|log=|count=|wal_records=|flush_batches=|rounds=|client_requests=)' | tr '\n' ' '
echo
for f in build/cl6/s*.log; do echo "== $f"; tail -2 "$f"; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL6_DONE
