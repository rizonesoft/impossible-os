#!/usr/bin/env bash
# arm-sequencer.sh -- arm the unattended overnight sequencer for impossible-os.
#
# Wraps the rizonetech overnight-runner plugin (the generic scheduler) but:
#   1. redirects the headless launch onto the repo-owned overnight-sequencer
#      skill via the .claude/state/sequencer-armed marker (run_phase_guard.py
#      hard-blocks anything else until overnight-sequencer is invoked), so the
#      plugin is NOT edited;
#   2. forces --mode bypassPermissions + a 10-min pure-failover watchdog;
#   3. turns ChromeMCP OFF at the systemd-unit level for THIS repo only (a
#      per-unit env drop-in; other projects' overnight runs keep ChromeMCP).
#
# Usage:
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh [--at "<calendar>"]
#   bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"
DOCTRINE="todo/TODO-Claude-Overnight-Runner.md"
UNIT="overnight-$(basename "$REPO_ROOT")"        # overnight-impossible-os
DROPIN_BASE="$HOME/.config/systemd/user"
MARKER=".claude/state/sequencer-armed"

PLUGIN_ARM="$(ls -d "$HOME"/.claude/plugins/cache/rizonetech/overnight-runner/*/scripts/overnight-arm.sh 2>/dev/null | sort -V | tail -1)"
[ -n "$PLUGIN_ARM" ] || { echo "FATAL: overnight-runner plugin not installed" >&2; exit 127; }

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

# Idempotent reset of the plugin's advisory run state. The plugin sets these at
# launch but only clears overnight-runner.json on a clean fixpoint; every other
# ending (usage-limit kill, watchdog reap, manual systemctl stop, crash) leaves
# active:true forever, which makes /overnight-runner:status falsely report a live
# run. The launch.lock flock self-releases on holder death, but the file lingers;
# .in_use/<pid> markers accumulate one per crashed/killed session. Nothing else
# reaps these -- so disarm owns the full teardown regardless of how the run ended.
reap_overnight_state() {
  python3 - <<'PY' || true
import json, os
p = ".claude/overnight/state/overnight-runner.json"
try:
    d = json.load(open(p))
    if d.get("active") or d.get("status") not in (None, "cleared"):
        d["active"] = False
        d["status"] = "cleared"
        json.dump(d, open(p, "w"), indent=2)
        print("  reset overnight-runner.json -> active:false (notes/cursor preserved)")
except FileNotFoundError:
    pass
except (ValueError, OSError) as e:
    print(f"  WARN: could not reset overnight-runner.json: {e}")
PY
  rm -f .claude/overnight/launch.lock && echo "  removed launch.lock" || true
  # Reap dead .in_use/<pid> markers in the newest plugin version dir.
  local iu; iu="$(dirname "$(dirname "$PLUGIN_ARM")")/.in_use"
  if [ -d "$iu" ]; then
    for m in "$iu"/*; do
      [ -e "$m" ] || continue
      pid="$(basename "$m")"
      case "$pid" in (*[!0-9]*) continue ;; esac   # skip non-PID names
      kill -0 "$pid" 2>/dev/null || { rm -f "$m" && echo "  reaped dead in_use marker $pid"; }
    done
  fi
}

if [ "${1:-}" = "--disarm" ]; then
  rm -f "$MARKER"
  python3 .claude/hooks/run_phase_guard.py clear "disarmed via arm-sequencer.sh" >/dev/null 2>&1 || true
  bash "$PLUGIN_ARM" "$DOCTRINE" --disarm || true
  remove_chromemcp_dropin
  reap_overnight_state
  systemctl --user daemon-reload 2>/dev/null || true
  echo "disarmed overnight sequencer (timers + watchdog + chromemcp drop-in + run state)"
  exit 0
fi

# ChromeMCP policy for THIS repo: impossible-os is kernel/OS work for the vast
# majority of runs, which never touch a browser -- so default to --no-browser
# (skips the ChromeMCP lane churn + waives browser gates, no noise). The ONLY
# impossible-os work that needs ChromeMCP is the gh-pages landing site; arm
# those runs with --with-browser to keep ChromeMCP fully on.
WITH_BROWSER=0
FORWARD_ARGS=()
for a in "$@"; do
  case "$a" in
    --with-browser|--gh-pages) WITH_BROWSER=1 ;;
    *) FORWARD_ARGS+=("$a") ;;
  esac
done

# Arm. Marker first, so the very first tool call of the headless run is already
# redirected onto overnight-sequencer.
mkdir -p .claude/state
: > "$MARKER"
if [ "$WITH_BROWSER" = "1" ]; then
  bash "$PLUGIN_ARM" "$DOCTRINE" --mode bypassPermissions --watchdog "*:0/10" ${FORWARD_ARGS[@]+"${FORWARD_ARGS[@]}"}
  remove_chromemcp_dropin
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP ON (gh-pages run)"
else
  # The plugin's --no-browser landed in overnight-runner 0.2.5. Older cached
  # versions reject unknown args, so only pass it when the resolved plugin
  # supports it; otherwise fall back to the env drop-in alone (still suppresses
  # session-level ChromeMCP auto-launch, just not the plugin's lane churn/gates).
  NB_FLAG=()
  if grep -q -- '--no-browser)' "$PLUGIN_ARM"; then
    NB_FLAG=(--no-browser)
  else
    echo "  NOTE: cached overnight-runner lacks --no-browser; relying on env drop-in only." >&2
    echo "        Update the plugin to 0.2.5+ for full ChromeMCP quieting (lane + gates)." >&2
  fi
  bash "$PLUGIN_ARM" "$DOCTRINE" --mode bypassPermissions --watchdog "*:0/10" ${NB_FLAG[@]+"${NB_FLAG[@]}"} ${FORWARD_ARGS[@]+"${FORWARD_ARGS[@]}"}
  write_chromemcp_dropin
  systemctl --user daemon-reload 2>/dev/null || true
  echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP OFF (kernel run; --no-browser + drop-in)"
fi
echo "  launch redirects to Skill(overnight-sequencer); doctrine: $DOCTRINE"
echo "  ChromeMCP: default OFF; arm with --with-browser for gh-pages landing-site runs"
echo "  disarm: bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"
