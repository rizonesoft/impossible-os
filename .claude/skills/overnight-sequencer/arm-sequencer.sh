#!/usr/bin/env bash
# arm-sequencer.sh -- arm the unattended overnight sequencer for impossible-os.
#
# Drives the REPO-VENDORED scheduler (scripts/overnight/overnight-arm.sh +
# overnight-launch.sh -- no external plugin dependency since 2026-06-16) and:
#   1. redirects the headless launch onto the repo-owned overnight-sequencer
#      skill via the .claude/state/sequencer-armed marker (run_phase_guard.py
#      hard-blocks anything else until overnight-sequencer is invoked);
#   2. forces --mode bypassPermissions + a 10-min pure-failover watchdog;
#   3. turns ChromeMCP OFF at the systemd-unit level for THIS repo only (a
#      per-unit env drop-in; the vendored launcher honors OVERNIGHT_NO_CHROMEMCP
#      and skips the lane logic entirely, so kernel runs make no browser noise).
#
# Usage:
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh [--at "<calendar>"]
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh --with-browser   # gh-pages
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh --model <m> --fallback-model <m>
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
DOCTRINE="todo/TODO-Claude-Overnight-Runner.md"
UNIT="overnight-$(basename "$REPO_ROOT")"        # overnight-impossible-os
DROPIN_BASE="$HOME/.config/systemd/user"
MARKER=".claude/state/sequencer-armed"

# ARM FROM THE PRIMARY WORKTREE ONLY (2026-07-31). REPO_ROOT is resolved from
# BASH_SOURCE, so invoking this script from the repair worktree would arm a run
# whose PROJECT_DIR, marker, canary stamp and drop-ins all point at the WRONG
# checkout -- and nothing downstream would say so, because every one of those
# paths would exist and look plausible. The run belongs in the primary
# worktree on main; the repair worktree exists only so an operator can fix the
# control plane without sharing the run's tree, index and build dir.
_PRIMARY_ROOT="$(dirname "$(git -C "$REPO_ROOT" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")"
if [ -n "$_PRIMARY_ROOT" ] && [ "$_PRIMARY_ROOT" != "/" ] && [ "$(cd "$REPO_ROOT" && pwd -P)" != "$(cd "$_PRIMARY_ROOT" && pwd -P)" ]; then
  # Not a refusal: there is exactly one right place to arm from, so go there.
  echo "arm-sequencer: running from the primary checkout $_PRIMARY_ROOT" >&2
  exec bash "$_PRIMARY_ROOT/.claude/skills/overnight-sequencer/arm-sequencer.sh" "$@"
fi

LOCAL_ARM="$REPO_ROOT/scripts/overnight/overnight-arm.sh"
[ -x "$LOCAL_ARM" ] || { echo "FATAL: vendored scheduler missing/not executable: $LOCAL_ARM" >&2; exit 127; }

write_chromemcp_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    d="$DROPIN_BASE/$svc.d"
    mkdir -p "$d"
    cat > "$d/no-chromemcp.conf" <<'EOF'
[Service]
# impossible-os has its own smoke/test infra (scripts/test-smoke.sh, test.sh,
# build.sh); it never drives ChromeMCP. Per-unit env: other projects keep it.
Environment=MCP_NO_AUTO_CHROME=1
Environment=MCP_NO_AUTO_BRIDGE=1
Environment=OVERNIGHT_NO_CHROMEMCP=1
EOF
  done
}

remove_chromemcp_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    rm -f "$DROPIN_BASE/$svc.d/no-chromemcp.conf"
    rmdir "$DROPIN_BASE/$svc.d" 2>/dev/null || true
  done
}

# The load-bearing headless discriminator. run_phase_guard.py governs ONLY a run
# whose hook subprocesses see OVERNIGHT_SEQUENCER_RUN=1; an interactive operator
# session in this same repo never sets it, so the guard (no-stop, no-self-disarm,
# phase enforcement) engages absolutely on the unattended run while never
# trapping the human. Written in BOTH browser and kernel arming paths, removed on
# disarm. This is what makes the death-thrash impossible AND fixes the
# interactive-session trap collision.
write_sequencer_env_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    d="$DROPIN_BASE/$svc.d"
    mkdir -p "$d"
    cat > "$d/sequencer-run.conf" <<'EOF'
[Service]
# Headless-run discriminator consumed by .claude/hooks/run_phase_guard.py.
# Present ONLY in the unattended systemd-launched run; absent in interactive
# operator sessions -- this is how the guard binds to the headless run alone.
Environment=OVERNIGHT_SEQUENCER_RUN=1
EOF
  done
}

remove_sequencer_env_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    rm -f "$DROPIN_BASE/$svc.d/sequencer-run.conf"
    rmdir "$DROPIN_BASE/$svc.d" 2>/dev/null || true
  done
}

# Model policy for the headless launch, captured at ARM time so every relaunch
# over the night uses the SAME primary the operator armed with -- not whatever
# `/model` happens to be saved when a watchdog fires hours later. The launcher
# turns these into `claude -p --model <primary> --fallback-model <fallback>`;
# the CLI re-tries the primary at the start of every turn and only drops to the
# fallback on turns where the primary is overloaded/unavailable (so a transient
# API error self-heals, a persistent outage effectively stays on the fallback,
# and recovery auto-returns -- no forcing, no fixed timer). ARM_PRIMARY empty
# means "inherit the saved default model" (reachable via `--model inherit`).
#
# DEFAULT POLICY (2026-07-03): Opus primary + Sonnet fallback. Fable 5
# (Mythos-tier) is overkill as the runner's primary -- the Codex
# adversarial/consistency/perf pipeline is the quality net, Opus is the
# doctrine's judgment floor for implement/review/receiving work, and Opus
# sidesteps the safeguard-flag friction on kernel/security sections (flags
# route to Opus anyway). The main loop must NOT drop to Sonnet (Sonnet is the
# read-only agent tier); Sonnet appears here only as the transient-overload
# fallback, re-tried from the primary each turn. Force per-arm with
# `--model fable-5` / `--model sonnet` / `--model inherit`. The operator's
# INTERACTIVE /model default is untouched (this policy lives in the systemd
# drop-in only).
write_sequencer_model_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    d="$DROPIN_BASE/$svc.d"
    mkdir -p "$d"
    # `if` blocks, NOT `[ -n X ] && echo`. A trailing conditional is the LAST
    # command in this group, so a false test makes the group exit 1, and that
    # status propagates group -> loop body -> `for` -> function return, where
    # `set -euo pipefail` (line 19) kills the script -- AFTER the file has been
    # written, which is why three separate investigations blamed the NEXT
    # function and left this documented as "root cause still unknown".
    #
    # ARM_EFFORT is empty by DEFAULT (and `--effort inherit` empties it), so
    # this fired on every default arm: 2026-07-20, -24 and -28, silently
    # skipping write_claude_token_dropin and dropping the run back onto the
    # shared credentials file and the single-use-refresh-token race the
    # long-lived token exists to end. Not intermittent -- it never once didn't
    # fire. Regression-tested in scripts/overnight/tests/test_arm_dropins.py.
    {
      echo "[Service]"
      echo "# Overnight model policy captured at arm time (arm-sequencer.sh)."
      if [ -n "$ARM_PRIMARY" ]; then
        echo "Environment=OVERNIGHT_MODEL=$ARM_PRIMARY"
      fi
      echo "Environment=OVERNIGHT_FALLBACK_MODEL=$ARM_FALLBACK"
      if [ -n "$ARM_EFFORT" ]; then
        echo "Environment=OVERNIGHT_EFFORT=$ARM_EFFORT"
      fi
    } > "$d/sequencer-model.conf"
  done
}

remove_sequencer_model_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    rm -f "$DROPIN_BASE/$svc.d/sequencer-model.conf"
    rmdir "$DROPIN_BASE/$svc.d" 2>/dev/null || true
  done
}

# Long-lived Claude OAuth token for the headless run (2026-07-20). All overnight
# runners on this machine share ONE global token file (KEY=VALUE format) so the
# unattended run stops racing interactive sessions on the single-use refresh
# token in ~/.claude/.credentials.json -- that race is what forced repeated
# /login re-auth. EnvironmentFile keeps the secret out of the drop-in and out
# of every repo; the leading '-' makes it optional, so arming before the token
# is minted still works (the run then falls back to the shared credentials
# file, racy but functional). Mint via conclave's token tooling or:
#   claude setup-token   ->  CLAUDE_CODE_OAUTH_TOKEN=<value> in the file below, chmod 600.
CLAUDE_TOKEN_ENV_FILE="$HOME/.conclave/secrets/claude-oauth-token.env"

write_claude_token_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    d="$DROPIN_BASE/$svc.d"
    mkdir -p "$d"
    cat > "$d/claude-oauth-token.conf" <<EOF
[Service]
# Machine-global long-lived Claude token (arm-sequencer.sh). Scopes the token
# to the overnight units only; interactive sessions keep /login + credentials
# file. Optional: absent file means fall back to shared credentials.
EnvironmentFile=-$CLAUDE_TOKEN_ENV_FILE
EOF
  done
  if [ ! -f "$CLAUDE_TOKEN_ENV_FILE" ]; then
    echo "WARN: no long-lived token file at $CLAUDE_TOKEN_ENV_FILE -- overnight run" >&2
    echo "      will share ~/.claude/.credentials.json with interactive sessions" >&2
    echo "      (refresh race can force /login). Mint one: claude setup-token" >&2
  elif ! grep -q '^CLAUDE_CODE_OAUTH_TOKEN=' "$CLAUDE_TOKEN_ENV_FILE"; then
    echo "WARN: $CLAUDE_TOKEN_ENV_FILE exists but has no CLAUDE_CODE_OAUTH_TOKEN= line" >&2
  fi
}

remove_claude_token_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    rm -f "$DROPIN_BASE/$svc.d/claude-oauth-token.conf"
    rmdir "$DROPIN_BASE/$svc.d" 2>/dev/null || true
  done
}

remove_runtime_file() {
  local p="$1"
  local label="$2"
  if [ -e "$p" ]; then
    rm -f "$p"
    echo "  removed $label"
  fi
}

# Idempotent teardown of run state. The launch.lock flock self-releases on
# holder death but the file lingers; the fixpoint sentinel and any legacy plugin
# overnight-runner.json (incl. a stale TERMINAL/user-decision blocker that would
# read to the NEXT run as "the operator reserved this, stop" -- the 2026-06-16
# self-disarm cause) are cleared so a re-arm always starts from a clean slate.
reap_overnight_state() {
  # Retire any skill-progress entry this run left LIVE. A finished or disarmed
  # run leaves `review-todo-section` / `implement-todo-section` marked in flight
  # (the compaction_orphaned flag only covers entries a PreCompact saw), and
  # section_commit_gate reads that as "a review is in flight" for ANY later
  # commit -- including an unrelated interactive one in a different session.
  # Observed 2026-07-28: a disarmed run left review-todo-section pointing at
  # TODO-21 section 19, and hours later a commit touching only boot_timing.c and
  # dpc.c was refused as that section's fix-loop commit. The gate cannot compare
  # sessions in --git-hook-mode (no stdin payload), so the run must clean up
  # after itself here.
  python3 - <<'PYSKILL' || true
import json, time
p = ".claude/state/skill-progress.json"
try:
    d = json.load(open(p))
except Exception:
    d = None
if isinstance(d, dict):
    retired = 0
    for k in ("review-todo-section", "implement-todo-section"):
        e = d.get(k)
        if isinstance(e, dict) and e.get("compaction_orphaned") is not True:
            # Preserve the record under an orphan key so the history is not
            # destroyed -- only its in-flight status is withdrawn.
            e["compaction_orphaned"] = True
            e["orphan_reason"] = "run disarmed with the skill still in flight"
            e["orphan_ts_ns"] = time.time_ns()
            d[f"{k}.orphan.{e['orphan_ts_ns']}"] = e
            del d[k]
            retired += 1
    if retired:
        json.dump(d, open(p, "w"), indent=2)
        print(f"  retired {retired} in-flight skill entr(ies) from skill-progress.json")
PYSKILL
  python3 - <<'PY' || true
import json
p = ".claude/overnight/state/overnight-runner.json"   # legacy plugin state, if present
try:
    d = json.load(open(p))
    changed = False
    if d.get("active") or d.get("status") not in (None, "cleared"):
        d["active"] = False; d["status"] = "cleared"; changed = True
    if d.get("blockers"):
        n = len(d["blockers"]); d["blockers"] = []; changed = True
        print(f"  cleared {n} stale blocker(s) from overnight-runner.json")
    if changed:
        json.dump(d, open(p, "w"), indent=2)
        print("  reset legacy overnight-runner.json -> active:false, blockers:[]")
except FileNotFoundError:
    pass
except (ValueError, OSError) as e:
    print(f"  WARN: could not reset overnight-runner.json: {e}")
PY
  remove_runtime_file .claude/state/run-deadline "run deadline"
  remove_runtime_file .claude/overnight/noship-streak "no-ship streak"
  remove_runtime_file .claude/overnight/launch.lock "launch.lock"
  remove_runtime_file .claude/state/sequencer-fixpoint "stale fixpoint sentinel"
}

if [ "${1:-}" = "--disarm" ]; then
  rm -f "$MARKER"
  python3 .claude/hooks/run_phase_guard.py clear "disarmed via arm-sequencer.sh" >/dev/null 2>&1 || true
  bash "$LOCAL_ARM" "$DOCTRINE" --disarm || true
  remove_chromemcp_dropin
  rm -f .claude/state/overnight-with-browser  # I5: clear the browser-lane positive signal
  remove_sequencer_env_dropin
  remove_sequencer_model_dropin
  remove_claude_token_dropin
  reap_overnight_state
  systemctl --user daemon-reload 2>/dev/null || true
  # The status page still read `Armed: True  Live: LIVE` five weeks after a stop:
  # nothing refreshed it after the last writer exited.
  python3 scripts/overnight/run-status.py . --stamp "$(date -Is)" >/dev/null 2>&1 || true
  echo "disarmed overnight sequencer (timers + watchdog + chromemcp drop-in + run state)"
  exit 0
fi

# --record-canary is retired with the canary gate (2026-11-07). Accepted and ignored
# so an old note or habit that still types it does nothing surprising.
if [ "${1:-}" = "--record-canary" ]; then
  echo "arm-sequencer: canary runs are retired; nothing to record."
  exit 0
fi

# ChromeMCP policy for THIS repo: impossible-os is kernel/OS work for the vast
# majority of runs, which never touch a browser -- so ChromeMCP is FAIL-SAFE OFF
# (I5): the launcher's browser-lane-enabled.sh acquires a lane ONLY on an explicit
# positive signal, so a kernel run skips the lane even if the env drop-in fails to
# propagate (the I5 bug: 8 kernel runs claimed an unused lane). The ONLY
# impossible-os work that needs ChromeMCP is the gh-pages landing site; arm those
# runs with --with-browser, which writes the .claude/state/overnight-with-browser
# sentinel the launcher reads. (The OVERNIGHT_NO_CHROMEMCP drop-in is retained as
# a redundant hard override.)
WITH_BROWSER=0
ARM_PRIMARY="opus"         # runner default: Opus primary (see policy block above)
ARM_FALLBACK="sonnet"      # transient overload fallback; primary re-tried each turn
ARM_EFFORT="medium"        # the writer runs at medium by operator decision (2026-11-06),
                           # pinned rather than left to the saved CLI default.
                           # --effort <level> overrides, --effort inherit restores
                           # the saved default; metrics records carry the value.
ARM_SKIP_PREFLIGHT=0       # --skip-preflight: skip the pre-arm health gate
# --hours N: bound the run. Written to .claude/state/run-deadline and enforced
# by deadline-check.sh at SPAWN time, so the run stops after its last segment's
# verified rollover -- a clean stop, not a kill. Empty means "run to fixpoint",
# the historical behaviour.
ARM_HOURS=""
FORWARD_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --with-browser|--gh-pages)
      WITH_BROWSER=1
      shift
      ;;
    --force)
      shift  # retired with the canary gate; accepted so old invocations still work
      ;;
    --skip-preflight)
      ARM_SKIP_PREFLIGHT=1
      shift
      ;;
    --hours)
      ARM_HOURS="${2:?--hours needs a number}"
      shift 2
      ;;
    --model)
      ARM_PRIMARY="${2:?--model needs a value}"
      [ "$ARM_PRIMARY" = "inherit" ] && ARM_PRIMARY=""  # saved-default passthrough
      shift 2
      ;;
    --fallback-model)
      ARM_FALLBACK="${2:?--fallback-model needs a value}"
      shift 2
      ;;
    --effort)
      ARM_EFFORT="${2:?--effort needs low|medium|high|xhigh|max|inherit}"
      [ "$ARM_EFFORT" = "inherit" ] && ARM_EFFORT=""
      shift 2
      ;;
    *)
      FORWARD_ARGS+=("$1")
      shift
      ;;
  esac
done

# Degenerate model policy guard: primary == fallback removes the degradation
# path entirely (an overload of the primary IS an overload of the fallback --
# the CLI would re-try the same saturated model and the run dies exactly as
# with no fallback at all). Warn loudly; the operator's choice still stands.
if [ -n "$ARM_PRIMARY" ] && [ "$ARM_PRIMARY" = "$ARM_FALLBACK" ]; then
  echo "WARN: --model and --fallback-model are BOTH '$ARM_PRIMARY' -- the fallback" >&2
  echo "      provides no availability diversity (a primary overload hits the" >&2
  echo "      fallback identically). Consider --fallback-model sonnet." >&2
fi

# ---- What makes running impossible ------------------------------------------
# These and the token and deny checks below are the only refusals left, because no
# amount of relaunching gets past them. Everything else starts.
# `claude` is not listed: the launcher's resolve_claude knows every install layout.
for _tool in git python3 clang-19 nasm ld.lld-19; do
  if ! command -v "$_tool" >/dev/null 2>&1; then
    echo "REFUSED to arm: $_tool is not installed. Run: bash scripts/setup.sh" >&2
    exit 1
  fi
done

# ---- A permission deny on a pipeline skill (v22, 2026-11-07) -----------------
# `Skill(review *)` refused review-todo-section for a whole run: no section could
# ship and every segment looked healthy. The rule is the operator's, so this names
# it and refuses; --force does not help, because no run can get past a deny.
if ! _denied="$(python3 "$REPO_ROOT/scripts/overnight/skill_denies.py" "$REPO_ROOT")"; then
  {
    echo "REFUSED to arm: a permission deny rule refuses a skill the pipeline needs."
    printf '  %s\n' "$_denied"
    echo "  Narrow the rule in settings.json, then confirm with one real call in a"
    echo "  FRESH session: a running session keeps the rules it started with."
  } >&2
  exit 1
fi

# Advisory: a park whose checkable blocker no longer holds.
python3 "$REPO_ROOT/scripts/overnight/stale_parks.py" --check 2>/dev/null || true

# ---- Fresh breaker + a token that authenticates (2026-09-28 canary) ---------
# An explicit arm starts clean: a finished run's backoff and no-ship streak made
# the next arm refuse (the DRYRUN below honours the backoff) and then stop at
# once. reset-breaker.sh skips itself while a run is live. The token probe makes
# one tiny authenticated call so a revoked token refuses HERE, attended, instead
# of dying at launch; --skip-preflight skips it with the rest of the gate.
bash "$REPO_ROOT/scripts/overnight/reset-breaker.sh" "$REPO_ROOT" "$UNIT" || true
if [ "$ARM_SKIP_PREFLIGHT" != "1" ]; then
  if ! bash "$REPO_ROOT/scripts/overnight/token-probe.sh" "$CLAUDE_TOKEN_ENV_FILE"; then
    echo "REFUSED to arm: the long-lived token does not authenticate (see above)." >&2
    exit 1
  fi
fi

# ---- Pre-arm health report ---------------------------------------------------
# Warns and starts. A red suite or selftest is something the run can repair, and the
# launcher's own runner-doctor still holds back a launch on a host that cannot work.
if [ "$ARM_SKIP_PREFLIGHT" != "1" ]; then
  if ! bash "$REPO_ROOT/scripts/overnight/pre-arm-check.sh" "$REPO_ROOT" "$DOCTRINE"; then
    echo "WARN: pre-arm health check found problems (above). Starting anyway; the run can repair these." >&2
  fi
fi

# Arm. Marker first, so the very first tool call of the headless run is already
# redirected onto overnight-sequencer. Drop-ins are written AFTER the transient
# unit exists (systemd-run created it) and before it fires (--at is +2min), then
# daemon-reload makes them effective for the scheduled start.
mkdir -p .claude/state
: > "$MARKER"
DEADLINE_FILE=".claude/state/run-deadline"
rm -f "$DEADLINE_FILE"
if [ -n "$ARM_HOURS" ]; then
  _deadline=$(( $(date +%s) + ARM_HOURS * 3600 ))
  echo "$_deadline" > "$DEADLINE_FILE"
  echo "run deadline: $(date -d "@$_deadline" -Is 2>/dev/null || echo "$_deadline") (${ARM_HOURS}h) -- stops after the last segment's verified rollover"
fi

# ---- Tail-completion guard ---------------------------------------------------
#
# Observed on the 2026-07-20 and 2026-07-24 arms: everything below
# write_sequencer_model_dropin silently did not run -- no token drop-in, no
# ChromeMCP drop-in, no daemon-reload, no summary. File mtimes proved the first
# two writers ran and the third did not, so execution stopped at or inside
# write_claude_token_dropin. It is NOT reproducible in isolation: that function
# traces to completion under `bash -x` with rc=0 against the real token file,
# all three writers are structurally identical, and the drop-in dir accepts
# writes from the same sandbox. Root cause still unknown.
#
# The load-bearing loss is `systemctl --user daemon-reload`. Drop-ins written
# seconds earlier are not guaranteed effective for a start scheduled ~2 min out,
# so a silent skip can launch the run WITHOUT the model pin, WITHOUT the
# OVERNIGHT_SEQUENCER_RUN=1 phase-guard discriminator, and WITHOUT the OAuth
# EnvironmentFile -- and nothing says so until 03:00.
#
# So the reload does not depend on reaching the end of the script any more: it
# moves into an EXIT trap, which fires however the script leaves. The stage
# marker turns an unexplained early exit from invisible into a named WARN, and
# the summary reports systemd's EFFECTIVE state rather than the armed INTENT,
# so a skip is visible AT ARM TIME instead of at 03:00.
ARM_TAIL_STAGE="pre-arm"
arm_tail_finalize() {
  local rc=$?
  # Unconditional: whatever drop-ins DID land must be made effective. Cheap and
  # idempotent, so running it on an error path costs nothing and running it on
  # the happy path is the normal case.
  systemctl --user daemon-reload 2>/dev/null || true

  if [ "$ARM_TAIL_STAGE" != "complete" ]; then
    {
      echo ""
      echo "WARN: arm tail did NOT complete (stopped after stage: $ARM_TAIL_STAGE, rc=$rc)."
      echo "      daemon-reload was run by the EXIT trap, so drop-ins that DID land"
      echo "      are effective -- but later ones may be missing. Verify below, and"
      echo "      re-run the arm if the effective state is wrong."
    } >&2
  fi

  # EFFECTIVE state, not intent. The three vars that must be present are
  # CLAUDE_PROJECT_DIR/OVERNIGHT_SEQUENCER_RUN (phase-guard discriminator) and
  # the model pin; the token arrives via EnvironmentFile, which shows separately.
  echo ""
  echo "  effective unit environment (systemctl show -- what the run will ACTUALLY see):"
  local eff
  eff="$(systemctl --user show "$UNIT.service" -p Environment -p EnvironmentFiles 2>/dev/null || true)"
  if [ -n "$eff" ]; then
    printf '%s\n' "$eff" | sed 's/^/    /'
  else
    echo "    (systemctl show unavailable)"
    return 0
  fi

  # ASSERT, do not merely print. Printing the effective environment is only
  # useful if a human reads it, and on 2026-07-28 the tail failure was caught
  # exactly that way -- by eye, minutes before the timer fired. The two
  # properties below are the ones whose absence silently degrades a whole run,
  # so they are checked rather than displayed:
  #
  #   token EnvironmentFile  absent -> the run shares ~/.claude/.credentials.json
  #                          with interactive sessions and re-enters the
  #                          single-use-refresh-token race that has killed runs.
  #   MCP_NO_AUTO_CHROME     absent -> a kernel run may acquire a browser lane
  #                          it never needs.
  #
  # WARN, not exit: the arm has already happened by the time this trap runs, so
  # failing here would leave armed timers with no message. A loud, specific
  # warning naming the repair is the useful output.
  local missing=""
  printf '%s\n' "$eff" | grep -q "$CLAUDE_TOKEN_ENV_FILE" \
    || missing="$missing\n    - token EnvironmentFile ($CLAUDE_TOKEN_ENV_FILE) -- the run will share the interactive credentials file (refresh race)"
  printf '%s\n' "$eff" | grep -q 'MCP_NO_AUTO_CHROME=1' \
    || missing="$missing\n    - MCP_NO_AUTO_CHROME=1 -- a kernel run may take a browser lane it does not need"
  if [ -n "$missing" ]; then
    {
      echo ""
      echo "WARN: the armed unit is MISSING drop-in state that should be effective:"
      printf "%b\n" "$missing"
      echo "  Repair before the timer fires: re-run this script, or hand-write the"
      echo "  drop-in under $DROPIN_BASE/$UNIT.service.d/ and \`systemctl --user daemon-reload\`."
    } >&2
  fi
}
trap arm_tail_finalize EXIT

bash "$LOCAL_ARM" "$DOCTRINE" --mode bypassPermissions --watchdog "*:0/10" ${FORWARD_ARGS[@]+"${FORWARD_ARGS[@]}"}
ARM_TAIL_STAGE="local-arm-done"
write_sequencer_env_dropin
ARM_TAIL_STAGE="env-dropin-written"
write_sequencer_model_dropin
ARM_TAIL_STAGE="model-dropin-written"
write_claude_token_dropin
ARM_TAIL_STAGE="token-dropin-written"
if [ "$WITH_BROWSER" = "1" ]; then
  remove_chromemcp_dropin
  : > .claude/state/overnight-with-browser   # I5: file-based positive signal the launcher reads
  # daemon-reload is owned by the EXIT trap -- one caller, so a future edit
  # cannot leave one branch reloading and the other not.
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP ON (gh-pages run)"
else
  write_chromemcp_dropin
  rm -f .claude/state/overnight-with-browser  # I5: no positive signal -> launcher skips the lane (fail-safe OFF)
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP OFF (kernel run)"
fi
echo "  model: ${ARM_PRIMARY:-<saved default>} primary, ${ARM_FALLBACK} fallback (claude --fallback-model; overload/unavailable only, re-tries primary each turn)"
echo "  launch redirects to Skill(overnight-sequencer); doctrine: $DOCTRINE"
echo "  monitor (from any dir): bash $REPO_ROOT/scripts/overnight/overnight-monitor.sh"
echo "  reports: $REPO_ROOT/.claude/overnight/reports/latest.log (created at first launch)"
echo "  scheduler: repo-vendored (scripts/overnight/), no external plugin dependency"
echo "  disarm: bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"
# Arming starts a countdown (v19 capture, 2026-09-03): an attended edit begun
# after the arm was swept into the run's first commit 3 minutes later. Say so.
_first_fire=$(systemctl --user show "$UNIT.timer" -p NextElapseUSecRealtime --value 2>/dev/null || true)
echo "  DEADLINE: the tree must be CLEAN by the first launch (${_first_fire:-the next timer tick}); do not begin an attended edit you cannot land before then -- disarm, land it, and re-arm instead."
# Reached only if every statement above ran. The EXIT trap reads this to decide
# between a clean summary and the did-not-complete WARN, and it is the last
# assignment on purpose: anything added below must move it further down.
ARM_TAIL_STAGE="complete"
