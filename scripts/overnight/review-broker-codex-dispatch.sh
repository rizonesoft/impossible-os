#!/usr/bin/env bash
# review-broker-codex-dispatch.sh -- broker leg of a multi-kind Codex review.
#
# One invocation dispatches ONE review kind in the background and registers it
# in the broker manifest; the session then polls all legs' log files IN-SESSION
# (a single blocking Bash sleep loop, ~0 model tokens) and, when the artifacts
# complete, reads a single combined envelope via
# scripts/overnight/review-envelope.py. The session never exits to wait.
#
#   bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: adversarial] <todo-path> <body>'
#
# NAMING IS LOAD-BEARING: the basename ends in "codex-dispatch.sh", the shape
# the shared hook parser (.claude/hooks/_codex_dispatch.py) already recognizes
# -- so each broker leg gets its own per-kind gate receipt exactly like a
# foreground scripts/codex-dispatch.sh call. Never bundle several dispatches
# behind one opaque shell command: the recorder can only attribute what is on
# the Bash command line.
#
# Output (stdout): one JSON line {kind, jobId, logFile, manifest} -- poll
# logFile IN-SESSION for the "Turn completed" sentinel (blocking sleep loop).
set -euo pipefail

# Section attribution from the prompt's FIRST line. v17 close-out (2026-08-29):
# the previous pattern spelled the section sign as a backslash-x byte escape
# inside double quotes, which bash does not interpret and ERE has no escape for, so the `§`
# branch NEVER matched and a sign-marked prompt took whatever `section NN`
# appeared later in the line -- observed attributing one section's leg to another.
# Now a real UTF-8 `§`, and the marker must be the first thing after the TODO
# path; a mention later in the line is used only when nothing follows the path.
# Exposed as `--section-of '<line>'` so the rule is testable without a dispatch.
_broker_section_of() {
    local line="$1" rest m
    rest="${line#*.md}"
    m="$(printf '%s\n' "$rest" | grep -oiE "^[[:space:]]*(section[[:space:]_:-]*|§[[:space:]]*)[0-9]+" | head -1 || true)"
    if [[ -z "$m" ]]; then
        m="$(printf '%s\n' "$rest" | grep -oiE "(section[[:space:]_:-]*|§[[:space:]]*)[0-9]+" | head -1 || true)"
    fi
    printf '%s' "$m" | grep -oE '[0-9]+' | tail -1 || true
}
if [[ "${1:-}" == "--section-of" ]]; then
    [[ $# -eq 2 ]] || { echo "usage: $0 --section-of '<first prompt line>'" >&2; exit 2; }
    _broker_section_of "$2"
    echo
    exit 0
fi

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 '<[review-kind: X] todo-path body>'" >&2
    exit 2
fi
PROMPT="$1"
FIRST_LINE="$(printf '%s\n' "$PROMPT" | sed -n '/[^[:space:]]/{p;q;}')"
if ! [[ "$FIRST_LINE" =~ ^\[review-kind:[[:space:]]*([a-zA-Z-]+)\][[:space:]]+todo/.+\.md([[:space:]]|$) ]]; then
    echo "ERROR: first nonblank prompt line must be '[review-kind: <kind>] <todo-path> ...'" >&2
    exit 2
fi
SELF_NAME="review-broker"
# A bare `--flag` token ANYWHERE in the prompt body is parsed as a companion CLI
# flag, because codex-companion.mjs `normalizeArgv` re-splits a single-argv
# prompt into shell-like tokens (codex-companion.mjs:127-135 ->
# lib/args.mjs splitRawArgumentString). MEASURED 2026-08-24 against the real
# parser: `--base / ` yields {base:"/"} and the review dies in 88 bytes on
# `git merge-base HEAD /`; worse, `--scope and` yields {scope:"and"} and the
# review RUNS, against the wrong scope, returning findings that look legitimate.
# Only `'` and `"` are quote characters there, so BACKTICKS neutralise the token
# (`--base` parses as an ordinary positional) -- which is also the correct
# markdown for naming an option in a review prompt. This run writes such prompts
# routinely for tooling sections, so the guard is argv-time and deterministic.
case "$PROMPT" in
    --[a-zA-Z]*|*[[:space:]]--[a-zA-Z]*)
        BAD_FLAG="$(printf '%s' "$PROMPT" | grep -oE '(^|[[:space:]])--[a-zA-Z][a-zA-Z0-9-]*' | head -1 | tr -d '[:space:]')"
        cat >&2 <<EOF
[$SELF_NAME] BLOCK -- the prompt body contains a bare CLI flag token: $BAD_FLAG

codex-companion re-splits a one-argv prompt into CLI tokens, so this is
consumed as a real flag rather than read as prose. Two outcomes, both silent:
  --base <x>   the review dies with 'Not a valid object name'
  --scope <x>  the review RUNS against the wrong scope and returns findings

Fix: wrap the flag in backticks -- \`$BAD_FLAG\` -- which the companion's
tokenizer leaves as an ordinary word (only ' and " are quotes there), and which
is the right markdown for naming an option anyway.
EOF
        exit 2
        ;;
esac

KIND="${BASH_REMATCH[1],,}"
TODO_PATH="$(printf '%s\n' "$FIRST_LINE" | grep -oE 'todo/[^[:space:]]+\.md' | head -1)"
# Section attribution (v14 close-out, 2026-08-16): the envelope was FILE-scoped
# only, so a section's review wave silently ingested an earlier section's legs
# from the same TODO. The prompt already names the section; record it so
# review-envelope.py --section can filter. Separator set matches the
# codex_review_completed.py recogniser (space, dash, underscore, colon).
SECTION="$(_broker_section_of "$FIRST_LINE")"

COMPANION="${CODEX_COMPANION_PATH:-$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs}"
[[ -f "$COMPANION" ]] || { echo "ERROR: codex-companion.mjs not found at $COMPANION" >&2; exit 1; }
export CODEX_REVIEWER_DISPATCH=1

MANIFEST_DIR=".claude/overnight/reviews"
mkdir -p "$MANIFEST_DIR"
MANIFEST="$MANIFEST_DIR/manifest.jsonl"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_FILE="$PWD/$MANIFEST_DIR/${STAMP}-${KIND}.out"
UNIT="codex-rev-${KIND}-${STAMP}-$$"

# CGROUP SURVIVAL IS LOAD-BEARING (live incident 2026-07-11): a review
# spawned inside the headless session's systemd service dies with the
# session -- when the session ENDS (a verified rollover, a crash, or a
# usage-limit exit) systemd's KillMode=control-group reaps every process
# left in the service's cgroup, killing an in-flight review with it. A
# review in its OWN transient scope survives the session's death, so the
# NEXT session can still read a completed artifact. systemd-run --collect
# gives it that scope; the "Turn completed" sentinel is what the in-session
# poll (and any recovering next session) watches for.
PROMPT_FILE="$(mktemp)"
printf '%s' "$PROMPT" > "$PROMPT_FILE"
# LIFECYCLE-WAIVER: this detaches a REVIEW (Codex) into its own scope so it
# survives the session's rollover -- it does NOT relaunch or watch the session.
# The session still polls this review's logFile IN-SESSION; nothing here is the
# removed exit-and-relaunch apparatus.
if systemd-run --user --collect "--unit=$UNIT" \
     "--setenv=CODEX_REVIEWER_DISPATCH=1" "--setenv=HOME=$HOME" \
     "--setenv=PATH=$PATH" --same-dir \
     /bin/bash -c "node '$COMPANION' adversarial-review \"\$(cat '$PROMPT_FILE')\" > '$LOG_FILE' 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> '$LOG_FILE'; rm -f '$PROMPT_FILE'" \
     >/dev/null 2>&1; then
  DETACH="systemd-run:$UNIT"
else
  # Non-systemd fallback: setsid detach (survives plain shells; NOT a
  # systemd cgroup -- callers inside a service should have systemd-run).
  # LIFECYCLE-WAIVER: detaches a REVIEW on non-systemd hosts, not the session.
  setsid bash -c "node '$COMPANION' adversarial-review \"\$(cat '$PROMPT_FILE')\" > '$LOG_FILE' 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> '$LOG_FILE'; rm -f '$PROMPT_FILE'" \
    < /dev/null >/dev/null 2>&1 &
  DETACH="setsid:$!"
fi

PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"
# Which TREE did this leg examine? (v16 carry, closed 2026-08-29.) Two legs once
# disagreed because one read the index and the other the working tree, and
# nothing in the artifacts could say so. Record HEAD, the index tree and a
# working-tree digest at dispatch time; all three are advisory strings.
TREE_HEAD="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
TREE_INDEX="$(git write-tree 2>/dev/null || echo unknown)"
TREE_WORK="$({ git diff HEAD -- 2>/dev/null; git status --porcelain 2>/dev/null; } | sha256sum | cut -d' ' -f1)"
if [[ -n "$SECTION" ]]; then
  printf '{"ts": %s, "kind": "%s", "todo": "%s", "section": %s, "jobId": "%s", "logFile": "%s", "prompt_sha256": "%s", "head": "%s", "index_tree": "%s", "worktree_sha256": "%s"}\n' \
    "$(date +%s)" "$KIND" "$TODO_PATH" "$SECTION" "$DETACH" "$LOG_FILE" "$PROMPT_SHA" "$TREE_HEAD" "$TREE_INDEX" "$TREE_WORK" >> "$MANIFEST"
else
  printf '{"ts": %s, "kind": "%s", "todo": "%s", "jobId": "%s", "logFile": "%s", "prompt_sha256": "%s", "head": "%s", "index_tree": "%s", "worktree_sha256": "%s"}\n' \
    "$(date +%s)" "$KIND" "$TODO_PATH" "$DETACH" "$LOG_FILE" "$PROMPT_SHA" "$TREE_HEAD" "$TREE_INDEX" "$TREE_WORK" >> "$MANIFEST"
fi

printf '{"kind": "%s", "jobId": "%s", "logFile": "%s", "manifest": "%s"}\n' \
  "$KIND" "$DETACH" "$LOG_FILE" "$MANIFEST"
