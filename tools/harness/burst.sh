#!/bin/bash
# burst.sh -- open-loop burst measurement (K >= N): the server's own capacity,
# without a closed-loop client self-limiting the offered rate.
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
N=${N:-2000}
PORT=${PORT:-9251}
BE=${BE:-$ROOT/build/perf/db-burst}
rm -rf $out/db-burst
./build/kdbsvr.exe init "disk://$BE" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://$BE" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/burst-srv.log 2>&1 &
sleep 6
for k in ${KS:-2000 512 128}; do
  echo "--- burst N=$N K=$k (K>=N means open-loop) ---"
  ./build/bench_rate.exe 127.0.0.1:$PORT $N $k 1 __b$k unique > $out/burst-$k.txt 2>&1
  grep -E "^PHASE\||^LAT\|" $out/burst-$k.txt | sed 's/^/  /'
  grep -oE "wal_stage_sync_us=[0-9]+|wal_stage_sync_n=[0-9]+|wal_stage_wake_us=[0-9]+|flush_batches=[0-9]+|flush_by_target=[0-9]+|flush_by_window=[0-9]+|flush_by_drain=[0-9]+|rounds=[0-9]+|wal_records=[0-9]+" $out/burst-$k.txt | tail -9 | tr '\n' ' '
  echo
done
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
echo done
