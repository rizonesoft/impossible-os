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
# The receipt's prompt hash is of the CALLER's prompt, taken before any lens,
# debt or repair text is appended: the recorder matches the receipt to the call
# by hashing the argument it saw on the command line.
CALLER_PROMPT_SHA="$(printf '%s' "$PROMPT" | sha256sum | cut -d' ' -f1)"

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
# Stored objects only, for every git read this pass starts (the reviewer's included): a replacement ref would
# show other bytes than the commit the receipt names and a push sends.
export GIT_NO_REPLACE_OBJECTS=1

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
# Prompt parsing shared by the three dispatchers, in memory (no pipeline).
# shellcheck source=scripts/lib/review-prompt.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/review-prompt.sh"
FIRST_LINE="$(review_prompt_first_line "$PROMPT")"
KIND="direct"
if [[ "$FIRST_LINE" =~ ^\[review-kind:[[:space:]]*([a-zA-Z-]+)\] ]]; then
    KIND="${BASH_REMATCH[1],,}"
fi
TODO_PATH="$(review_prompt_todo_path "$FIRST_LINE")"
SECTION="$(review_prompt_section_of "$FIRST_LINE")"
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
PIN_KIND=0; PIN_TOOL=""; PATHS_FILE=""; REPAIR_HEAD=""; REPAIR_FROM=""; CAND_HEAD=""; CAND_WORK=""
EMPTY_TREE_OID="4b825dc642cb6eb9a060e54bf8d69288fbee4904"
# The candidate's fingerprint and the files a pass covers come from one reader (scripts/review_candidate.py),
# which reads each whole or refuses: links are not followed, a FIFO is never opened, a failed read is an error,
# and environment pathspec settings are dropped. It leaves this dispatcher's own reviews directory out.
CAND_TOOL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/review_candidate.py"
# HEAD, the index digest and the working-tree fingerprint, read together (scripts/review_candidate.py launch).
candidate_id() { python3 "$CAND_TOOL" launch --exclude "$REVIEWS_DIR"; }
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
    # Inside the reviews directory, which no candidate read covers: under a TMPDIR inside the checkout, the
    # file would be part of the tree it describes.
    PATHS_FILE="$(mktemp "$REVIEWS_DIR/.paths.XXXXXX")" || exit 2
    trap 'rm -f "$PATHS_FILE" "$PATHS_FILE.untracked"' EXIT
    # The candidate is pinned once, before anything is derived from it: the changed paths (and so the
    # lenses), the ancestry check, the prompt and the receipt all name this commit and this working tree,
    # and a commit or an edit by another caller while the pass is prepared refuses it below.
    CAND_HEAD="$(git rev-parse --quiet --verify 'HEAD^{commit}' 2>/dev/null)" || {
        echo "[$SELF_NAME] BLOCK -- HEAD could not be read" >&2; exit 2; }
    # The changed files decide the conditional lenses, so a base that cannot
    # be read refuses the pass: an empty list would drop those lenses silently.
    # A section built while the reviewer was down starts at its recorded base, the commit before its code
    # (scripts/overnight/review_owed.py). Its plan pass is dispatched late, so a base taken from that pass, or
    # from HEAD, would show the reviewer little or nothing of the section. Every pass after the plan is held to
    # the recorded base: taken when none is given, refused when another is.
    OWED_BASE=""
    if [ "$KIND" != "plan" ]; then
        OWED_BASE="$(python3 "$PIN_TOOL" --owed-base "$TODO_PATH" "$SECTION" --root "$(git rev-parse --show-toplevel 2>/dev/null || pwd)")" || {
            echo "[$SELF_NAME] BLOCK -- whether this section's review is owed could not be read (reason above)" >&2; exit 2; }
    fi
    if [ -n "$OWED_BASE" ]; then
        BASE_OID="$(git rev-parse --quiet --verify "${OWED_BASE}^{commit}" 2>/dev/null)" || {
            echo "[$SELF_NAME] BLOCK -- the recorded base of this owed section is not a commit here: $OWED_BASE" >&2; exit 2; }
        if [ -n "${REVIEW_BASE:-}" ] && [ "$(git rev-parse --quiet --verify "${REVIEW_BASE}^{commit}" 2>/dev/null)" != "$BASE_OID" ]; then
            echo "[$SELF_NAME] BLOCK -- this section was built while the reviewer was down and starts at ${BASE_OID:0:10}; REVIEW_BASE names another commit. Leave it unset or give that one." >&2
            exit 2
        fi
    elif [ "$KIND" = "reconciliation" ]; then
        # A reconciliation reviews the same whole section the completion before it did: its base is that
        # pass's recorded base, used for the lenses and the prompt alike, and a REVIEW_BASE naming another
        # commit is refused (review, 2026-11-21: HEAD~1 had been offered as "the whole section").
        # One read of one receipt gives both ends: the section base and the last reviewed candidate.
        RECON_RANGE="$(python3 "$PIN_TOOL" --repair-range "$TODO_PATH" "$SECTION" --manifest "$REVIEWS_DIR/manifest.jsonl")" || {
            echo "[$SELF_NAME] BLOCK -- the section base of the last reviewed candidate could not be read (reason above)" >&2; exit 2; }
        if [ -z "$RECON_RANGE" ]; then
            echo "[$SELF_NAME] BLOCK -- a reconciliation follows a completion: this section has no checked completion or reconciliation pass to start the repair from" >&2
            exit 2
        fi
        RECON_BASE="${RECON_RANGE%% *}"
        REPAIR_FROM="${RECON_RANGE#* }"
        if [ "$RECON_BASE" = "$EMPTY_TREE_OID" ]; then
            BASE_OID="$EMPTY_TREE_OID"                         # the completion of a first commit, against the empty tree
        else
            BASE_OID="$(git rev-parse --quiet --verify "${RECON_BASE}^{commit}" 2>/dev/null)" || {
                echo "[$SELF_NAME] BLOCK -- the section base ${RECON_BASE:0:10} the completion reviewed is not a commit here" >&2; exit 2; }
        fi
        if [ -n "${REVIEW_BASE:-}" ] && [ "$REVIEW_BASE" != "$BASE_OID" ] \
                && [ "$(git rev-parse --quiet --verify "${REVIEW_BASE}^{commit}" 2>/dev/null)" != "$BASE_OID" ]; then
            echo "[$SELF_NAME] BLOCK -- the completion reviewed this section from ${BASE_OID:0:10}; REVIEW_BASE names another commit. Leave it unset or give that one." >&2
            exit 2
        fi
    elif [ -n "${REVIEW_BASE:-}" ]; then
        BASE_OID="$(git rev-parse --quiet --verify "${REVIEW_BASE}^{commit}" 2>/dev/null)" || {
            echo "[$SELF_NAME] BLOCK -- REVIEW_BASE is not a commit here: $REVIEW_BASE" >&2; exit 2; }
        # A pass after the plan reviews the whole section: a base past the section's recorded start would
        # leave its first commits unreviewed while the receipt names its last one. The stamp gate refuses
        # such a pass; refusing it here saves the review.
        if [ "$KIND" != "plan" ]; then
            SECTION_START="$(python3 "$PIN_TOOL" --section-start "$TODO_PATH" "$SECTION" --manifest "$REVIEWS_DIR/manifest.jsonl")" || {
                echo "[$SELF_NAME] BLOCK -- where this section started could not be read (reason above)" >&2; exit 2; }
            if [ -n "$SECTION_START" ] && [ "$BASE_OID" != "$EMPTY_TREE_OID" ] \
                    && ! git merge-base --is-ancestor "$BASE_OID" "$SECTION_START" 2>/dev/null; then
                echo "[$SELF_NAME] BLOCK -- REVIEW_BASE ${BASE_OID:0:10} is past where this section started (${SECTION_START:0:10}), so its first commits would go unreviewed; leave REVIEW_BASE unset or give a commit at or before the start" >&2
                exit 2
            fi
        fi
    elif [ "$KIND" = "plan" ]; then
        # Before the code: the last commit, for the lenses only.
        BASE_OID="$(git rev-parse --quiet --verify "${CAND_HEAD}~1^{commit}" 2>/dev/null)" \
            || BASE_OID="$EMPTY_TREE_OID"                       # a first commit: against the empty tree
    else
        # Every later pass reviews the whole section, so with no REVIEW_BASE it starts where the section did:
        # its newest checked plan pass's candidate in the manifest this dispatcher writes. A start not on record
        # is refused rather than guessed (HEAD~1 had made a multi-commit section's receipt cover its last commit).
        SECTION_START="$(python3 "$PIN_TOOL" --section-start "$TODO_PATH" "$SECTION" --manifest "$REVIEWS_DIR/manifest.jsonl")" || {
            echo "[$SELF_NAME] BLOCK -- where this section started could not be read (reason above)" >&2; exit 2; }
        if [ -z "$SECTION_START" ]; then
            echo "[$SELF_NAME] BLOCK -- this section's start is not on record (no checked plan pass); give REVIEW_BASE=<the commit before its code>" >&2
            exit 2
        fi
        BASE_OID="$(git rev-parse --quiet --verify "${SECTION_START}^{commit}" 2>/dev/null)" || {
            echo "[$SELF_NAME] BLOCK -- the section's recorded start ${SECTION_START:0:10} is not a commit here" >&2; exit 2; }
    fi
    # Every base is on the candidate's own history: a receipt left behind by a reset or rebase, or a stale
    # REVIEW_BASE from another branch, would compare unrelated snapshots. (The empty tree, a root's base, is
    # no commit.)
    if [ "$BASE_OID" != "$EMPTY_TREE_OID" ] \
            && ! git merge-base --is-ancestor "$BASE_OID" "$CAND_HEAD" 2>/dev/null; then
        echo "[$SELF_NAME] BLOCK -- the section base ${BASE_OID:0:10} is not on the candidate's history (a reset or rebase left it behind); give REVIEW_BASE=<the commit before the section's code>" >&2
        exit 2
    fi
    # Both sides of every rename and every untracked file are part of the candidate: a kernel source moved
    # out, or a new one not yet added, decides its lenses too.
    # One read gives the working tree's fingerprint and the covered paths from the same status walk; the
    # launch check below compares a fresh fingerprint against it.
    UNTRACKED_FILE="$PATHS_FILE.untracked"
    CAND_WORK="$(python3 "$CAND_TOOL" prepare --base "$BASE_OID" --head "$CAND_HEAD" --exclude "$REVIEWS_DIR" \
            --out "$PATHS_FILE" --untracked-out "$UNTRACKED_FILE")" || {
        echo "[$SELF_NAME] BLOCK -- the working tree or the changed files of this pass could not be read (reason above)" >&2; exit 2; }
    PIN_LENSES="$(python3 "$PIN_TOOL" --lenses "$KIND" --paths-file0 "$PATHS_FILE")" || {
        echo "[$SELF_NAME] BLOCK -- the lenses of a '$KIND' pass could not be read" >&2; exit 2; }
    if [ -n "$OWED_BASE" ]; then
        PROMPT="$PROMPT

This section was built while the reviewer was unavailable."
    fi
    if [ -n "$OWED_BASE" ] || [ "$KIND" = "round" ] || [ "$KIND" = "performance" ] || [ "$KIND" = "completion" ]; then
        # The receipt names this range and fingerprints the working tree, so the reviewer is shown both.
        PROMPT="$PROMPT

This pass reviews the whole section: git diff $BASE_OID $CAND_HEAD, the candidate HEAD $CAND_HEAD, plus the index (git diff --cached $CAND_HEAD) and the working files against it (git diff), shown apart because a staged change the working file undoes is in neither combined diff."
    fi
    # Untracked entries appear in no diff. A short list goes in the prompt; a long one is kept beside the
    # transcript (the reviews directory, which the fingerprint leaves out) and named, since the prompt is one
    # argument and a long one would not launch.
    if [ -s "$UNTRACKED_FILE" ]; then
        if [ "$(wc -l < "$UNTRACKED_FILE")" -le 50 ] && [ "$(wc -c < "$UNTRACKED_FILE")" -le 8192 ]; then
            PROMPT="$PROMPT

These untracked entries are part of the candidate and appear in no diff; read each one:
$(cat "$UNTRACKED_FILE")"
        else
            UNTRACKED_KEPT="$(cd "$REVIEWS_DIR" && pwd)/$(date +%Y%m%d-%H%M%S)-$$-${KIND}-untracked.txt"
            cp "$UNTRACKED_FILE" "$UNTRACKED_KEPT" || {
                echo "[$SELF_NAME] BLOCK -- the untracked-entry list could not be kept for the reviewer" >&2; exit 2; }
            PROMPT="$PROMPT

$(wc -l < "$UNTRACKED_FILE") untracked entries are part of the candidate and appear in no diff; read each one listed in $UNTRACKED_KEPT"
        fi
    fi
    # A reconciliation reviews the newest repair, so it names the repair's two ends (operator, 2026-11-20): the
    # last reviewed candidate, the head of this section's newest checked completion or reconciliation pass, and
    # the repaired candidate, HEAD with the working tree. With no such pass on record there is no repair to name.
    if [ "$KIND" = "reconciliation" ]; then
        if [ -z "${REPAIR_FROM:-}" ]; then                     # an owed section: its base came from the debt record
            REPAIR_FROM="$(python3 "$PIN_TOOL" --repair-base "$TODO_PATH" "$SECTION" --manifest "$REVIEWS_DIR/manifest.jsonl")" || {
                echo "[$SELF_NAME] BLOCK -- the last reviewed candidate of this section could not be read (reason above)" >&2; exit 2; }
        fi
        if [ -z "$REPAIR_FROM" ]; then
            echo "[$SELF_NAME] BLOCK -- a reconciliation follows a completion: this section has no checked completion or reconciliation pass to start the repair from" >&2
            exit 2
        fi
        REPAIR_HEAD="$CAND_HEAD"
        REPAIR_OID="$(git rev-parse --quiet --verify "${REPAIR_FROM}^{commit}" 2>/dev/null)" \
            && git merge-base --is-ancestor "$REPAIR_OID" "$REPAIR_HEAD" 2>/dev/null || {
            echo "[$SELF_NAME] BLOCK -- the last reviewed candidate ${REPAIR_FROM:0:10} is not a commit in this history (a reset or rebase left its receipt behind); dispatch the completion again" >&2; exit 2; }
        PROMPT="$PROMPT

The newest repair runs from the last reviewed candidate, $REPAIR_OID, to the repaired candidate, HEAD $REPAIR_HEAD: the committed repair (git diff $REPAIR_OID $REPAIR_HEAD), the index against it (git diff --cached $REPAIR_HEAD) and the working files against the index (git diff), each shown, since a staged change can undo a committed one. Look there first, then review the whole section: git diff $BASE_OID HEAD"
    fi
    PROMPT="$PROMPT

$PIN_LENSES"
fi
STAMP="$(date +%Y%m%d-%H%M%S)"
# The transcript itself is created before the reviewer starts: an existing
# directory this process cannot write would otherwise be found only by tee,
# with the review already running and nowhere to keep it.
if mkdir -p "$REVIEWS_DIR" 2>/dev/null \
        && _probe="$(cd "$REVIEWS_DIR" && pwd)/${STAMP}-$$-${KIND}-direct.out" \
        && : 2>/dev/null > "$_probe"; then
    # The process id is in the name: two passes of one kind started in the same
    # second must not share a transcript, or one is checked against the other's verdict.
    LOG_FILE="$(cd "$REVIEWS_DIR" && pwd)/${STAMP}-$$-${KIND}-direct.out"
else
    LOG_FILE=""
fi
if [ -n "$LOG_FILE" ]; then
    set -o pipefail
    # The candidate is identified BEFORE the reviewer runs. Read afterwards, a
    # commit made by another caller during the review would be named as the
    # one reviewed, and the commit gate would bind the pass to it.
    LAUNCH_ID="$(candidate_id)" || {
        echo "[$SELF_NAME] BLOCK -- the working tree could not be read at launch (reason above)" >&2; exit 2; }
    read -r TREE_HEAD TREE_INDEX TREE_WORK <<< "$LAUNCH_ID"
    # A detached job from the broker carries the candidate the broker registered: one that moved before this
    # wrapper started is not the one its launch row names.
    if [ -n "${CODEX_EXPECT_CANDIDATE:-}" ] && [ "$CODEX_EXPECT_CANDIDATE" != "$LAUNCH_ID" ]; then
        echo "[$SELF_NAME] BLOCK -- the candidate changed between the broker's launch and this $KIND pass (HEAD, the index or the working tree moved); dispatch it again" >&2
        exit 2
    fi
    if [ -n "${CAND_HEAD:-}" ] && { [ "$TREE_HEAD" != "$CAND_HEAD" ] || [ "$TREE_WORK" != "$CAND_WORK" ]; }; then
        echo "[$SELF_NAME] BLOCK -- the candidate changed while the $KIND pass was prepared (HEAD ${CAND_HEAD:0:10} -> ${TREE_HEAD:0:10}, or the working tree moved); dispatch it again" >&2
        exit 2
    fi
    # The receipt's ts is the LAUNCH time, as the broker's row is: the outage
    # check selects attempts by when they started.
    LAUNCH_TS="$(date +%s)"
    # Where the transcript goes (for the reader). The recorder's proof that a
    # review started is the receipt row written below, never printed text.
    echo "[codex-dispatch] transcript: $LOG_FILE" >&2
    node "$COMPANION" adversarial-review -- "$PROMPT" 2>&1 | tee "$LOG_FILE"
    rc=$?
    if [ "$PIN_KIND" = "1" ] && [ "$rc" = "0" ]; then
        NOW_ID="$(candidate_id)" || NOW_ID="unreadable"
        if [ "$NOW_ID" != "$LAUNCH_ID" ]; then
            rc=3
            echo "[$SELF_NAME] INCOMPLETE -- the candidate changed while it was being reviewed (HEAD ${TREE_HEAD:0:10} -> ${NOW_ID:0:10}, or the index or the working tree moved)" \
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
        PIN_MISSING="$(python3 "$PIN_TOOL" --verdicts "$KIND" --transcript "$LOG_FILE" --paths-file0 "$PATHS_FILE")"
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
    PROMPT_SHA="$CALLER_PROMPT_SHA"
    # Written by a JSON serializer: a path or TODO name holding a quote, a
    # backslash or a newline must not produce a line no reader can parse.
    python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/overnight/manifest-line.py" --append "$REVIEWS_DIR/manifest.jsonl" \
        "int:ts=$LAUNCH_TS" "kind=$KIND" "todo=$TODO_PATH" "opt:int:section=$SECTION" "jobId=direct:$STAMP-$$" \
        "logFile=$LOG_FILE" "prompt_sha256=$PROMPT_SHA" "head=$TREE_HEAD" "index_tree=$TREE_INDEX" \
        "worktree_sha256=$TREE_WORK" "route=codex-dispatch.sh" "int:rc=$rc" "bool:pass_checked=$PASS_CHECKED" \
        "highest=$PASS_HIGHEST" "opt:base=${BASE_OID:-}" "bool:done_file=true" \
        "opt:parent=${CODEX_BROKER_JOB:-}" "opt:parent_log=${CODEX_BROKER_LOG:-}" \
        || { echo "[$SELF_NAME] the receipt could not be written to $REVIEWS_DIR/manifest.jsonl; a pass kind will not be credited" >&2
             # A review with no receipt cannot be credited or found: report it
             # incomplete (rc 3, dispatched again), never as its own success.
             rc=3; }
    # Published LAST, after the receipt: the waiter ends on this file, never on
    # log text (a live review can print a line that looks like the one above),
    # and whoever it releases reads the receipt next.
    # Through a file created here under a fresh name (mktemp, exclusive): nothing
    # that took a shared temporary name, such as a FIFO, can hold the publication.
    # -T: the status is renamed onto that exact path, never into a directory there.
    if ! { done_tmp=$(mktemp "$LOG_FILE.done.XXXXXX") && echo "$rc" > "$done_tmp" \
           && mv -fT "$done_tmp" "$LOG_FILE.done"; }; then
        [ -n "${done_tmp:-}" ] && rm -f "$done_tmp"
        echo "[$SELF_NAME] could not publish $LOG_FILE.done; a waiter on this log will report it stale" >&2
        exit 75
    fi
    exit "$rc"
fi
# No writable reviews directory. A marked review would leave no receipt and no
# done file, so nothing could find or credit it: refuse it before it runs.
if [ "$KIND" != "direct" ]; then
    echo "[$SELF_NAME] BLOCK -- no writable reviews directory ($REVIEWS_DIR) to keep this review's transcript and receipt" >&2
    exit 2
fi
exec node "$COMPANION" adversarial-review -- "$PROMPT"
