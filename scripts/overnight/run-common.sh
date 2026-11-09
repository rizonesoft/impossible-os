#!/usr/bin/env bash
# run-common.sh -- how the run's one session starts. Shared by run.sh and heartbeat.sh,
# and frozen with them. Sourced with the working directory at the repo root.
RUN_SESSION="${RUN_SESSION:-impossible-os-run}"
RUN_HEARTBEAT_SESSION="${RUN_HEARTBEAT_SESSION:-impossible-os-heartbeat}"
# tmux matches a bare -t name by prefix, so always target sessions exactly. A heartbeat
# named after the run made a dead run look alive (found on review, 2026-11-07).
run_target() { printf '=%s' "$1"; }
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

# The run's goal, set by the operator 2026-11-09. Both prompts carry it, so every start
# and every restart begins with it.
RUN_GOAL="The aim: a product that rivals Windows and Linux. Finish the roadmap to a verified fixpoint at that standard: a section is done when it is complete, polished and works end to end, not when its checklist is ticked. File every adjacent gap a real user would hit next in its owning TODO, without duplicating existing work."
RUN_PROMPT="$RUN_GOAL You are running the Impossible OS roadmap to completion. Invoke the overnight-sequencer skill and follow todo/TODO-Claude-Overnight-Runner.md. This one session does all the work: after a section ships, go straight on to the next one. Make decisions yourself and record them. A failure stops one item, never the run. Stop only at a verified fixpoint."
RUN_RESUME_PROMPT="$RUN_GOAL The session restarted. Read the state on disk (git status, git log, the run_phase_guard.py cursor) and continue the run. Invoke the overnight-sequencer skill if you no longer have it in context."

# A bounded run: `run.sh start --ships N` stops after N sections are through their
# post-ship review and pushed. The stop guard and the heartbeat both read this file.
RUN_SHIP_LIMIT=".claude/state/run-ship-limit"
run_limit_note() {
  [ -s "$RUN_SHIP_LIMIT" ] || return 0
  printf ' This run is bounded: ship %s section(s) all the way, through post-ship review and pushed, and then stop and say so. Do not start another.' "$(awk 'NR==1{print $1}' "$RUN_SHIP_LIMIT")"
}

# run_launch fresh|resume: start the session in tmux, with ChromeMCP off for this
# repo only (the env the systemd units set until 2026-11-07). It ends when claude exits, which
# is how the heartbeat knows it died. The run has its own session id, because
# --continue would resume whichever conversation in this folder was newest, and that
# can be an attended session rather than the run.
RUN_SESSION_ID_FILE=".claude/state/run-session-id"
RUN_TRANSCRIPTS="${HEARTBEAT_TRANSCRIPTS:-$HOME/.claude/projects/$(printf '%s' "$PWD" | sed 's#[^A-Za-z0-9]#-#g')}"
run_transcript() { printf '%s/%s.jsonl' "$RUN_TRANSCRIPTS" "$(cat "$RUN_SESSION_ID_FILE" 2>/dev/null)"; }
run_launch() {
  local session_flag prompt id
  # An id with no transcript never became a conversation, and --resume would fail on
  # it every time, so it starts fresh instead.
  if [ "$1" = resume ] && [ -s "$RUN_SESSION_ID_FILE" ] && [ -f "$(run_transcript)" ]; then
    id="$(cat "$RUN_SESSION_ID_FILE")"
    session_flag="--resume $id"
    prompt="$RUN_RESUME_PROMPT"
  else
    id="$(python3 -c 'import uuid; print(uuid.uuid4())')"
    printf '%s\n' "$id" > "$RUN_SESSION_ID_FILE"
    session_flag="--session-id $id"
    prompt="$RUN_PROMPT"
  fi
  prompt="$prompt$(run_limit_note)"
  tmux new-session -d -s "$RUN_SESSION" -x 200 -y 50 -c "$PWD" \
    -e OVERNIGHT_SEQUENCER_RUN=1 -e CLAUDE_PROJECT_DIR="$PWD" \
    -e MCP_NO_AUTO_CHROME=1 -e MCP_NO_AUTO_BRIDGE=1 \
    -e CLAUDE_CODE_AUTO_COMPACT_WINDOW="$RUN_COMPACT_AT" \
    "claude $session_flag --dangerously-skip-permissions --model '$RUN_MODEL' --effort '$RUN_EFFORT' \"$prompt\""
}
