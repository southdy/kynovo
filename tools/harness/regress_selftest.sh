#!/usr/bin/env bash
# Self-test of the `regress` gate detector itself.  A gate is only trustworthy if it FAILS when it should,
# so this checks the cases explicitly:
#   * exit 0 with NO verdict line  -> FAIL      (the "0 failures must not mean 0 data" case)
#   * crashing command, no verdict -> FAIL
#   * exit 0 with a verdict line   -> pass
#   * a layer that never returns   -> FAIL rc=124 within its budget   (E1: the watchdog)
#
# The functions under test are EXTRACTED FROM build.sh BY NAME and eval'd.  An earlier version redefined
# reg_report/reg_gate inline, which tests a lookalike that is free to drift from the gate it claims to test -
# the same "the instrument lies" shape as the review's E5, one level up (fourth review round E5).
set -u
cd "$(dirname "${0:-0}")/../.." || exit 1
BUILD_SH=./build.sh
[ -f "$BUILD_SH" ] || { echo "SELFTEST|FAIL|no $BUILD_SH to extract the detector from"; exit 1; }

extract(){ # extract <funcname>: the function body, from `name(){` to the first line that is exactly `}`
  awk -v fn="$1" 'index($0,fn"(){")==1{p=1} p{print} p&&/^}$/{exit}' "$BUILD_SH"
}
for fn in reg_report reg_gate; do
  body="$(extract "$fn")"
  case "$body" in
    *"$fn(){"*) : ;;
    *) echo "SELFTEST|FAIL|could not extract $fn from $BUILD_SH (the detector moved or was renamed)"; exit 1 ;;
  esac
  eval "$body"
done

TMP="$(mktemp -d)"
REG_DIR="$TMP"; REG_PASS=0; REG_FAIL=0
# The counters are CUMULATIVE across the cases (the real reg_gate increments them), so the expectations below
# count up: 0/1, 1/1, 1/2, 1/3.
fails=0
case_run(){ # case_run <label> <want_pass> <want_fail> <want_line> <reg_gate args...>
  # reg_gate is called in THIS shell, not in a command substitution: it runs in a subshell there and the
  # REG_PASS/REG_FAIL it increments would be thrown away (measured: all four cases read pass=0 fail=0).
  local label="$1" want_pass="$2" want_fail="$3" want_line="$4" last
  shift 4
  reg_gate "$@" >"$TMP/out.txt" 2>&1
  if [ "$REG_PASS" != "$want_pass" ] || [ "$REG_FAIL" != "$want_fail" ]; then
    echo "  NOT OK $label: pass=$REG_PASS fail=$REG_FAIL (wanted pass=$want_pass fail=$want_fail)"
    fails=$((fails+1)); return
  fi
  last="$(grep -E '^GATE\|' "$TMP/out.txt" | tail -1)"
  case "$last" in
    *"$want_line"*) echo "  ok    $label: $last" ;;
    *) echo "  NOT OK $label: last GATE line was [$last], wanted one containing [$want_line]"
       fails=$((fails+1)) ;;
  esac
}

case_run silent-but-zero-exit 0 1 'FAIL' \
         silent_but_zero_exit 'SUMMARY: [0-9]+/[0-9]+ passed' true
case_run good-verdict         1 1 '|ok|' \
         good_verdict 'SUMMARY: [0-9]+/[0-9]+ passed' bash -c 'echo "SUMMARY: 3/3 passed"'
case_run crashing-no-verdict  1 2 'rc=7' \
         crashing_no_verdict 'SUMMARY: [0-9]+/[0-9]+ passed' bash -c 'echo boom; exit 7'
REG_LAYER_TIMEOUT_S=3 case_run hanging-layer 1 3 'rc=124 [layer timeout 3s]' \
         hanging_layer 'SUMMARY: [0-9]+/[0-9]+ passed' sleep 30
rm -rf "$TMP"

if [ "$fails" -ne 0 ]; then
  echo "SELFTEST|FAIL|$fails of 4 detector cases behaved wrongly"
  exit 1
fi
echo "SELFTEST|PASS|the detector FAILs a silent layer, a crashing layer and a hanging one and passes a verdict line (4/4)"
