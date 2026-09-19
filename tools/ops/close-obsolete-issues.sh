#!/usr/bin/env bash
# Close the issues listed in ops/close.txt, one number per line.
# Runs in the cloud (workflow Ops) with the workflow's own GITHUB_TOKEN; --dry-run prints the plan and
# touches nothing, which is how this script is verified locally (no token exists on a dev machine).
set -u
cd "$(dirname "$0")/../.." || exit 1
LIST=ops/close.txt
[ -f "$LIST" ] || { echo "OPS|FAIL|$LIST missing"; exit 1; }
dry=0
[ "${1:-}" = "--dry-run" ] && dry=1
n_closed=0; n_fail=0
while read -r num note; do
  case "$num" in ''|'#'*) continue ;; esac
  if [ "$dry" = 1 ]; then
    echo "OPS|would-close|#$num|${note:-}"
  else
    if gh issue close "$num" --comment "${note:-Closed as obsolete.}" >/dev/null 2>&1; then
      echo "OPS|closed|#$num"
    else
      echo "OPS|FAIL|#$num could not be closed"; n_fail=$((n_fail+1))
    fi
  fi
  n_closed=$((n_closed+1))
done < "$LIST"
echo "OPS|close-obsolete-issues|count=$n_closed failed=$n_fail dry_run=$dry"
[ "$n_fail" = 0 ] || exit 1
