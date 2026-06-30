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
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh --driver codex
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh --with-browser   # gh-pages
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

write_codex_driver_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    d="$DROPIN_BASE/$svc.d"
    mkdir -p "$d"
    cat > "$d/codex-driver.conf" <<'EOF'
[Service]
# Explicit Codex active-driver mode. This is runtime state only; doctrine and
# skills remain repo-owned, and reviewer evidence must still be a separate run.
Environment=OVERNIGHT_DRIVER=codex
Environment=AI_WORKFLOW_CODEX_DRIVER=1
Environment=AI_WORKFLOW_ENFORCE_SHARED_GATES=1
Environment=OVERNIGHT_STREAM_KEEP_UNKNOWN=1
Environment=OVERNIGHT_CODEX_SANDBOX=danger-full-access
EOF
  done
}

remove_codex_driver_dropin() {
  for svc in "$UNIT.service" "$UNIT-watchdog.service"; do
    rm -f "$DROPIN_BASE/$svc.d/codex-driver.conf"
    rmdir "$DROPIN_BASE/$svc.d" 2>/dev/null || true
  done
}

clear_codex_supervisor_state() {
  for p in .codex/overnight/codex-supervisor.block .codex/overnight/codex-no-progress; do
    if [ -e "$p" ]; then
      rm -f "$p"
      echo "  removed stale $p"
    fi
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
  remove_runtime_file .codex/overnight/launch.lock "codex launch.lock"
  clear_codex_supervisor_state
  remove_runtime_file .claude/state/sequencer-fixpoint "stale fixpoint sentinel"
}

if [ "${1:-}" = "--disarm" ]; then
  rm -f "$MARKER"
  python3 .claude/hooks/run_phase_guard.py clear "disarmed via arm-sequencer.sh" >/dev/null 2>&1 || true
  bash "$LOCAL_ARM" "$DOCTRINE" --disarm || true
  remove_chromemcp_dropin
  remove_sequencer_env_dropin
  remove_codex_driver_dropin
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
DRIVER="claude"
FORWARD_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --with-browser|--gh-pages)
      WITH_BROWSER=1
      shift
      ;;
    --codex-driver)
      DRIVER="codex"
      shift
      ;;
    --driver)
      DRIVER="${2:?--driver needs claude or codex}"
      shift 2
      ;;
    *)
      FORWARD_ARGS+=("$1")
      shift
      ;;
  esac
done

case "$DRIVER" in
  claude|codex) ;;
  *) echo "FATAL: unsupported driver: $DRIVER (expected claude|codex)" >&2; exit 2 ;;
esac

# Arm. Marker first, so the very first tool call of the headless run is already
# redirected onto overnight-sequencer. Drop-ins are written AFTER the transient
# unit exists (systemd-run created it) and before it fires (--at is +2min), then
# daemon-reload makes them effective for the scheduled start.
mkdir -p .claude/state
: > "$MARKER"
bash "$LOCAL_ARM" "$DOCTRINE" --mode bypassPermissions --driver "$DRIVER" --watchdog "*:0/10" ${FORWARD_ARGS[@]+"${FORWARD_ARGS[@]}"}
write_sequencer_env_dropin
if [ "$DRIVER" = "codex" ]; then
  clear_codex_supervisor_state
  write_codex_driver_dropin
else
  remove_codex_driver_dropin
fi
if [ "$WITH_BROWSER" = "1" ]; then
  remove_chromemcp_dropin
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP ON (gh-pages run)"
else
  write_chromemcp_dropin
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP OFF (kernel run)"
fi
if [ "$DRIVER" = "codex" ]; then
  echo "  launch uses Codex supervisor with bounded exec steps; doctrine: $DOCTRINE"
  echo "  Codex reports: .codex/overnight/reports/latest.log"
else
  echo "  launch redirects to Skill(overnight-sequencer); doctrine: $DOCTRINE"
  echo "  Claude reports: .claude/overnight/reports/latest.log"
fi
echo "  scheduler: repo-vendored (scripts/overnight/), no external plugin dependency"
echo "  disarm: bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"
