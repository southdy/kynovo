#!/bin/bash
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl4 && mkdir -p build/cl4
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://$ROOT/build/cl4/n$i" >/dev/null || echo "init $i FAILED"; done
SPEC="1@127.0.0.1:8111:8211,2@127.0.0.1:8112:8212,3@127.0.0.1:8113:8213"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://$ROOT/build/cl4/n$1" "$SPEC" > build/cl4/s$1.log 2>&1 & }
start_node 1 8111 8211
start_node 2 8112 8212
start_node 3 8113 8213
sleep 8
echo "alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
for i in 1 2 3; do echo "s$i.log_bytes=$(wc -c < build/cl4/s$i.log)"; sed -n '1,2p' build/cl4/s$i.log; done
LEADER=""
for p in 8111 8112 8113; do
  if timeout 60 ./build/bench_rate.exe 127.0.0.1:$p 1 1 1 __e$p 2>/dev/null | grep -q PHASE; then LEADER=$p; break; fi
done
echo "LEADER=$LEADER"
[ -z "$LEADER" ] && { echo "NO LEADER"; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL4_DONE; exit 1; }
echo "--- 3-node commit throughput (K=8) ---"
timeout 120 ./build/bench_rate.exe 127.0.0.1:$LEADER 1000 8 16 __k__ > build/cl4/w.txt 2>&1
grep -E "^PHASE|^LAT" build/cl4/w.txt
echo "--- write a known value through the leader ---"
printf 'SET __probe__ hello-cluster\nGET __probe__\n' | timeout 60 ./build/kdbctl.exe 127.0.0.1:$LEADER 2>&1 | head -4
echo "--- kill the leader ($LEADER) ---"
KPID=$(netstat -ano 2>/dev/null | grep -E ":$LEADER .*LISTENING" | awk '{print $NF}' | head -1)
taskkill /F /PID "$KPID" >/dev/null 2>&1
echo "leader_winpid=$KPID alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
t0=$(date +%s%N); NEW=""
for i in $(seq 1 200); do
  for p in 8111 8112 8113; do
    [ "$p" = "$LEADER" ] && continue
    if timeout 60 ./build/bench_rate.exe 127.0.0.1:$p 1 1 1 __f$i 2>/dev/null | grep -q PHASE; then NEW=$p; break 2; fi
  done
done
t1=$(date +%s%N)
echo "FAILOVER new_leader=$NEW time_ms=$(( (t1-t0)/1000000 )) (resolution: one blocking write probe)"
echo "--- SURVIVAL: the value written before the failover ---"
printf 'GET __probe__\n' | timeout 60 ./build/kdbctl.exe 127.0.0.1:$NEW 2>&1 | head -4
if printf 'GET __probe__\n' | timeout 60 ./build/kdbctl.exe 127.0.0.1:$NEW 2>/dev/null | grep -q hello-cluster; then echo "SURVIVAL=PASS"; else echo "SURVIVAL=NO_EVIDENCE"; fi
echo "--- post-failover throughput (2 nodes) ---"
timeout 180 ./build/bench_rate.exe 127.0.0.1:$NEW 500 8 16 __k2__ > build/cl4/w2.txt 2>&1
grep -E "^PHASE|^LAT" build/cl4/w2.txt
echo "--- restart the killed node and check rejoin ---"
case $LEADER in 8111) RP=8111; start_node 1 8111 8211;; 8112) RP=8112; start_node 2 8112 8212;; 8113) RP=8113; start_node 3 8113 8213;; esac
sleep 15
echo "alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe) restarted_port=$RP"
printf 'GET __probe__\n' | timeout 60 ./build/kdbctl.exe 127.0.0.1:$RP 2>&1 | head -3
if printf 'GET __probe__\n' | timeout 60 ./build/kdbctl.exe 127.0.0.1:$RP 2>/dev/null | grep -q hello-cluster; then echo "REJOIN_READ=PASS"; else echo "REJOIN_READ=NO_EVIDENCE"; fi
for f in build/cl4/s*.log; do echo "== $f"; tail -2 "$f"; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL4_DONE
