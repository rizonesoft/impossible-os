#!/usr/bin/env bash
# Non-model session watcher for the overnight sequencer.
#
# Spawned DETACHED by run_phase_guard.py (`wait` and `rollover` verbs) from
# inside the headless session, so it inherits the run's environment
# (OVERNIGHT_SEQUENCER_RUN=1, OVERNIGHT_MODEL, OVERNIGHT_EFFORT, PATH).
# It wakes the sequencer EXACTLY ONCE and exits; the *:0/10 watchdog timer
# remains the fallback if this process dies.
#
#   session-watcher.sh <project-dir> wait <gen>      poll the declared wait via
#                                              `run_phase_guard.py wait-ready`
#                                              (rc 3 = still waiting) until
#                                              ready/expired, then wake.
#   session-watcher.sh <project-dir> relaunch <gen>  wake as soon as the
#                                              current session has exited.
#
# "Wake" = wait for the launch flock to be free (the session that spawned us
# has stopped), then hand off to overnight-launch.sh via `exec` (NOT a
# detached `setsid ... &`). This watcher runs in its OWN systemd transient
# scope (KillMode=control-group); a backgrounded child would be killed the
# instant this script exits and the scope's cgroup is torn down -- the b80919e7
# cgroup-escape bug, shifted one level downstream (live failure 2026-07-11
# 12:02: "waking sequencer" logged, no runner survived). exec REPLACES this
# process with the launcher, so the scope's main process IS the launcher and
# the scope lives exactly as long as the run needs.
#
# FAIL-CLOSED: a guard/state error (wait-ready rc not in {0,3}) is NOT treated
# as "ready to wake" -- waking into an unresolved wait could bypass an
# unfinished review. On a state error the watcher keeps polling (to the hard
# deadline) rather than waking.
#
# GENERATION: a superseding wait bumps the gen and cancels this watcher's unit.
# Before waking, the watcher re-reads the wait state and aborts if its gen no
# longer matches -- defence against a stale generation double-waking.
set -u

PROJECT="${1:?project dir required}"
MODE="${2:?mode required: wait|relaunch}"
GEN="${3:-0}"
TODO_FILE="${4:-todo/TODO-Claude-Overnight-Runner.md}"

GUARD="$PROJECT/.claude/hooks/run_phase_guard.py"
LOCKFILE="$PROJECT/.claude/overnight/launch.lock"
ARMED="$PROJECT/.claude/state/sequencer-armed"
STATE="$PROJECT/.claude/state/sequencer-run.json"
LAUNCHER="$PROJECT/scripts/overnight/overnight-launch.sh"
LOG="$PROJECT/.claude/overnight/watcher.log"

# Poll cadence (override for tests; default 15s keeps the live run cheap).
POLL_SECS="${SEQ_WATCHER_POLL_SECS:-15}"

log() { echo "$(date -Is) [watcher:$MODE:gen$GEN:$$] $*" >> "$LOG"; }

# Current wait generation from the atomic state file (empty on any error).
cur_gen() {
  python3 -c "import json,sys
try:
    print((json.load(open('$STATE')).get('waiting') or {}).get('gen',''))
except Exception:
    pass" 2>/dev/null
}

log "started"

if [ "$MODE" = "wait" ]; then
  DEADLINE=$(( $(date +%s) + 7500 ))
  while :; do
    [ -f "$ARMED" ] || { log "disarmed while waiting; exiting without wake"; exit 0; }
    # Superseded by a newer wait generation? Abort silently (the new watcher owns it).
    G="$(cur_gen)"
    if [ -n "$G" ] && [ "$G" != "$GEN" ]; then
      log "superseded by gen=$G; exiting without wake"; exit 0
    fi
    rc=0
    python3 "$GUARD" wait-ready >/dev/null 2>&1 || rc=$?
    case "$rc" in
      3) : ;;                                   # still waiting
      0) log "wait resolved (ready/expired/cleared)"; break ;;
      *) log "wait-ready state error (rc=$rc); FAIL-CLOSED, keep polling" ;;
    esac
    [ "$(date +%s)" -ge "$DEADLINE" ] && { log "hard deadline; waking anyway"; break; }
    sleep "$POLL_SECS"
  done
fi

# Wait for the spawning session to actually exit (flock free). Bounded: if the
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

# Final generation re-check (wait mode): a supersede between poll-exit and now
# must not double-wake.
if [ "$MODE" = "wait" ]; then
  G="$(cur_gen)"
  if [ -n "$G" ] && [ "$G" != "$GEN" ]; then
    log "superseded by gen=$G at wake time; exiting without wake"; exit 0
  fi
fi

log "waking sequencer via exec (launcher becomes this scope's main process)"
exec bash "$LAUNCHER" "$PROJECT" "$TODO_FILE" bypassPermissions >> "$LOG" 2>&1 < /dev/null
