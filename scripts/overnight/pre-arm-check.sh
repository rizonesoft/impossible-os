#!/usr/bin/env bash
# pre-arm-check.sh -- health gate run by arm-sequencer.sh BEFORE arming.
#
# Catches "armed into a broken control plane": a dead codex login, a missing
# compiler, corrupt guard/recorder state, a launcher that no longer reaches its
# pre-Claude exit, or a failing runner test suite -- all of which would
# otherwise only surface AFTER the watchdog fired an unattended session at
# night. Runs three deterministic checks and refuses the arm (exit 1) on any
# hard failure.
#
#   bash scripts/overnight/pre-arm-check.sh <repo_dir> <todo_file>
#
# Checks (all HARD):
#   1. runner-doctor        -- host health (toolchain, codex auth, state files)
#   2. launcher DRYRUN      -- overnight-launch.sh reaches its pre-Claude exit
#   3. runner test suite    -- the control-plane contract tests pass
#
# Opt-out: ARM_SKIP_PREFLIGHT=1 (logged loudly by the caller).
set -uo pipefail

REPO="${1:?usage: pre-arm-check.sh <repo_dir> <todo_file>}"
TODO="${2:?usage: pre-arm-check.sh <repo_dir> <todo_file>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FAIL=0

echo "== pre-arm health check =="

# 1) runner-doctor (hard: exit 1 iff a hard-fail check tripped)
if [ -f "$HERE/runner-doctor.py" ]; then
  if DOUT="$(cd "$REPO" && python3 "$HERE/runner-doctor.py" . 2>&1)"; then
    echo "  ok   runner-doctor"
  else
    echo "  FAIL runner-doctor (host health):"
    printf '%s\n' "$DOUT" | tail -12 | sed 's/^/       | /'
    FAIL=1
  fi
else
  echo "  skip runner-doctor (not present)"
fi

# 2) launcher DRYRUN -- exercises snooze/backoff/flock/report path WITHOUT
#    systemd or Claude; must reach the "DRYRUN ok" pre-Claude exit.
if [ -f "$HERE/overnight-launch.sh" ]; then
  if DOUT="$(OVERNIGHT_SEQUENCER_DRYRUN=1 bash "$HERE/overnight-launch.sh" "$REPO" "$TODO" bypassPermissions 2>&1)" \
     && printf '%s' "$DOUT" | grep -q "DRYRUN ok:"; then
    echo "  ok   launcher DRYRUN"
  else
    echo "  FAIL launcher DRYRUN (did not reach pre-Claude exit):"
    printf '%s\n' "$DOUT" | tail -12 | sed 's/^/       | /'
    FAIL=1
  fi
else
  echo "  FAIL launcher missing: $HERE/overnight-launch.sh"
  FAIL=1
fi

# 3) runner test suite
if [ -f "$HERE/tests/run-all.sh" ]; then
  if SOUT="$(bash "$HERE/tests/run-all.sh" 2>&1)"; then
    echo "  ok   runner test suite"
  else
    echo "  FAIL runner test suite:"
    printf '%s\n' "$SOUT" | grep -E "FAIL|failed" | sed 's/^/       | /'
    FAIL=1
  fi
else
  echo "  skip runner test suite (run-all.sh not present)"
fi

if [ "$FAIL" -ne 0 ]; then
  echo "== pre-arm check FAILED -- arming refused =="
  exit 1
fi
echo "== pre-arm check passed =="
exit 0
