#!/bin/bash
# Honest 3-node baseline: the LEADER is resolved from STATS (state=3), never guessed, and the
# follower case is included to show what a rejected write looks like (RESP| body + empty read).
cd "$(dirname "$0")/../.." || exit 1
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl10 && mkdir -p build/cl10
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://D:/kynovo/build/cl10/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8181:8281,2@127.0.0.1:8182:8282,3@127.0.0.1:8183:8283"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://D:/kynovo/build/cl10/n$1" "$SPEC" > build/cl10/s$1.log 2>&1 & }
start_node 1 8181 8281
start_node 2 8182 8282
start_node 3 8183 8283
sleep 8
STATS(){ timeout 60 $B 127.0.0.1:$1 1 1 1 __s 0 2>/dev/null | grep -m1 '^STATS_BEFORE'; }
F(){ echo "$1" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2; }
LEADER=""; FOLLOWER=""
for p in 8181 8182 8183; do
  S=$(STATS $p); st=$(F "$S" state)
  echo "port $p state=$st commit=$(F "$S" commit) wal_records=$(F "$S" wal_records)"
  if [ "$st" = "3" ]; then LEADER=$p; else [ -z "$FOLLOWER" ] && FOLLOWER=$p; fi
done
echo "LEADER=$LEADER FOLLOWER=$FOLLOWER alive=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
[ -z "$LEADER" ] && { echo NO_LEADER; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL10_DONE; exit 1; }
echo "=== A) what a write to a FOLLOWER really is ==="
timeout 120 $B 127.0.0.1:$FOLLOWER 200 32 37 __f__ 1 > build/cl10/f.txt 2>&1
grep -E '^PHASE|^LAT|^RESP' build/cl10/f.txt
printf "read back from the follower: "; timeout 60 $B 127.0.0.1:$FOLLOWER 50 1 37 __f__ 1 get 2>&1 | grep -E '^GET\|'
echo "=== B) the honest baseline: writes to the LEADER (state=3) ==="
for K in 8 32 128; do
  L0=$(STATS $LEADER)
  timeout 240 $B 127.0.0.1:$LEADER 500 $K 37 "__k${K}_" 1 > build/cl10/k$K.txt 2>&1
  L1=$(STATS $LEADER)
  printf "K=%-4s %s | %s\n" "$K" "$(grep -E '^PHASE' build/cl10/k$K.txt|sed 's/PHASE|//')" "$(grep -E '^LAT' build/cl10/k$K.txt|sed 's/LAT|//')"
  grep -E '^RESP' build/cl10/k$K.txt
  printf "     leader deltas: wal_records %s->%s flush_batches %s->%s commit %s->%s applied %s->%s\n" \
    "$(F "$L0" wal_records)" "$(F "$L1" wal_records)" "$(F "$L0" flush_batches)" "$(F "$L1" flush_batches)" \
    "$(F "$L0" commit)" "$(F "$L1" commit)" "$(F "$L0" applied)" "$(F "$L1" applied)"
  printf "     read back 50 of them: "; timeout 60 $B 127.0.0.1:$LEADER 50 1 37 "__k${K}_" 1 get 2>&1 | grep -E '^GET\|'
done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL10_DONE
