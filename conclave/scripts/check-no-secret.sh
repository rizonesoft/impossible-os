#!/usr/bin/env bash
# Pre-commit guard: never let a real OpenRouter key into tracked files.
set -euo pipefail

if git diff --cached --name-only | grep -qx secret; then
  echo "REFUSING: 'secret' is staged -- it must stay gitignored."
  exit 1
fi

# Block a real-looking key in any staged addition, EXCEPT the placeholder.
if git diff --cached -U0 -- . ':(exclude)secret.example' \
   | grep -E '^\+' | grep -E 'sk-or-v1-[0-9a-f]{32,}' >/dev/null; then
  echo "REFUSING: a real-looking OpenRouter key is staged."
  exit 1
fi

exit 0
