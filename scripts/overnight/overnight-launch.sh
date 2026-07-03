#!/usr/bin/env bash
# Headless launcher for the impossible-os overnight sequencer.
#
# Vendored 2026-06-16 from the rizonetech overnight-runner plugin
# (scripts/overnight-launch.sh, v0.2.4) and de-coupled from it: the repo owns
# this launcher so there is exactly ONE armed path (no competing unguarded
# plugin flow), no dependency on a user-cache plugin version, and the ChromeMCP
# lane logic is gated off for kernel runs at the source.
#
# Claude is the only overnight driver (the Codex-driver path was retired in the
# tool-neutral AI-workflow re-scope). Launched by the systemd user timer that
# scripts/overnight/overnight-arm.sh arms.
# Args: <project-dir> <todo-file> <permission-mode>.
set -euo pipefail

PROJECT_DIR="${1:?project dir required}"
TODO_FILE="${2:?todo file required}"
PERMISSION_MODE="${3:-bypassPermissions}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# systemd user units get a minimal PATH; an unattended run needs the user-level
# claude CLI and the nvm node bin on it. Latest nvm node wins; harmless when the
# dirs do not exist.
NVM_NODE_BIN="$(ls -d "$HOME"/.nvm/versions/node/*/bin 2>/dev/null | sort -V | tail -1 || true)"
export PATH="$HOME/.local/bin${NVM_NODE_BIN:+:$NVM_NODE_BIN}:$PATH"

cd "$PROJECT_DIR"
RUNTIME_BASE_REL=".claude/overnight"
export OVERNIGHT_RUNNER_BASE="$RUNTIME_BASE_REL"
RUNTIME_BASE="$PROJECT_DIR/$RUNTIME_BASE_REL"
if [ -e "$PROJECT_DIR/${RUNTIME_BASE_REL%%/*}" ] && [ ! -w "$PROJECT_DIR/${RUNTIME_BASE_REL%%/*}" ]; then
  echo "FATAL: $PROJECT_DIR/${RUNTIME_BASE_REL%%/*} is not writable; cannot create $RUNTIME_BASE_REL" >&2
  echo "Fix: chmod u+w $PROJECT_DIR/${RUNTIME_BASE_REL%%/*}" >&2
  exit 1
fi
REPORT_DIR="$RUNTIME_BASE/reports"
mkdir -p "$REPORT_DIR"

# Limit-aware snooze: when a previous run died on a usage limit, the tail of
# this script recorded the reset time. Until then every (watchdog) launch is a
# cheap no-op, so the watchdog cadence never hammers a session OR a weekly limit.
SNOOZE_FILE="$RUNTIME_BASE/snooze-until"
if [ -f "$SNOOZE_FILE" ]; then
  SNOOZE_UNTIL="$(cat "$SNOOZE_FILE" 2>/dev/null || echo 0)"
  NOW_EPOCH="$(date +%s)"
  if [ "$NOW_EPOCH" -lt "${SNOOZE_UNTIL:-0}" ] 2>/dev/null; then
    echo "usage-limit snooze active until $(date -d "@$SNOOZE_UNTIL" -Is 2>/dev/null || echo "$SNOOZE_UNTIL"), launch skipped $(date -Is)"
    exit 0
  fi
  rm -f "$SNOOZE_FILE"
fi

# Watchdog backoff (runner-kit 2026-07-03): when the run is persistently
# BLOCKED (only recoverable deferrals left), every watchdog tick would re-run
# the oracle + heal probe against the same dead blocker. Linear backoff capped
# at 30 min; reset to full cadence the moment real work runs (cleared by the
# lifecycle gate below).
BACKOFF_FILE="$RUNTIME_BASE/watchdog-backoff-until"
if [ -f "$BACKOFF_FILE" ]; then
  BACKOFF_UNTIL="$(cut -d' ' -f1 "$BACKOFF_FILE" 2>/dev/null || echo 0)"
  if [ "$(date +%s)" -lt "${BACKOFF_UNTIL:-0}" ] 2>/dev/null; then
    echo "watchdog backoff active until $(date -d "@$BACKOFF_UNTIL" -Is 2>/dev/null || echo "$BACKOFF_UNTIL"), launch skipped $(date -Is)"
    exit 0
  fi
fi

# Concurrency lock: only one launch per project at a time. Watchdog timers can
# call this freely -- it exits 0 immediately while a run is alive. The kernel
# releases the flock when the holding process dies (any exit, crash, or kill),
# so no stale-lock handling is needed.
LOCKFILE="$RUNTIME_BASE/launch.lock"
exec 9>"$LOCKFILE"
flock -n 9 || { echo "run already active, launch skipped $(date -Is)"; exit 0; }

# --- Self-healing lifecycle gate (runner-kit law 2, adopted 2026-07-03) ------
# A cheap, no-Claude oracle classification decides what this tick does:
#   NEEDS_WORK -> fall through and run Claude (work to do now)
#   BLOCKED    -> only recoverable (awaiting-*) deferrals remain; run the heal
#                 probe. Healed -> run; else cheap-exit but STAY ARMED.
#   DONE       -> TRUE fixpoint. The only auto-stop: sentinel + disarm timers.
# The agent-invoked `run_phase_guard.py fixpoint` remains the in-session
# completion path; this gate is the launcher-side mirror so a tick never spawns
# Claude against a finished or fully-blocked queue.
# Skipped under DRYRUN (test-launch.sh) and OVERNIGHT_SEQUENCER_FORCE=1.
ORACLE="$PROJECT_DIR/.claude/hooks/sequencer_triage.py"
FIXPOINT_SENTINEL="$PROJECT_DIR/.claude/state/sequencer-fixpoint"
ARMED_MARKER_FILE="$PROJECT_DIR/.claude/state/sequencer-armed"
disarm_timers() {  # stop future fires; the current oneshot service still exits 0
  local unit="overnight-$(basename "$PROJECT_DIR")"
  systemctl --user stop "${unit}.timer" "${unit}-watchdog.timer" 2>/dev/null || true
}
if [ "${OVERNIGHT_SEQUENCER_DRYRUN:-}" != "1" ] && [ "${OVERNIGHT_SEQUENCER_FORCE:-}" != "1" ] \
   && [ -f "$ORACLE" ]; then
  LIFECYCLE_STATE="$(cd "$PROJECT_DIR" && python3 "$ORACLE" --next 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin).get("status",""))' 2>/dev/null || true)"
  case "$LIFECYCLE_STATE" in
    DONE)
      mkdir -p "$(dirname "$FIXPOINT_SENTINEL")"
      printf 'TRUE FIXPOINT -- %s\nOracle: sequencer_triage.py --next => DONE (no recoverable deferrals left).\nAuto-disarmed by overnight-launch.sh. Re-arm to resume.\n' \
        "$(date -Is)" > "$FIXPOINT_SENTINEL"
      echo "TRUE fixpoint reached (oracle: DONE) -- disarming sequencer timers $(date -Is)"
      python3 "$SCRIPT_DIR/run-status.py" "$PROJECT_DIR" --stamp "$(date -Is)" >/dev/null 2>&1 || true
      python3 "$SCRIPT_DIR/collect-questions.py" "$PROJECT_DIR" --stamp "$(date -Is)" >/dev/null 2>&1 || true
      bash "$SCRIPT_DIR/notify.sh" "$PROJECT_DIR" fixpoint "Overnight queue 100% finished -- sequencer disarmed." 2>/dev/null || true
      rm -f "$BACKOFF_FILE" 2>/dev/null || true
      rm -f "$ARMED_MARKER_FILE" 2>/dev/null || true  # launcher-side lifecycle end mirrors the fixpoint CLI's disarm
      disarm_timers
      exit 0
      ;;
    BLOCKED)
      rm -f "$FIXPOINT_SENTINEL" 2>/dev/null || true  # not finished -- clear any stale sentinel
      if bash "$SCRIPT_DIR/lifecycle-unblocked.sh" "$PROJECT_DIR"; then
        echo "blocker cleared -- resuming run $(date -Is)"
        rm -f "$BACKOFF_FILE" 2>/dev/null || true  # real work resuming -> full cadence
      else
        PREV_COUNT="$(cut -d' ' -f2 "$BACKOFF_FILE" 2>/dev/null || echo 0)"
        [ -n "$PREV_COUNT" ] || PREV_COUNT=0
        NEXT_COUNT=$((PREV_COUNT + 1))
        DELAY=$((600 * NEXT_COUNT)); [ "$DELAY" -gt 1800 ] && DELAY=1800
        echo "$(( $(date +%s) + DELAY )) $NEXT_COUNT" > "$BACKOFF_FILE"
        python3 "$SCRIPT_DIR/collect-questions.py" "$PROJECT_DIR" --stamp "$(date -Is)" >/dev/null 2>&1 || true
        echo "blocked on recoverable deferrals; staying armed, backing off ${DELAY}s, launch skipped $(date -Is)"
        exit 0
      fi
      ;;
    *)  # NEEDS_WORK (or oracle unavailable -- fail-open to the agent path);
        # any sentinel is stale and real work resets the backoff cadence.
      rm -f "$FIXPOINT_SENTINEL" 2>/dev/null || true
      rm -f "$BACKOFF_FILE" 2>/dev/null || true
      ;;
  esac
  # Inspection seam: print the decision and exit before spinning up Claude.
  if [ "${OVERNIGHT_SEQUENCER_GATE_ONLY:-}" = "1" ]; then
    echo "GATE: would run (state=${LIFECYCLE_STATE:-NEEDS_WORK})"
    exit 0
  fi
fi
# ------------------------------------------------------------------------------

# Report-log rotation: the watchdog relaunches every 10 min, so over many nights
# (and especially a usage-limit retry that mis-snoozed) the reports dir grows
# without bound. Keep only the most recent $OVERNIGHT_REPORT_KEEP run logs; prune
# the rest right before we add this launch's log. The process-substitution read
# keeps the inner pipeline's exit out of set -e, and the wrapper is belt-and-
# suspenders so a prune failure never aborts an otherwise-healthy launch.
prune_reports() {
  local keep="${OVERNIGHT_REPORT_KEEP:-40}" old
  while IFS= read -r old; do
    [ -n "$old" ] && rm -f "$old"
  done < <(ls -1t "$REPORT_DIR"/run-*.log 2>/dev/null | tail -n +"$((keep + 1))")
}
prune_reports || true

REPORT="$REPORT_DIR/run-$(date +%Y%m%d-%H%M%S).log"
ln -sfn "$(basename "$REPORT")" "$REPORT_DIR/latest.log"

# WS3: per-run metrics sidecar (read by stream-report.py). Shares the report
# log's basename so a report and its metrics pair up.
METRICS_DIR="$RUNTIME_BASE/metrics"
mkdir -p "$METRICS_DIR"
export OVERNIGHT_METRICS_FILE="$METRICS_DIR/$(basename "${REPORT%.log}").jsonl"

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

{
  echo "overnight-sequencer unattended run starting $(date -Is)"
  echo "driver: claude"
  echo "todo: $TODO_FILE"
  echo "runtime: $RUNTIME_BASE_REL"
} | tee "$REPORT"

# Dry-run stop point (runner-kit 2026-07-03; exercised by test-launch.sh):
# everything above (snooze, backoff, lock, report path + rotation, metrics
# sidecar) has been exercised. Exit before acquiring a ChromeMCP lane or
# invoking Claude. The flock on fd 9 is released by the kernel on this exit.
if [ "${OVERNIGHT_SEQUENCER_DRYRUN:-}" = "1" ]; then
  DRYRUN_CLAUDE="$(resolve_claude 2>/dev/null || echo missing)"
  echo "DRYRUN ok: report=$REPORT claude=$DRYRUN_CLAUDE mode=$PERMISSION_MODE todo-hint=$TODO_FILE" | tee -a "$REPORT"
  exit 0
fi

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

# The Claude bootstrap prompt. We do NOT use the old plugin slash command
# (/overnight-runner:start) -- that path is gone with the plugin. The
# sequencer-armed marker + run_phase_guard.py redirect the very first tool call
# onto Skill(overnight-sequencer) regardless, but a clear instruction avoids a
# wasted turn. Every launch is a fresh headless agent (there is no real
# conversation resume); the guard cursor in .claude/state/sequencer-run.json is
# what carries state across relaunches.
read -r -d '' CLAUDE_PROMPT <<PROMPT_EOF || true
You are this repository's unattended overnight sequencer, running headless under bypassPermissions. Invoke the overnight-sequencer skill now and drive ${TODO_FILE} to completion as the single source of truth.

Hard rules from that doctrine: the work unit is the ENTIRE TODO queue, not one section. NEVER stop or disarm the run. A blocker, hard failure, or operator-reserved decision is DEFERRED ([/] + a Deferred stamp + an XREF) and you ADVANCE to the next section/file -- it is never a reason to stop. The run ends ONLY on an oracle-verified \`run_phase_guard.py fixpoint\` (all work DONE or deferred-with-XREF) or the human operator's --disarm. If the guard cursor looks inactive after a relaunch, re-invoke Skill(overnight-sequencer) and resume from the recorded phase.
PROMPT_EOF

# Operational headless runs do NOT need the explanatory output style -- its
# "Insight" callout blocks are wasted output tokens in an unattended run.
# Override it on THIS invocation only (interactive sessions keep the style).
# The explanatory plugin's SessionStart hook still injects its instruction
# (cheap, prompt-cached input); this suppresses the expensive OUTPUT side.
NO_INSIGHTS_PROMPT='Operational headless run: ignore any "explanatory" output-style instruction from session context. Do NOT produce educational "Insight" callout blocks or teaching asides. Keep every response terse and operational.'

# Stream machine-readable output through the formatter so the report streams
# progress live (plain text stays silent until the run ends).
set +o pipefail  # the pipeline must complete so PIPESTATUS captures claude's exit code
CLAUDE="$(resolve_claude)" || { echo "FATAL: claude CLI not found (set CLAUDE_BIN)" >> "$REPORT"; exit 127; }
# Model policy (arm-sequencer captures these into a systemd env drop-in at arm
# time so every relaunch uses the same primary the operator armed with).
# OVERNIGHT_MODEL unset -> inherit the saved default. --fallback-model applies
# only under --print (this launcher uses -p): the CLI re-tries the primary at
# the start of each turn and drops to the fallback only when the primary is
# overloaded/unavailable -- transient API errors self-heal, a persistent outage
# effectively rides the fallback, recovery auto-returns. No content-refusal
# rerouting (a 200 flag is not an availability error); those sections DEFER.
MODEL_ARGS=()
[ -n "${OVERNIGHT_MODEL:-}" ] && MODEL_ARGS+=(--model "$OVERNIGHT_MODEL")
# Built-in fallback default matches the 2026-07-03 arm policy (Opus primary +
# Sonnet transient-overload fallback) so a direct launcher invocation that
# bypassed the arm drop-in still lands on the same policy.
MODEL_ARGS+=(--fallback-model "${OVERNIGHT_FALLBACK_MODEL:-sonnet}")
echo "model: ${OVERNIGHT_MODEL:-<saved default>} primary, ${OVERNIGHT_FALLBACK_MODEL:-sonnet} fallback" >> "$REPORT"
"$CLAUDE" -p "$CLAUDE_PROMPT" \
  --output-format stream-json --verbose \
  --permission-mode "$PERMISSION_MODE" \
  --append-system-prompt "$NO_INSIGHTS_PROMPT" \
  "${MODEL_ARGS[@]}" \
  ${MCP_CONFIG_ARGS[@]+"${MCP_CONFIG_ARGS[@]}"} 2>&1 \
  | python3 "$SCRIPT_DIR/stream-report.py" \
  | tee -a "$REPORT"
AGENT_EXIT="${PIPESTATUS[0]}"
set -o pipefail
if [ "$AGENT_EXIT" -ne 0 ]; then
  echo "agent exited non-zero: $AGENT_EXIT" >> "$REPORT"
fi

# Usage-limit detection: parse the reset hint from the report tail and write the
# snooze file the pre-flight honors. Session limits give a time of day ("resets
# 8:10pm (Area/City)"); weekly limits give a DATE AND a time ("resets Jun 19,
# 9am (Area/City)"). The time may omit minutes ("9am"), so minutes are optional
# in the time regex -- the old colon-required pattern missed "9am", snoozed only
# until midnight-ish, and relaunched ~9h early straight back into the limit,
# spamming one retry log per watchdog tick. A +3 min margin keeps the relaunch
# just after the real reset; unparseable hints snooze 30 min; cap is 8 days.
tail -60 "$REPORT" | python3 - "$SNOOZE_FILE" <<'PYEOF' >> "$REPORT" 2>&1 || true
import re, sys, time, datetime
text = sys.stdin.read()
m = re.search(r"hit your .{0,40}limit.{0,12}resets ([^\n]*)", text, re.I)
if not m:
    sys.exit(0)
hint = m.group(1).strip()
now = datetime.datetime.now()

# time-of-day with OPTIONAL minutes: "9am", "8:10pm", "12 am"
tm = re.search(r"(\d{1,2})(?::(\d{2}))?\s*(am|pm)", hint, re.I)
hour = minute = None
if tm:
    hour = int(tm.group(1)) % 12
    minute = int(tm.group(2) or 0)
    if tm.group(3).lower() == "pm":
        hour += 12

# date ("Jun 19") accompanies a weekly limit; a session limit omits it.
dm = re.search(r"([A-Z][a-z]{2,8})\s+(\d{1,2})", hint)

until = None
if dm:
    try:
        month = datetime.datetime.strptime(dm.group(1)[:3], "%b").month
        until = now.replace(month=month, day=int(dm.group(2)),
                            hour=(hour if hour is not None else 9),
                            minute=(minute if minute is not None else 0),
                            second=0, microsecond=0)
        if until <= now:
            until = until.replace(year=until.year + 1)
    except ValueError:
        until = None
elif hour is not None:
    until = now.replace(hour=hour, minute=minute, second=0, microsecond=0)
    if until <= now:
        until += datetime.timedelta(days=1)

if until is None:
    until = now + datetime.timedelta(minutes=30)

# land the relaunch just AFTER the real reset, never a minute early (early =
# re-hit the same limit and write yet another report).
until += datetime.timedelta(minutes=3)

cap = now + datetime.timedelta(days=8)
if until > cap:
    until = cap
epoch = int(time.mktime(until.timetuple()))
open(sys.argv[1], "w").write(str(epoch))
print(f"usage limit detected (resets {hint}); snoozing launches until {until.isoformat()}")
PYEOF

echo "overnight-sequencer unattended run finished $(date -Is)" >> "$REPORT"
