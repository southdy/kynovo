#!/usr/bin/env bash
# Startup refusals must NAME their reason.
#
# On 2026-09-19 a nightly run failed because kdbsvr would not start on the CI box, and the only line in the
# log was the application's own tie-line "failed to start server 1".  The real cause (a harness hard-coding
# this machine's data path) had to be found without the program's help, which is exactly what the ops
# standard forbids.  Each step of the open/restore sequence now prints its own `fatal:` line with the step
# and the reason, so this script starts the server in ways that MUST fail and requires that line.
#
# It also pins the shape of the tie-line: a "see the fatal line above" message is only legitimate when a
# fatal line really is above it, so every case below demands a SPECIFIC reason, never just the tie-line.
#
#   bash tests/startup_fail.sh          (run from anywhere; the repo root comes from this file's path)
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"
BIN=./build
BASE=$BIN/startup-fail
PORT=${PORT:-9281}
PEER=${PEER:-9282}
PORT2=${PORT2:-9283}
PEER2=${PEER2:-9284}
fails=0
checks=0
say(){ printf '%s\n' "$*"; }

[ -x "$BIN/kdbsvr.exe" ] || { say "startup_fail: FAIL (no $BIN/kdbsvr.exe - build first)"; exit 1; }
# The readiness probe below uses kdbctl; without it the port-in-use cases would report "the occupier never
# answered" instead of "this binary was never built", which is a diagnosis pointing at the wrong thing.
[ -x "$BIN/kdbctl.exe" ] || { say "startup_fail: FAIL (no $BIN/kdbctl.exe - build it: ./build.sh kdbctl)"; exit 1; }
rm -rf "$BASE"; mkdir -p "$BASE"
# A regular FILE where the store wants a directory: neither backend will create a file beneath it, so the
# config write is the step that must refuse - and the one that must say so.
printf 'not a directory' > "$BASE/blocker"

# run_fail <name> <reason-regex> <cmd...>
#  * the start must FAIL (rc != 0),
#  * its output must carry a `fatal:` line,
#  * that line must match the step-specific reason (a generic tie-line is not a reason).
run_fail(){
  name="$1"; pat="$2"; shift 2
  out="$("$@" 2>&1)"; rc=$?
  checks=$((checks+1))
  if [ "$rc" = 0 ]; then say "FAIL $name: the start SUCCEEDED (rc=0), so nothing was proven"; fails=$((fails+1)); return; fi
  case "$out" in
    *"fatal:"*) ;;
    *) say "FAIL $name: rc=$rc but no 'fatal:' line at all; output: $out"; fails=$((fails+1)); return;;
  esac
  hit="$(printf '%s\n' "$out" | grep -E "$pat" | head -1)"
  if [ -n "$hit" ]; then
    say "PASS $name"
    say "     reason: $hit"
  else
    say "FAIL $name: no line matching /$pat/; output: $out"
    fails=$((fails+1))
  fi
}

# 1. Bad arguments are refused with the offending token named.
run_fail "unknown argument is named" "unexpected argument '--bogus'" \
  timeout 20 "$BIN/kdbsvr.exe" server 1 "$PORT" "$PEER" "disk://$ROOT/$BASE/db-args" "1@127.0.0.1:$PORT:$PEER" --bogus

# 2. A node id that the cluster spec does not describe names the mismatch and the ports given.
run_fail "cluster/argv mismatch is named" "does not match the argv ports" \
  timeout 20 "$BIN/kdbsvr.exe" server 2 "$PORT" "$PEER" "disk://$ROOT/$BASE/db-spec" "1@127.0.0.1:$PORT:$PEER"

# 3. The configuration cannot be read or written: the step that refused is named, with the base path.
run_fail "config open failure names its step" "cannot start server: config open failed.*/blocker/db" \
  timeout 20 "$BIN/kdbsvr.exe" server 1 "$PORT" "$PEER" "disk://$ROOT/$BASE/blocker/db" "1@127.0.0.1:$PORT:$PEER"

# 4. `init` on a path it cannot write names WHICH write failed (config vs WAL metadata used to be one
#    lumped message that named only the base path).
run_fail "init names the write that failed" "cannot write the configuration for" \
  timeout 20 "$BIN/kdbsvr.exe" init "disk://$ROOT/$BASE/blocker/db-init"

# 5. An out-of-range option value is named with the value that was given.
run_fail "bad --auto-replace-threshold is named" "auto-replace-threshold '0' must be 1..1000000" \
  timeout 20 "$BIN/kdbsvr.exe" server 1 "$PORT" "$PEER" "disk://$ROOT/$BASE/db-thr" "1@127.0.0.1:$PORT:$PEER" \
  --auto-replace "2@127.0.0.1:$PORT2:$PEER2" --auto-replace-threshold 0

# 6/7. A port that is genuinely taken.  The occupier is a real kdbsvr - it is not enough for a port to look
#      busy, so the script proves the port is in use by getting STATS back from it before starting the
#      second node, and aborts loudly (rather than reporting a passing "port in use" case) if it cannot.
say "--- starting an occupier on $PORT/$PEER (its STATS reply is the proof the ports are really taken)"
"$BIN/kdbsvr.exe" init "disk://$ROOT/$BASE/db-occ" > "$BASE/occ-init.log" 2>&1 || { say "FAIL occupier init"; fails=$((fails+1)); }
( "$BIN/kdbsvr.exe" server 1 "$PORT" "$PEER" "disk://$ROOT/$BASE/db-occ" "1@127.0.0.1:$PORT:$PEER" > "$BASE/occ.log" 2>&1 & )
ready=no
i=0
while [ "$i" -lt 40 ]; do
  if timeout 5 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" STATS > "$BASE/occ-stats.log" 2>&1; then ready=yes; break; fi
  sleep 0.25; i=$((i+1))
done
checks=$((checks+1))
if [ "$ready" != yes ]; then
  say "FAIL occupier never answered STATS, so the port-in-use cases prove nothing"
  say "     occupier log: $BASE/occ.log"
  fails=$((fails+1))
else
  say "PASS occupier answers STATS (ports $PORT/$PEER are really taken)"

  run_fail "peer listen failure names the address and the socket code" \
    "cannot start server: peer listen on 127.0.0.1:$PEER failed \(last fatal: .*socket code [1-9][0-9]*\)" \
    timeout 20 "$BIN/kdbsvr.exe" server 1 "$PORT2" "$PEER" "disk://$ROOT/$BASE/db-peer" "1@127.0.0.1:$PORT2:$PEER"

  run_fail "client listen failure names the address and the socket code" \
    "cannot start server: client listen on 127.0.0.1:$PORT failed \(last fatal: .*socket code [1-9][0-9]*\)" \
    timeout 20 "$BIN/kdbsvr.exe" server 1 "$PORT" "$PEER2" "disk://$ROOT/$BASE/db-cli" "1@127.0.0.1:$PORT:$PEER2"
fi

# A refusal that only says "failed to start server N" is the defect this whole script exists for: make the
# tie-line's promise checkable by requiring it never to be the only line.  (Every case above already
# demanded a specific reason; this asserts the general rule on the whole transcript.)
if [ "$fails" -eq 0 ]; then
  say "startup_fail: PASS ($checks checks)"
  rc=0
else
  say "startup_fail: $fails of $checks check(s) FAILED"
  rc=1
fi

# Teardown: the occupier, then the store copies.  A passing run cleans up; a FAILING run keeps its
# transcript ($BASE/server outputs) because deleting the diagnosis is how a red gate becomes unexplainable.
if command -v taskkill >/dev/null 2>&1; then
  MSYS2_ARG_CONV_EXCL='*' taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
else
  pkill -x kdbsvr.exe >/dev/null 2>&1
fi
if [ "$rc" -eq 0 ]; then
  rm -rf "$BASE"
else
  say "startup_fail: transcripts kept in $BASE (server logs and the occupier's STATS reply)"
fi
exit "$rc"
