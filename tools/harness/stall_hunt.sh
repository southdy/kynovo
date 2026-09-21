#!/usr/bin/env bash
# NOTE: this is a REPORT, not a gate.  It prints what it measured and exits 0 whatever the numbers say, and
# nothing compares them: the only harness in ./build.sh's gate chain is soak_release.sh.  Read the numbers, not
# the final "done" (review 4th round E5).
# Stall hunt on a RELEASE build.  Control for the earlier
#   pipe: stalled after 5 s (queued=127 done=0 in_flight=0)
# observations, which came from the DEBUG binary with Application Verifier Heaps enabled - a much
# slower server, so the client's 5 s no-answer threshold may simply have been exceeded.
#
# The harness certifies itself: a round that does not report ok=<N> (server not reachable, usage
# error, wrong argv) aborts the run instead of counting as clean - an empty transcript must never
# be read as "no stalls".
export PATH="/d/MinW64-15.2.0/bin:$PATH"
export MSYS2_ARG_CONV_EXCL='*'   # MSYS must not rewrite argv for native binaries

set -u
cd "$(dirname "${0:-0}")/../.."
PORT=${PORT:-9931}
PEER=${PEER:-9932}
RUNS=${RUNS:-20}
K=${K:-128}
N=${N:-4000}
DIR=build/stall
rm -rf "$DIR"; mkdir -p "$DIR"
BASE="disk://build/stall/n1"   # relative to the repo root this script cds into (was $ROOT: never set)
ADDR="127.0.0.1:$PORT"
./build/kdbsvr.exe init "$BASE" > "$DIR/init.log" 2>&1 || { echo "FATAL: init failed"; cat "$DIR/init.log"; exit 1; }
./build/kdbsvr.exe server 1 "$PORT" "$PEER" "$BASE" "1@127.0.0.1:$PORT:$PEER" > "$DIR/server.log" 2>&1 &
SRV=$!
ready=0
for w in 1 2 3 4 5 6 7 8 9 10; do
  if ./build/kdbctl.exe "$ADDR" STATS > "$DIR/ready.log" 2>&1; then ready=1; break; fi
  sleep 1
done
if [ "$ready" != "1" ]; then
  echo "FATAL: server not answering after 10s; log:"; tail -5 "$DIR/server.log"; kill $SRV 2>/dev/null; exit 1
fi
echo "server up (pid=$SRV), STATS ok"
stalls=0; ok=0; bad=0
for i in $(seq 1 "$RUNS"); do
  out=$(./build/kdbctl.exe "$ADDR" PIPE "$K" "$N" SET ph_ v 2>&1 | tr -d '\r')
  line=$(echo "$out" | grep -E "pipe mode|stalled|error|refused|failed" | tail -1)
  case "$line" in
    *stalled*) stalls=$((stalls+1)); printf "run%-3s STALL: %s\n" "$i" "$line"
        echo "  --- context ---"; echo "  alive=$(kill -0 $SRV 2>/dev/null && echo 1 || echo 0)"
        ./build/kdbctl.exe "$ADDR" STATS 2>&1 | head -14 | sed 's/^/  /'; tail -6 "$DIR/server.log" | sed 's/^/  log: /' ;;
    *ok=*)     ok=$((ok+1)); [ $((i % 5)) -eq 0 ] && printf "run%-3s %s\n" "$i" "$line" ;;
    *)         bad=$((bad+1)); printf "run%-3s UNEXPECTED: %s\n" "$i" "$line"
               echo "=== ABORT: round $i reported no ok=<N> (see above) - an empty transcript must not read as \"no stalls\" ==="
               kill $SRV 2>/dev/null; wait $SRV 2>/dev/null; exit 1 ;;
  esac
done
alive=$(kill -0 $SRV 2>/dev/null && echo 1 || echo 0)
echo "=== release stall hunt: runs=$RUNS ok=$ok stalls=$stalls unexpected=$bad server_alive=$alive ==="
kill $SRV 2>/dev/null; wait $SRV 2>/dev/null
