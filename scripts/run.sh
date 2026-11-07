#!/usr/bin/env bash
# run.sh -- the run, under one roof: one Claude session in tmux, plus its heartbeat.
#
#   bash scripts/run.sh start     start the run, or resume it if it was running
#   bash scripts/run.sh attach    watch it and talk to it (detach: Ctrl-b d)
#   bash scripts/run.sh status    alive or not, and the health numbers
#   bash scripts/run.sh stop      stop it; `start` picks the work up again
#
# Subagents and Codex still run separately. Everything else is this one session, so
# there is nothing to relaunch and no second process to coordinate with. Design:
# docs/specs/2026-11-07-run-supervisor-design.md. A run may not edit this file.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$ROOT"
# shellcheck source=scripts/overnight/run-common.sh
. "$ROOT/scripts/overnight/run-common.sh"
RUN_T="$(run_target "$RUN_SESSION")"
HEART_T="$(run_target "$RUN_HEARTBEAT_SESSION")"

case "${1:-status}" in
  start)
    for tool in tmux git python3 clang-19 nasm ld.lld-19; do
      command -v "$tool" >/dev/null 2>&1 || { echo "REFUSED: $tool is not installed. Run: bash scripts/setup.sh" >&2; exit 1; }
    done
    if ! denied="$(python3 scripts/overnight/skill_denies.py "$ROOT")"; then
      { echo "REFUSED: a permission deny refuses a skill the run needs:"; printf '  %s\n' "$denied"; } >&2
      exit 1
    fi
    mkdir -p .claude/state
    rm -f .claude/state/sequencer-fixpoint
    date -Is > "$RUN_MARKER"
    if tmux has-session -t "$RUN_T" 2>/dev/null; then
      echo "already running. Watch it: bash scripts/run.sh attach"
    else
      # Resumes this run's own conversation if it has one.
      run_launch resume
      echo "started. Watch it: bash scripts/run.sh attach"
    fi
    tmux has-session -t "$HEART_T" 2>/dev/null \
      || tmux new-session -d -s "$RUN_HEARTBEAT_SESSION" "bash '$ROOT/scripts/overnight/heartbeat.sh'"
    ;;
  attach)
    exec tmux attach -t "$RUN_T"
    ;;
  status)
    tmux has-session -t "$RUN_T" 2>/dev/null && echo "session: alive" || echo "session: not running"
    tmux has-session -t "$HEART_T" 2>/dev/null && echo "heartbeat: alive" || echo "heartbeat: not running"
    [ -f .claude/overnight/run-health.json ] && cat .claude/overnight/run-health.json
    ;;
  stop)
    # Stopping is the operator's. The run's own session must not be able to end itself.
    if [ "${OVERNIGHT_SEQUENCER_RUN:-}" = "1" ]; then
      echo "REFUSED: the run cannot stop itself. Stop it from your own shell." >&2
      exit 1
    fi
    rm -f "$RUN_MARKER"
    tmux kill-session -t "$HEART_T" 2>/dev/null || true
    tmux kill-session -t "$RUN_T" 2>/dev/null || true
    python3 .claude/hooks/run_phase_guard.py clear "stopped via run.sh" >/dev/null 2>&1 || true
    echo "stopped. \`start\` resumes this run's conversation; delete .claude/state/run-session-id for a new one."
    ;;
  *)
    echo "usage: run.sh start|attach|status|stop" >&2
    exit 2
    ;;
esac
