#!/usr/bin/env bash
# reset-breaker.sh <project_dir> <unit> -- an explicit ARM starts with a clean breaker.
#
# Observed 2026-09-28: after a revoked-token stop, the re-arm was refused by its
# own pre-arm check (the launcher DRYRUN honoured the old run's watchdog backoff),
# and once that was cleared by hand the first launch stopped at once on
# "2 consecutive segments shipped no section" -- the two 5-second 401 launches of
# the PREVIOUS arm. Breaker state describes the run that produced it; arming is
# the operator saying "start now", so it must not inherit a finished run's trips.
#
# Only arm-sequencer.sh calls this. The launcher never does: inside a live run
# (watchdog relaunches) the breaker is doing its job and must be honoured.
# Skipped while either overnight timer is active, so a mistaken re-arm of a live
# run cannot wipe its breaker. Prints what it cleared, so a recurring cause stays
# visible at arm time.
set -u

PROJECT_DIR="${1:?usage: reset-breaker.sh <project_dir> <unit>}"
UNIT="${2:?usage: reset-breaker.sh <project_dir> <unit>}"

if [ "${RESET_BREAKER_ASSUME_STOPPED:-}" != "1" ]; then
  for t in "$UNIT.timer" "$UNIT-watchdog.timer"; do
    if systemctl --user is-active --quiet "$t" 2>/dev/null; then
      echo "breaker reset: skipped -- $t is active (a run is live; its breaker stays)"
      exit 0
    fi
  done
fi

cleared=""
for rel in .claude/overnight/watchdog-backoff-until \
           .claude/overnight/state/unproductive-streak \
           .claude/overnight/noship-streak \
           .claude/overnight/NEEDS-OPERATOR.md; do
  p="$PROJECT_DIR/$rel"
  [ -e "$p" ] || continue
  val="$(head -c 60 "$p" 2>/dev/null | head -1 | tr -d '\r')"
  rm -f "$p"
  cleared="$cleared $(basename "$rel")=${val:-present}"
done
if [ -n "$cleared" ]; then
  echo "breaker reset:${cleared}"
else
  echo "breaker reset: nothing to clear"
fi
