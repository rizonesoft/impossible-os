#!/usr/bin/env bash
# run-common.sh -- how the run's one session starts. Shared by run.sh and heartbeat.sh,
# and frozen with them. Sourced with the working directory at the repo root.
RUN_SESSION="${RUN_SESSION:-impossible-os-run}"
RUN_MARKER=".claude/state/sequencer-armed"
RUN_COMPACT_AT="${RUN_COMPACT_AT:-400000}"
RUN_MODEL="${RUN_MODEL:-opus}"
RUN_EFFORT="${RUN_EFFORT:-medium}"
# Optional alert destination, kept outside the repo. Never printed.
RUN_NOTIFY_ENV="${RUN_NOTIFY_ENV:-$HOME/.config/impossible-os/notify.env}"
if [ -f "$RUN_NOTIFY_ENV" ]; then
  # shellcheck disable=SC1090
  set -a; . "$RUN_NOTIFY_ENV"; set +a
fi

RUN_PROMPT="You are running the Impossible OS roadmap to completion. Invoke the overnight-sequencer skill and follow todo/TODO-Claude-Overnight-Runner.md. This one session does all the work: after a section ships, go straight on to the next one. Make decisions yourself and record them. A failure stops one item, never the run. Stop only at a verified fixpoint."
RUN_RESUME_PROMPT="The session restarted. Read the state on disk (git status, git log, the run_phase_guard.py cursor) and continue the run. Invoke the overnight-sequencer skill if you no longer have it in context."

# run_launch fresh|resume: start the session in tmux. It ends when claude exits, which
# is how the heartbeat knows it died. The run has its own session id, because
# --continue would resume whichever conversation in this folder was newest, and that
# can be an attended session rather than the run.
RUN_SESSION_ID_FILE=".claude/state/run-session-id"
run_launch() {
  local session_flag prompt id
  if [ "$1" = resume ] && [ -s "$RUN_SESSION_ID_FILE" ]; then
    id="$(cat "$RUN_SESSION_ID_FILE")"
    session_flag="--resume $id"
    prompt="$RUN_RESUME_PROMPT"
  else
    id="$(python3 -c 'import uuid; print(uuid.uuid4())')"
    printf '%s\n' "$id" > "$RUN_SESSION_ID_FILE"
    session_flag="--session-id $id"
    prompt="$RUN_PROMPT"
  fi
  tmux new-session -d -s "$RUN_SESSION" -x 200 -y 50 -c "$PWD" \
    -e OVERNIGHT_SEQUENCER_RUN=1 -e CLAUDE_PROJECT_DIR="$PWD" \
    -e CLAUDE_CODE_AUTO_COMPACT_WINDOW="$RUN_COMPACT_AT" \
    "claude $session_flag --dangerously-skip-permissions --model '$RUN_MODEL' --effort '$RUN_EFFORT' \"$prompt\""
}
