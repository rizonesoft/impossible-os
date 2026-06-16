#!/usr/bin/env bash
# Headless launcher for the impossible-os overnight sequencer.
#
# Vendored 2026-06-16 from the rizonetech overnight-runner plugin
# (scripts/overnight-launch.sh, v0.2.4) and de-coupled from it: the repo owns
# this launcher so there is exactly ONE armed path (no competing unguarded
# plugin flow), no dependency on a user-cache plugin version, and the ChromeMCP
# lane logic is gated off for kernel runs at the source.
#
# Launched by the systemd user timer that scripts/overnight/overnight-arm.sh
# arms.  Args: <project-dir> <todo-file> <permission-mode>.
set -euo pipefail

PROJECT_DIR="${1:?project dir required}"
TODO_FILE="${2:?todo file required}"
PERMISSION_MODE="${3:-bypassPermissions}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# systemd user units get a minimal PATH; an unattended run needs the user-level
# CLIs (claude, codex) and the nvm node bin on it. Latest nvm node wins;
# harmless when the dirs do not exist.
NVM_NODE_BIN="$(ls -d "$HOME"/.nvm/versions/node/*/bin 2>/dev/null | sort -V | tail -1 || true)"
export PATH="$HOME/.local/bin${NVM_NODE_BIN:+:$NVM_NODE_BIN}:$PATH"

cd "$PROJECT_DIR"
export OVERNIGHT_RUNNER_BASE=.claude/overnight
REPORT_DIR="$PROJECT_DIR/.claude/overnight/reports"
mkdir -p "$REPORT_DIR"

# Limit-aware snooze: when a previous run died on a usage limit, the tail of
# this script recorded the reset time. Until then every (watchdog) launch is a
# cheap no-op, so the watchdog cadence never hammers a session OR a weekly limit.
SNOOZE_FILE="$PROJECT_DIR/.claude/overnight/snooze-until"
if [ -f "$SNOOZE_FILE" ]; then
  SNOOZE_UNTIL="$(cat "$SNOOZE_FILE" 2>/dev/null || echo 0)"
  NOW_EPOCH="$(date +%s)"
  if [ "$NOW_EPOCH" -lt "${SNOOZE_UNTIL:-0}" ] 2>/dev/null; then
    echo "usage-limit snooze active until $(date -d "@$SNOOZE_UNTIL" -Is 2>/dev/null || echo "$SNOOZE_UNTIL"), launch skipped $(date -Is)"
    exit 0
  fi
  rm -f "$SNOOZE_FILE"
fi

# Concurrency lock: only one launch per project at a time. Watchdog timers can
# call this freely -- it exits 0 immediately while a run is alive. The kernel
# releases the flock when the holding process dies (any exit, crash, or kill),
# so no stale-lock handling is needed.
LOCKFILE="$PROJECT_DIR/.claude/overnight/launch.lock"
exec 9>"$LOCKFILE"
flock -n 9 || { echo "run already active, launch skipped $(date -Is)"; exit 0; }

REPORT="$REPORT_DIR/run-$(date +%Y%m%d-%H%M%S).log"

# systemd user units don't inherit the login shell's PATH; resolve claude
# explicitly. CLAUDE_BIN overrides; then PATH, then common install homes.
resolve_claude() {
  if [ -n "${CLAUDE_BIN:-}" ] && [ -x "$CLAUDE_BIN" ]; then echo "$CLAUDE_BIN"; return; fi
  if command -v claude >/dev/null 2>&1; then command -v claude; return; fi
  for c in "$HOME/.local/bin/claude" "$HOME/.claude/local/claude" \
           "$HOME"/.nvm/versions/node/*/bin/claude; do
    [ -x "$c" ] && { echo "$c"; return; }
  done
  return 1
}

echo "overnight-sequencer unattended run starting $(date -Is)" | tee "$REPORT"
CLAUDE="$(resolve_claude)" || { echo "FATAL: claude CLI not found (set CLAUDE_BIN)" >> "$REPORT"; exit 127; }

# ChromeMCP lane isolation is OFF for kernel runs (OVERNIGHT_NO_CHROMEMCP=1, set
# by arm-sequencer.sh's per-unit env drop-in). impossible-os kernel work never
# drives a browser, and the lane-claim churn produced noise + spurious CDP
# errors in the report. A --with-browser (gh-pages) arm leaves OVERNIGHT_NO_-
# CHROMEMCP unset, so the lane logic below runs as before.
MCP_CONFIG_ARGS=()
if [ -z "${OVERNIGHT_NO_CHROMEMCP:-}" ]; then
  CHROMEMCP_BIN="$(command -v chromemcp || true)"
  [ -z "$CHROMEMCP_BIN" ] && [ -x "$HOME/ChromeMCP/chromemcp" ] && CHROMEMCP_BIN="$HOME/ChromeMCP/chromemcp"
  LANE=""
  LANE_MCP_CONFIG=""
  cleanup_lane() {
    [ -n "$LANE_MCP_CONFIG" ] && rm -f "$LANE_MCP_CONFIG"
    if [ -n "$LANE" ]; then
      "$CHROMEMCP_BIN" lane down --client claude "$LANE" >> "$REPORT" 2>&1 || true
      "$CHROMEMCP_BIN" lane release --client claude "$LANE" >> "$REPORT" 2>&1 || true
    fi
  }
  trap cleanup_lane EXIT
  if [ -n "$CHROMEMCP_BIN" ] && "$CHROMEMCP_BIN" lane help >/dev/null 2>&1; then
    if LANE_ENV="$("$CHROMEMCP_BIN" lane acquire --client claude \
          --owner "overnight-sequencer:$$" --format shell 2>> "$REPORT")"; then
      eval "$LANE_ENV"
      LANE="$CHROMEMCP_LANE"
      echo "claimed claude ChromeMCP lane $LANE ($MCP_URL)" >> "$REPORT"
      if "$CHROMEMCP_BIN" lane up --client claude "$LANE" >> "$REPORT" 2>&1; then
        TOKEN="$(MCP_TOKEN_PATH="$MCP_TOKEN_PATH" "$CHROMEMCP_BIN" token 2>/dev/null || true)"
        if [ -n "$TOKEN" ]; then
          LANE_MCP_CONFIG="$(mktemp)"
          printf '{"mcpServers":{"chromemcp":{"type":"http","url":"%s","headers":{"Authorization":"Bearer %s"}}}}\n' \
            "$MCP_URL" "$TOKEN" > "$LANE_MCP_CONFIG"
          MCP_CONFIG_ARGS=(--mcp-config "$LANE_MCP_CONFIG")
        else
          echo "lane $LANE token unavailable; session keeps the default chromemcp registration" >> "$REPORT"
        fi
        export CHROMEMCP_MCP_URL="$MCP_URL"
      else
        echo "lane $LANE stack failed to start; continuing on the default ChromeMCP stack" >> "$REPORT"
        unset CHROMEMCP_LANE CHROMEMCP_LANE_CLIENT CHROMEMCP_LANE_SUFFIX CHROMEMCP_CODEX_LANE_SUFFIX
      fi
      unset PORT UPSTREAM_PORT CDP_PORT MCP_URL MCP_TOKEN_PATH MCP_CHROME_PROFILE_NAME
    else
      echo "no free claude ChromeMCP lane; continuing on the default stack" >> "$REPORT"
    fi
  fi
fi

# The bootstrap prompt. We do NOT use the old plugin slash command
# (/overnight-runner:start) -- that path is gone with the plugin. The
# sequencer-armed marker + run_phase_guard.py redirect the very first tool call
# onto Skill(overnight-sequencer) regardless, but a clear instruction avoids a
# wasted turn. Every launch is a fresh headless agent (there is no real
# conversation resume); the guard cursor in .claude/state/sequencer-run.json is
# what carries state across relaunches.
read -r -d '' PROMPT <<PROMPT_EOF || true
You are this repository's unattended overnight sequencer, running headless under bypassPermissions. Invoke the overnight-sequencer skill now and drive ${TODO_FILE} to completion as the single source of truth.

Hard rules from that doctrine: the work unit is the ENTIRE TODO queue, not one section. NEVER stop or disarm the run. A blocker, hard failure, or operator-reserved decision is DEFERRED ([/] + a Deferred stamp + an XREF) and you ADVANCE to the next section/file -- it is never a reason to stop. The run ends ONLY on an oracle-verified \`run_phase_guard.py fixpoint\` (all work DONE or deferred-with-XREF) or the human operator's --disarm. If the guard cursor looks inactive after a relaunch, re-invoke Skill(overnight-sequencer) and resume from the recorded phase.
PROMPT_EOF

# stream-json through the formatter so the report streams progress live (plain
# `--output-format text` stays silent until the run ends).
set +o pipefail  # the pipeline must complete so PIPESTATUS captures claude's exit code
"$CLAUDE" -p "$PROMPT" \
  --output-format stream-json --verbose \
  --permission-mode "$PERMISSION_MODE" \
  ${MCP_CONFIG_ARGS[@]+"${MCP_CONFIG_ARGS[@]}"} 2>&1 \
  | python3 "$SCRIPT_DIR/stream-report.py" \
  | tee -a "$REPORT"
CLAUDE_EXIT="${PIPESTATUS[0]}"
set -o pipefail
if [ "$CLAUDE_EXIT" -ne 0 ]; then
  echo "claude exited non-zero: $CLAUDE_EXIT" >> "$REPORT"
fi

# Usage-limit detection: parse the reset hint from the report tail and write the
# snooze file the pre-flight honors. Session limits give a time of day ("resets
# 8:10pm (Area/City)"); weekly limits give a date. Best-effort: unparseable
# hints snooze 30 minutes (one watchdog tick); parses are capped at 8 days.
tail -40 "$REPORT" | python3 - "$SNOOZE_FILE" <<'PYEOF' >> "$REPORT" 2>&1 || true
import re, sys, time, datetime
text = sys.stdin.read()
m = re.search(r"hit your .{0,30}limit.{0,10}resets ([^\n]*)", text, re.I)
if not m:
    sys.exit(0)
hint = m.group(1).strip()
now = datetime.datetime.now()
until = None
tm = re.search(r"(\d{1,2}):(\d{2})\s*(am|pm)", hint, re.I)
if tm:
    h, mn = int(tm.group(1)) % 12, int(tm.group(2))
    if tm.group(3).lower() == "pm":
        h += 12
    until = now.replace(hour=h, minute=mn, second=0, microsecond=0)
    if until <= now:
        until += datetime.timedelta(days=1)
else:
    dm = re.search(r"([A-Z][a-z]{2,8})\s+(\d{1,2})", hint)
    if dm:
        try:
            month = datetime.datetime.strptime(dm.group(1)[:3], "%b").month
            until = now.replace(month=month, day=int(dm.group(2)),
                                hour=0, minute=15, second=0, microsecond=0)
            if until <= now:
                until = until.replace(year=until.year + 1)
        except ValueError:
            pass
if until is None:
    until = now + datetime.timedelta(minutes=30)
cap = now + datetime.timedelta(days=8)
if until > cap:
    until = cap
epoch = int(time.mktime(until.timetuple()))
open(sys.argv[1], "w").write(str(epoch))
print(f"usage limit detected (resets {hint}); snoozing launches until {until.isoformat()}")
PYEOF

echo "overnight-sequencer unattended run finished $(date -Is)" >> "$REPORT"
