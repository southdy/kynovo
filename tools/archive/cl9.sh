#!/bin/bash
# (a) Nail the fast path: write a burst to a FOLLOWER (relay path) and sample BOTH the
# follower's and the LEADER's counters around it.  If the leader fsyncs+commits the burst
# (flush_batches/commit advance at the same order as the writes) then the high throughput
# is a legitimate relayed/pipelined commit, not an early ack.
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl9 && mkdir -p build/cl9
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://$ROOT/build/cl9/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8171:8271,2@127.0.0.1:8172:8272,3@127.0.0.1:8173:8273"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://$ROOT/build/cl9/n$1" "$SPEC" > build/cl9/s$1.log 2>&1 & }
start_node 1 8171 8271
start_node 2 8172 8272
start_node 3 8173 8273
sleep 8
STATS(){ timeout 60 $B 127.0.0.1:$1 1 1 1 __s 0 2>/dev/null | grep -m1 '^STATS_BEFORE' ; }
F(){ echo "$1" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2; }
LEADER=""; FOLLOWER=""
for p in 8171 8172 8173; do
  S=$(STATS $p); st=$(F "$S" state); echo "port $p state=$st commit=$(F "$S" commit) applied=$(F "$S" applied)"
  if [ "$st" = "3" ]; then LEADER=$p; else [ -z "$FOLLOWER" ] && FOLLOWER=$p; fi
done
echo "LEADER=$LEADER FOLLOWER=$FOLLOWER alive=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
[ -z "$LEADER" ] && { echo NO_LEADER; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL9_DONE; exit 1; }
[ -z "$FOLLOWER" ] && { echo NO_FOLLOWER; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL9_DONE; exit 1; }
echo "--- write a 500-key burst to the FOLLOWER ($FOLLOWER) ---"
L0=$(STATS $LEADER); W0=$(STATS $FOLLOWER)
timeout 240 $B 127.0.0.1:$FOLLOWER 500 32 37 __r__ 1 > build/cl9/burst.txt 2>&1
grep -E '^PHASE|^LAT' build/cl9/burst.txt
L1=$(STATS $LEADER); W1=$(STATS $FOLLOWER)
row(){ printf "%-9s wal_records %s->%s  flush_batches %s->%s  flush_writes %s->%s  commit %s->%s  applied %s->%s  client_requests %s->%s\n" "$1" \
  "$(F "$2" wal_records)" "$(F "$3" wal_records)" \
  "$(F "$2" flush_batches)" "$(F "$3" flush_batches)" \
  "$(F "$2" flush_writes)" "$(F "$3" flush_writes)" \
  "$(F "$2" commit)" "$(F "$3" commit)" \
  "$(F "$2" applied)" "$(F "$3" applied)" \
  "$(F "$2" client_requests)" "$(F "$3" client_requests)"; }
row LEADER "$L0" "$L1"
row FOLLOWER "$W0" "$W1"
echo "--- immediately read the burst keys back from BOTH nodes (linearizability check) ---"
printf "from leader:   "; timeout 60 $B 127.0.0.1:$LEADER 100 1 37 __r__ 1 get 2>&1 | grep -E '^GET\|'
printf "from follower: "; timeout 60 $B 127.0.0.1:$FOLLOWER 100 1 37 __r__ 1 get 2>&1 | grep -E '^GET\|'
echo "--- and a second burst to the LEADER for contrast ---"
timeout 240 $B 127.0.0.1:$LEADER 500 32 37 __l__ 1 > build/cl9/burst2.txt 2>&1
grep -E '^PHASE|^LAT' build/cl9/burst2.txt
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL9_DONE
