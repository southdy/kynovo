#!/bin/bash
# pageheap_hunt.sh -- enable Application Verifier's Heaps checks for the symbol-bearing server, then
# drive repeated deep pipelined loads.  Page heap faults AT the write to freed memory, so the
# backtrace names the corruptor instead of the detector.
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
RUNS=${RUNS:-40}
N=${N:-4000}
K=${K:-128}
PORT=${PORT:-9491}
mkdir -p $out
echo "=== enabling Application Verifier Heaps for kdbsvr_dbg.exe ==="
appverif.exe -enable Heaps -for build/kdbsvr_dbg.exe 2>&1 | head -5
appverif.exe -querystate -for build/kdbsvr_dbg.exe 2>&1 | grep -iE "heaps|enabled|state" | head -6
echo "=== rebuild the debug binary (needs -lwinmm, -pthread) ==="
gcc -std=c89 -O0 -g -Wall -Wextra -Wno-unused-function -pthread -o build/kdbsvr_dbg.exe code/kdbsvr.c -lws2_32 -lwinmm 2>&1 | grep -E "error|undefined" | head -3
ls -l build/kdbsvr_dbg.exe | awk '{print "dbg bytes:",$5}'
taskkill /F /IM kdbsvr_dbg.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-ph
./build/kdbsvr.exe init "disk://$ROOT/build/perf/db-ph" >/dev/null 2>&1
: > $out/gdb-ph.txt
gdb --batch -ex "run" -ex "bt full" -ex "quit" \
    --args ./build/kdbsvr_dbg.exe server 1 $PORT $((PORT+1)) "disk://$ROOT/build/perf/db-ph" "1@127.0.0.1:$PORT:$((PORT+1))" \
    > $out/gdb-ph.txt 2>&1 &
sleep 12
echo "alive after start: $(ps -W | grep -ci kdbsvr_dbg)"
for i in $(seq 1 $RUNS); do
  res=$(timeout 90 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET ph_$i payload 2>&1 | tr -d '\r' | grep -E "^pipe mode|^pipe: stalled" | head -1)
  alive=$(ps -W | grep -ci kdbsvr_dbg)
  echo "run$i alive=$alive ${res:-<no report>}"
  if [ "$alive" = "0" ]; then echo "=== process gone during round $i ==="; break; fi
done
echo "=== frames ==="
grep -E "^#[0-9]+ " $out/gdb-ph.txt | head -26
echo "=== verdict / heap lines ==="
grep -nE "Free Heap block|Access violation|received signal|corrupt" $out/gdb-ph.txt | head -8
taskkill /F /IM kdbsvr_dbg.exe >/dev/null 2>&1
echo "=== disabling Heaps again ==="
appverif.exe -disable Heaps -for build/kdbsvr_dbg.exe 2>&1 | head -3
echo done
