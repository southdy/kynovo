#!/usr/bin/env bash
# CLI smoke test: the one-shot client is the only consumer of the TCP accept path and it had
# no test at all - a wait-loop bug that made every command hang, and a "timeout looks like
# success" bug (exit 0 with no output), both lived here unseen.  Run from the repo root:
#   bash tests/cli_smoke.sh
set -u
# Repo root from THIS script's own location (the data store path must not depend on where the
# tree was cloned).  pwd -W gives the native D:/... form that the disk:// backend expects; plain
# pwd is the fallback for non-MSYS shells.
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd -W 2>/dev/null || pwd)"
BIN=./build
PORT=${PORT:-9271}
PEER=${PEER:-9272}
DIR=$BIN/cli-smoke
fails=0
say(){ printf '%s\n' "$*"; }
check(){ # check <desc> <actual> <expected>
  if [ "$2" = "$3" ]; then say "PASS $1"; else say "FAIL $1 (got '$2' want '$3')"; fails=$((fails+1)); fi
}
contain(){ # contain <desc> <haystack> <needle>
  case "$2" in *"$3"*) say "PASS $1";; *) say "FAIL $1 (no '$3' in '$2')"; fails=$((fails+1));; esac
}

rm -rf "$DIR"; mkdir -p "$DIR"
"$BIN/kdbsvr.exe" init "disk://$ROOT/$DIR/db" >/dev/null || { say "FAIL server init"; exit 1; }
( "$BIN/kdbsvr.exe" server 1 "$PORT" "$PEER" "disk://$ROOT/$DIR/db" "1@127.0.0.1:$PORT:$PEER" > "$DIR/server.log" 2>&1 & )
sleep 5

# happy path: exit 0 and the expected payload on stdout
out=$(timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" SET k v 2>&1); check "SET exit" "$?" "0"; contain "SET output" "$out" "ok"
out=$(timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" GET k 2>&1); check "GET exit" "$?" "0"; contain "GET value" "$out" "v"
out=$(timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" STATS 2>&1); check "STATS exit" "$?" "0"; contain "STATS leader" "$out" "state=3"
out=$(timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" MEMBERS 2>&1); check "MEMBERS exit" "$?" "0"; contain "MEMBERS body" "$out" "1@127.0.0.1"
out=$(timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" DEL k 2>&1); check "DEL exit" "$?" "0"
out=$(timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" GET k 2>&1); check "GET(missing) exit" "$?" "0"; contain "GET(missing) body" "$out" "not found"

# failures must NOT look like success
timeout 30 "$BIN/kdbctl.exe" "127.0.0.1:1" GET k >/dev/null 2>&1; check "dead host exit" "$?" "1"
timeout 20 "$BIN/kdbctl.exe" "127.0.0.1:$PORT" BOGUS >/dev/null 2>&1; check "unknown command exit" "$?" "1"

MSYS2_ARG_CONV_EXCL='*' taskkill /F /IM kdbsvr.exe >/dev/null 2>&1
if [ "$fails" -eq 0 ]; then say "cli_smoke: PASS"; exit 0; fi
say "cli_smoke: $fails check(s) FAILED"; exit 1
