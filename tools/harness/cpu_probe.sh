#!/bin/bash
# cpu_probe.sh [tag] -- measure server-side CPU vs wall time for a pipelined load.
# Answers: is the event loop CPU-bound (burning cycles per round) or waiting?
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
out=build/perf
TAG=${1:-a}
N=${N:-4000}
K=${K:-32}
PORT=${PORT:-9221}
BE=${BE:-disk}
if [ "${BE}" = "mem" ]; then be_uri="mem://perf-$TAG"; else be_uri="disk://D:/kynovo/build/perf/db-cpu-$TAG"; fi
mkdir -p $out
rm -rf $out/db-cpu-$TAG
./build/kdbsvr.exe init "${be_uri}" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "${be_uri}" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/cpu-$TAG.log 2>&1 &
sleep 6
echo "--- ps -W rows matching kdbsvr ---"
ps -W | grep -i kdbsvr | head -3
PID=$(ps -W | grep -i kdbsvr | head -1 | awk '{print $4}')
echo "winpid=$PID"
A=$(./build/proc_time.exe $PID | sed 's/.*total_ms=//')
W0=$(date +%s%N)
./build/bench_rate.exe 127.0.0.1:$PORT $N $K 1 __cpu$TAG unique > $out/cpuab-$TAG.txt 2>&1
W1=$(date +%s%N)
B=$(./build/proc_time.exe $PID | sed 's/.*total_ms=//')
echo "ops: $(grep -m1 '^PHASE|' $out/cpuab-$TAG.txt)"
echo "server_cpu_before_ms=$A after_ms=$B wall_ms=$(( (W1-W0)/1000000 ))"
echo "counters: $(grep -oE 'rounds=[0-9]+|flush_batches=[0-9]+|wal_records=[0-9]+|flush_by_target=[0-9]+' $out/cpuab-$TAG.txt | tr '\n' ' ')"
taskkill /F /PID $PID >/dev/null 2>&1
sleep 1
echo done
