#!/usr/bin/env bash
# Report whether an overnight run is ACTUALLY live, from every source at once.
#
# WHY THIS EXISTS. The obvious check -- `systemctl --user is-active
# overnight-<repo>.service` -- LIES, and lies in the direction that matters.
# Arming schedules the main timer for the next slot; the watchdog timer runs an
# independent `*:0/10` cadence. When those coincide (observed 2026-07-24: both
# at 21:30:00) BOTH fire, `flock` correctly lets one win, and the loser exits.
# If the WATCHDOG won, the run is healthy and executing -- while the MAIN unit
# reads `inactive`. An operator checking the main unit alone concludes the run
# died and re-arms on top of a working run.
#
# The collision itself is benign (that is what the lock is for). Only the
# REPORTING was wrong, and until now there was no liveness reporting at all --
# `overnight-monitor.sh` is a log tailer with no systemctl calls, so the
# operator had nothing to consult except the misleading manual check.
#
# Liveness = ANY of:
#   1. the main unit is active
#   2. the WATCHDOG unit is active          <- the case the naive check misses
#   3. the launcher's flock is HELD (unforgeable: the kernel drops it when the
#      holder dies, and nothing that merely mentions the launcher can hold it)
#   4. the latest report log grew within FRESH_MINUTES
#
# Sources 3 and 4 also cover the case where systemd is unavailable entirely
# (a plain launcher invocation, or a WSL session without user systemd), which
# the unit checks alone would report as dead.
#
# Usage:
#   bash scripts/overnight/run-liveness.sh            # human summary, exit 0/1
#   bash scripts/overnight/run-liveness.sh --quiet    # exit status only
#   bash scripts/overnight/run-liveness.sh --selftest
#
# Exit 0 = live, 1 = not live, 2 = usage error.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${OVERNIGHT_LIVENESS_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
UNIT="overnight-$(basename "$REPO_ROOT")"
REPORT_DIR="${OVERNIGHT_REPORT_DIR:-$REPO_ROOT/.claude/overnight/reports}"
FRESH_MINUTES="${OVERNIGHT_LIVENESS_FRESH_MIN:-15}"

QUIET=0
case "${1:-}" in
  --quiet) QUIET=1 ;;
  --selftest) ;;
  "") ;;
  *) echo "usage: run-liveness.sh [--quiet|--selftest]" >&2; exit 2 ;;
esac

unit_active() {
  # `is-active` prints and exits non-zero for inactive; treat any error
  # (no systemd, unknown unit) as "not active" rather than as a failure --
  # sources 3 and 4 still get their say.
  systemctl --user is-active "$1" >/dev/null 2>&1
}

launcher_running() {
  # The launcher's own flock is the ONLY trustworthy process evidence:
  # `overnight-launch.sh:85` does `exec 9>"$LOCKFILE"; flock -n 9`, and the
  # kernel releases that lock when the holder dies by ANY route (exit, crash,
  # kill). So "cannot acquire it" means a launcher is genuinely running, and
  # there is nothing to spoof.
  #
  # The first cut used `pgrep -f "overnight-launch\.sh"` and was WRONG in
  # production, not just in tests: `pgrep -f` matches the full command line of
  # every process, so any shell that merely MENTIONS the launcher matched it --
  # an operator grepping for it, a monitoring one-liner, or (how this was
  # caught) a `git commit` whose message contained the filename. That reported
  # a live run when nothing was running, which is precisely the class of lie
  # this script exists to eliminate.
  local lock="$REPO_ROOT/.claude/overnight/launch.lock"
  [ -e "$lock" ] || return 1
  command -v flock >/dev/null 2>&1 || return 1   # cannot test -> claim nothing
  # Acquiring means NO launcher holds it; the subshell drops it immediately.
  if ( exec 9>>"$lock"; flock -n 9 ) 2>/dev/null; then
    return 1
  fi
  return 0
}

report_fresh() {
  local newest
  # A DRYRUN writes a report log with the SAME name shape into the SAME
  # directory, so `pre-arm-check.sh` (which runs a launcher DRYRUN) leaves
  # minutes-fresh logs behind. Counting those as evidence made this script
  # answer ARMED -- "the schedule is ticking, do not re-arm" -- immediately
  # after a pre-arm check on a repo that was not armed at all, which is the
  # exact wrong instruction at the exact moment an operator asks. Skip any log
  # that never got past the dry run. (Observed 2026-07-28.)
  newest="$(ls -t "$REPORT_DIR"/run-*.log 2>/dev/null \
            | while read -r f; do grep -q 'DRYRUN ok:' "$f" 2>/dev/null || echo "$f"; done \
            | head -1)"
  [ -n "$newest" ] || return 1
  [ -n "$(find "$newest" -mmin "-$FRESH_MINUTES" 2>/dev/null)" ]
}

selftest() {
  local fails=0
  # An UNHELD lock must never read as a running launcher. This is the
  # regression the pgrep version failed: any process merely mentioning the
  # launcher filename made it claim a live run.
  local tmp0; tmp0="$(mktemp -d)"
  mkdir -p "$tmp0/.claude/overnight"; : > "$tmp0/.claude/overnight/launch.lock"
  ( REPO_ROOT="$tmp0"; launcher_running ) \
    && { echo "FAIL: unheld lock reported as a running launcher"; fails=1; }
  # A HELD lock must read as running.
  ( exec 9>>"$tmp0/.claude/overnight/launch.lock"; flock -n 9; sleep 3 ) &
  local holder=$!; sleep 0.3
  ( REPO_ROOT="$tmp0"; launcher_running ) \
    || { echo "FAIL: held lock not reported as running"; fails=1; }
  kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null
  rm -rf "$tmp0"
  # report_fresh must be false against an empty dir, and true for a new file.
  local tmp; tmp="$(mktemp -d)"
  OVERNIGHT_REPORT_DIR="$tmp" bash "$0" --quiet >/dev/null 2>&1
  REPORT_DIR="$tmp" report_fresh && { echo "FAIL: empty dir reported fresh"; fails=1; }
  touch "$tmp/run-x.log"
  REPORT_DIR="$tmp" report_fresh || { echo "FAIL: new log not reported fresh"; fails=1; }
  # An OLD log must not read as fresh.
  touch -d '2 hours ago' "$tmp/run-x.log"
  REPORT_DIR="$tmp" report_fresh && { echo "FAIL: 2h-old log reported fresh"; fails=1; }
  # A DRYRUN log is not evidence of a ticking schedule, however fresh.
  rm -f "$tmp"/run-*.log
  printf 'DRYRUN ok: report=%s\n' "$tmp/run-d.log" > "$tmp/run-d.log"
  REPORT_DIR="$tmp" report_fresh && { echo "FAIL: DRYRUN log reported fresh"; fails=1; }
  # ...but a real log sitting BESIDE a newer DRYRUN log still counts.
  touch "$tmp/run-real.log"; touch "$tmp/run-d.log"
  REPORT_DIR="$tmp" report_fresh || { echo "FAIL: real log masked by DRYRUN"; fails=1; }
  rm -rf "$tmp"
  [ "$fails" = 0 ] && echo "run-liveness selftest OK" || return 1
}

[ "${1:-}" = "--selftest" ] && { selftest; exit $?; }

MAIN=0; WATCH=0; PROC=0; LOG=0
unit_active "$UNIT.service"          && MAIN=1
unit_active "$UNIT-watchdog.service" && WATCH=1
launcher_running                     && PROC=1
report_fresh                         && LOG=1

# THREE states, not two. Collapsing these to live/dead is what made the naive
# check useless in the first place, and a fresh log is NOT the same evidence as
# a running process: the watchdog fires every 10 min, writes a launch log, and
# exits, so a recent log proves the schedule is ticking -- not that a run is
# executing right now.
#   RUNNING : a launcher process is up, or either unit is active
#   ARMED   : no process, but the schedule is demonstrably alive (fresh log)
#   DEAD    : no evidence at all
STATE="DEAD"
[ $((MAIN + WATCH + PROC)) -gt 0 ] && STATE="RUNNING"
[ "$STATE" = "DEAD" ] && [ "$LOG" = 1 ] && STATE="ARMED"
LIVE=0
[ "$STATE" != "DEAD" ] && LIVE=1

if [ "$QUIET" = 0 ]; then
  echo "overnight run: $STATE"
  yn() { [ "$1" = 1 ] && echo yes || echo no; }
  echo "  main unit active ($UNIT.service):      $(yn $MAIN)"
  echo "  watchdog unit active ($UNIT-watchdog): $(yn $WATCH)"
  echo "  launcher process running:              $(yn $PROC)"
  echo "  report log grew in last ${FRESH_MINUTES}m:          $(yn $LOG)"
  # Only claim the watchdog is carrying the run when it demonstrably IS.
  if [ "$STATE" = "RUNNING" ] && [ "$MAIN" = 0 ] && [ "$WATCH" = 1 ]; then
    echo
    echo "  NOTE: the main unit reads inactive but the run IS executing under the"
    echo "  WATCHDOG unit -- the normal timer-collision outcome (the watchdog won"
    echo "  the flock). Do NOT re-arm; you would stack a second launcher on a"
    echo "  healthy run. This is the case a bare 'is-active' on the main unit"
    echo "  reports as dead."
  elif [ "$STATE" = "ARMED" ]; then
    echo
    echo "  NOTE: no launcher process and no active unit, but a report log grew"
    echo "  within ${FRESH_MINUTES}m -- the watchdog schedule is ticking and its launches are"
    echo "  exiting quickly (armed-but-idle, or each tick is short-circuiting)."
    echo "  Read the newest run-*.log to see which before re-arming."
  fi
fi
exit $((1 - LIVE))
