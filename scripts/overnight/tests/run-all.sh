#!/usr/bin/env bash
# run-all.sh -- run every overnight-runner control-plane test and report.
#
# Until 2026-07-11 the scripts/overnight/tests/*.py files had NO central runner
# and were wired into neither test-tooling.sh, the git hooks, nor CI -- so a
# control-plane regression could commit and push with green hooks (this is how
# the runner kept breaking). This script is the single entry point the push
# gate (.githooks/pre-push) and any operator invoke to exercise the whole set:
#
#     bash scripts/overnight/tests/run-all.sh
#
# It runs every test_*.py in this dir plus the sibling test-launch.sh DRYRUN
# smoke, prints one line per test, and exits nonzero if any test fails. Stdlib
# + bash only; no pytest dependency (matches the plain-assert house style).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAIL=0
PASS=0

run_one() {
  local name="$1"; shift
  if out="$("$@" 2>&1)"; then
    printf '  ok   %s\n' "$name"
    PASS=$((PASS + 1))
  else
    printf '  FAIL %s\n' "$name"
    printf '%s\n' "$out" | sed 's/^/       | /'
    FAIL=$((FAIL + 1))
  fi
}

echo "== overnight-runner test suite =="

# All plain-assert python contract tests in this directory.
while IFS= read -r t; do
  [ -n "$t" ] || continue
  run_one "$(basename "$t")" python3 "$t"
done < <(ls -1 "$HERE"/test_*.py 2>/dev/null)

# The launcher DRYRUN + flock smoke (bash, not python).
if [ -f "$HERE/../test-launch.sh" ]; then
  run_one "test-launch.sh" bash "$HERE/../test-launch.sh"
fi

echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
