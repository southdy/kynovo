#!/bin/bash
# Verify the new one-shot kdbctl (no TTY) and the ok/err labelling of bench_rate.
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
K=./build/kdbctl.exe
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM kdbctl.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl13 && mkdir -p build/cl13
echo "=== single node: one-shot SET / GET / STATS / unknown (no tty, plain pipes) ==="
./build/kdbsvr.exe init "disk://$ROOT/build/cl13/a" >/dev/null
(./build/kdbsvr.exe server 1 8401 8402 "disk://$ROOT/build/cl13/a" "1@127.0.0.1:8401:8402" > build/cl13/a.log 2>&1 &)
sleep 6
printf "  SET -> "; timeout 30 $K 127.0.0.1:8401 SET __probe__ hello-cluster 2>&1 | head -2 | tr '\n' ' '; echo
printf "  GET -> "; timeout 30 $K 127.0.0.1:8401 GET __probe__ 2>&1 | head -2 | tr '\n' ' '; echo
printf "  STATS -> "; timeout 30 $K 127.0.0.1:8401 STATS 2>&1 | head -1 | cut -c1-120; echo
printf "  BOGUS -> "; timeout 30 $K 127.0.0.1:8401 BOGUS 2>&1 | head -1; echo
printf "  bench_rate valid-label on a lone node -> "; timeout 60 $B 127.0.0.1:8401 10 2 16 __v 0 2>&1 | grep -E '^PHASE_VALID|^PROBE' | tr '\n' ' '; echo
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
echo "=== 3 nodes: one-shot against a FOLLOWER must follow the redirect (dissertation 6.2) ==="
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://$ROOT/build/cl13/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8411:8421,2@127.0.0.1:8412:8422,3@127.0.0.1:8413:8423"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://$ROOT/build/cl13/n$1" "$SPEC" > build/cl13/s$1.log 2>&1 & }
start_node 1 8411 8421
start_node 2 8412 8422
start_node 3 8413 8423
sleep 8
LEADER=""; FOLLOWER=""
for p in 8411 8412 8413; do
  line=$(timeout 30 $K 127.0.0.1:$p STATS 2>/dev/null | head -1)
  st=$(echo "$line" | tr ' ' '\n' | grep -m1 '^state=' | cut -d= -f2)
  echo "  port $p state=$st"
  if [ "$st" = "3" ]; then LEADER=$p; else [ -z "$FOLLOWER" ] && FOLLOWER=$p; fi
done
echo "  LEADER=$LEADER FOLLOWER=$FOLLOWER"
if [ -n "$LEADER" ] && [ -n "$FOLLOWER" ]; then
  printf "  one-shot SET on the LEADER   -> "; timeout 30 $K 127.0.0.1:$LEADER SET __shared__ yes-on-leader 2>&1 | head -1; echo
  printf "  one-shot GET on the FOLLOWER -> "; timeout 30 $K 127.0.0.1:$FOLLOWER GET __shared__ 2>&1 | head -2 | tr '\n' ' '; echo
  printf "  bench_rate at the FOLLOWER (must report redirect + follow) -> "
  timeout 120 $B 127.0.0.1:$FOLLOWER 20 2 16 __x 1 2>&1 | grep -E '^PROBE|^REDIRECT|^TARGET|^PHASE_VALID|^PHASE' | tr '\n' ' '; echo
fi
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo CL13_DONE
