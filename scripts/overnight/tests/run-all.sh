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

# Drop git's per-invocation environment before running anything. When this
# suite is invoked FROM a git hook -- which is the normal path, since the
# always-on control-plane gate runs it at pre-commit and pre-push -- git has
# exported GIT_INDEX_FILE, GIT_DIR, GIT_WORK_TREE and friends pointing at THIS
# repo. Several tests create a throwaway repo under /tmp and commit in it;
# those commits then honour the inherited GIT_INDEX_FILE, try to build a tree
# from this repo's index inside the temp repo's object database, and die with
#   error: invalid object 100644 <sha> for 'docs/test-coverage/coverage.json'
#   error: Error building trees
# Measured 2026-07-28: test_receipts, test_run_outcome, test_section_checkpoint
# and test_worktree_hash all pass standalone and all fail under
# `GIT_INDEX_FILE=.git/index`, which is precisely the hook environment. The
# failure is therefore a property of HOW the gate is invoked, not of the code
# under test -- the worst kind of gate failure, because it blocks a correct
# commit and points at innocent tests.
unset GIT_INDEX_FILE GIT_DIR GIT_WORK_TREE GIT_PREFIX GIT_OBJECT_DIRECTORY \
      GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_AUTHOR_DATE \
      GIT_COMMITTER_DATE GIT_EDITOR 2>/dev/null || true

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

# SAME CLASS AS THE GIT-ENV BLOCK ABOVE: a property of how the suite is
# invoked, not of the code under test. `test_stub_lint_coverage` exercises
# lint's stub-behind-stamp COVERAGE line, which needs `build/todo-cache.json`;
# without it the check reports "cache not found" and the test fails naming a
# fixture, so the suite announces a regression nobody introduced.
#
# The cache goes missing routinely and for good reasons: `build-and-validate.sh`
# WITHOUT `--keep-cache` deletes the artifact it just validated, and any clean
# build removes `build/` wholesale. Measured 2026-08-08: 67/69 at pre-commit
# minutes after the same suite returned 69/69, with nothing between the two runs
# but a lint rebuild and a smoke-matrix clean build. That is almost certainly
# the unnamed `68 passed, 1 failed` intermittent this suite has been carrying.
#
# Building it costs ~1.2s and is idempotent, so the suite declares its own
# precondition rather than reporting a phantom failure. If the build fails the
# suite continues: the affected test will then fail honestly, which is the
# correct outcome for a cache that cannot be built at all.
if [ ! -f "$REPO/build/todo-cache.json" ]; then
  echo "run-all: build/todo-cache.json absent -- building it (suite precondition)"
  bash "$REPO/scripts/todo-graph/build-and-validate.sh" --keep-cache \
      >/dev/null 2>&1 || echo "run-all: cache build FAILED -- dependent tests will report it"
fi

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
