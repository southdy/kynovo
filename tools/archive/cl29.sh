#!/bin/bash
# Does one-shot kdbctl work against a 3-node cluster (a real leader exists to answer the
# frozen follower read)?  All previous CLI tests were against a SINGLE node.
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"; export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe; K=./build/kdbctl.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1; sleep 1
rm -rf build/cl29 && mkdir -p build/cl29
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://$ROOT/build/cl29/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8771:8781,2@127.0.0.1:8772:8782,3@127.0.0.1:8773:8783"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://$ROOT/build/cl29/n$1" "$SPEC" > build/cl29/s$1.log 2>&1 & }
start_node 1 8771 8781
start_node 2 8772 8782
start_node 3 8773 8783
sleep 9
echo "=== who is the leader (probe each port; bench_rate follows the redirect hint) ==="
LEADER=""
for p in 8771 8772 8773; do
  line=$(timeout 30 $B 127.0.0.1:$p 1 1 1 k 0 members 2>&1 | grep -E '^PROBE' | head -1)
  echo "  port $p: $line"
  case "$line" in
    *"status=0"*) LEADER=$p ;;
    *leader=127.0.0.1:*) [ -z "$LEADER" ] && LEADER=$(echo "$line" | sed 's/.*leader=127.0.0.1://; s/ .*//') ;;
  esac
done
echo "LEADER=$LEADER"
echo "=== one-shot kdbctl against the LEADER (3-node cluster) ==="
printf "  SET  -> "; timeout 30 $K 127.0.0.1:$LEADER SET __p__ hello-3node 2>&1 | tr '\r' ' ' | tr '\n' '|'; echo
printf "  GET  -> "; timeout 30 $K 127.0.0.1:$LEADER GET __p__ 2>&1 | tr '\r' ' ' | tr '\n' '|'; echo
printf "  MEMBERS -> "; timeout 30 $K 127.0.0.1:$LEADER MEMBERS 2>&1 | tr '\r' ' ' | tr '\n' '|'; echo
echo "=== and against a FOLLOWER (must follow the redirect) ==="
FOLLOWER=""; for p in 8771 8772 8773; do [ "$p" != "$LEADER" ] && FOLLOWER=$p; done
printf "  GET via follower $FOLLOWER -> "; timeout 30 $K 127.0.0.1:$FOLLOWER GET __p__ 2>&1 | tr '\r' ' ' | tr '\n' '|'; echo
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo CL29_DONE
