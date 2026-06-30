#!/usr/bin/env bash
# codex-bg-dispatch.sh -- background-mode Codex dispatch fallback for the
# 10-min Bash wall.
#
# Usage:
#   bash scripts/codex-bg-dispatch.sh "<prompt>"
#
# Use this ONLY when a foreground codex-companion call (e.g. design review)
# hits Bash's 600000ms (10 min) wall and gets truncated. Most Codex reviews
# fit in foreground; do not reach for this preemptively.
#
# Mechanism: dispatches via `codex-companion.mjs task --background --json`,
# which spawns a detached worker and returns immediately with {jobId, logFile}.
# The receiving-review-required PreToolUse hook auto-gates subsequent Edit/
# Write/MultiEdit calls on `last-codex-review.json` (which the existing
# codex_review_completed.py PostToolUse hook writes when it sees this Bash
# invocation). To clear the gate, agent reads the result and runs
# `Skill(superpowers:receiving-code-review)`.
#
# Output: prints the JSON returned by codex-companion (jobId, logFile, status)
# plus a one-line poll hint. Caller parses the JSON and polls via
# `node codex-companion.mjs status <jobId> --wait --timeout-ms 240000`.

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <prompt>" >&2
    echo "Example: $0 'Design review for TODO-XX section N. Plan: ...'" >&2
    exit 2
fi

PROMPT="$1"
# Default to the codex plugin's marketplaces install path under $HOME.
# The plugin is per-user, so $HOME is the correct anchor (works on any host
# that installed the codex plugin via /plugin install).
COMPANION="${CODEX_COMPANION_PATH:-$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs}"

if [[ ! -f "$COMPANION" ]]; then
    echo "ERROR: codex-companion.mjs not found at $COMPANION" >&2
    echo "Set CODEX_COMPANION_PATH if the codex plugin lives elsewhere." >&2
    exit 1
fi
export CODEX_REVIEWER_DISPATCH=1

# task --background --json returns immediately with {jobId, status, logFile}.
# The detached worker continues reasoning in the background (no Bash wall).
OUTPUT="$(node "$COMPANION" task --background --json "$PROMPT")"

echo "$OUTPUT"
echo ""
JOB_ID="$(printf '%s' "$OUTPUT" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("jobId",""))' 2>/dev/null || true)"
if [[ -n "$JOB_ID" ]]; then
    echo "Poll: node \"$COMPANION\" status \"$JOB_ID\" --wait --timeout-ms 240000"
    echo "Status only: node \"$COMPANION\" status \"$JOB_ID\""
fi
