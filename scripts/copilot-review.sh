#!/usr/bin/env bash
# External adversarial-style review from any shell (local or CI).
# Prefer GitHub Copilot CLI when installed; otherwise print how to run Codex in Claude Code.
#
# Install Copilot CLI (see https://github.com/features/copilot/cli ):
#   npm install -g @github/copilot
#   copilot auth login
#
# Usage:
#   bash scripts/copilot-review.sh "Your full review prompt with file paths and severity rubric"
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PROMPT="${*:-}"
if [ -z "$PROMPT" ]; then
  echo "usage: bash scripts/copilot-review.sh \"<review prompt>\"" >&2
  exit 2
fi

if command -v copilot >/dev/null 2>&1; then
  # Programmatic one-shot (non-interactive). --allow-all reduces tool-approval stalls for read-only review flows.
  exec copilot --allow-all -p "$PROMPT"
fi

echo "GitHub Copilot CLI (copilot) not found on PATH." >&2
echo "Install: npm install -g @github/copilot  then: copilot auth login" >&2
echo "Or in Claude Code with the OpenAI Codex plugin, run adversarial-review via codex-companion.mjs (see skill step that mentions CODEX_COMPANION)." >&2
echo "" >&2
echo "Prompt that was not executed:" >&2
echo "$PROMPT" >&2
exit 1
