#!/bin/bash
# conn_leak.sh -- does a server that has served several short-lived clients keep accepting new ones?
# Each invocation below is a fresh process => a fresh connection (the CLI's normal usage pattern).
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
out=build/perf
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-leak
./build/kdbsvr.exe init "disk://D:/kynovo/build/perf/db-leak" >/dev/null 2>&1
./build/kdbsvr.exe server 1 9441 9442 "disk://D:/kynovo/build/perf/db-leak" "1@127.0.0.1:9441:9442" > $out/leak-srv.log 2>&1 &
sleep 6
echo "=== sequential short-lived clients (each = 1 connection) ==="
fail=0
for i in $(seq 1 40); do
  outp=$(timeout 15 ./build/kdbctl.exe 127.0.0.1:9441 SET lk_$i v 2>&1 | tr -d '\r' | tail -1)
  case "$outp" in
    ok) : ;;
    *) fail=$((fail+1)); echo "  client #$i -> [$outp]  (first failures start here)"; [ $fail -ge 3 ] && break ;;
  esac
done
echo "connections attempted=$i failures=$fail"
echo "=== server stats ==="
timeout 15 ./build/kdbctl.exe 127.0.0.1:9441 STATS 2>&1 | tr -d '\r' | tr ' ' '\n' | grep -E "^(client_requests|connection|conn|rounds)=" | tr '\n' ' '
echo
echo "=== server log ==="
tail -12 $out/leak-srv.log
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
