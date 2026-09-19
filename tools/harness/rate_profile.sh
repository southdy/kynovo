#!/usr/bin/env bash
# rate_profile.sh -- one run of the open-loop load with the numbers that explain it.
#
# Prints, for one backend and one flush configuration: throughput, latency percentiles, and the four
# quantities that say WHERE the ceiling comes from - the achieved batch size, the requests admitted
# per event-loop round, the sync rate, and the server's own CPU per request.  A throughput number
# without those four is not an explanation.
#
#   usage: tools/harness/rate_profile.sh <base-uri> [k] [flush-items] [flush-bytes] [n] [port]
#   e.g.:  tools/harness/rate_profile.sh disk:///tmp/p 512 128 262144 2000 10601
#          tools/harness/rate_profile.sh mem://p       512 128 1048576
#
# Verdict line (parseable): PROFILE|backend=.. k=.. items=.. bytes=.. ops_s=.. p50=.. p99=.. batch=..
#                                  per_round=.. sync_per_s=.. cpu_us_per_op=..
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
URI="${1:-disk:///tmp/rate_profile}"
K="${2:-512}"
ITEMS="${3:-128}"
BYTES="${4:-262144}"
N="${5:-2000}"
PORT="${6:-10601}"
BIN="$ROOT/build"
TMP="$(mktemp -d)"
SERVER_PID=""

cleanup(){
  if [ -f "$TMP/port" ]; then
    "$BIN/kdbctl.exe" "127.0.0.1:$(cat "$TMP/port")" SHUTDOWN >/dev/null 2>&1
    sleep 1
  fi
  if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" >/dev/null 2>&1; fi
  pkill -x kdbsvr.exe >/dev/null 2>&1
  rm -rf "$TMP" >/dev/null 2>&1
  case "$URI" in disk://*) rm -rf "${URI#disk://}" >/dev/null 2>&1 ;; esac   # only a real path
}
trap cleanup EXIT

if [ "${URI%%:*}" = disk ]; then
  "$BIN/kdbsvr.exe" init "$URI" --flush-items "$ITEMS" --flush-window-ms 1 --flush-bytes "$BYTES" >/dev/null 2>&1 || {
    echo "PROFILE|fail reason=init-failed uri=$URI"; exit 1; }
else
  "$BIN/kdbsvr.exe" init "$URI" --flush-items "$ITEMS" --flush-window-ms 1 --flush-bytes "$BYTES" >/dev/null 2>&1
fi
echo "$PORT" > "$TMP/port"
"$BIN/kdbsvr.exe" server 1 "$PORT" "$((PORT+1))" "$URI" "1@127.0.0.1:$PORT:$((PORT+1))" > "$TMP/server.log" 2>&1 &
SERVER_PID=$!

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if "$BIN/kdbctl.exe" "127.0.0.1:$PORT" STATS >/dev/null 2>&1; then ready=1; break; fi
  sleep 1
done
if [ "$ready" != 1 ]; then
  echo "PROFILE|fail reason=server-never-answered (see $TMP/server.log)"; cat "$TMP/server.log"; exit 1
fi

# One value per field, guaranteed: sed with -n and head -1, then strip CR.  (Capturing a
# grep -o straight into a variable is what filled these with newlines and broke the arithmetic.)
# The name must start at a word boundary or "rounds" matches inside "slow_rounds" and the read comes
# back 0 - which happened, and made a perfectly healthy run look like it never had a round.
field(){ tr -d '\r' < "$1" | sed -n "s/.*[^a-z_]$2=\([0-9]*\).*/\1/p" | head -1; }

statline(){
  "$BIN/kdbctl.exe" "127.0.0.1:$PORT" STATS 2>&1 | tr '\n' ' '
}

# Server CPU before/after, from the proc filesystem where it exists, else from ps.
cpu_ticks(){
  pid="$1"
  if [ -r "/proc/$pid/stat" ]; then
    awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null | head -1
  else
    ps -o time= -p "$pid" 2>/dev/null | head -1
  fi
}
U0="$(cpu_ticks "$SERVER_PID")"

"$BIN/bench_rate.exe" "127.0.0.1:$PORT" "$N" "$K" 1 __profile unique > "$TMP/rate.txt" 2>&1
U1="$(cpu_ticks "$SERVER_PID")"
S="$(statline)"
echo "$S" > "$TMP/stats.txt"

OPS="$(field "$TMP/rate.txt" ops_per_s)"
WALL="$(field "$TMP/rate.txt" wall_us)"
# The latency line is the ONLY place p50/p99/max describe a request; STATS also carries *_us_max
# fields, so parse them off the LAT line rather than off "the last max= in the file".
LATLINE="$(tr -d '\r' < "$TMP/rate.txt" | grep '^LAT|' | head -1)"
lat_field(){ printf '%s' "$LATLINE" | sed -n "s/.*[ |]$1=\([0-9]*\).*/\1/p"; }
P50="$(lat_field p50)"
P99="$(lat_field p99)"
MAX="$(lat_field max)"
FB="$(field "$TMP/stats.txt" flush_batches)"
FW="$(field "$TMP/stats.txt" flush_writes)"
CR="$(field "$TMP/stats.txt" client_requests)"
RD="$(field "$TMP/stats.txt" rounds)"
SE="$(field "$TMP/stats.txt" sync_us_ewma)"
POLL_US="$(field "$TMP/stats.txt" poll_us_ewma)"
DRIVE_US="$(field "$TMP/stats.txt" round_us_ewma)"
[ -n "$POLL_US" ] || POLL_US=0
[ -n "$DRIVE_US" ] || DRIVE_US=0
WM="$(field "$TMP/stats.txt" window_ms)"
[ -n "$FB" ] || FB=0
[ -n "$FW" ] || FW=0
[ -n "$RD" ] || RD=0
[ -n "$CR" ] || CR=0
[ -n "$OPS" ] || OPS=0
[ -n "$WALL" ] || WALL=0
[ -n "$U0" ] || U0=0
[ -n "$U1" ] || U1=0

if [ "$OPS" = 0 ]; then
  echo "PROFILE|fail reason=no-throughput (client produced no PHASE line)"; cat "$TMP/rate.txt" | head -5; exit 1
fi

BATCH=0; [ "$FB" -gt 0 ] && BATCH=$(( FW / FB ))
PER_ROUND=0; [ "$RD" -gt 0 ] && PER_ROUND=$(( CR / RD ))
SYNC_RATE=0; [ "$WALL" -gt 0 ] && SYNC_RATE=$(( FB * 1000000 / WALL ))
CPU_PER_OP=0
case "$U0" in ''|*[!0-9]*) CPU_PER_OP=-1 ;; *) CPU_PER_OP=$(( (U1-U0) * 10000 / OPS )) ;; esac

# Round cost split at the poll boundary.  poll_us is receive+decode+SEND (all inside cemon
# callbacks); drive_us is raft advance, apply and flush bookkeeping.  Per frame is what matters for
# throughput: if it is tens of microseconds, the loop is pricing work per request that could be
# priced per batch.
PER_FRAME=0
if [ "$PER_ROUND" -gt 0 ]; then PER_FRAME=$(( (POLL_US + DRIVE_US) / PER_ROUND )); fi
echo "PROFILE|backend=${URI%%:*} k=$K items=$ITEMS bytes=$BYTES ops_s=$OPS p50=$P50 p99=$P99 max=$MAX batch=$BATCH per_round=$PER_ROUND sync_per_s=$SYNC_RATE window_ms=$WM sync_ewma_us=$SE cpu_us_per_op=$CPU_PER_OP rounds=$RD client_requests=$CR poll_us_ewma=$POLL_US drive_us_ewma=$DRIVE_US us_per_frame=$PER_FRAME"
