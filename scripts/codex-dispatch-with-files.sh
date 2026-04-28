#!/usr/bin/env bash
# codex-dispatch-with-files.sh -- TODO-08 stamp-region completeness work
# (Codex Prompt-Scope Helper for Re-Reviews).
#
# Codex CLI's adversarial-review wrapper defaults to working-tree-diff scope.
# When re-reviewing a section whose implementation is already committed and
# the working tree is clean, Codex sees an empty diff and returns
# "approve / no findings" while the actual files are unreviewed.
#
# This wrapper bridges that gap: when the prompt names files (via the
# in-repo `[review-kind: ...]` + TODO path + section marker convention)
# AND the working tree is clean, gather the committed content of those
# files via `git show HEAD -- <files>` and prepend a
# `--- COMMITTED FILE CONTENT ---` block to the prompt before invoking
# the plugin's codex-companion.mjs adversarial-review.
#
# Dirty tree (working changes present) -> pass through unchanged. The
# plugin's default working-tree-diff scope already covers that case.
#
# Usage:
#   scripts/codex-dispatch-with-files.sh "<prompt>"
#
# Drop-in replacement for:
#   node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<prompt>"
#
# Plugin file is NOT modified (lives in plugin-cache; overwritten on
# plugin update). All augmentation happens prompt-side in this wrapper.
#
# Exit codes mirror codex-companion.mjs.

set -u

PROMPT="${1:-}"
if [ -z "$PROMPT" ]; then
    echo "[codex-dispatch] error: prompt argument required" >&2
    exit 2
fi

CODEX_COMPANION="$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs"
if [ ! -f "$CODEX_COMPANION" ]; then
    echo "[codex-dispatch] error: codex-companion.mjs not found at $CODEX_COMPANION" >&2
    exit 2
fi

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if [ -z "$REPO_ROOT" ]; then
    # Not in a repo: pass through unchanged.
    exec node "$CODEX_COMPANION" adversarial-review "$PROMPT"
fi

# Clean-tree detection: `git diff --quiet HEAD --` exits 0 when no
# unstaged changes; combined with `git diff --cached --quiet` for the
# staged side. If either reports changes, the working tree is "dirty"
# and we pass through.
if ! git -C "$REPO_ROOT" diff --quiet HEAD -- 2>/dev/null \
   || ! git -C "$REPO_ROOT" diff --cached --quiet 2>/dev/null; then
    exec node "$CODEX_COMPANION" adversarial-review "$PROMPT"
fi

# Clean tree path: extract repo-relative file mentions from the prompt.
# Match the canonical source-tree roots we care about (src/, include/,
# scripts/, .claude/, docs/, todo/) so plain English references like
# "src/kernel/foo.c" or "scripts/test-tooling.sh" are picked up.
# Conservative regex: file path + canonical source extension.
FILE_LIST="$(printf '%s\n' "$PROMPT" | grep -oE \
    '(src|include|scripts|\.claude|docs|todo)/[A-Za-z0-9_/.-]+\.(c|h|cpp|hpp|asm|S|sh|py|mjs|md|json|toml)' \
    | sort -u)"

if [ -z "$FILE_LIST" ]; then
    # Prompt names no files -- nothing to embed. Pass through.
    exec node "$CODEX_COMPANION" adversarial-review "$PROMPT"
fi

# Cap embedded content to keep the prompt under the 80-line ceiling +
# Bash 10-min wall safe. Per file: 200 lines max; total: 8 files max.
# A re-review whose scope exceeds these caps should split into smaller
# dispatches.
MAX_FILES=8
MAX_LINES_PER_FILE=200

EMBED=""
EMBED_FILES=0
while IFS= read -r f; do
    [ -n "$f" ] || continue
    EMBED_FILES=$((EMBED_FILES + 1))
    if [ "$EMBED_FILES" -gt "$MAX_FILES" ]; then
        EMBED="$EMBED
... [truncated: $EMBED_FILES file(s) named in prompt; capped at $MAX_FILES] ..."
        break
    fi
    # Verify the file is tracked + present at HEAD.
    if ! git -C "$REPO_ROOT" cat-file -e "HEAD:$f" 2>/dev/null; then
        EMBED="$EMBED

--- $f ---
[file not present at HEAD; possibly added in working tree or moved]"
        continue
    fi
    CONTENT="$(git -C "$REPO_ROOT" show "HEAD:$f" 2>/dev/null | head -n "$MAX_LINES_PER_FILE")"
    LINE_COUNT="$(printf '%s\n' "$CONTENT" | wc -l)"
    EMBED="$EMBED

--- $f (HEAD; first $LINE_COUNT line(s)) ---
$CONTENT"
done <<< "$FILE_LIST"

if [ -z "$EMBED" ]; then
    exec node "$CODEX_COMPANION" adversarial-review "$PROMPT"
fi

AUGMENTED="$PROMPT

--- COMMITTED FILE CONTENT ---
(working tree clean; embedding HEAD content of files named in this prompt
so Codex sees real diff context instead of an empty working-tree diff)
$EMBED
--- END COMMITTED FILE CONTENT ---"

exec node "$CODEX_COMPANION" adversarial-review "$AUGMENTED"
