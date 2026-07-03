#!/usr/bin/env bash
# Tail the latest overnight report from ANY directory.
#
# Resolves the repo root from this script's own location (BASH_SOURCE), so it
# no longer depends on $PWD -- the old relative-path version only worked when
# run from the repo root. When no report exists yet (armed but the first
# launch has not fired), waits for the first one to appear so the command is
# usable immediately after arming; pass --no-wait to fail fast instead.
#
#   bash scripts/overnight/overnight-monitor.sh            # from anywhere
#   OVERNIGHT_MONITOR_LINES=200 bash .../overnight-monitor.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPORT_DIR="$REPO_ROOT/.claude/overnight/reports"
LATEST="$REPORT_DIR/latest.log"
LINES="${OVERNIGHT_MONITOR_LINES:-80}"
WAIT=1
[ "${1:-}" = "--no-wait" ] && WAIT=0

resolve_target() {
  # Prefer the latest.log symlink (repointed each launch); fall back to the
  # newest run-*.log by mtime. Absolute paths throughout.
  if [ -e "$LATEST" ]; then
    printf '%s\n' "$LATEST"
    return
  fi
  ls -1t "$REPORT_DIR"/run-*.log 2>/dev/null | head -1 || true
}

TARGET="$(resolve_target)"
if [ -z "${TARGET:-}" ]; then
  if [ "$WAIT" -eq 0 ]; then
    echo "no overnight report yet under $REPORT_DIR" >&2
    exit 1
  fi
  echo "no report yet under $REPORT_DIR -- waiting for the first launch" >&2
  echo "(Ctrl-C to stop; pass --no-wait to fail fast instead)" >&2
  while [ -z "${TARGET:-}" ]; do
    sleep 3
    TARGET="$(resolve_target)"
  done
fi

echo "monitoring $TARGET"
# -F follows by name: survives the latest.log symlink being repointed to a new
# run-*.log on each watchdog relaunch.
exec tail -n "$LINES" -F "$TARGET"
