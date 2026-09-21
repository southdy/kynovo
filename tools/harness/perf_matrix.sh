#!/bin/bash
# NOTE: this is a REPORT, not a gate.  It prints what it measured and exits 0 whatever the numbers say, and
# nothing compares them: the only harness in ./build.sh's gate chain is soak_release.sh.  Read the numbers, not
# the final "done" (review 4th round E5).
# perf_matrix.sh -- attributed performance matrix (no source instrumentation).
# Splits cost into: per-round (STATS rounds), per-write, WAL/disk (mem vs disk),
# payload (1B vs 4KiB) and batching (K=1/32/128) using server-side CPU time.
# Usage: bash tools/harness/perf_matrix.sh   (N=2000 by default, override with N=...)
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
out=build/perf
N=${N:-2000}
rm -rf $out; mkdir -p $out

stats(){
  ./build/kdbctl.exe 127.0.0.1:${1:-9211} STATS 2>/dev/null | tr ' ' '\n' \
    | grep -E '^(rounds|client_requests|flush_batches|wal_records|write_bytes|state|leader)=' | tr '\n' ' '
}
srvpid=0
TAG=-
pid_win(){ ps -W | awk '/kdbsvr\.exe/ {print $4; exit}'; }
start_server(){
  TAG=${1:-run}; be=${2:-mem}; cp=${3:-9211}; pp=${4:-9212}
  rm -rf $out/db-$TAG
  ./build/kdbsvr.exe init "$be$out/db-$TAG" >/dev/null 2>&1
  ./build/kdbsvr.exe server 1 $cp $pp "$be$out/db-$TAG" "1@127.0.0.1:$cp:$pp" > $out/srv-$TAG.log 2>&1 &
  srvpid=$!
  sleep 5
}
stop_server(){
  ./build/proc_time.exe $(pid_win) | sed "s/^/server_cpu /"
  taskkill /F /PID $srvpid >/dev/null 2>&1
  sleep 1
}
case_run(){ # tag n k vsize mode
  local tag=${1:-run} n=${2:-2000} k=${3:-32} vs=${4:-0} mode=${5:-1} port=${6:-9211}
  local a b cpu0 cpu1
  a=$(stats $port)
  cpu0=$(./build/proc_time.exe $(pid_win) | sed 's/.*total_ms=//')
  ./build/bench_rate.exe 127.0.0.1:$port $n $k $vs __$tag $mode > $out/$tag.txt 2>&1
  cpu1=$(./build/proc_time.exe $(pid_win) | sed 's/.*total_ms=//')
  b=$(stats $port)
  local ops=$(grep -m1 '^PHASE|' $out/$tag.txt | sed 's/.*ops_per_s=//')
  # mode= is what the TOOL reported doing, not what was requested: passing `unique` to a tool that parsed the flag
  # with atoi() reported mode=unique for a single-key run (review 4th round E6).  If the tool printed no WORKLOAD
  # line, say so instead of echoing the request as if it were the measurement.
  local wl=$(grep -m1 '^WORKLOAD|' $out/$tag.txt | sed 's/.*mode=//; s/ .*//')
  echo "CASE $tag n=$n k=$k vsize=$vs mode=${wl:-unspecified-requested-$mode} ops_per_s=$ops cpu_ms_before=$cpu0 cpu_ms_after=$cpu1"
  echo "  stats_before: $a"
  echo "  stats_after : $b"
}

echo "=== fsync baseline ==="
timeout 120 ./build/bench_fsync.exe 2>&1 | grep -E "^batch=(1|100) " 

echo "=== disk backend (D:) ==="
start_server disk "disk://" 9211 9212
case_run set1b_k32  $N 32  1    unique
case_run get1b_k32  $N 32  1    unique
case_run set4k_k32  $N 32  4096 unique
case_run set1b_k128 $N 128 1    unique
case_run set1b_k1   300 1  1    unique
stop_server

echo "=== mem backend ==="
start_server mem "mem://" 9213 9214
case_run set1b_k32_mem $N 32  1    unique 9213
case_run set4k_k32_mem $N 32  4096 unique 9213
stop_server

echo "=== done ==="
