#!/usr/bin/env bash
# Fixture-only regression for overnight-launch.sh (vendored from runner-kit
# v0.4.0, project-ized 2026-07-03). Exercises pre-flight, snooze, backoff,
# flock, report-path selection, claude-bin resolution, and the lifecycle-gate
# inspection seam via the dry-run guard WITHOUT invoking systemd, Claude,
# Codex, or a ChromeMCP lane. No side effects beyond .claude/overnight/
# (gitignored).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# Snapshot the REAL reports dir so the isolation regression at the bottom can
# prove the DRYRUNs never touched it (2026-07-30: before the launcher's DRYRUN
# temp-dir redirect, the DRYRUNs here wrote stub logs, repointed latest.log,
# and their retention prune EVICTED six real canary logs in one evening).
REAL_BEFORE="$(ls -1 "$REPO_ROOT/.claude/overnight/reports"/run-*.log 2>/dev/null | sort | md5sum | cut -d" " -f1)"
LINK_BEFORE="$(readlink "$REPO_ROOT/.claude/overnight/reports/latest.log" 2>/dev/null || true)"
LAUNCH="$SCRIPT_DIR/overnight-launch.sh"

fail() { echo "test-launch FAIL: $1" >&2; exit 1; }

# claude may not be on PATH in CI; a dummy executable satisfies resolve_claude
# (the dry-run never calls it).
if command -v claude >/dev/null 2>&1; then
  export CLAUDE_BIN="$(command -v claude)"
else
  export CLAUDE_BIN="$(command -v true)"
fi

FIXTURE_TODO="todo/TODO-Claude-Overnight-Runner.md"

# If a REAL run is active, it holds the flock; a DRYRUN launch then correctly
# declines ("run already active, launch skipped") and never reaches the DRYRUN
# stop point. That is PROVEN behavior (the flock is the concurrency guard), not
# a failure -- so the two lock-dependent checks (1, 2) are SKIPPED, not failed,
# while the run is active. This lets the control-plane gate run test-launch even
# during a live overnight run (e.g. the runner committing its own change). The
# snooze/backoff/gate checks below short-circuit BEFORE the flock and always run.
RUN_ACTIVE=0
if flock -n "$REPO_ROOT/.claude/overnight/launch.lock" -c true 2>/dev/null; then :; else RUN_ACTIVE=1; fi

if [ "$RUN_ACTIVE" = "1" ]; then
  echo "test-launch NOTE: a run is active (flock held) -- skipping the 2 DRYRUN lock checks (the launcher correctly declines a concurrent launch)."
else
  # 1) Dry-run completes with exit 0 and reports a selected report path.
  OUT="$(OVERNIGHT_SEQUENCER_DRYRUN=1 bash "$LAUNCH" "$REPO_ROOT" "$FIXTURE_TODO" bypassPermissions 2>&1)" \
    || fail "dry-run exited non-zero"
  echo "$OUT" | grep -q "DRYRUN ok: report=" || fail "no report path selected"
  echo "$OUT" | grep -q "todo-hint=$FIXTURE_TODO" || fail "todo hint not threaded"

  # 2) The flock is released after exit -- a second dry-run must also succeed
  #    (a held lock would print 'run already active' and exit before DRYRUN).
  OUT2="$(OVERNIGHT_SEQUENCER_DRYRUN=1 bash "$LAUNCH" "$REPO_ROOT" "$FIXTURE_TODO" bypassPermissions 2>&1)" \
    || fail "second dry-run exited non-zero (lock not released?)"
  echo "$OUT2" | grep -q "DRYRUN ok" || fail "second dry-run did not reach DRYRUN stop point"
  echo "$OUT2" | grep -q "run already active" && fail "lock was not released between runs"
fi

# 3) Active snooze short-circuits before the dry-run stop point.
SNOOZE_FILE="$REPO_ROOT/.claude/overnight/snooze-until"
echo "$(( $(date +%s) + 3600 ))" > "$SNOOZE_FILE"
OUT3="$(OVERNIGHT_SEQUENCER_DRYRUN=1 bash "$LAUNCH" "$REPO_ROOT" "$FIXTURE_TODO" bypassPermissions 2>&1)" \
  || fail "snooze run exited non-zero"
echo "$OUT3" | grep -q "usage-limit snooze active" || fail "active snooze not honored"
rm -f "$SNOOZE_FILE"

# 4) Active watchdog backoff short-circuits the same way (impossible-os
#    addition: the backoff pre-flight landed with the lifecycle gate).
BACKOFF_FILE="$REPO_ROOT/.claude/overnight/watchdog-backoff-until"
echo "$(( $(date +%s) + 3600 )) 1" > "$BACKOFF_FILE"
OUT4="$(OVERNIGHT_SEQUENCER_DRYRUN=1 bash "$LAUNCH" "$REPO_ROOT" "$FIXTURE_TODO" bypassPermissions 2>&1)" \
  || fail "backoff run exited non-zero"
echo "$OUT4" | grep -q "watchdog backoff active" || fail "active backoff not honored"
rm -f "$BACKOFF_FILE"

# 5) Lifecycle-gate inspection seam. Only exercised when the live oracle says
#    NEEDS_WORK: a DONE queue would take the real disarm branch (sentinel +
#    timer stop -- a run action, not a test), and a BLOCKED queue's outcome
#    depends on the heal probe. Both are skipped, not failed.
ORACLE_STATE="$(cd "$REPO_ROOT" && python3 .claude/hooks/sequencer_triage.py --next 2>/dev/null \
  | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))' 2>/dev/null || true)"
# The gate seam also runs AFTER the flock, so it is skipped while a run is active.
# THIRD unreachable state, added 2026-08-24: overnight-launch.sh runs
# deadline-check.sh (at :123) LONG before the gate seam, so once a run is past
# its deadline the launcher stops there and the seam can never print anything.
# Without this arm the suite reports `gate seam did not print its decision` --
# an environmental stop wearing a machinery failure's name, which costs a real
# diagnosis every time a close-out runs the suite on a stopped run. Detected by
# asking deadline-check.sh itself (exit 10 = STOP) rather than re-deriving the
# rule, so the skip can never disagree with the launcher about the deadline.
DEADLINE_STOP=0
if [ -f "$SCRIPT_DIR/deadline-check.sh" ]; then
  bash "$SCRIPT_DIR/deadline-check.sh" "$REPO_ROOT" >/dev/null 2>&1 || DEADLINE_STOP=1
fi
if [ "$RUN_ACTIVE" = "1" ]; then
  echo "test-launch NOTE: gate-seam check skipped (a run is active; the gate runs after the flock)"
elif [ "$DEADLINE_STOP" = "1" ]; then
  echo "test-launch NOTE: gate-seam check skipped (deadline reached; the launcher stops before the seam)"
elif [ "$ORACLE_STATE" = "NEEDS_WORK" ]; then
  OUT5="$(OVERNIGHT_SEQUENCER_GATE_ONLY=1 bash "$LAUNCH" "$REPO_ROOT" "$FIXTURE_TODO" bypassPermissions 2>&1)" \
    || fail "gate-only run exited non-zero"
  echo "$OUT5" | grep -q "GATE: would run (state=NEEDS_WORK)" || fail "gate seam did not print its decision"
  [ -f "$REPO_ROOT/.claude/state/sequencer-fixpoint" ] && fail "gate-only run fabricated the fixpoint sentinel"
else
  echo "test-launch NOTE: gate-seam check skipped (oracle state '$ORACLE_STATE', not NEEDS_WORK)"
fi

# DRYRUN isolation regression: nothing above may have touched the real dir.
REAL_AFTER="$(ls -1 "$REPO_ROOT/.claude/overnight/reports"/run-*.log 2>/dev/null | sort | md5sum | cut -d" " -f1)"
[ "$REAL_AFTER" = "$REAL_BEFORE" ] || fail "DRYRUN touched the real reports dir (stub written / log pruned)"
LINK_AFTER="$(readlink "$REPO_ROOT/.claude/overnight/reports/latest.log" 2>/dev/null || true)"
[ "$LINK_AFTER" = "$LINK_BEFORE" ] || fail "DRYRUN repointed latest.log"

echo "test-launch PASS"
