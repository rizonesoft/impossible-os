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
# OVERNIGHT_REPORT_DIR override exists for the rollover test harness; the run
# itself always uses the default path.
REPORT_DIR="${OVERNIGHT_REPORT_DIR:-$REPO_ROOT/.claude/overnight/reports}"
LATEST="$REPORT_DIR/latest.log"
LINES="${OVERNIGHT_MONITOR_LINES:-80}"
WAIT=1
[ "${1:-}" = "--no-wait" ] && WAIT=0

# Liveness header (2026-07-27). This script is a log TAILER -- it never told the
# operator whether a run was actually alive, so the fallback was a bare
# `systemctl --user is-active` on the main unit, which reports "inactive" while
# the watchdog is carrying the run after a timer collision. Print the real
# three-state answer first; never let its failure block the tail.
bash "$SCRIPT_DIR/run-liveness.sh" 2>/dev/null || true
echo

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

# Rollover-aware follow. `tail -F latest.log` does NOT survive rollover on GNU
# coreutils 9.x: -F follows the symlink's RESOLVED inode and does not re-resolve
# when the symlink is repointed to the new run-*.log (empirically verified on
# coreutils 9.4). Instead, tail the resolved file directly and poll the symlink;
# when it repoints (watchdog relaunch / rollover), kill the old tail and re-follow
# the new target, printing a marker so the transition is visible.
echo "monitoring $(readlink -f "$LATEST" 2>/dev/null || echo "$TARGET") (rollover-aware; Ctrl-C to stop)"
prev=""
cleanup() { [ -n "${tp:-}" ] && kill "$tp" 2>/dev/null; exit 0; }
trap cleanup INT TERM
while true; do
  tgt="$(readlink -f "$LATEST" 2>/dev/null || true)"
  [ -z "$tgt" ] || [ ! -e "$tgt" ] && { tgt="$(resolve_target)"; }
  if [ -z "${tgt:-}" ] || [ ! -e "$tgt" ]; then sleep 1; continue; fi
  if [ "$tgt" != "$prev" ]; then
    [ -n "$prev" ] && printf '\n=== rollover -> %s ===\n' "$(basename "$tgt")"
    prev="$tgt"
    tail -n "$LINES" -f "$tgt" &
    tp=$!
  fi
  # Poll until the symlink repoints or the current tail dies, then re-follow.
  while [ "$(readlink -f "$LATEST" 2>/dev/null || true)" = "$tgt" ]; do
    kill -0 "$tp" 2>/dev/null || break
    sleep 2
  done
  kill "$tp" 2>/dev/null; wait "$tp" 2>/dev/null || true
done
