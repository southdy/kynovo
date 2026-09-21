#!/bin/bash
# watch_counters.sh -- run repeated pipelined loads on ONE server and print the admission-related
# counters after each run.  A counter that grows monotonically is the leak behind
# "the Nth client gets a silently closed connection" (K_REQUEST_INFLIGHT_MAX = 16384).
set -u
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'
cd "$(dirname "${0:-0}")/../.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"   # native form the disk:// backend expects
out=build/perf
PORT=${PORT:-9451}
N=${N:-4000}
K=${K:-128}
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
sleep 1
rm -rf $out/db-watch
./build/kdbsvr.exe init "disk://$ROOT/build/perf/db-watch" >/dev/null 2>&1
./build/kdbsvr.exe server 1 $PORT $((PORT+1)) "disk://$ROOT/build/perf/db-watch" "1@127.0.0.1:$PORT:$((PORT+1))" > $out/watch-srv.log 2>&1 &
sleep 6
# The field names below are the ones code/kserver.h:3830 actually emits (`wal_inflight=` is the WAL-side
# counterpart of the admission counters).  The old list grepped client_requests / request_count / request_bytes /
# wal_inflight_count / client_connection_count - none of which the server has ever printed - so the harness meant to
# watch for an in-flight leak printed an empty field set and left the judgement to the reader (review 4th round E5).
MISSING=0
stats(){
  local raw s
  raw=$(timeout 15 ./build/kdbctl.exe 127.0.0.1:$PORT STATS 2>&1 | tr -d '\r')
  s=$(printf '%s\n' "$raw" | tr ' ' '\n' \
        | grep -E "^(wal_inflight|pending_requests|pending_request_bytes|client_connections|rounds|flush_batches)=" \
        | tr '\n' ' ')
  case "$s" in
    *pending_request_bytes=*) : ;;
    *) MISSING=$((MISSING+1))
       s="STATS-FIELDS-MISSING got=[$s] first_line=[$(printf '%s' "$raw" | head -1 | cut -c1-70)]" ;;
  esac
  printf '%s\n' "$s"
}
field(){ printf '%s' "$1" | tr ' ' '\n' | grep -E "^$2=" | head -1 | cut -d= -f2; }
echo "run0 (before): $(stats)"
FIRST_PENDING=$(field "$(stats)" pending_requests)
LAST_PENDING=0
for i in $(seq 1 ${RUNS:-6}); do
  res=$(timeout 60 ./build/kdbctl.exe 127.0.0.1:$PORT PIPE $K $N SET w_$i payload 2>&1 | tr -d '\r' | grep -E "^pipe mode|^pipe: stal")
  echo "run$i: ${res:-<no report>}"
  cur=$(stats)
  echo "      after: $cur"
  LAST_PENDING=$(field "$cur" pending_requests)
done
echo "=== server alive: $(ps -W | grep -c -i kdbsvr) ==="
echo "=== server log (fatal/warning would appear here) ==="
tail -20 $out/watch-srv.log
taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
if [ "$MISSING" -gt 0 ]; then
  echo "WATCH|FAIL|the STATS line never carried the admission counters ($MISSING run(s)) - nothing was measured"
  exit 1
fi
if [ -z "${FIRST_PENDING:-}" ] || [ -z "${LAST_PENDING:-}" ]; then
  echo "WATCH|FAIL|could not read pending_requests out of STATS (before=[$FIRST_PENDING] after=[$LAST_PENDING])"
  exit 1
fi
if [ "$LAST_PENDING" -gt "${FIRST_PENDING:-0}" ]; then
  echo "WATCH|FAIL|pending_requests grew $FIRST_PENDING -> $LAST_PENDING over ${RUNS:-6} runs: the admission leak is back"
  exit 1
fi
echo "WATCH|PASS|admission counters present in every run; pending_requests $FIRST_PENDING -> $LAST_PENDING"
echo done
