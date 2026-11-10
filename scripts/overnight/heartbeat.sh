#!/usr/bin/env bash
# heartbeat.sh -- checks the run's one session from outside. Frozen: a run may not edit it.
#
# Relaunching is not how this design works, so all of this is only for faults:
#   dead session    restart it with --resume, backing off if it keeps dying
#   idle session    nudge it, unless someone is attached; this also clears a finished
#                   usage-limit wait
#   no ship in 6 h  supervisor.py alerts, and the session is told to find out why
#
# bash heartbeat.sh --once runs one check (tests use this). Without it, every 5 minutes.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
cd "$ROOT"
# shellcheck source=scripts/overnight/run-common.sh
. "$ROOT/scripts/overnight/run-common.sh"
STATE=".claude/overnight/heartbeat-state"
TICK_SECS="${HEARTBEAT_TICK_SECS:-300}"
IDLE_SECS="${HEARTBEAT_IDLE_SECS:-2700}"       # longer than any bounded tool call
MAX_BACKOFF_SECS=3600
mkdir -p "$(dirname "$STATE")"

idle_seconds() {
  # This session's own transcript, or the launch time if it has not written one yet.
  # An attended session writing beside it says nothing about this run.
  local transcript; transcript="$(run_transcript)"
  [ -f "$transcript" ] || transcript="$RUN_SESSION_ID_FILE"
  [ -f "$transcript" ] || { echo 0; return; }
  echo $(( $(date +%s) - $(stat -c %Y "$transcript") ))
}

state_get() { awk -v k="$1" '$1==k{print $2}' "$STATE" 2>/dev/null | tail -1; }
state_set() {
  { grep -v "^$1 " "$STATE" 2>/dev/null; echo "$1 $2"; } > "$STATE.tmp"
  mv "$STATE.tmp" "$STATE"
}

RUN_T="$(run_target "$RUN_SESSION")"
# A person attached may be mid-sentence, and Enter would send their half-typed prompt.
# A read-only viewer (`run.sh watch`) cannot type, so it does not count:
# a watch window left open must not switch the nudges off.
someone_attached() { tmux list-clients -t "$RUN_T" -F '#{client_readonly}' 2>/dev/null | grep -qx 0; }
nudge() {
  someone_attached && return 0
  [ "$(idle_seconds)" -ge "$IDLE_SECS" ] || return 0  # the supervisor call took time
  tmux send-keys -t "$RUN_T:" -l "$1" 2>/dev/null
  tmux send-keys -t "$RUN_T:" Enter 2>/dev/null
}

check() {
  # Stopped by an operator, or finished: the heartbeat's job is over.
  [ -f "$RUN_MARKER" ] || return 10
  [ -f .claude/state/sequencer-fixpoint ] && return 10
  # A bounded run that shipped its sections is finished. The session stays open to talk to.
  if python3 .claude/hooks/run_phase_guard.py ship-limit-reached 2>/dev/null; then
    bash scripts/overnight/notify.sh "$ROOT" info "The run shipped its sections and stopped." >/dev/null 2>&1 || true
    return 10
  fi
  local now idle; now="$(date +%s)"
  if ! tmux has-session -t "$RUN_T" 2>/dev/null; then
    local deaths wait_until
    deaths="$(state_get deaths)"; deaths="${deaths:-0}"
    wait_until="$(state_get wait_until)"; wait_until="${wait_until:-0}"
    [ "$now" -ge "$wait_until" ] || return 0
    deaths=$((deaths + 1))
    local backoff=$(( TICK_SECS * (2 ** (deaths > 5 ? 5 : deaths)) ))
    [ "$backoff" -gt "$MAX_BACKOFF_SECS" ] && backoff="$MAX_BACKOFF_SECS"
    state_set deaths "$deaths"
    state_set wait_until "$((now + backoff))"
    state_set last_restart "$now"
    run_launch resume
    bash scripts/overnight/notify.sh "$ROOT" critical "The run's session died and was restarted (death $deaths)." >/dev/null 2>&1 || true
    return 0
  fi
  # An hour alive after a restart means the restart worked, so stop backing off.
  local last; last="$(state_get last_restart)"
  if [ -n "$last" ] && [ $((now - last)) -ge "$MAX_BACKOFF_SECS" ]; then
    state_set deaths 0; state_set wait_until 0; state_set last_restart ""
  fi
  idle="$(idle_seconds)"
  local args=()
  # Someone attached may be using the tree, so the supervisor must not undo under them.
  if [ "$idle" -ge "$IDLE_SECS" ] && ! someone_attached; then args+=(--idle); fi
  local health
  health="$(timeout 2400 python3 scripts/overnight/supervisor.py "$ROOT" ${args[@]+"${args[@]}"} 2>/dev/null || true)"
  if [ "$idle" -ge "$IDLE_SECS" ]; then
    if printf '%s' "$health" | grep -q '"repair_needed": true'; then
      nudge "Nothing has shipped for hours. Before more section work, find out why from git log, the gate log and the test artifacts. Fix the cause in the layer that owns it, or record it and move to another section."
    else
      nudge "Continue the run."
    fi
  fi
  return 0
}

if [ "${1:-}" = "--once" ]; then
  check
  rc=$?
  [ "$rc" -eq 10 ] && exit 0
  exit "$rc"
fi
while :; do
  check
  [ $? -eq 10 ] && exit 0
  sleep "$TICK_SECS"
done
