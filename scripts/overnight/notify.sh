#!/usr/bin/env bash
# Best-effort operator notification (overnight sequencer observability).
#
# Used by scripts/overnight/overnight-launch.sh to surface the two events the
# operator must see overnight: a TRUE fixpoint (run finished + disarmed) and a
# CRITICAL fault (run stopped, needs a human). Headless bash can't call the
# Claude PushNotification tool, so this is intentionally simple and
# dependency-light:
#   1. Always writes/updates .claude/overnight/NEEDS-OPERATOR.md (a durable
#      marker the morning-after operator + run-status surface).
#   2. If OVERNIGHT_NOTIFY_URL is set, POSTs the message to it (Slack/ntfy/webhook)
#      so the alert can reach a phone *during* the night. Failure is ignored.
#
# Usage: notify.sh <PROJECT_DIR> <level: fixpoint|critical|info> <message...>
set -euo pipefail

PROJECT_DIR="${1:?project dir required}"; shift
LEVEL="${1:?level required}"; shift
MESSAGE="${*:-overnight sequencer notification}"
STAMP="$(date -Is)"

MARKER="$PROJECT_DIR/.claude/overnight/NEEDS-OPERATOR.md"
mkdir -p "$(dirname "$MARKER")"
{
  echo "# Overnight sequencer -- operator attention"
  echo
  echo "- when: $STAMP"
  echo "- level: $LEVEL"
  echo "- message: $MESSAGE"
  echo
  echo "See .claude/overnight/run-status.md for the queue state and"
  echo "todo/TODO-Claude-Overnight-Runner.md Run Log for the punch-list."
} > "$MARKER"
echo "notify[$LEVEL]: $MESSAGE (marker: $MARKER)"

if [ -n "${OVERNIGHT_NOTIFY_URL:-}" ]; then
  # ntfy-style plain-text POST works for ntfy.sh and most webhook receivers;
  # Slack/Discord incoming webhooks accept a JSON {"text":...} body instead.
  curl -fsS -m 10 -X POST \
    -H "Title: Impossible OS overnight ($LEVEL)" \
    -d "$MESSAGE ($STAMP)" \
    "$OVERNIGHT_NOTIFY_URL" >/dev/null 2>&1 \
    && echo "notify: webhook delivered" \
    || echo "notify: webhook delivery failed (ignored)"
fi
