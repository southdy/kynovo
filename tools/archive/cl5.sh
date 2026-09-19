#!/bin/bash
cd "$(dirname "$0")/../.." || exit 1
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl5 && mkdir -p build/cl5
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://D:/kynovo/build/cl5/n$i" >/dev/null || echo "init $i FAILED"; done
SPEC="1@127.0.0.1:8121:8221,2@127.0.0.1:8122:8222,3@127.0.0.1:8123:8223"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://D:/kynovo/build/cl5/n$1" "$SPEC" > build/cl5/s$1.log 2>&1 & }
start_node 1 8121 8221
start_node 2 8122 8222
start_node 3 8123 8223
sleep 8
echo "alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
for i in 1 2 3; do echo "s$i: $(head -2 build/cl5/s$i.log | tr '\n' '|')"; done
LEADER=""
for p in 8121 8122 8123; do
  if timeout 60 $B 127.0.0.1:$p 1 1 1 __e$p 0 2>/dev/null | grep -q PHASE; then LEADER=$p; break; fi
done
echo "LEADER=$LEADER"
[ -z "$LEADER" ] && { echo "NO LEADER"; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL5_DONE; exit 1; }
echo "--- write 200 distinct keys, 37-byte values (leader) ---"
timeout 180 $B 127.0.0.1:$LEADER 200 8 37 __k__ 1 > build/cl5/w.txt 2>&1
grep -E "^PHASE|^LAT" build/cl5/w.txt
echo "--- READ BACK through the leader (key __k__7 must be 37 bytes) ---"
timeout 60 $B 127.0.0.1:$LEADER 1 1 37 __k__7 1 get 2>&1 | head -3
echo "--- sanity: a key that was never written ---"
timeout 60 $B 127.0.0.1:$LEADER 1 1 37 __nope__ 0 get 2>&1 | head -3
echo "--- kill the leader ($LEADER) ---"
KPID=$(netstat -ano 2>/dev/null | grep -E ":$LEADER .*LISTENING" | awk '{print $NF}' | head -1)
taskkill /F /PID "$KPID" >/dev/null 2>&1
echo "leader_winpid=$KPID alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
t0=$(date +%s%N); NEW=""
for i in $(seq 1 200); do
  for p in 8121 8122 8123; do
    [ "$p" = "$LEADER" ] && continue
    if timeout 60 $B 127.0.0.1:$p 1 1 1 __f$i 0 2>/dev/null | grep -q PHASE; then NEW=$p; break 2; fi
  done
done
t1=$(date +%s%N)
echo "FAILOVER new_leader=$NEW time_ms=$(( (t1-t0)/1000000 ))"
echo "--- SURVIVAL: read committed keys through the NEW leader ---"
timeout 60 $B 127.0.0.1:$NEW 1 1 37 __k__7 1 get 2>&1 | head -3
S=$(timeout 60 $B 127.0.0.1:$NEW 20 1 37 __k__ 1 get 2>&1 | grep "^GET|")
echo "survival_20keys: $S"
echo "$S" | grep -q "ok=20 empty=0 err=0" && echo "SURVIVAL=PASS" || echo "SURVIVAL=FAIL_OR_PARTIAL"
echo "--- post-failover throughput (2 nodes) ---"
timeout 180 $B 127.0.0.1:$NEW 500 8 16 __k2__ 1 > build/cl5/w2.txt 2>&1
grep -E "^PHASE|^LAT" build/cl5/w2.txt
echo "--- restart the killed node ---"
case $LEADER in 8121) RP=8121; start_node 1 8121 8221;; 8122) RP=8122; start_node 2 8122 8222;; 8123) RP=8123; start_node 3 8123 8223;; esac
sleep 15
echo "alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe) restarted=$RP"
echo "--- REJOIN: read the pre-failover key from the restarted node ---"
timeout 60 $B 127.0.0.1:$RP 5 1 37 __k__ 1 get 2>&1 | head -3
R=$(timeout 60 $B 127.0.0.1:$RP 20 1 37 __k__ 1 get 2>&1 | grep "^GET|")
echo "rejoin_20keys: $R"
echo "$R" | grep -q "ok=20 empty=0 err=0" && echo "REJOIN=PASS" || echo "REJOIN=FAIL_OR_PARTIAL"
echo "--- 3-node again after rejoin ---"
timeout 120 $B 127.0.0.1:$NEW 500 8 16 __k3__ 1 > build/cl5/w3.txt 2>&1
grep -E "^PHASE|^LAT" build/cl5/w3.txt
for f in build/cl5/s*.log; do echo "== $f"; tail -2 "$f"; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL5_DONE
