#!/usr/bin/env bash
# Tail the latest overnight report.
set -euo pipefail

BASE=".claude/overnight"
REPORT_DIR="$BASE/reports"
LATEST="$REPORT_DIR/latest.log"
if [ -e "$LATEST" ]; then
  TARGET="$LATEST"
else
  TARGET="$(ls -1t "$REPORT_DIR"/run-*.log 2>/dev/null | head -1 || true)"
fi

if [ -z "${TARGET:-}" ]; then
  echo "no overnight report found under $REPORT_DIR" >&2
  exit 1
fi

echo "monitoring $TARGET"
tail -n "${OVERNIGHT_MONITOR_LINES:-80}" -F "$TARGET"
