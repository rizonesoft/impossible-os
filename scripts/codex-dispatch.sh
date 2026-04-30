#!/usr/bin/env bash
# scripts/codex-dispatch.sh -- canonical Codex dispatch wrapper.
#
# Owner: 00-infrastructure/TODO-08-automation-hardening (prompt argument
# escaping doctrine section). Companion to scripts/lint.sh Check 12 which
# is the LOAD-BEARING safety: it scans source text BEFORE the shell parses
# anything. This wrapper is a convenience layer that:
#   1. Enforces a single argv contract (argc == 1) so multi-argv misuse
#      where the caller forgot to single-quote the full prompt fails
#      loud rather than silently dropping the TODO path / body.
#   2. Adds defense-in-depth: emits ADVICE on argv[1] containing literal
#      $( or ${ tokens (could be intentional in single-quotes; could be
#      residual post-expansion). Does not block.
#   3. Provides a stable hook-recognized invocation shape so
#      .claude/hooks/_review_kind.py and codex_review_completed.py can
#      attribute the dispatch to the (todo_path, review-kind) pair.
#
# Usage:
#
#   RECOMMENDED: heredoc + variable -- safe for any prompt body, including
#   contractions ("doesn't", "wrapper's"), C literals ('\0'), backticks,
#   and parentheses. The single-quoted heredoc terminator prevents bash
#   from expanding $(...), ${...}, or backticks inside the body, and the
#   variable expansion preserves whitespace as one argv:
#
#     PROMPT=$(cat <<'EOF'
#     [review-kind: adversarial] todo/01-boot-platform/TODO-01-...md §N
#
#     <multi-line prompt body, free to use apostrophes, parens, C tokens>
#     EOF
#     )
#     bash scripts/codex-dispatch.sh "$PROMPT"
#
#   ACCEPTABLE for short, apostrophe-free prompts: inline single-quote.
#   Will FAIL when the body contains a literal apostrophe (collapses the
#   outer quote) or a backtick (becomes command substitution if any
#   intermediate layer evaluates it):
#
#     bash scripts/codex-dispatch.sh '[review-kind: adversarial] <todo-path> <body>'
#
# Exit codes:
#   0  -- prompt validated; node exec replaces this process; node's exit
#         code is what the parent shell sees.
#   1  -- argc != 1 (multi-argv misuse).
#   *  -- whatever node exits with (when exec succeeded).

set -u

if [ "$#" -ne 1 ]; then
    cat >&2 <<EOF
[codex-dispatch] BLOCK -- expected exactly 1 argv (the prompt body), got $#.

The wrapper requires the FULL prompt as one argv. RECOMMENDED shape
(safe for any body, including apostrophes / backticks / parens):

  PROMPT=\$(cat <<'INNER'
  [review-kind: <kind>] <todo-path> <body>
  INNER
  )
  bash scripts/codex-dispatch.sh "\$PROMPT"

Inline single-quoted form works ONLY if the body has no apostrophes or
backticks:

  bash scripts/codex-dispatch.sh '[review-kind: <kind>] <todo-path> <body>'

If you wrote the prompt unquoted or with multiple separate quoted
arguments, bash split it on whitespace and only the first word reached
the wrapper. The TODO path + body would be silently dropped, the
dispatch would land without the attribution markers, and the
section-commit gate would block the next commit citing "no per-kind
stamp".

The escaping discipline is enforced at lint time (scripts/lint.sh
Check 12) so source-text examples cannot drift away from a safe form.
EOF
    exit 1
fi

PROMPT="$1"

# Defense-in-depth ADVICE only -- the shell already evaluated anything
# the caller wrote unquoted. Both intentional ($( in single-quoted text)
# and residual (post-expansion result happened to contain $) cases land
# here legitimately. Real anti-substitution discipline is single-quoting
# at the source + lint Check 12 catching authoring errors.
case "$PROMPT" in
    *'$('*|*'${'*)
        echo "[codex-dispatch] ADVICE -- prompt contains literal '\$(' or '\${' tokens." >&2
        echo "[codex-dispatch]   If you intended substitution, this dispatch ran AFTER" >&2
        echo "[codex-dispatch]   bash already evaluated it (so the substituted value is" >&2
        echo "[codex-dispatch]   in the prompt now). If you intended literal text, you" >&2
        echo "[codex-dispatch]   correctly single-quoted -- ignore this advice." >&2
        ;;
esac

# Hook attribution: the wrapper invocation IS the canonical shape that
# .claude/hooks/_review_kind.py recognizes. The hook scans for either
# `codex-companion.mjs adversarial-review` (legacy direct shape) OR
# `codex-dispatch.sh` (this wrapper). exec replaces this process with
# node so the dispatched output streams directly to the parent shell.
exec node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "$PROMPT"
