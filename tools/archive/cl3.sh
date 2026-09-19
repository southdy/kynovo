#!/bin/bash
# Same-host 3-process cluster probe: real peer protocol over loopback.
cd "$(dirname "$0")/../.." || exit 1
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl3 && mkdir -p build/cl3
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://D:/kynovo/build/cl3/n$i" >/dev/null || echo "init $i FAILED"; done
SPEC="1@127.0.0.1:8101:8201,2@127.0.0.1:8102:8202,3@127.0.0.1:8103:8203"
./build/kdbsvr.exe server 1 8101 8201 "disk://D:/kynovo/build/cl3/n1" "$SPEC" > build/cl3/s1.log 2>&1 &
./build/kdbsvr.exe server 2 8102 8202 "disk://D:/kynovo/build/cl3/n2" "$SPEC" > build/cl3/s2.log 2>&1 &
./build/kdbsvr.exe server 3 8103 8203 "disk://D:/kynovo/build/cl3/n3" "$SPEC" > build/cl3/s3.log 2>&1 &
echo "started(win)=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
sleep 8
LEADER=""
for p in 8101 8102 8103; do
  if ./build/bench_rate.exe 127.0.0.1:$p 1 1 1 __e$p 2>/dev/null | grep -q PHASE; then LEADER=$p; break; fi
done
echo "ELECTED leader_port=$LEADER"
echo "--- node states (STATS via kdbctl if available) ---"
for p in 8101 8102 8103; do printf "port %s: " $p; echo "STATS" | ./build/kdbctl.exe 127.0.0.1:$p 2>&1 | tr '\n' ' ' | cut -c1-200; echo; done
if [ -z "$LEADER" ]; then echo "NO LEADER: aborting"; exit 1; fi
echo "--- multi-node commit throughput (leader $LEADER) ---"
./build/bench_rate.exe 127.0.0.1:$LEADER 2000 32 1    __clk32__ > build/cl3/k32.txt 2>&1
./build/bench_rate.exe 127.0.0.1:$LEADER 2000 1  1    __clk1__  > build/cl3/k1.txt  2>&1
./build/bench_rate.exe 127.0.0.1:$LEADER 1000 32 4096 __cl4k__  > build/cl3/v4k.txt 2>&1
./build/bench_rate.exe 127.0.0.1:$LEADER 100  32 65536 __cl64k__ > build/cl3/v64k.txt 2>&1
grep -hE "^PHASE|^LAT" build/cl3/k32.txt build/cl3/k1.txt build/cl3/v4k.txt build/cl3/v64k.txt
echo "--- committed before failover: GET __clk32__1999 ---"
echo "GET __clk32__1999" | ./build/kdbctl.exe 127.0.0.1:$LEADER 2>&1 | head -2
echo "--- failover: kill leader $LEADER ---"
KPID=$(netstat -ano 2>/dev/null | grep -E ":$LEADER .*LISTENING" | awk '{print $NF}' | head -1)
echo "leader winpid=$KPID"
taskkill /F /PID "$KPID" >/dev/null 2>&1
echo "alive after kill: $(ps | grep -c kdbsvr)"
t0=$(date +%s%N)
NEW=""
ticks=0
for i in $(seq 1 500); do
  ticks=$((ticks+1))
  for p in 8101 8102 8103; do
    [ "$p" = "$LEADER" ] && continue
    if ./build/bench_rate.exe 127.0.0.1:$p 1 1 1 __fo$i 2>/dev/null | grep -q PHASE; then NEW=$p; break 2; fi
  done
done
t1=$(date +%s%N)
echo "FAILOVER new_leader=$NEW probes=$ticks time_ms=$(( (t1-t0)/1000000 )) (resolution = 1 probe iteration)"
echo "--- data survived? GET __clk32__1999 on new leader ---"
echo "GET __clk32__1999" | ./build/kdbctl.exe 127.0.0.1:$NEW 2>&1 | head -2
echo "--- post-failover throughput (2 nodes) ---"
./build/bench_rate.exe 127.0.0.1:$NEW 1000 32 1 __clk32b__ > build/cl3/k32b.txt 2>&1
grep -hE "^PHASE|^LAT" build/cl3/k32b.txt
echo "--- restart the killed node and check it catches up ---"
case $LEADER in
  8101) ./build/kdbsvr.exe server 1 8101 8201 "disk://D:/kynovo/build/cl3/n1" "$SPEC" > build/cl3/s1b.log 2>&1 & ;;
  8102) ./build/kdbsvr.exe server 2 8102 8202 "disk://D:/kynovo/build/cl3/n2" "$SPEC" > build/cl3/s2b.log 2>&1 & ;;
  8103) ./build/kdbsvr.exe server 3 8103 8203 "disk://D:/kynovo/build/cl3/n3" "$SPEC" > build/cl3/s3b.log 2>&1 & ;;
esac
sleep 10
ok=0
for i in $(seq 1 20); do
  if echo "GET __clk32__1999" | ./build/kdbctl.exe 127.0.0.1:$NEW 2>/dev/null | grep -q 1999; then ok=$((ok+1)); fi
done
echo "rejoin: reads_ok=$ok/20 ; alive=$(ps | grep -c kdbsvr)"
echo "--- cluster logs (tail) ---"
for f in build/cl3/s*.log; do echo "== $f"; tail -3 "$f"; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo CL3_DONE
