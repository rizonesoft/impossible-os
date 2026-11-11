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
    # `s3` straight after the path, the form item prompts use: the section glyph is
    # discouraged in source, which led writers to it and dropped the leg from the
    # envelope. Anchored, so `x86 s3 sleep` later in the prompt is not a section.
    if [[ -z "$m" ]]; then
        m="$(printf '%s\n' "$rest" | grep -oE "^[[:space:]]*s[0-9]+([^[:alnum:]]|$)" | head -1 | grep -oE "[0-9]+" | head -1 || true)"
    fi
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

The backtick must TOUCH the flag. A code span with a space before the flag,
such as \`git diff $BAD_FLAG\`, still splits into a bare $BAD_FLAG token and is
refused again: write \`git diff\` \`$BAD_FLAG\`, or name the option in prose.
EOF
        exit 2
        ;;
esac

KIND="${BASH_REMATCH[1],,}"
# A pass of the reviewer pin (.conclave/reviewers.toml) is run by
# scripts/codex-dispatch.sh inside the detached job below: that wrapper checks
# the pin, appends the pass's lenses, reads the returned review for a verdict
# per lens and its HIGHEST level, and writes the receipt the commit gate
# judges. The job's exit status is the wrapper's, so an incomplete pass ends
# "Turn completed (rc=3)" here. REVIEW_BASE is passed through.
PASS_KIND=0
case "$KIND" in plan|round|performance|completion|reconciliation) PASS_KIND=1 ;; esac
PASS_WRAPPER="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/codex-dispatch.sh"

# BACKEND OUTAGE GATE (2026-09-28): a Codex outage used to cost a retry storm
# (5 dispatch+wait pairs, ~4m20s, run-20260903-160406.log:423-436) before the
# run concluded "outage" and deferred. The failed legs already say so; read
# them first. Exit 75 = defer this review and continue other work; clears when
# a leg succeeds or 20 minutes pass. Override: CODEX_OUTAGE_OVERRIDE=1.
if [[ "${CODEX_OUTAGE_OVERRIDE:-}" != "1" ]]; then
    _OUTAGE_RC=0
    _OUTAGE_WHY="$(python3 "$(dirname "$0")/codex-outage-check.py" --manifest "${BROKER_MANIFEST:-.claude/overnight/reviews/manifest.jsonl}")" || _OUTAGE_RC=$?
    if [[ $_OUTAGE_RC -eq 75 ]]; then
        {
            echo "[$SELF_NAME] REFUSED -- the Codex backend looks DOWN: $_OUTAGE_WHY"
            echo
            echo "Do not retry. Defer this review (park the section with the outage as its"
            echo "blocker) and continue with work that needs no Codex. The gate clears by itself"
            echo "once a leg succeeds or 20 minutes pass since those failures; to probe the"
            echo "backend deliberately, re-run with CODEX_OUTAGE_OVERRIDE=1."
        } >&2
        exit 75
    fi
fi
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
# The process id is in the name: two reviews of one kind started in the same
# second (two sections, say) must not write one transcript.
LOG_FILE="$PWD/$MANIFEST_DIR/${STAMP}-$$-${KIND}.out"
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
# Every value placed in the detached command is shell-quoted by printf %q. A
# path or a ref may hold an apostrophe (refs/heads/owner's-fix is a valid ref),
# which inside hand-written single quotes ends the quoting.
q() { printf '%q' "$1"; }
if [[ "$PASS_KIND" == "1" ]]; then
  # The wrapper's own messages (a refusal, an incomplete pass) go to the log too.
  # The detached job gets a fresh environment, so everything the wrapper reads is named here.
  RUN="REVIEW_BASE=$(q "${REVIEW_BASE:-}") CODEX_REVIEWS_DIR=$(q "$PWD/$MANIFEST_DIR") CODEX_COMPANION_PATH=$(q "$COMPANION") bash $(q "$PASS_WRAPPER") \"\$(cat $(q "$PROMPT_FILE"))\""
else
  RUN="node $(q "$COMPANION") adversarial-review \"\$(cat $(q "$PROMPT_FILE"))\""
fi
Q_LOG="$(q "$LOG_FILE")"; Q_PROMPT="$(q "$PROMPT_FILE")"

# REGISTERED BEFORE IT IS LAUNCHED. The gate reads the newest manifest line for
# a pass. Written after the launch, this line could land after a fast job's
# finished receipt and hide it. Which tree the review examines is recorded at
# the same moment (v16 carry, closed 2026-08-29: HEAD, the index tree and a
# working-tree digest; all three are advisory strings).
DETACH="detached:$UNIT"
PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"
TREE_HEAD="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
TREE_INDEX="$(git write-tree 2>/dev/null || echo unknown)"
TREE_WORK="$({ git diff HEAD -- 2>/dev/null; git status --porcelain 2>/dev/null; } | sha256sum | cut -d' ' -f1)"
# Written by a JSON serializer, as the receipt is: a path holding a quote, a
# backslash or a newline must not leave a line no reader can parse.
MANIFEST_LINE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/manifest-line.py"
python3 "$MANIFEST_LINE" --append "$MANIFEST" \
    "int:ts=$(date +%s)" "kind=$KIND" "todo=$TODO_PATH" "opt:int:section=$SECTION" "jobId=$DETACH" \
    "logFile=$LOG_FILE" "prompt_sha256=$PROMPT_SHA" "head=$TREE_HEAD" "index_tree=$TREE_INDEX" \
    "worktree_sha256=$TREE_WORK" || {
  echo "ERROR: the dispatch could not be registered in $MANIFEST; nothing was launched" >&2
  rm -f "$PROMPT_FILE"
  exit 1
}
# LIFECYCLE-WAIVER: detaches a REVIEW into its own scope (the reasons are above
# PROMPT_FILE); it does not relaunch or watch the session.
if systemd-run --user --collect "--unit=$UNIT" \
     "--setenv=CODEX_REVIEWER_DISPATCH=1" "--setenv=HOME=$HOME" \
     "--setenv=PATH=$PATH" --same-dir \
     /bin/bash -c "$RUN > $Q_LOG 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> $Q_LOG; rm -f $Q_PROMPT" \
     >/dev/null 2>&1; then
  :
else
  # Non-systemd fallback: setsid detach (survives plain shells; NOT a
  # systemd cgroup -- callers inside a service should have systemd-run).
  # LIFECYCLE-WAIVER: detaches a REVIEW on non-systemd hosts, not the session.
  setsid bash -c "$RUN > $Q_LOG 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> $Q_LOG; rm -f $Q_PROMPT" \
    < /dev/null >/dev/null 2>&1 &
  :
fi

python3 "$MANIFEST_LINE" "kind=$KIND" "jobId=$DETACH" "logFile=$LOG_FILE" "manifest=$MANIFEST"
