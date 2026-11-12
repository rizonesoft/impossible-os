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

SELF_NAME="codex-dispatch"
# The prompt is passed after `--`. Given ONE argument, codex-companion.mjs re-splits
# it on whitespace (normalizeArgv -> lib/args.mjs splitRawArgumentString) and reads
# any word starting with `-` as its own option: `--base /` killed a review,
# `--scope and` ran one against the wrong scope, and on 2026-11-12 `git -C after`
# became --cwd after and every pass died "git is not installed". After `--` the
# prompt is one positional, kept exactly as written.

# Hook attribution: the wrapper invocation IS the canonical shape that
# .claude/hooks/_review_kind.py recognizes. The hook scans for either
# `codex-companion.mjs adversarial-review` (legacy direct shape) OR
# `codex-dispatch.sh` (this wrapper). exec replaces this process with
# node so the dispatched output streams directly to the parent shell.
export CODEX_REVIEWER_DISPATCH=1

# Retain the transcript and register the leg (v17 close-out, 2026-08-29). Two
# live failures had one cause -- this wrapper streamed to stdout and kept
# NOTHING: (1) three post-ship legs piped through `tail -2` lost their entire
# review bodies with no artifact to recover them from (~11 minutes re-dispatched);
# (2) `review-envelope.py --section N` reads the BROKER's manifest, never saw
# the legs dispatched through here, and served a STALE wave from an earlier
# round as `all_clean: true`. So this path now tees to the same reviews
# directory the broker uses and appends the same manifest shape, and the two
# dispatch routes can no longer disagree about what happened.
COMPANION="${CODEX_COMPANION_PATH:-$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs}"
REVIEWS_DIR="${CODEX_REVIEWS_DIR:-.claude/overnight/reviews}"
FIRST_LINE="$(printf '%s\n' "$PROMPT" | sed -n '/[^[:space:]]/{p;q;}')"
KIND="direct"
if [[ "$FIRST_LINE" =~ ^\[review-kind:[[:space:]]*([a-zA-Z-]+)\] ]]; then
    KIND="${BASH_REMATCH[1],,}"
fi
TODO_PATH="$(printf '%s\n' "$FIRST_LINE" | grep -oE 'todo/[^[:space:]]+\.md' | head -1 || true)"
_rest="${FIRST_LINE#*.md}"
_m="$(printf '%s\n' "$_rest" | grep -oiE "^[[:space:]]*(section[[:space:]_:-]*|§[[:space:]]*)[0-9]+" | head -1 || true)"
[[ -n "$_m" ]] || _m="$(printf '%s\n' "$_rest" | grep -oiE "(section[[:space:]_:-]*|§[[:space:]]*)[0-9]+" | head -1 || true)"
SECTION="$(printf '%s' "$_m" | grep -oE '[0-9]+' | tail -1 || true)"
# A pass of the reviewer pin (.conclave/reviewers.toml): the pin must hold and
# Codex must be configured for its model, the pass's lenses are appended with
# the verdict line each one owes, and below the returned review is checked for
# those lines. A lens with no verdict is an incomplete pass, never a clean one
# (the combined prompt was split in April 2026 because one angle was read and
# the rest skimmed). REVIEW_BASE names the commit the section started from;
# its changed files decide the conditional lenses.
#
# The review recorder reads the marker more loosely than the line above does
# (leading space, `review_kind`, any case, anywhere on the first line). A marker
# only it would read names a kind this wrapper never saw: the pass would be
# credited with no pin check and no lens check. So the marker is written one
# way, once, at the start of the first line, or the dispatch is refused.
MARKERS="$(printf '%s\n' "$FIRST_LINE" | grep -oiE '\[[[:space:]]*review[-_ ]kind[[:space:]]*:' | wc -l)"
if [ "$MARKERS" -gt 1 ] || { [ "$MARKERS" = "1" ] && [ "$KIND" = "direct" ]; }; then
    echo "[$SELF_NAME] BLOCK -- write the marker once, as the first thing on the first line: [review-kind: <kind>]" >&2
    exit 2
fi
PIN_KIND=0; PIN_TOOL=""; PATHS_FILE=""
case "$KIND" in plan|round|performance|completion|reconciliation) PIN_KIND=1 ;; esac
if [ "$PIN_KIND" = "1" ]; then
    PIN_TOOL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/review_pin.py"
    python3 "$PIN_TOOL" --check >&2 || {
        echo "[$SELF_NAME] BLOCK -- the reviewer pin refuses a '$KIND' dispatch (reason above)" >&2; exit 2; }
    # The commit gate finds a pass by its TODO and section, read from this line.
    if [ -z "$TODO_PATH" ] || [ -z "$SECTION" ]; then
        echo "[$SELF_NAME] BLOCK -- a '$KIND' pass names its TODO and section on the first line: [review-kind: $KIND] todo/<path>.md section <N>" >&2
        exit 2
    fi
    # A pass is validated from its transcript, so one that cannot be kept is not dispatched.
    mkdir -p "$REVIEWS_DIR" 2>/dev/null || {
        echo "[$SELF_NAME] BLOCK -- no writable reviews directory ($REVIEWS_DIR); a '$KIND' pass cannot be checked" >&2; exit 2; }
    PATHS_FILE="$(mktemp -t codex-dispatch-paths.XXXXXX)" || exit 2
    trap 'rm -f "$PATHS_FILE"' EXIT
    # The changed files decide the conditional lenses, so a base that cannot
    # be read refuses the pass: an empty list would drop those lenses silently.
    if [ -n "${REVIEW_BASE:-}" ]; then
        BASE_OID="$(git rev-parse --quiet --verify "${REVIEW_BASE}^{commit}" 2>/dev/null)" || {
            echo "[$SELF_NAME] BLOCK -- REVIEW_BASE is not a commit here: $REVIEW_BASE" >&2; exit 2; }
    else
        BASE_OID="$(git rev-parse --quiet --verify 'HEAD~1^{commit}' 2>/dev/null)" \
            || BASE_OID="4b825dc642cb6eb9a060e54bf8d69288fbee4904"      # a first commit: against the empty tree
    fi
    { git diff --name-only "$BASE_OID" HEAD && git diff --name-only HEAD; } > "$PATHS_FILE.all" 2>/dev/null || {
        rm -f "$PATHS_FILE.all"
        echo "[$SELF_NAME] BLOCK -- the changed files of this pass could not be listed" >&2; exit 2; }
    sort -u "$PATHS_FILE.all" > "$PATHS_FILE"; rm -f "$PATHS_FILE.all"
    PIN_LENSES="$(python3 "$PIN_TOOL" --lenses "$KIND" --paths-file "$PATHS_FILE")" || {
        echo "[$SELF_NAME] BLOCK -- the lenses of a '$KIND' pass could not be read" >&2; exit 2; }
    PROMPT="$PROMPT

$PIN_LENSES"
fi
STAMP="$(date +%Y%m%d-%H%M%S)"
if mkdir -p "$REVIEWS_DIR" 2>/dev/null; then
    # The process id is in the name: two passes of one kind started in the same
    # second must not share a transcript, or one is checked against the other's verdict.
    LOG_FILE="$(cd "$REVIEWS_DIR" && pwd)/${STAMP}-$$-${KIND}-direct.out"
else
    LOG_FILE=""
fi
if [ -n "$LOG_FILE" ]; then
    echo "[codex-dispatch] transcript: $LOG_FILE" >&2
    set -o pipefail
    # The candidate is identified BEFORE the reviewer runs. Read afterwards, a
    # commit made by another caller during the review would be named as the
    # one reviewed, and the commit gate would bind the pass to it.
    TREE_HEAD="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    TREE_INDEX="$(git write-tree 2>/dev/null || echo unknown)"
    TREE_WORK="$({ git diff HEAD -- 2>/dev/null; git status --porcelain 2>/dev/null; } | sha256sum | cut -d' ' -f1)"
    node "$COMPANION" adversarial-review -- "$PROMPT" 2>&1 | tee "$LOG_FILE"
    rc=$?
    if [ "$PIN_KIND" = "1" ] && [ "$rc" = "0" ]; then
        NOW_HEAD="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
        NOW_WORK="$({ git diff HEAD -- 2>/dev/null; git status --porcelain 2>/dev/null; } | sha256sum | cut -d' ' -f1)"
        if [ "$NOW_HEAD" != "$TREE_HEAD" ] || [ "$NOW_WORK" != "$TREE_WORK" ]; then
            rc=3
            echo "[$SELF_NAME] INCOMPLETE -- the candidate changed while it was being reviewed (HEAD ${TREE_HEAD:0:10} -> ${NOW_HEAD:0:10}, or the working tree moved)" \
                | tee -a "$LOG_FILE" >&2
            echo "[$SELF_NAME]   This pass read a candidate that no longer exists. Dispatch it again." | tee -a "$LOG_FILE" >&2
        fi
    fi
    # The receipt below says whether THIS wrapper checked the pass. The review
    # recorder credits a pass kind only against such a receipt with rc 0, so a
    # pass dispatched any other way, or one a lens was skipped in, earns nothing.
    PASS_CHECKED=false; PASS_HIGHEST=""
    if [ "$PIN_KIND" = "1" ]; then PASS_CHECKED=true; fi
    if [ "$PIN_KIND" = "1" ] && [ "$rc" = "0" ]; then
        PIN_MISSING="$(python3 "$PIN_TOOL" --verdicts "$KIND" --transcript "$LOG_FILE" --paths-file "$PATHS_FILE")"
        case "$?" in
            0) ;;
            1) rc=3
               echo "[$SELF_NAME] INCOMPLETE -- the review gave no verdict line for: $(printf '%s' "$PIN_MISSING" | tr '\n' ' ')" \
                   | tee -a "$LOG_FILE" >&2
               echo "[$SELF_NAME]   This pass does not count as clean. Dispatch it again." | tee -a "$LOG_FILE" >&2 ;;
            *) rc=3
               echo "[$SELF_NAME] INCOMPLETE -- the lens verdicts could not be checked" | tee -a "$LOG_FILE" >&2 ;;
        esac
    fi
    # The worst level the review declares goes on the receipt: the commit gate
    # refuses a stamp whose pass ended at a blocking level, so a completion
    # that returned gaps cannot stand as the last word.
    if [ "$PIN_KIND" = "1" ] && [ "$rc" = "0" ]; then
        PASS_HIGHEST="$(python3 "$PIN_TOOL" --highest --transcript "$LOG_FILE")" || {
            rc=3; PASS_HIGHEST=""
            echo "[$SELF_NAME] INCOMPLETE -- the review has no HIGHEST: line (P1, P2, P3 or none)" | tee -a "$LOG_FILE" >&2; }
    fi
    echo "Turn completed (rc=$rc)" >> "$LOG_FILE"
    PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"
    # Written by a JSON serializer: a path or TODO name holding a quote, a
    # backslash or a newline must not produce a line no reader can parse.
    python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/overnight/manifest-line.py" --append "$REVIEWS_DIR/manifest.jsonl" \
        "int:ts=$(date +%s)" "kind=$KIND" "todo=$TODO_PATH" "opt:int:section=$SECTION" "jobId=direct:$STAMP-$$" \
        "logFile=$LOG_FILE" "prompt_sha256=$PROMPT_SHA" "head=$TREE_HEAD" "index_tree=$TREE_INDEX" \
        "worktree_sha256=$TREE_WORK" "route=codex-dispatch.sh" "int:rc=$rc" "bool:pass_checked=$PASS_CHECKED" \
        "highest=$PASS_HIGHEST" \
        || echo "[$SELF_NAME] the receipt could not be written to $REVIEWS_DIR/manifest.jsonl; a pass kind will not be credited" >&2
    exit "$rc"
fi
# No writable reviews directory: behave exactly as before (nothing retained).
exec node "$COMPANION" adversarial-review -- "$PROMPT"
