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
#   10 -> STOP       deadline reached, or an abort condition tripped; the
#                    caller should disarm and notify
#
# Abort conditions are deliberately few and mechanical. "An unexplained gate
# refusal" is an operator judgement and is NOT automated here -- a criterion
# that cannot be evaluated without a human is not an abort condition, it is a
# checkpoint question.
#
# Usage: deadline-check.sh <project_dir>
set -uo pipefail

PROJECT_DIR="${1:?usage: deadline-check.sh <project_dir>}"
STATE_DIR="$PROJECT_DIR/.claude/state"
RUNTIME_DIR="$PROJECT_DIR/.claude/overnight"
DEADLINE_FILE="$STATE_DIR/run-deadline"
NOSHIP_FILE="$RUNTIME_DIR/noship-streak"
NOSHIP_LIMIT="${OVERNIGHT_NOSHIP_LIMIT:-2}"

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

# --- abort: consecutive segments that shipped nothing --------------------
# run-outcome.py already classifies each segment; this counter is maintained by
# the launcher from that verdict. A run that produces nothing twice running is
# either wedged or looping, and either way an operator should look before more
# hours are spent.
if [ -f "$NOSHIP_FILE" ]; then
  STREAK="$(awk 'NR==1{print $1+0}' "$NOSHIP_FILE" 2>/dev/null || echo 0)"
  if [ "${STREAK:-0}" -ge "$NOSHIP_LIMIT" ]; then
    echo "deadline-check: STOP -- ${STREAK} consecutive segments shipped no section (limit ${NOSHIP_LIMIT})"
    exit 10
  fi
fi

exit 0
