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

# Cap the V8 heap of every node process in the run (claude CLI included): an
# unattended session that balloons must abort at 4GB instead of dragging the
# whole 14GB VM into swap-death (2026-07-20 crash: three node processes at
# 3-5GB each took the VM down mid-rollover).
export NODE_OPTIONS="--max-old-space-size=4096${NODE_OPTIONS:+ $NODE_OPTIONS}"

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

# NOTE: there is no structural-wait gate. Reviews are polled IN-SESSION in a
# single blocking Bash call (a sleeping shell costs ~0 model tokens), so a
# session never exits to wait on a review. The only relaunch mechanism is the
# *:0/10 watchdog (crash / usage-limit / verified rollover). The custom
# wait/wake/watcher apparatus was removed 2026-07-11 after it deadlocked the
# runner -- the flock below is the sole concurrency guard.

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

# Runner-doctor (2026-07-11): deterministic pre-launch health check + stale
# context pruning. A dead Codex login, corrupt recorder state, or a missing
# compiler fails HERE for free instead of after Opus loaded its context.
# Hard failure -> notify + 30-min backoff, stay armed (transient host issues
# heal; persistent ones surface to the operator via notify + questions file).
if [ "${OVERNIGHT_SEQUENCER_DRYRUN:-}" != "1" ] && [ -f "$SCRIPT_DIR/runner-doctor.py" ]; then
  if ! DOCTOR_JSON="$(cd "$PROJECT_DIR" && python3 "$SCRIPT_DIR/runner-doctor.py" . 2>&1)"; then
    echo "runner-doctor HARD failure; launch skipped $(date -Is)"
    echo "$DOCTOR_JSON" | tail -20
    echo "$(( $(date +%s) + 1800 )) 1" > "$BACKOFF_FILE"
    bash "$SCRIPT_DIR/notify.sh" "$PROJECT_DIR" critical \
      "runner-doctor failed pre-launch (see report); watchdog backing off 30 min, still armed." 2>/dev/null || true
    exit 0
  fi
fi

# WS3: per-run metrics sidecar (read by stream-report.py). Shares the report
# log's basename so a report and its metrics pair up. Defined before the prune
# so sidecars rotate with their logs.
METRICS_DIR="$RUNTIME_BASE/metrics"
mkdir -p "$METRICS_DIR"

# Report-log rotation: the watchdog relaunches every 10 min, so over many nights
# (and especially a usage-limit retry that mis-snoozed) the reports dir grows
# without bound. Keep only the most recent $OVERNIGHT_REPORT_KEEP run logs; prune
# the rest right before we add this launch's log. Metrics sidecars pair 1:1
# with run logs (same basename) and rotate to the same depth -- they were
# unrotated until 2026-07-04. The process-substitution read keeps the inner
# pipeline's exit out of set -e, and the wrapper is belt-and-suspenders so a
# prune failure never aborts an otherwise-healthy launch.
prune_reports() {
  local keep="${OVERNIGHT_REPORT_KEEP:-20}" old
  while IFS= read -r old; do
    [ -n "$old" ] && rm -f "$old"
  done < <(ls -1t "$REPORT_DIR"/run-*.log 2>/dev/null | tail -n +"$((keep + 1))")
  while IFS= read -r old; do
    [ -n "$old" ] && rm -f "$old"
  done < <(ls -1t "$METRICS_DIR"/run-*.jsonl 2>/dev/null | tail -n +"$((keep + 1))")
}
# DRYRUN isolation (2026-07-30, found live). A DRYRUN reached this point with
# the REAL report dir: it wrote a 377-byte stub, REPOINTED latest.log at it
# (the monitor's follow target), and -- the expensive half -- ran prune_reports
# against the real directory, so six test-suite runs in one evening EVICTED six
# real canary run logs at keep=20. The DRYRUN's purpose ends at validating
# selection + lock logic; give it a throwaway runtime so it can never touch a
# real report, the real symlink, or the real retention window.
if [ "${OVERNIGHT_SEQUENCER_DRYRUN:-}" = "1" ]; then
  REPORT_DIR="$(mktemp -d)"
  METRICS_DIR="$REPORT_DIR/metrics"
  mkdir -p "$METRICS_DIR"
fi

prune_reports || true

REPORT="$REPORT_DIR/run-$(date +%Y%m%d-%H%M%S).log"
ln -sfn "$(basename "$REPORT")" "$REPORT_DIR/latest.log"
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

# R1 (2026-07-19): a (re)launch IS a fresh worker context -- stamp the
# rotation boundary so a crash/usage-limit relaunch does not force the fresh
# worker through a redundant rollover before its first section. No-op when
# the run has not started (`start` sets its own epoch). Best-effort.
python3 "$PROJECT_DIR/.claude/hooks/run_phase_guard.py" mark-rotation 2>/dev/null || true

# ChromeMCP lane isolation (I5): FAIL-SAFE OFF. The lane is acquired ONLY on an
# explicit positive browser signal (OVERNIGHT_WITH_BROWSER=1 or the sentinel file
# .claude/state/overnight-with-browser, written by arm-sequencer.sh
# --with-browser) -- delegated to browser-lane-enabled.sh so it is unit-testable.
# A kernel run gets NO lane even if the old OVERNIGHT_NO_CHROMEMCP env drop-in
# failed to propagate through the systemd/watchdog chain (the I5 bug: all 8
# kernel runs claimed an unused lane + emitted CDP noise). OVERNIGHT_NO_CHROMEMCP=1
# stays a hard override that always skips.
_HERE_LAUNCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MCP_CONFIG_ARGS=()
if bash "$_HERE_LAUNCH/browser-lane-enabled.sh"; then
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

# Byte-stable worker prompts (2026-07-11): every fresh worker session should
# present an IDENTICAL prompt prefix so the provider prompt cache re-hits
# across section workers and watchdog relaunches.
#   - --exclude-dynamic-system-prompt-sections moves per-machine sections
#     (cwd, env, git status) out of the system prompt into the first user
#     message; dynamic cursor/status data already arrives via the SessionStart
#     brief (user-side, after the cached prefix).
#   - The explanatory-output-style plugin is disabled per-invocation via
#     --settings (interactive sessions keep it). Probe 2026-07-11: headless -p
#     sessions never received its SessionStart instruction anyway, so the old
#     contradictory NO_INSIGHTS suppression prompt was dead weight -- removed;
#     the settings disable is belt-and-suspenders.
#   - Tool-schema trim: the unattended run has a TESTED default denylist
#     (OVERNIGHT_DISALLOWED_DEFAULT below) of tools it provably never uses.
#     AskUserQuestion is the safe floor -- run_phase_guard.py HARD-BLOCKS it
#     in every phase (selftest-covered), so it can never be called
#     successfully; dropping its schema is pure win. Operators EXTEND (not
#     replace) via OVERNIGHT_DISALLOWED_TOOLS. Load-bearing HOOKS are always
#     active (never --bare / safe mode); this trims tool SCHEMAS only.
STABLE_PROMPT_ARGS=(--exclude-dynamic-system-prompt-sections
  --settings '{"enabledPlugins":{"explanatory-output-style@claude-plugins-official":false}}')
OVERNIGHT_DISALLOWED_DEFAULT="AskUserQuestion"
DISALLOWED_TOOLS="$OVERNIGHT_DISALLOWED_DEFAULT"
[ -n "${OVERNIGHT_DISALLOWED_TOOLS:-}" ] && \
  DISALLOWED_TOOLS="$DISALLOWED_TOOLS ${OVERNIGHT_DISALLOWED_TOOLS}"
DISALLOWED_ARGS=(--disallowedTools "$DISALLOWED_TOOLS")

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
# Orchestrator model: PINNED opus by default (runner-kit v0.10.0 parity --
# the run must not depend on whatever the interactive CLI default happens to
# be, even on a direct launcher invocation that bypassed the arm drop-in).
# OVERNIGHT_MODEL overrides; the literal value `inherit` restores no-pin.
# Fallback default matches the arm policy (Sonnet, transient-overload only).
MODEL_ARGS=()
OVERNIGHT_MODEL_EFFECTIVE="${OVERNIGHT_MODEL:-opus}"
[ "$OVERNIGHT_MODEL_EFFECTIVE" != "inherit" ] && MODEL_ARGS+=(--model "$OVERNIGHT_MODEL_EFFECTIVE")
MODEL_ARGS+=(--fallback-model "${OVERNIGHT_FALLBACK_MODEL:-sonnet}")
# Effort pin (A/B knob, 2026-07-11): unset/`inherit` keeps the CLI's saved
# default (this host inherits High). Set OVERNIGHT_EFFORT=medium via
# `arm-sequencer.sh --effort medium` to run an A/B leg; stream-report tags
# every metrics record with the effective effort so runs are comparable.
if [ -n "${OVERNIGHT_EFFORT:-}" ] && [ "$OVERNIGHT_EFFORT" != "inherit" ]; then
  MODEL_ARGS+=(--effort "$OVERNIGHT_EFFORT")
fi
echo "model: ${OVERNIGHT_MODEL_EFFECTIVE} primary, ${OVERNIGHT_FALLBACK_MODEL:-sonnet} fallback, effort ${OVERNIGHT_EFFORT:-inherit}" >> "$REPORT"

# Snapshot for the post-run circuit breaker: a run that ends with HEAD
# unmoved, a nonzero exit, or a sub-15-min zero-commit session counts as
# unproductive (run-outcome.py classifies; Codex-runner lesson 2026-07-04).
RUN_START_EPOCH="$(date +%s)"
START_HEAD="$(git -C "$PROJECT_DIR" rev-parse HEAD 2>/dev/null || echo "")"
"$CLAUDE" -p "$CLAUDE_PROMPT" \
  --output-format stream-json --verbose \
  --permission-mode "$PERMISSION_MODE" \
  "${STABLE_PROMPT_ARGS[@]}" \
  "${MODEL_ARGS[@]}" \
  ${DISALLOWED_ARGS[@]+"${DISALLOWED_ARGS[@]}"} \
  ${MCP_CONFIG_ARGS[@]+"${MCP_CONFIG_ARGS[@]}"} 2>&1 \
  | python3 "$SCRIPT_DIR/stream-report.py" \
  | tee -a "$REPORT"
AGENT_EXIT="${PIPESTATUS[0]}"
set -o pipefail
if [ "$AGENT_EXIT" -ne 0 ]; then
  echo "agent exited non-zero: $AGENT_EXIT" >> "$REPORT"
fi

# Usage-limit detection: parse the reset hint from the report tail and write the
# snooze file the pre-flight honors. Logic + parsing rationale live in the
# standalone, test-backed parse-usage-limit.py, which reads the report FILE by
# path (NOT stdin) -- the old inline `tail -60 | python3 - <<'PYEOF'` had the
# heredoc clobber stdin, so `sys.stdin.read()` hit EOF, the banner never parsed,
# and the runner never snoozed on a real limit (2026-07-12 P1.4 fix).
python3 "$SCRIPT_DIR/parse-usage-limit.py" "$REPORT" "$SNOOZE_FILE" >> "$REPORT" 2>&1 || true

# Unproductive-run circuit breaker (Codex-runner lesson, 2026-07-04): classify
# this run; after 3 consecutive dead runs, run-outcome.py writes the same
# watchdog-backoff-until file the pre-flight honors (30 min escalating to a
# 60 min cap) so a persistent environment failure cannot burn tokens at full
# watchdog cadence all night. The run stays ARMED (doctrine: only a verified
# fixpoint or the operator disarms); the trip is surfaced to the operator.
# A usage-limit death is excluded -- the snooze file above already governs it.
SNOOZED_ARG=()
if [ -f "$SNOOZE_FILE" ]; then SNOOZED_ARG=(--snoozed); fi
# T3-4: per-section cost report, appended to the run report with ZERO model
# cost. section-cost-report.py was fully built (per-section turns, output and
# cache-read tokens, sidechain share, agents dispatched, plus cost normalized by
# changed LOC and per shipped commit, with advisory soft-SLO flags) and had NO
# CALLER anywhere -- an audit for "bookkeeping still done in model turns" found
# the opposite problem: a finished deterministic report nothing was running.
# Best-effort and never fatal: a reporting failure must not affect the run's
# outcome classification below.
if [ -f "${OVERNIGHT_METRICS_FILE:-}" ]; then
  {
    echo ""
    echo "=== cost summary ==="
    timeout 120 python3 "$SCRIPT_DIR/cost-summary.py" \
      "$OVERNIGHT_METRICS_FILE" --project "$PROJECT_DIR" 2>&1 || \
      echo "(cost summary unavailable)"
    echo ""
    echo "=== per-section cost report ==="
    timeout 120 python3 "$SCRIPT_DIR/section-cost-report.py" \
      "$OVERNIGHT_METRICS_FILE" --project "$PROJECT_DIR" 2>&1 || \
      echo "(section-cost-report unavailable)"
  } >> "$REPORT" 2>&1 || true
fi

OUTCOME_JSON="$(python3 "$SCRIPT_DIR/run-outcome.py" "$PROJECT_DIR" \
  --exit "$AGENT_EXIT" --start-head "${START_HEAD:-}" \
  --run-secs "$(( $(date +%s) - RUN_START_EPOCH ))" \
  ${SNOOZED_ARG[@]+"${SNOOZED_ARG[@]}"} 2>>"$REPORT" || true)"
echo "run outcome: ${OUTCOME_JSON:-unavailable}" >> "$REPORT"
if printf '%s' "$OUTCOME_JSON" | grep -q '"breaker": true'; then
  python3 "$SCRIPT_DIR/collect-questions.py" "$PROJECT_DIR" --stamp "$(date -Is)" >/dev/null 2>&1 || true
  bash "$SCRIPT_DIR/notify.sh" "$PROJECT_DIR" critical \
    "Circuit breaker tripped: consecutive unproductive runs; watchdog backing off (still armed). Check $REPORT" 2>/dev/null || true
fi

echo "overnight-sequencer unattended run finished $(date -Is)" >> "$REPORT"
