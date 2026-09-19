#!/bin/bash
# dissertation 6.2 conformance test: point the probe at a FOLLOWER and check that it is told
# the leader's address and reconnects there (the recommended option 1), so a measured phase
# on a follower produces the same committed data as one on the leader.
cd "$(dirname "$0")/../.." || exit 1
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl11 && mkdir -p build/cl11
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://D:/kynovo/build/cl11/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8191:8291,2@127.0.0.1:8192:8292,3@127.0.0.1:8193:8293"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://D:/kynovo/build/cl11/n$1" "$SPEC" > build/cl11/s$1.log 2>&1 & }
start_node 1 8191 8291
start_node 2 8192 8292
start_node 3 8193 8293
sleep 8
STATS(){ timeout 60 $B 127.0.0.1:$1 1 1 1 __s 0 2>/dev/null | grep -m1 '^STATS_BEFORE'; }
F(){ echo "$1" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2; }
LEADER=""; FOLLOWER=""
for p in 8191 8192 8193; do
  S=$(STATS $p); st=$(F "$S" state); echo "port $p state=$st commit=$(F "$S" commit)"
  if [ "$st" = "3" ]; then LEADER=$p; else [ -z "$FOLLOWER" ] && FOLLOWER=$p; fi
done
echo "LEADER=$LEADER FOLLOWER=$FOLLOWER"
[ -z "$LEADER" ] && { echo NO_LEADER; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL11_DONE; exit 1; }
echo "=== A) point the probe at the FOLLOWER ($FOLLOWER): must be redirected to the leader ==="
L0=$(STATS $LEADER)
timeout 240 $B 127.0.0.1:$FOLLOWER 500 32 37 __a__ 1 > build/cl11/a.txt 2>&1
grep -E '^PROBE|^REDIRECT|^PHASE|^LAT|^RESP' build/cl11/a.txt
L1=$(STATS $LEADER)
printf "leader deltas: commit %s->%s applied %s->%s wal_records %s->%s flush_batches %s->%s\n" \
  "$(F "$L0" commit)" "$(F "$L1" commit)" "$(F "$L0" applied)" "$(F "$L1" applied)" \
  "$(F "$L0" wal_records)" "$(F "$L1" wal_records)" "$(F "$L0" flush_batches)" "$(F "$L1" flush_batches)"
printf "read back from the FOLLOWER: "; timeout 60 $B 127.0.0.1:$FOLLOWER 50 1 37 __a__ 1 get 2>&1 | grep -E '^GET\|'
printf "read back from the LEADER:   "; timeout 60 $B 127.0.0.1:$LEADER 50 1 37 __a__ 1 get 2>&1 | grep -E '^GET\|'
echo "=== B) point the probe at the LEADER ($LEADER) ==="
L2=$(STATS $LEADER)
timeout 240 $B 127.0.0.1:$LEADER 500 32 37 __b__ 1 > build/cl11/b.txt 2>&1
grep -E '^PROBE|^REDIRECT|^PHASE|^LAT|^RESP' build/cl11/b.txt
L3=$(STATS $LEADER)
printf "leader deltas: commit %s->%s applied %s->%s\n" "$(F "$L2" commit)" "$(F "$L3" commit)" "$(F "$L2" applied)" "$(F "$L3" applied)"
printf "read back from the LEADER:   "; timeout 60 $B 127.0.0.1:$LEADER 50 1 37 __b__ 1 get 2>&1 | grep -E '^GET\|'
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL11_DONE
