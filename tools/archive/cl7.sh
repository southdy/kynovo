#!/bin/bash
# Where does the ~8ms per-request latency live?  Discriminate LOCAL flush-window vs
# cross-node replication by running the SAME shape in a 1-node cluster and in a 3-node
# cluster (leader 1, then leader != 1 after a failover), sweeping the in-flight depth K.
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl7 && mkdir -p build/cl7
run_phase(){  # $1 label  $2 port  $3 K
  timeout 240 $B 127.0.0.1:$2 500 $3 37 "__$1" 1 > build/cl7/$1.txt 2>&1
  printf "%-14s K=%-4s %s | %s\n" "$1" "$3" "$(grep -E '^PHASE' build/cl7/$1.txt | sed 's/PHASE|//')" "$(grep -E '^LAT' build/cl7/$1.txt | sed 's/LAT|//')"
  grep -m1 '^STATS_AFTER' build/cl7/$1.txt | tr ' ' '\n' | grep -E '^(flush_by_window=|flush_by_drain=|flush_by_target=|flush_batches=|wal_records=|rounds=)' | tr '\n' ' '
  echo
}
find_writer(){ # $1 = port list; echoes the first port that accepts a write
  local found=""
  for p in $1; do if timeout 60 $B 127.0.0.1:$p 1 1 1 __p$p 0 2>/dev/null | grep -q PHASE; then found=$p; break; fi; done
  echo "$found"
}
echo "=== A) single-node cluster, K sweep (no replication at all) ==="
./build/kdbsvr.exe init "disk://$ROOT/build/cl7/a" >/dev/null
(./build/kdbsvr.exe server 1 8141 8241 "disk://$ROOT/build/cl7/a" "1@127.0.0.1:8141:8241" > build/cl7/a.log 2>&1 &)
sleep 6
for K in 8 32 128 256; do run_phase sn_K$K 8141 $K; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
echo "=== B) 3-node cluster ==="
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://$ROOT/build/cl7/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8151:8251,2@127.0.0.1:8152:8252,3@127.0.0.1:8153:8253"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://$ROOT/build/cl7/n$1" "$SPEC" > build/cl7/s$1.log 2>&1 & }
start_node 1 8151 8251
start_node 2 8152 8252
start_node 3 8153 8253
sleep 8
L1=$(find_writer "8151 8152 8153")
echo "first writer (pre-failover) = $L1  alive=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
for K in 8 32 128 256; do run_phase pre_K$K $L1 $K; done
echo "--- kill the node that has been serving ($L1) ---"
KPID=$(netstat -ano 2>/dev/null | grep -E ":$L1 .*LISTENING" | awk '{print $NF}' | head -1)
taskkill /F /PID "$KPID" >/dev/null 2>&1
sleep 1
OTHERS="8151 8152 8153"; case "$L1" in 8151) OTHERS="8152 8153";; 8152) OTHERS="8151 8153";; 8153) OTHERS="8151 8152";; esac
L2=$(find_writer "$OTHERS")
echo "writer after failover = $L2  alive=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
for K in 8 32 128 256; do run_phase post_K$K $L2 $K; done
for f in build/cl7/s*.log; do echo "== $f"; tail -2 "$f"; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL7_DONE
