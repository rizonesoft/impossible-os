#!/usr/bin/env bash
# Non-model session watcher for the overnight sequencer.
#
# Spawned DETACHED by run_phase_guard.py (`wait` and `rollover` verbs) from
# inside the headless session, so it inherits the run's environment
# (OVERNIGHT_SEQUENCER_RUN=1, OVERNIGHT_MODEL, OVERNIGHT_EFFORT, PATH).
# It wakes the sequencer EXACTLY ONCE and exits; the *:0/10 watchdog timer
# remains the fallback if this process dies.
#
#   session-watcher.sh <project-dir> wait      poll the declared wait via
#                                              `run_phase_guard.py wait-ready`
#                                              (rc 3 = still waiting) until
#                                              ready/expired, then wake.
#   session-watcher.sh <project-dir> relaunch  wake as soon as the current
#                                              session has exited.
#
# "Wake" = wait for the launch flock to be free (the session that spawned us
# has stopped), then invoke overnight-launch.sh detached. The launcher's own
# flock, snooze, backoff, and lifecycle gates all still apply -- this script
# adds no policy, only the wake-up.
set -u

PROJECT="${1:?project dir required}"
MODE="${2:?mode required: wait|relaunch}"
TODO_FILE="${3:-todo/TODO-Claude-Overnight-Runner.md}"

GUARD="$PROJECT/.claude/hooks/run_phase_guard.py"
LOCKFILE="$PROJECT/.claude/overnight/launch.lock"
ARMED="$PROJECT/.claude/state/sequencer-armed"
LAUNCHER="$PROJECT/scripts/overnight/overnight-launch.sh"
LOG="$PROJECT/.claude/overnight/watcher.log"

log() { echo "$(date -Is) [watcher:$MODE:$$] $*" >> "$LOG"; }

log "started"

if [ "$MODE" = "wait" ]; then
  # Poll the declared wait. rc 3 = still waiting; anything else (ready,
  # expired, wait cleared, guard error) ends the poll. Hard ceiling matches
  # the guard's WAIT_TIMEOUT_MAX_S plus slack so a guard failure cannot make
  # this loop immortal.
  DEADLINE=$(( $(date +%s) + 7500 ))
  while :; do
    [ -f "$ARMED" ] || { log "disarmed while waiting; exiting without wake"; exit 0; }
    rc=0
    python3 "$GUARD" wait-ready >/dev/null 2>&1 || rc=$?
    [ "$rc" != 3 ] && { log "wait resolved (rc=$rc)"; break; }
    [ "$(date +%s)" -ge "$DEADLINE" ] && { log "hard deadline; waking anyway"; break; }
    sleep 15
  done
fi

# Wait for the spawning session to actually exit (flock free). The Stop hook
# has already permitted the stop; this is normally seconds. Bounded: if the
# session refuses to die within 20 min, leave the wake to the watchdog.
FREED=0
for _ in $(seq 1 240); do
  if flock -n "$LOCKFILE" -c true 2>/dev/null; then FREED=1; break; fi
  sleep 5
done
if [ "$FREED" != 1 ]; then
  log "launch lock still held after 20 min; leaving the wake to the watchdog"
  exit 0
fi

[ -f "$ARMED" ] || { log "disarmed; exiting without wake"; exit 0; }

log "waking sequencer (launcher detached)"
setsid bash "$LAUNCHER" "$PROJECT" "$TODO_FILE" bypassPermissions \
  >> "$LOG" 2>&1 < /dev/null &
exit 0
