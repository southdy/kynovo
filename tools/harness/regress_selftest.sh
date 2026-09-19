#!/usr/bin/env bash
# Self-test of the `regress` gate detector itself.  A gate is only trustworthy if it FAILS when it
# should, so this checks the three cases explicitly:
#   * exit 0 with NO verdict line  -> FAIL  (this is the "0 failures must not mean 0 data" case)
#   * crashing command, no verdict -> FAIL
#   * exit 0 with a verdict line   -> pass
# Run: bash tools/harness/regress_selftest.sh     (expected: pass=1 fail=2)
set -u
cd "$(dirname "$0")/../.." || exit 1
TMP="$(mktemp -d)"
REG_DIR="$TMP"; REG_PASS=0; REG_FAIL=0
reg_report(){ local t1; t1="$(date +%s)"; if [ "$2" = ok ]; then REG_PASS=$((REG_PASS+1)); else REG_FAIL=$((REG_FAIL+1)); fi; printf 'GATE|%s|%s|%s\n' "$1" "$2" "$3"; }
reg_gate(){ local name="$1" pat="$2" log rc got; shift 2; log="$REG_DIR/$name.log"; "$@" >"$log" 2>&1; rc=$?; got="$(grep -E "$pat" "$log" | tail -1)"; if [ "$rc" = 0 ] && [ -n "$got" ]; then reg_report "$name" ok "$got"; else reg_report "$name" FAIL "rc=$rc verdict=${got:-<no verdict line>}"; fi; }
reg_gate silent_but_zero_exit 'SUMMARY: [0-9]+/[0-9]+ passed' true
reg_gate good_verdict          'SUMMARY: [0-9]+/[0-9]+ passed' bash -c 'echo "SUMMARY: 3/3 passed"'
reg_gate crashing_no_verdict   'SUMMARY: [0-9]+/[0-9]+ passed' bash -c 'echo boom; exit 7'
rm -rf "$TMP"
echo "regress-detector selftest: pass=$REG_PASS fail=$REG_FAIL (expected pass=1 fail=2)"
[ "$REG_PASS" = 1 ] && [ "$REG_FAIL" = 2 ] || { echo "DETECTOR SELFTEST FAILED" >&2; exit 1; }
