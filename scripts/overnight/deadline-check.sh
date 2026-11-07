#!/usr/bin/env bash
# deadline-check.sh -- should this run still be spawning segments?
#
# WHY (2026-07-31): the runner had NO time bound. It ran to fixpoint or until a
# human disarmed it, which makes "arm it for 24 hours and judge the evidence"
# impossible to express and turns every bounded experiment into a manual vigil.
#
# THE STOP IS CLEAN BY CONSTRUCTION. This is checked at SPAWN time, so a run
# that is past its deadline simply does not start another segment -- the last
# segment already ended at its own verified rollover, with a clean tree,
# everything pushed and receipts valid. Nothing is killed mid-section. That is
# why the deadline is enforced here and NOT by a systemd timer firing --disarm,
# which would land wherever it happened to land.
#
# Verdicts (stdout is one line; the exit code is the machine-readable part):
#   0  -> CONTINUE   spawn the segment
#   10 -> STOP       the deadline was reached; the caller should disarm and notify
#
# The deadline is the only stop here. A fault never ends a run.
#
# Usage: deadline-check.sh <project_dir>
set -uo pipefail

PROJECT_DIR="${1:?usage: deadline-check.sh <project_dir>}"
STATE_DIR="$PROJECT_DIR/.claude/state"
DEADLINE_FILE="$STATE_DIR/run-deadline"

NOW="$(date +%s)"

# --- deadline -----------------------------------------------------------
if [ -f "$DEADLINE_FILE" ]; then
  DEADLINE="$(awk 'NR==1{print $1}' "$DEADLINE_FILE" 2>/dev/null || echo "")"
  case "$DEADLINE" in
    ''|*[!0-9]*)
      # An unreadable deadline must NOT silently mean "run forever" -- that is
      # the failure mode this file exists to remove. Say so, and continue: a
      # corrupt bound is an operator problem, not a reason to stop a healthy run.
      echo "deadline-check: CONTINUE (deadline file unreadable: '${DEADLINE}')"
      ;;
    *)
      if [ "$NOW" -ge "$DEADLINE" ]; then
        echo "deadline-check: STOP -- deadline reached ($(date -d "@$DEADLINE" -Is 2>/dev/null || echo "$DEADLINE")); the previous segment ended at a verified rollover, so this is a clean stop"
        exit 10
      fi
      REMAIN=$(( (DEADLINE - NOW) / 60 ))
      echo "deadline-check: CONTINUE (${REMAIN} min remaining)"
      ;;
  esac
else
  echo "deadline-check: CONTINUE (no deadline set -- runs to fixpoint)"
fi

# A no-ship streak used to end the run here at 2 segments, which made any one fault
# fatal. It now selects a repair segment in the launcher instead (2026-11-07).

exit 0
