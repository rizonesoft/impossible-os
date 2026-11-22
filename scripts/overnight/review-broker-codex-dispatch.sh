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
# logFile IN-SESSION until <logFile>.done appears (blocking sleep loop).
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
    # shellcheck source=scripts/lib/review-prompt.sh
    . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/review-prompt.sh"
    review_prompt_section_of "$1"
}
if [[ "${1:-}" == "--section-of" ]]; then
    [[ $# -eq 2 ]] || { echo "usage: $0 --section-of '<first prompt line>'" >&2; exit 2; }
    _broker_section_of "$2"
    echo
    exit 0
fi

if [[ $# -ne 1 ]]; then
    echo "[review-broker] BLOCK -- expected exactly one prompt argument. Usage: $0 '<[review-kind: X] todo-path body>'" >&2
    exit 2
fi
PROMPT="$1"
# Every value placed in the detached command is shell-quoted by printf %q. A
# path or a ref may hold an apostrophe (refs/heads/owner's-fix is a valid ref),
# which inside hand-written single quotes ends the quoting.
q() { printf '%q' "$1"; }
LIMITER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/review-runtime-limit.sh"
# A caller's hard limit (CODEX_REVIEW_MAX_SECS) is one absolute deadline, taken at entry: the outage check, the
# candidate read, the service manager's submission and the detached job each run under review-runtime-limit.sh
# until it, so no step before the review is given back.
# The steps end a reserve before it (a tenth of the limit, a tenth of a second to a second), which is kept for
# publishing a result the broker owns, so even that is done by the deadline.
LIMIT_END=""
LIMIT_FINAL=""
if [[ -n "${CODEX_REVIEW_MAX_SECS:-}" ]]; then
  [[ "$CODEX_REVIEW_MAX_SECS" =~ ^[1-9][0-9]*$ ]] || {
    echo "[review-broker] BLOCK -- CODEX_REVIEW_MAX_SECS must be a whole number of seconds" >&2; exit 2; }
  read -r LIMIT_UP _ < /proc/uptime
  LIMIT_FINAL=$(( ${LIMIT_UP%.*} * 100 + 10#${LIMIT_UP#*.} + CODEX_REVIEW_MAX_SECS * 100 ))
  RESERVE=$(( CODEX_REVIEW_MAX_SECS * 10 ))
  (( RESERVE < 10 )) && RESERVE=10
  (( RESERVE > 100 )) && RESERVE=100
  LIMIT_END=$(( LIMIT_FINAL - RESERVE ))
fi
# One command string, under the caller's limit when there is one (124: the limit ended it).
limited() {
  if [[ -n "$LIMIT_END" ]]; then bash "$LIMITER" --until-cs "$LIMIT_END" "$1"; else bash -c "$1"; fi
}
# Publishing a result the broker owns: the same, with the reserve.
publish() {
  if [[ -n "$LIMIT_FINAL" ]]; then bash "$LIMITER" --until-cs "$LIMIT_FINAL" "$1"; else bash -c "$1"; fi
}
# Prompt parsing shared by the three dispatchers, in memory (no pipeline).
# shellcheck source=scripts/lib/review-prompt.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../lib/review-prompt.sh"
FIRST_LINE="$(review_prompt_first_line "$PROMPT")"
if ! [[ "$FIRST_LINE" =~ ^\[review-kind:[[:space:]]*([a-zA-Z-]+)\][[:space:]]+todo/.+\.md([[:space:]]|$) ]]; then
    echo "[review-broker] BLOCK -- first nonblank prompt line must be '[review-kind: <kind>] <todo-path> ...'" >&2
    exit 2
fi
SELF_NAME="review-broker"
# The prompt is passed after `--`. Given ONE argument, codex-companion.mjs re-splits
# it on whitespace (normalizeArgv -> lib/args.mjs splitRawArgumentString) and reads
# any word starting with `-` as its own option: `--base /` killed a review,
# `--scope and` ran one against the wrong scope, and on 2026-11-12 `git -C after`
# became --cwd after and every pass died "git is not installed". After `--` the
# prompt is one positional, kept exactly as written.

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
    _OUTAGE_WHY="$(limited "python3 $(q "$(dirname "$0")/codex-outage-check.py") --manifest $(q "${BROKER_MANIFEST:-.claude/overnight/reviews/manifest.jsonl}")")" || _OUTAGE_RC=$?
    if [[ $_OUTAGE_RC -eq 124 && -n "$LIMIT_END" ]]; then
        echo "[$SELF_NAME] BLOCK -- the outage check took the whole CODEX_REVIEW_MAX_SECS; nothing was launched" >&2
        exit 2
    fi
    if [[ $_OUTAGE_RC -eq 75 ]]; then
        {
            echo "[$SELF_NAME] BLOCK -- REFUSED, the Codex backend looks DOWN: $_OUTAGE_WHY"
            echo
            echo "Do not retry. Defer this review (park the section with the outage as its"
            echo "blocker) and continue with work that needs no Codex. The gate clears by itself"
            echo "once a leg succeeds or 20 minutes pass since those failures; to probe the"
            echo "backend deliberately, re-run with CODEX_OUTAGE_OVERRIDE=1."
        } >&2
        exit 75
    fi
fi
TODO_PATH="$(review_prompt_todo_path "$FIRST_LINE")"
# Section attribution (v14 close-out, 2026-08-16): the envelope was FILE-scoped
# only, so a section's review wave silently ingested an earlier section's legs
# from the same TODO. The prompt already names the section; record it so
# review-envelope.py --section can filter. Separator set matches the
# codex_review_completed.py recogniser (space, dash, underscore, colon).
SECTION="$(_broker_section_of "$FIRST_LINE")"

COMPANION="${CODEX_COMPANION_PATH:-$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs}"
[[ -f "$COMPANION" ]] || { echo "[review-broker] BLOCK -- codex-companion.mjs not found at $COMPANION" >&2; exit 1; }
export CODEX_REVIEWER_DISPATCH=1

MANIFEST_DIR=".claude/overnight/reviews"     # made with the prompt below, under the deadline
MANIFEST="$MANIFEST_DIR/manifest.jsonl"
STAMP="$(date +%Y%m%d-%H%M%S)"
# The process id is in the name: two reviews of one kind started in the same
# second (two sections, say) must not write one transcript.
LOG_FILE="$PWD/$MANIFEST_DIR/${STAMP}-$$-${KIND}.out"
UNIT="codex-rev-${KIND}-${STAMP}-$$"
# The launch row's HEAD, index digest and working-tree fingerprint, read together before anything is written for
# the job: the same reader the dispatcher checks against (scripts/review_candidate.py), leaving out the reviews
# directory the detached wrapper is given below, so both digests cover one scope; a commit or an index change
# during the read refuses, so the three name one candidate.
READ_CANDIDATE="python3 $(q "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/review_candidate.py") launch --exclude $(q "$PWD/$MANIFEST_DIR")"
READ_RC=0
TREE_LAUNCH="$(limited "$READ_CANDIDATE")" || READ_RC=$?
if [[ "$READ_RC" == 124 ]]; then
  echo "[review-broker] BLOCK -- reading the candidate took the whole CODEX_REVIEW_MAX_SECS; nothing was launched" >&2; exit 2
elif [[ "$READ_RC" != 0 ]]; then
  echo "[review-broker] BLOCK -- the working tree could not be read (reason above); nothing was launched" >&2; exit 2
fi
read -r TREE_HEAD TREE_INDEX TREE_WORK <<< "$TREE_LAUNCH"

# CGROUP SURVIVAL IS LOAD-BEARING (live incident 2026-07-11): a review
# spawned inside the headless session's systemd service dies with the
# session -- when the session ENDS (a verified rollover, a crash, or a
# usage-limit exit) systemd's KillMode=control-group reaps every process
# left in the service's cgroup, killing an in-flight review with it. A
# review in its OWN transient scope survives the session's death, so the
# NEXT session can still read a completed artifact. systemd-run --collect
# gives it that scope; <logFile>.done (the exit status) is what the in-session
# poll (and any recovering next session) watches for.
# In the reviews directory, which the launch read above left out: anywhere else (a TMPDIR inside the
# checkout) the file would change the tree the launch row names.
# The directory, the hash and then the file, made under the caller's deadline: a stalled filesystem ends with
# the limit. The name is this broker's own (its process id, stamp and kind), created exclusively, so whoever
# ends the attempt knows what to remove; a failure removes it with the reserve.
export PROMPT
PROMPT_FILE="$PWD/$MANIFEST_DIR/.prompt.${STAMP}-$$-${KIND}"
PREP_RC=0
PREP="$(limited "mkdir -p $(q "$MANIFEST_DIR") && s=\$(printf '%s' \"\$PROMPT\" | sha256sum) && ( set -C; printf '%s' \"\$PROMPT\" > $(q "$PROMPT_FILE") ) && printf '%s' \"\${s%% *}\"")" || PREP_RC=$?
if [[ "$PREP_RC" != 0 || ! "$PREP" =~ ^[0-9a-f]{64}$ ]]; then
  publish "rm -f $(q "$PROMPT_FILE")" || echo "review-broker: $PROMPT_FILE could not be removed in time" >&2
  if [[ "$PREP_RC" == 124 && -n "$LIMIT_END" ]]; then
    echo "review-broker: writing the prompt took the whole CODEX_REVIEW_MAX_SECS; nothing was launched" >&2
  else
    echo "review-broker: the prompt could not be written to $MANIFEST_DIR (exit $PREP_RC); nothing was launched" >&2
  fi
  exit 2
fi
PROMPT_SHA="$PREP"
if [[ "$PASS_KIND" == "1" ]]; then
  # The wrapper's own messages (a refusal, an incomplete pass) go to the log too.
  # The detached job gets a fresh environment, so everything the wrapper reads is named here.
  # CODEX_EXPECT_CANDIDATE: the candidate the launch row below names; the wrapper refuses another.
  RUN="CODEX_BROKER_JOB=$(q "detached:$UNIT") CODEX_BROKER_LOG=$(q "$LOG_FILE") CODEX_EXPECT_CANDIDATE=$(q "$TREE_HEAD $TREE_INDEX $TREE_WORK") REVIEW_BASE=$(q "${REVIEW_BASE:-}") CODEX_REVIEWS_DIR=$(q "$PWD/$MANIFEST_DIR") CODEX_COMPANION_PATH=$(q "$COMPANION") bash $(q "$PASS_WRAPPER") \"\$(cat $(q "$PROMPT_FILE"))\""
else
  RUN="node $(q "$COMPANION") adversarial-review -- \"\$(cat $(q "$PROMPT_FILE"))\""
fi
# A hard runtime limit (CODEX_REVIEW_MAX_SECS) for a caller that must finish:
# review-runtime-limit.sh runs the review in its own process group and ends the
# WHOLE group within the limit (TERM, then KILL, the grace inside the budget),
# before the job publishes its status (124). No process of the attempt can
# outlive it or write to its log afterwards.
if [[ -n "$LIMIT_END" ]]; then
  RUN="bash $(q "$LIMITER") --until-cs $LIMIT_END $(q "$RUN")"
fi
Q_LOG="$(q "$LOG_FILE")"; Q_PROMPT="$(q "$PROMPT_FILE")"

# REGISTERED BEFORE IT IS LAUNCHED. The gate reads the newest manifest line for
# a pass. Written after the launch, this line could land after a fast job's
# finished receipt and hide it. Which tree the review examines is recorded at
# the same moment (v16 carry, closed 2026-08-29: HEAD, a digest of the index (the field keeps its index_tree name) and a
# working-tree digest; all three are advisory strings).
DETACH="detached:$UNIT"
# Written by a JSON serializer, as the receipt is: a path holding a quote, a
# backslash or a newline must not leave a line no reader can parse.
MANIFEST_LINE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/manifest-line.py"
# ONE OWNER FOR A JOB'S RESULT. The log file is created exclusively (noclobber) by whichever comes first: the
# detached job, at its start, before it reads the prompt or runs anything; or the broker, when the launch failed
# or was cut short after the job's name was given out. The other leaves the result alone, so a unit that starts
# late never writes over a published result, and a launch cut short still ends with exactly one.
# Published with the reserve, so the caller is answered by its deadline even on a stalled filesystem.
finalize_unlaunched() {   # $1: the status to publish when the broker owns the result
  publish "( set -C; : > $Q_LOG ) 2>/dev/null || exit 0; echo 'Turn completed (rc=$1)' >> $Q_LOG; echo $1 > $Q_LOG.done.tmp && mv -f $Q_LOG.done.tmp $Q_LOG.done; rm -f $Q_PROMPT" \
    || echo "review-broker: the result ($1) could not be published by the deadline" >&2
}
# From here until the service manager has the job, the broker owns its result: a cancellation of the broker
# publishes it (128 + the signal), so a registered attempt never waits for a result no one will write.
cancelled_launch() { trap - TERM INT HUP; finalize_unlaunched "$1"; exit "$1"; }
trap 'cancelled_launch 143' TERM
trap 'cancelled_launch 130' INT
trap 'cancelled_launch 129' HUP
REGISTER="python3 $(q "$MANIFEST_LINE") --append $(q "$MANIFEST")"
for field in "int:ts=$(date +%s)" "kind=$KIND" "todo=$TODO_PATH" "opt:int:section=$SECTION" "jobId=$DETACH" \
    "logFile=$LOG_FILE" "prompt_sha256=$PROMPT_SHA" "head=$TREE_HEAD" "index_tree=$TREE_INDEX" \
    "worktree_sha256=$TREE_WORK" "bool:done_file=true"; do
  REGISTER+=" $(q "$field")"
done
REGISTER_RC=0
limited "$REGISTER" || REGISTER_RC=$?
if [[ "$REGISTER_RC" != 0 ]]; then
  # The row may have been written before the registrar was cut short: the attempt it names is ended here.
  finalize_unlaunched "$REGISTER_RC"
  rm -f "$PROMPT_FILE"
  if [[ "$REGISTER_RC" == 124 && -n "$LIMIT_END" ]]; then
    echo "[review-broker] BLOCK -- registering the dispatch took the whole CODEX_REVIEW_MAX_SECS; nothing was launched" >&2
    exit 2
  fi
  echo "[review-broker] BLOCK -- the dispatch could not be registered in $MANIFEST; nothing was launched" >&2
  exit 1
fi
JOB="( set -C; : > $Q_LOG ) 2>/dev/null || exit 0; $RUN > $Q_LOG 2>&1; rc=\$?; echo \"Turn completed (rc=\$rc)\" >> $Q_LOG; echo \$rc > $Q_LOG.done.tmp && mv -f $Q_LOG.done.tmp $Q_LOG.done; rm -f $Q_PROMPT"
# The broker's answer, made now under the deadline: after the submission only a builtin prints it.
RESPONSE=""
RESPONSE_RC=0
RESPONSE="$(limited "python3 $(q "$MANIFEST_LINE") $(q "kind=$KIND") $(q "jobId=$DETACH") $(q "logFile=$LOG_FILE") $(q "manifest=$MANIFEST")")" || RESPONSE_RC=$?
if [[ "$RESPONSE_RC" != 0 ]]; then
  finalize_unlaunched "$RESPONSE_RC"
  if [[ "$RESPONSE_RC" == 124 && -n "$LIMIT_END" ]]; then
    echo "review-broker: making the answer took the whole CODEX_REVIEW_MAX_SECS; nothing was launched" >&2
  else
    echo "review-broker: the answer could not be made (exit $RESPONSE_RC); nothing was launched" >&2
  fi
  exit 2
fi
# LIFECYCLE-WAIVER: detaches a REVIEW into its own scope (the reasons are above PROMPT_FILE); it does not relaunch or watch the session.
SUBMIT="systemd-run --user --collect $(q "--unit=$UNIT") $(q "--setenv=CODEX_REVIEWER_DISPATCH=1") $(q "--setenv=HOME=$HOME") $(q "--setenv=PATH=$PATH") --same-dir /bin/bash -c $(q "$JOB") >/dev/null 2>&1"
SUBMIT_RC=0
limited "$SUBMIT" || SUBMIT_RC=$?
if [[ "$SUBMIT_RC" == 0 ]]; then
  trap - TERM INT HUP                    # the unit owns the result now
elif [[ "$SUBMIT_RC" -ge 128 ]]; then
  # Cancelled (the limiter's own 128 + signal), or the service manager ended by a signal: whether the unit
  # was created is unknown, so no second copy is started. The result goes to its one owner, as below.
  trap - TERM INT HUP
  finalize_unlaunched "$SUBMIT_RC"
  echo "review-broker: the submission was cancelled (exit $SUBMIT_RC); no fallback was started" >&2
  exit "$SUBMIT_RC"
elif [[ "$SUBMIT_RC" == 124 && -n "$LIMIT_END" ]]; then
  # The limit ended the submission itself. Whether the unit was created is unknown, so no second copy is
  # started and the service manager is not asked again (that wait would outlast the deadline). The result
  # goes to its one owner: a unit that already started keeps it; otherwise it is recorded here as 124, and a
  # unit that starts later finds the log taken and exits before it reads or writes anything.
  trap - TERM INT HUP
  finalize_unlaunched 124
else
  # Non-systemd fallback: setsid detach (survives plain shells; NOT a
  # systemd cgroup -- callers inside a service should have systemd-run).
  # LIFECYCLE-WAIVER: detaches a REVIEW on non-systemd hosts, not the session.
  setsid bash -c "$JOB" < /dev/null >/dev/null 2>&1 &
  trap - TERM INT HUP
fi

printf '%s\n' "$RESPONSE"
