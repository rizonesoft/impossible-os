#!/usr/bin/env bash
# run.sh -- the run, under one roof: one Claude session in tmux, plus its heartbeat.
#
#   bash scripts/run.sh start     start the run, or resume it if it was running
#   bash scripts/run.sh start --ships N   the same, stopping after N sections ship
#   bash scripts/run.sh attach    watch it and talk to it (detach: Ctrl-b d)
#   bash scripts/run.sh status    alive or not, and the health numbers
#   bash scripts/run.sh stop      stop it; `start` picks the work up again
#   bash scripts/run.sh autostart start it only if it was live (the boot service runs this)
#
# `start` installs a systemd user service that runs `autostart` when WSL boots, so a
# run that was live comes back after a WSL restart or a Windows reboot, and a stopped
# run stays stopped. The service records the PATH `start` was run with.
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
RUN_SYSTEMD_DIR="${RUN_SYSTEMD_DIR:-$HOME/.config/systemd/user}"
RUN_UNIT="impossible-os-run.service"

# One value inside a double-quoted systemd setting: backslash, quote and the %
# specifier are escaped, and `$` too where systemd would expand it (ExecStart).
sd_quote() {
  local v="$1"
  v="${v//\\/\\\\}"
  v="${v//\"/\\\"}"
  v="${v//%/%%}"
  [ "${2:-}" = exec ] && v="${v//\$/\$\$}"
  printf '%s' "$v"
}

# The boot service. Rewritten on every start so it carries the current PATH and repo path.
install_autostart() {
  [ "${RUN_NO_AUTOSTART:-}" = "1" ] && return 0
  command -v systemctl >/dev/null 2>&1 \
    || { echo "note: no systemctl, so the run will not come back after a WSL restart"; return 0; }
  mkdir -p "$RUN_SYSTEMD_DIR"
  local unit="$RUN_SYSTEMD_DIR/$RUN_UNIT" tmp
  tmp="$(mktemp)"
  {
    echo "[Unit]"
    echo "Description=Impossible OS run: resume at boot if it was live"
    echo "After=default.target"
    echo ""
    echo "[Service]"
    echo "Type=oneshot"
    echo "# The run's tmux server lives in this service's cgroup; it must outlive ExecStart."
    echo "RemainAfterExit=yes"
    echo "Environment=\"PATH=$(sd_quote "$PATH")\""
    echo "ExecStart=/bin/bash \"$(sd_quote "$ROOT/scripts/run.sh" exec)\" autostart"
    echo ""
    echo "[Install]"
    echo "WantedBy=default.target"
  } > "$tmp"
  if cmp -s "$tmp" "$unit"; then
    rm -f "$tmp"
  else
    mv "$tmp" "$unit"
    systemctl --user daemon-reload >/dev/null 2>&1 \
      || echo "note: systemctl --user daemon-reload failed, so the boot service may still be the old one"
  fi
  systemctl --user enable "$RUN_UNIT" >/dev/null 2>&1 \
    || echo "note: could not enable $RUN_UNIT, so the run will not come back after a WSL restart"
}

case "${1:-status}" in
  start)
    for tool in tmux git python3 clang-19 nasm ld.lld-19; do
      command -v "$tool" >/dev/null 2>&1 || { echo "REFUSED: $tool is not installed. Run: bash scripts/setup.sh" >&2; exit 1; }
    done
    # The session is `claude` in a tmux pane: without it on PATH the pane dies at once and
    # `start` would still say "started". A non-login shell (a desktop shortcut) lacks ~/.local/bin.
    command -v claude >/dev/null 2>&1 \
      || { echo "REFUSED: claude is not on PATH ($PATH). Start from a login shell (bash -l)." >&2; exit 1; }
    if ! denied="$(python3 scripts/overnight/skill_denies.py "$ROOT")"; then
      { echo "REFUSED: a permission deny refuses a skill the run needs:"; printf '  %s\n' "$denied"; } >&2
      exit 1
    fi
    mkdir -p .claude/state
    rm -f .claude/state/sequencer-fixpoint
    # A bound belongs to the run, so a restart keeps it and its starting point, and only
    # `stop` clears it. Recovering from a crash must not turn a bounded run unbounded.
    if [ "${2:-}" = "--ships" ]; then
      case "${3:-}" in
        ''|*[!0-9]*|0) echo "usage: run.sh start [--ships N]" >&2; exit 2 ;;
      esac
      if [ -s "$RUN_SHIP_LIMIT" ]; then
        echo "keeping this run's bound: $(awk 'NR==1{print $1}' "$RUN_SHIP_LIMIT") section(s). To change it: run.sh stop first."
      else
        # Counted from here, so the sections it ships are the only ones that count.
        printf '%s %s\n' "$3" "$(git rev-parse HEAD)" > "$RUN_SHIP_LIMIT"
      fi
    fi
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
    install_autostart
    ;;
  autostart)
    # At boot: bring back only a run that was live. `stop` removes the marker and a
    # verified fixpoint leaves its sentinel, so a stopped or finished run stays down.
    # A bounded run that shipped its sections is finished too, though its marker stays.
    if [ ! -f "$RUN_MARKER" ] || [ -f .claude/state/sequencer-fixpoint ] \
       || python3 .claude/hooks/run_phase_guard.py ship-limit-reached 2>/dev/null; then
      echo "no live run to resume"
      exit 0
    fi
    exec bash "$ROOT/scripts/run.sh" start
    ;;
  attach)
    exec tmux attach -t "$RUN_T"
    ;;
  status)
    tmux has-session -t "$RUN_T" 2>/dev/null && echo "session: alive" || echo "session: not running"
    tmux has-session -t "$HEART_T" 2>/dev/null && echo "heartbeat: alive" || echo "heartbeat: not running"
    if systemctl --user is-enabled "$RUN_UNIT" >/dev/null 2>&1; then
      echo "autostart: on (a live run resumes when WSL boots)"
    else
      echo "autostart: off (the next start turns it on)"
    fi
    [ -f .claude/overnight/run-health.json ] && cat .claude/overnight/run-health.json
    ;;
  stop)
    # Stopping is the operator's. The run's own session must not be able to end itself.
    if [ "${OVERNIGHT_SEQUENCER_RUN:-}" = "1" ]; then
      echo "REFUSED: the run cannot stop itself. Stop it from your own shell." >&2
      exit 1
    fi
    rm -f "$RUN_MARKER" "$RUN_SHIP_LIMIT"
    tmux kill-session -t "$HEART_T" 2>/dev/null || true
    tmux kill-session -t "$RUN_T" 2>/dev/null || true
    python3 .claude/hooks/run_phase_guard.py clear "stopped via run.sh" >/dev/null 2>&1 || true
    echo "stopped. \`start\` resumes this run's conversation; delete .claude/state/run-session-id for a new one."
    ;;
  *)
    echo "usage: run.sh start|attach|status|stop|autostart" >&2
    exit 2
    ;;
esac
