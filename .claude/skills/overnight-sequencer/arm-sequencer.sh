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

if [ "${1:-}" = "--disarm" ]; then
  rm -f "$MARKER"
  python3 .claude/hooks/run_phase_guard.py clear "disarmed via arm-sequencer.sh" >/dev/null 2>&1 || true
  bash "$PLUGIN_ARM" "$DOCTRINE" --disarm || true
  remove_chromemcp_dropin
  systemctl --user daemon-reload 2>/dev/null || true
  echo "disarmed overnight sequencer (timers + watchdog + chromemcp drop-in)"
  exit 0
fi

# Arm. Marker first, so the very first tool call of the headless run is already
# redirected onto overnight-sequencer.
mkdir -p .claude/state
: > "$MARKER"
bash "$PLUGIN_ARM" "$DOCTRINE" --mode bypassPermissions --watchdog "*:0/10" "$@"
write_chromemcp_dropin
systemctl --user daemon-reload 2>/dev/null || true
echo "armed overnight sequencer: bypassPermissions, watchdog *:0/10, ChromeMCP off (this repo only)"
echo "  launch redirects to Skill(overnight-sequencer); doctrine: $DOCTRINE"
echo "  disarm: bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm"
