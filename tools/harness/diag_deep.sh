#!/bin/bash
# diagnose deep-K stall: is the server closing the connection under a 2000-deep in-flight burst?
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
out=build/perf
echo "=== previous server log tail (capacity warnings / closes) ==="
tail -12 $out/front-srv.log 2>/dev/null
echo "=== leftover processes ==="
ps -W | grep -i kdbsvr | head -3
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-hang
./build/kdbsvr.exe init "disk://D:/kynovo/build/perf/db-hang" >/dev/null 2>&1
./build/kdbsvr.exe server 1 9421 9422 "disk://D:/kynovo/build/perf/db-hang" "1@127.0.0.1:9421:9422" > $out/hang-srv.log 2>&1 &
sleep 6
echo "=== single deep run: K=2000, N=2000, 40s cap ==="
timeout 40 ./build/kdbctl.exe 127.0.0.1:9421 PIPE 2000 2000 SET hh_ payload 2>&1 | tail -5
echo "cli rc=$?"
echo "=== server log ==="
tail -8 $out/hang-srv.log
echo "=== stats after ==="
./build/kdbctl.exe 127.0.0.1:9421 STATS 2>&1 | tr ' ' '\n' | grep -E "^(client_requests|flush_batches|rounds|wal_records)=" | tr '\n' ' '
echo
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
