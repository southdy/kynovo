#!/bin/bash
# Decisive check: is a client write ACKED before the leader fsyncs it?
# For one single write (K=1) we sample the LEADER's counters immediately before the write,
# immediately after the ack, and again 200ms later.  If the ack arrives while the record is
# in the file but NOT yet flushed (flush_batches did not advance at ack time), the client was
# acked before durability.
cd "$(dirname "$0")/../.." || exit 1
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl8 && mkdir -p build/cl8
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://D:/kynovo/build/cl8/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8161:8261,2@127.0.0.1:8162:8262,3@127.0.0.1:8163:8263"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://D:/kynovo/build/cl8/n$1" "$SPEC" > build/cl8/s$1.log 2>&1 & }
start_node 1 8161 8261
start_node 2 8162 8262
start_node 3 8163 8263
sleep 8
# identify port -> id / role from STATS
stat_field(){ timeout 60 $B 127.0.0.1:$1 1 1 1 __q 0 2>/dev/null | grep -m1 '^STATS_BEFORE' | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2; }
LEADER_PORT=""
for p in 8161 8162 8163; do
  id=$(stat_field $p id); st=$(stat_field $p state); ld=$(stat_field $p leader)
  echo "port $p: id=$id state=$st leader=$ld"
  if [ "$st" = "3" ]; then LEADER_PORT=$p; fi
done
echo "LEADER_PORT=$LEADER_PORT alive=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
[ -z "$LEADER_PORT" ] && { echo NO_LEADER; taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; echo CL8_DONE; exit 1; }
# make sure there is steady write traffic so batches are in flight, then probe single writes
timeout 120 $B 127.0.0.1:$LEADER_PORT 400 32 37 __bg__ 1 > build/cl8/bg.txt 2>&1
grep -E '^PHASE' build/cl8/bg.txt
echo "--- 6 single writes: leader counters at ack time vs 200ms later ---"
printf "%-4s %-12s %-24s %-24s %s\n" itr ack_lat_us "leader@before->ack" "leader@ack->+200ms" verdict
for i in 1 2 3 4 5 6; do
  A=$(timeout 60 $B 127.0.0.1:$LEADER_PORT 1 1 1 __az 0 2>/dev/null | grep -m1 '^STATS_BEFORE')
  walA=$(echo "$A" | tr ' ' '\n' | grep -m1 '^wal_records=' | cut -d= -f2)
  flA=$(echo "$A"  | tr ' ' '\n' | grep -m1 '^flush_batches=' | cut -d= -f2)
  LAT=$(timeout 60 $B 127.0.0.1:$LEADER_PORT 1 1 37 __one$i 0 2>&1 | grep -m1 '^LAT' | sed 's/LAT|//')
  Bb=$(timeout 60 $B 127.0.0.1:$LEADER_PORT 1 1 1 __bz 0 2>/dev/null | grep -m1 '^STATS_BEFORE')
  walB=$(echo "$Bb" | tr ' ' '\n' | grep -m1 '^wal_records=' | cut -d= -f2)
  flB=$(echo "$Bb"  | tr ' ' '\n' | grep -m1 '^flush_batches=' | cut -d= -f2)
  ack=$(echo "$LAT" | tr ' ' '\n' | grep -m1 '^max=' | cut -d= -f2)
  sleep 0.2
  C=$(timeout 60 $B 127.0.0.1:$LEADER_PORT 1 1 1 __cz 0 2>/dev/null | grep -m1 '^STATS_BEFORE')
  walC=$(echo "$C" | tr ' ' '\n' | grep -m1 '^wal_records=' | cut -d= -f2)
  flC=$(echo "$C"  | tr ' ' '\n' | grep -m1 '^flush_batches=' | cut -d= -f2)
  v="ok(waited for a fsync)"
  if [ "$flB" = "$flA" ] && [ "$walB" != "$walA" ]; then v="ACK_BEFORE_FSYNC?"; fi
  printf "%-4s %-12s wal %s->%s fl %s->%s %-14s wal %s->%s fl %s->%s  %s\n" "$i" "$ack" "$walA" "$walB" "$flA" "$flB" "" "$walB" "$walC" "$flB" "$flC" "$v"
done
echo "--- read back the single-write keys from the leader ---"
timeout 60 $B 127.0.0.1:$LEADER_PORT 6 1 37 __one 1 get 2>&1 | grep -E '^(GET|VALUE)\|'
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL8_DONE
