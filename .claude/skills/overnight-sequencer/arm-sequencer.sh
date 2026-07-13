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
# Canary stamp: the git HEAD that last passed a GREEN ATTENDED canary run
# (operator watched >=1 section ship + >=1 rollover). An unattended arm is
# refused when the control plane changed since this stamp -- see the canary
# gate below and `--record-canary`.
CANARY_STAMP=".claude/state/sequencer-canary-ok"

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
    {
      echo "[Service]"
      echo "# Overnight model policy captured at arm time (arm-sequencer.sh)."
      [ -n "$ARM_PRIMARY" ] && echo "Environment=OVERNIGHT_MODEL=$ARM_PRIMARY"
      echo "Environment=OVERNIGHT_FALLBACK_MODEL=$ARM_FALLBACK"
      [ -n "$ARM_EFFORT" ] && echo "Environment=OVERNIGHT_EFFORT=$ARM_EFFORT"
    } > "$d/sequencer-model.conf"
  done
}

remove_sequencer_model_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    rm -f "$DROPIN_BASE/$svc.d/sequencer-model.conf"
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
  reap_overnight_state
  systemctl --user daemon-reload 2>/dev/null || true
  echo "disarmed overnight sequencer (timers + watchdog + chromemcp drop-in + run state)"
  exit 0
fi

# --record-canary: stamp the current HEAD as canary-passed. Run this from an
# INTERACTIVE session AFTER watching a clean attended run (>=1 section ship +
# >=1 rollover -> relaunch). It records HEAD so subsequent unattended arms with
# no further control-plane change proceed without re-prompting; any later
# control-plane edit re-arms the gate. Optional note as the next argument.
if [ "${1:-}" = "--record-canary" ]; then
  mkdir -p .claude/state
  head_sha="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  ts="$(date -Is 2>/dev/null || echo unknown)"
  note="${2:-operator-attended canary}"
  printf '%s %s %s\n' "$head_sha" "$ts" "$note" > "$CANARY_STAMP"
  echo "recorded canary stamp: ${head_sha:0:12} @ $ts ($note)"
  echo "  unattended arms now proceed until the control plane changes again."
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
ARM_EFFORT=""              # empty = inherit the CLI's saved default (High on this
                           # host). A/B knob: --effort medium pins the run's
                           # reasoning effort; metrics records carry the value so
                           # medium-vs-high legs are comparable. Default stays
                           # inherit-High until quality holds on medium.
ARM_FORCE=0                # --force: arm despite an unproven control-plane change
ARM_SKIP_PREFLIGHT=0       # --skip-preflight: skip the pre-arm health gate
FORWARD_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --with-browser|--gh-pages)
      WITH_BROWSER=1
      shift
      ;;
    --force)
      ARM_FORCE=1
      shift
      ;;
    --skip-preflight)
      ARM_SKIP_PREFLIGHT=1
      shift
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

# ---- Pre-arm health gate (guardrail Layer 3) --------------------------------
# Refuse to arm into a broken control plane: runner-doctor + a launcher DRYRUN
# + the runner test suite must all pass. A red host or a broken launcher used
# to surface only AFTER the watchdog fired an unattended session at night --
# catch it here, attended.
if [ "$ARM_SKIP_PREFLIGHT" != "1" ]; then
  if ! bash "$REPO_ROOT/scripts/overnight/pre-arm-check.sh" "$REPO_ROOT" "$DOCTRINE"; then
    echo "REFUSED to arm: pre-arm health check failed (fix the above, or re-run with --skip-preflight to override)." >&2
    exit 1
  fi
else
  echo "WARN: --skip-preflight -- pre-arm health gate bypassed." >&2
fi

# ---- Canary gate (guardrail Layer 4) ----------------------------------------
# An UNATTENDED arm on an UNPROVEN control-plane change is what broke the runner
# repeatedly. Require a green ATTENDED canary (recorded via --record-canary)
# whenever a FLOW-CRITICAL control-plane path changed since the last stamp.
#
# Canary tiering (P0.0, 2026-07-12): only FLOW-CRITICAL changes re-arm this gate
# (--flow-critical subtracts the deterministic allowlist,
# control-plane-deterministic.txt). A change limited to deterministically-
# covered files (metrics/receipts/reporting -- all test-backed) is proven by the
# pre-arm-check suite run, not a watched canary, so it does NOT trip this gate.
# The FULL-manifest suite-run gate (pre-commit/pre-push/lint) is unchanged.
canary_unproven=""
# The unattended run executes the live WORKING TREE, so uncommitted flow-critical
# control-plane changes can never have been proven by any stamp -- flag them first.
cp_dirty="$( { git diff --name-only 2>/dev/null; git diff --cached --name-only 2>/dev/null; } | sort -u | bash "$REPO_ROOT/scripts/overnight/control-plane-match.sh" --flow-critical || true)"
if [ -n "$cp_dirty" ]; then
  canary_unproven="uncommitted control-plane changes in the working tree (commit + canary them first):
$(printf '%s\n' "$cp_dirty" | sed 's/^/    /')"
elif [ ! -f "$CANARY_STAMP" ]; then
  canary_unproven="no canary has ever been recorded"
else
  stamp_sha="$(awk 'NR==1{print $1}' "$CANARY_STAMP" 2>/dev/null)"
  if [ -z "$stamp_sha" ] || ! git rev-parse --quiet --verify "$stamp_sha^{commit}" >/dev/null 2>&1; then
    canary_unproven="canary stamp references an unknown commit (${stamp_sha:-empty})"
  else
    cp_changed="$(git diff --name-only "$stamp_sha" HEAD 2>/dev/null | bash "$REPO_ROOT/scripts/overnight/control-plane-match.sh" --flow-critical || true)"
    [ -n "$cp_changed" ] && canary_unproven="flow-critical control plane changed since the last canary (${stamp_sha:0:12}):
$(printf '%s\n' "$cp_changed" | sed 's/^/    /')"
  fi
fi
if [ -n "$canary_unproven" ]; then
  if [ "$ARM_FORCE" = "1" ]; then
    echo "WARN: arming an UNPROVEN control plane (--force accepted the risk): $canary_unproven" >&2
  else
    {
      echo "REFUSED to arm unattended: $canary_unproven"
      echo "  The control plane drives the whole unattended run. Prove it first:"
      echo "    1. Run an ATTENDED session; watch >=1 section ship + a rollover->relaunch."
      echo "    2. bash .claude/skills/overnight-sequencer/arm-sequencer.sh --record-canary"
      echo "    3. Re-arm."
      echo "  Or override now, accepting the risk: re-run with --force."
    } >&2
    exit 1
  fi
fi

# Arm. Marker first, so the very first tool call of the headless run is already
# redirected onto overnight-sequencer. Drop-ins are written AFTER the transient
# unit exists (systemd-run created it) and before it fires (--at is +2min), then
# daemon-reload makes them effective for the scheduled start.
mkdir -p .claude/state
: > "$MARKER"
bash "$LOCAL_ARM" "$DOCTRINE" --mode bypassPermissions --watchdog "*:0/10" ${FORWARD_ARGS[@]+"${FORWARD_ARGS[@]}"}
write_sequencer_env_dropin
write_sequencer_model_dropin
if [ "$WITH_BROWSER" = "1" ]; then
  remove_chromemcp_dropin
  : > .claude/state/overnight-with-browser   # I5: file-based positive signal the launcher reads
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP ON (gh-pages run)"
else
  write_chromemcp_dropin
  rm -f .claude/state/overnight-with-browser  # I5: no positive signal -> launcher skips the lane (fail-safe OFF)
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP OFF (kernel run)"
fi
echo "  model: ${ARM_PRIMARY:-<saved default>} primary, ${ARM_FALLBACK} fallback (claude --fallback-model; overload/unavailable only, re-tries primary each turn)"
echo "  launch redirects to Skill(overnight-sequencer); doctrine: $DOCTRINE"
echo "  monitor (from any dir): bash $REPO_ROOT/scripts/overnight/overnight-monitor.sh"
echo "  reports: $REPO_ROOT/.claude/overnight/reports/latest.log (created at first launch)"
echo "  scheduler: repo-vendored (scripts/overnight/), no external plugin dependency"
echo "  disarm: bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"
