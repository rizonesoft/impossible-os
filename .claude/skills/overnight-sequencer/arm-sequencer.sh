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
  remove_sequencer_env_dropin
  remove_sequencer_model_dropin
  reap_overnight_state
  systemctl --user daemon-reload 2>/dev/null || true
  echo "disarmed overnight sequencer (timers + watchdog + chromemcp drop-in + run state)"
  exit 0
fi

# ChromeMCP policy for THIS repo: impossible-os is kernel/OS work for the vast
# majority of runs, which never touch a browser -- so default ChromeMCP OFF via
# the OVERNIGHT_NO_CHROMEMCP env drop-in, which the vendored launcher reads to
# skip the lane logic entirely (no lane churn, no spurious CDP errors). The ONLY
# impossible-os work that needs ChromeMCP is the gh-pages landing site; arm those
# runs with --with-browser to leave the env unset and let the lane logic run.
WITH_BROWSER=0
ARM_PRIMARY="opus"         # runner default: Opus primary (see policy block above)
ARM_FALLBACK="sonnet"      # transient overload fallback; primary re-tried each turn
ARM_EFFORT=""              # empty = inherit the CLI's saved default (High on this
                           # host). A/B knob: --effort medium pins the run's
                           # reasoning effort; metrics records carry the value so
                           # medium-vs-high legs are comparable. Default stays
                           # inherit-High until quality holds on medium.
FORWARD_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --with-browser|--gh-pages)
      WITH_BROWSER=1
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
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP ON (gh-pages run)"
else
  write_chromemcp_dropin
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP OFF (kernel run)"
fi
echo "  model: ${ARM_PRIMARY:-<saved default>} primary, ${ARM_FALLBACK} fallback (claude --fallback-model; overload/unavailable only, re-tries primary each turn)"
echo "  launch redirects to Skill(overnight-sequencer); doctrine: $DOCTRINE"
echo "  monitor (from any dir): bash $REPO_ROOT/scripts/overnight/overnight-monitor.sh"
echo "  reports: $REPO_ROOT/.claude/overnight/reports/latest.log (created at first launch)"
echo "  scheduler: repo-vendored (scripts/overnight/), no external plugin dependency"
echo "  disarm: bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"
