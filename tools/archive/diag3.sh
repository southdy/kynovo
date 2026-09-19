#!/bin/bash
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
SPEC="1@127.0.0.1:8101:8201 2@127.0.0.1:8102:8202 3@127.0.0.1:8103:8203"
./build/kdbsvr.exe server 1 8101 8201 "disk://$ROOT/build/cl3/n1" "$SPEC" > build/cl3/x1.log 2>&1 &
A=$!
./build/kdbsvr.exe server 2 8102 8202 "disk://$ROOT/build/cl3/n2" "$SPEC" > build/cl3/x2.log 2>&1 &
B=$!
./build/kdbsvr.exe server 3 8103 8203 "disk://$ROOT/build/cl3/n3" "$SPEC" > build/cl3/x3.log 2>&1 &
C=$!
sleep 7
echo "msys_pids=$A,$B,$C"
echo "alive_win=$(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
echo "--- listeners on 81xx/82xx ---"
netstat -ano 2>/dev/null | grep -E ":8[12][0-9][0-9] " | head -10
echo "--- log sizes ---"
wc -c build/cl3/x1.log build/cl3/x2.log build/cl3/x3.log 2>/dev/null | head -4
echo "--- probe each client port (SET then GET) ---"
for p in 8101 8102 8103; do
  printf "SET %s: " $p
  ./build/bench_rate.exe 127.0.0.1:$p 1 1 1 __z 2>&1 | head -2 | tr '\n' ' '
  echo
done
echo "--- simple TCP connect test ---"
for p in 8101 8102 8103 8201 8202 8203; do
  printf "connect %s: " $p
  (exec 3<>/dev/tcp/127.0.0.1/$p) 2>/dev/null && echo OK || echo REFUSED
done
kill -9 $A $B $C 2>/dev/null
sleep 1
echo "after kill: $(ps -W 2>/dev/null | grep -c kdbsvr.exe)"
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo DIAG_DONE
