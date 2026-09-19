#!/bin/bash
# repro_deep.sh -- reproduce the K=512 in-flight burst against a fresh server and capture:
#   the CLI's own report, the server log, whether the server is still alive, and its exit state.
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "$0")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
echo "=== previous sweep's server log (what the 512/1024 runs left behind) ==="
tail -20 $out/front-srv.log 2>/dev/null
echo
echo "=== fresh server ==="
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-deep
./build/kdbsvr.exe init "disk://$ROOT/build/perf/db-deep" >/dev/null 2>&1
./build/kdbsvr.exe server 1 9431 9432 "disk://$ROOT/build/perf/db-deep" "1@127.0.0.1:9431:9432" > $out/deep-srv.log 2>&1 &
srvshell=$!
sleep 6
echo "alive before: $(ps -W | grep -c -i kdbsvr)"
echo "=== PIPE 512 (N=4000), 60s cap ==="
timeout 60 ./build/kdbctl.exe 127.0.0.1:9431 PIPE 512 4000 SET dp_ payload 2>&1 | tail -3
echo "cli rc=$?"
sleep 1
echo "alive after : $(ps -W | grep -c -i kdbsvr)"
echo "=== server log after ==="
tail -20 $out/deep-srv.log
echo "=== can it still serve? ==="
timeout 20 ./build/kdbctl.exe 127.0.0.1:9431 GET dp_0 2>&1 | tail -2
echo "get rc=$?"
echo "=== control: same burst at K=256 ==="
timeout 60 ./build/kdbctl.exe 127.0.0.1:9431 PIPE 256 4000 SET dp2_ payload 2>&1 | tail -2
echo "alive after K=256: $(ps -W | grep -c -i kdbsvr)"
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
