#!/bin/bash
# Why did all three ports report state=3 (RAFT_LEADER)?  Distinguish:
#   (1) genuine concurrent leadership  -> terms equal, all self-leader, writes diverging
#   (2) three independent single-node clusters -> peer links broken, writes not shared
cd "$(dirname "$0")/../.." || exit 1
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
B=./build/bench_rate.exe
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
sleep 1
rm -rf build/cl12 && mkdir -p build/cl12
for i in 1 2 3; do ./build/kdbsvr.exe init "disk://D:/kynovo/build/cl12/n$i" >/dev/null; done
SPEC="1@127.0.0.1:8201:8301,2@127.0.0.1:8202:8302,3@127.0.0.1:8203:8303"
start_node(){ ./build/kdbsvr.exe server $1 $2 $3 "disk://D:/kynovo/build/cl12/n$1" "$SPEC" > build/cl12/s$1.log 2>&1 & }
start_node 1 8201 8301
start_node 2 8202 8302
start_node 3 8203 8303
sleep 8
show(){  # full identity fields of one node
  local line=$(timeout 60 $B 127.0.0.1:$1 1 1 1 __i 0 2>/dev/null | grep -m1 '^STATS_BEFORE')
  printf "  port %s: %s\n" "$1" "$(echo "$line" | cut -d'|' -f2 | cut -c1-160)"
}
echo "=== roles after 8s (first fields are id/state/leader/term/commit/applied/LII/log) ==="
for p in 8201 8202 8203; do show $p; done
echo "=== 3s later ==="
sleep 3
for p in 8201 8202 8203; do show $p; done
echo "=== do all three ACCEPT a write (each thinks it is leader)? ==="
for p in 8201 8202 8203; do
  printf "  port %s: " $p
  timeout 60 $B 127.0.0.1:$p 1 1 11 __w$p 0 2>&1 | grep -E '^(PROBE|RESP)' | tr '\n' ' '
  echo
done
echo "=== is the cluster SHARED? write a key on 8201 and read it on 8202/8203 ==="
timeout 60 $B 127.0.0.1:8201 1 1 11 __shared__ 0 >/dev/null 2>&1
sleep 1
for p in 8201 8202 8203; do
  printf "  read on %s: " $p
  timeout 60 $B 127.0.0.1:$p 1 1 11 __shared__ 0 get 2>&1 | grep -E '^GET\|'
done
echo "=== peer ports listening? ==="
netstat -ano 2>/dev/null | grep -E ":(8301|8302|8303) " | awk '{print $2, $4}' | sort -u | head -6
echo "=== server logs ==="
for f in build/cl12/s*.log; do echo "  == $f"; head -4 "$f"; done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM bench_rate.exe >/dev/null 2>&1
echo CL12_DONE
