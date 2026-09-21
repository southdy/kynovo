#!/bin/bash
# NOTE: this is a REPORT, not a gate.  It prints what it measured and exits 0 whatever the numbers say, and
# nothing compares them: the only harness in ./build.sh's gate chain is soak_release.sh.  Read the numbers, not
# the final "done" (review 4th round E5).
# crash_hunt.sh -- drive repeated deep pipelined loads until the server dies, with the server
# running UNDER gdb and built with symbols (-O0 -g), so the death leaves a backtrace.
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
RUNS=${RUNS:-40}
N=${N:-4000}
K=${K:-128}
PORT=${PORT:-9461}
mkdir -p $out
echo "=== building a symbol-bearing server (-O0 -g, no source change) ==="
gcc -std=c89 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -o build/kdbsvr_dbg.exe code/kdbsvr.c -lws2_32 -lwinmm 2>&1 | grep -E "error|undefined" | head -5
echo "dbg binary: $(ls -l build/kdbsvr_dbg.exe | awk '{print $5}') bytes"
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
taskkill /F /IM kdbsvr_dbg.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-crash
./build/kdbsvr.exe init "disk://$ROOT/build/perf/db-crash" >/dev/null 2>&1
: > $out/gdb-out.txt
echo "=== starting server under gdb --batch ==="
gdb --batch -ex "run" -ex "bt full" -ex "quit" \
    --args ./build/kdbsvr_dbg.exe server 1 $PORT $((PORT+1)) "disk://$ROOT/build/perf/db-crash" "1@127.0.0.1:$PORT:$((PORT+1))" \
    > $out/gdb-out.txt 2>&1 &
gdbpid=$!
sleep 10
echo "alive after start: $(ps -W | grep -ci kdbsvr)"
for i in $(seq 1 $RUNS); do
  res=$(timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET ch_$i payload 2>&1 | tr -d '\r' | grep -E "^pipe mode|^pipe: stalled" | head -1)
  alive=$(ps -W | grep -ci kdbsvr_dbg)
  echo "run$i alive=$alive ${res:-<no report>}"
  if [ "$alive" = "0" ]; then
    echo "=== server died during run $i ==="
    break
  fi
done
taskkill /F /IM kdbsvr_dbg.exe >/dev/null 2>&1
taskkill /F /PID $gdbpid >/dev/null 2>&1
sleep 2
echo "=== gdb evidence (signal / backtrace / exit) ==="
grep -nE "Program received signal|Program exited|Thread .* received signal|^#[0-9]+ |SIGSEGV|ACCESS_VIOLATION|fatal:|warning:" $out/gdb-out.txt | head -40
echo "=== gdb tail (last 12) ==="
tail -12 $out/gdb-out.txt
echo "=== app stdout/stderr went to gdb's stdout above; server log: ==="
echo done
