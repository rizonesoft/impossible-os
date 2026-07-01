#!/usr/bin/env bash
# Pre-commit guard: never let a live OpenRouter (or similar) key into tracked files.
# Repo-wide -- covers conclave/secret and any other file whose basename is `secret`.
# Folded from conclave/.githooks/pre-commit when the Conclave engine moved in-repo
# (a nested .githooks is inert under impossible-os's single core.hooksPath).
set -euo pipefail

# 1. Refuse any staged secret file: `secret`, `secrets`, `secret.json`, `secrets.json`
#    (e.g. conclave/secret, conclave/secrets.json) -- but NOT `*.example` templates.
staged_secret="$(git diff --cached --name-only | grep -E '(^|/)secrets?(\.json)?$' | grep -vE '\.example' || true)"
if [ -n "$staged_secret" ]; then
  echo "REFUSING: a secret file is staged -- it must stay gitignored:" >&2
  echo "$staged_secret" >&2
  exit 1
fi

# 2. Block a real-looking OpenRouter key in any staged addition, except *secret.example.
if git diff --cached -U0 -- . ':(exclude)*secret.example' \
   | grep -E '^\+' | grep -Eq 'sk-or-v1-[0-9a-f]{32,}'; then
  echo "REFUSING: a real-looking OpenRouter key (sk-or-v1-...) is staged." >&2
  exit 1
fi

exit 0
