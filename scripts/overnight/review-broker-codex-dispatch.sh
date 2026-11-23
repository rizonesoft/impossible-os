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

# THE SUPERVISOR FIRST. A dispatch (not the --section-of helper above) starts review_runtime_limit.py before
# anything else: no file is read or written, no diagnostic is printed and no helper is sourced until every step
# runs as its child, under the one deadline CODEX_REVIEW_MAX_SECS gives (taken here, at entry) and its one
# cancellation. The supervisor learns the result's names when the inner run says which kind it is (`kind`),
# before the inner run writes anything; the prompt reaches the inner run in memory. What is wrong with the call
# (the argument count, a malformed limit) is reported by the inner run, supervised: a malformed limit is given
# ten seconds to say so.
if [[ "${1:-}" != "--section-of" && -z "${REVIEW_BROKER_INNER:-}" ]]; then
  printf -v BROKER_STAMP '%(%Y%m%d-%H%M%S)T' -1
  case "${BASH_SOURCE[0]}" in */*) BROKER_HERE="${BASH_SOURCE[0]%/*}" ;; *) BROKER_HERE="." ;; esac
  REVIEW_BROKER_REFUSE=""
  LIMIT_END=""
  LIMIT_FINAL=""
  read -r LIMIT_UP _ < /proc/uptime
  LIMIT_NOW=$(( ${LIMIT_UP%.*} * 100 + 10#${LIMIT_UP#*.} ))
  if [[ -n "${CODEX_REVIEW_MAX_SECS:-}" ]]; then
    if [[ "$CODEX_REVIEW_MAX_SECS" =~ ^[1-9][0-9]{0,8}$ ]]; then
      LIMIT_FINAL=$(( LIMIT_NOW + CODEX_REVIEW_MAX_SECS * 100 ))
      # The steps end a reserve before the deadline (a tenth of the limit, a tenth of a second to a second),
      # kept for publishing a result the broker owns, so even that is done by the deadline.
      RESERVE=$(( CODEX_REVIEW_MAX_SECS * 10 ))
      (( RESERVE < 10 )) && RESERVE=10
      (( RESERVE > 100 )) && RESERVE=100
      LIMIT_END=$(( LIMIT_FINAL - RESERVE ))
    else
      REVIEW_BROKER_REFUSE="CODEX_REVIEW_MAX_SECS must be a whole number of seconds"
      LIMIT_FINAL=$(( LIMIT_NOW + 1000 ))
      LIMIT_END=$(( LIMIT_FINAL - 100 ))
    fi
  fi
  export REVIEW_BROKER_INNER=1 BROKER_STAMP BROKER_ID="$$" LIMIT_END LIMIT_FINAL REVIEW_BROKER_REFUSE
  export REVIEW_BROKER_ARGC="$#"
  BROKER_DIR="$PWD/.claude/overnight/reviews"
  BROKER_SELF="$(printf '%q' "${BASH_SOURCE[0]}")"
  if [[ -n "$LIMIT_END" ]]; then
    exec python3 "$BROKER_HERE/review_runtime_limit.py" --broker "$BROKER_DIR/${BROKER_STAMP}-$$-{kind}.out" \
      "$BROKER_DIR/.prompt.${BROKER_STAMP}-$$-{kind}" --broker-prompt "${1-}" --publish-by "$LIMIT_FINAL" \
      --until-cs "$LIMIT_END" "bash $BROKER_SELF"
  fi
  exec python3 "$BROKER_HERE/review_runtime_limit.py" --broker "$BROKER_DIR/${BROKER_STAMP}-$$-{kind}.out" \
    "$BROKER_DIR/.prompt.${BROKER_STAMP}-$$-{kind}" --broker-prompt "${1-}" --no-deadline "bash $BROKER_SELF"
fi
LIMIT_END="${LIMIT_END:-}"
LIMIT_FINAL="${LIMIT_FINAL:-}"
if [[ -n "${REVIEW_BROKER_REFUSE:-}" ]]; then
    echo "[review-broker] BLOCK -- $REVIEW_BROKER_REFUSE" >&2
    exit 2
fi
if [[ -n "${REVIEW_BROKER_INNER:-}" && "${REVIEW_BROKER_ARGC:-1}" != 1 ]]; then
    echo "[review-broker] BLOCK -- expected exactly one prompt argument. Usage: $0 '<[review-kind: X] todo-path body>'" >&2
    exit 2
fi

# The inner run under the supervisor reads the prompt from an in-memory file the supervisor made from its own
# raw argument (REVIEW_BROKER_PROMPT_FD): no exec string carries it quoted (that doubled it) or as an
# environment string (the same per-string limit as an argument), so the longest prompt a reviewer takes as its
# argument reaches it, trailing newlines included (the `x` keeps command substitution from removing them).
if [[ -n "${REVIEW_BROKER_INNER:-}" && $# -eq 0 && -n "${REVIEW_BROKER_PROMPT_FD:-}" ]]; then
    if ! BROKER_PROMPT_TEXT="$(cat "/dev/fd/$REVIEW_BROKER_PROMPT_FD" && printf x)"; then
        echo "[review-broker] BLOCK -- the dispatch's prompt could not be read from the supervisor" >&2
        exit 2
    fi
    set -- "${BROKER_PROMPT_TEXT%x}"
    unset BROKER_PROMPT_TEXT
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
LIMITER="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/review_runtime_limit.py"
PASS_DIR_LIB="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/lib/pass-dir.sh"
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
MANIFEST_DIR=".claude/overnight/reviews"
# What the supervisor is told: the step under way (named when the limit ends it), that the result is now the
# broker's to publish, the answer, and a job to start once the attempt is torn down.
tell() { printf '%s\n' "$*" >&"$REVIEW_BROKER_HANDOFF_FD"; }
# The result's names follow from the kind: the supervisor learns it before anything is written.
tell "kind $KIND"
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
    tell "step the outage check"
    _OUTAGE_WHY="$(python3 "$(dirname "$0")/codex-outage-check.py" --manifest "${BROKER_MANIFEST:-.claude/overnight/reviews/manifest.jsonl}")" || _OUTAGE_RC=$?
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

COMPANION="${CODEX_COMPANION_PATH:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/tools/vendor/codex-companion/scripts/codex-companion.mjs}"
[[ -f "$COMPANION" ]] || { echo "[review-broker] BLOCK -- codex-companion.mjs not found at $COMPANION" >&2; exit 1; }
export CODEX_REVIEWER_DISPATCH=1

MANIFEST="$MANIFEST_DIR/manifest.jsonl"
STAMP="$BROKER_STAMP"
# The broker's process id is in the name: two reviews of one kind started in the same
# second (two sections, say) must not write one transcript.
LOG_FILE="$PWD/$MANIFEST_DIR/${STAMP}-${BROKER_ID}-${KIND}.out"
UNIT="codex-rev-${KIND}-${STAMP}-${BROKER_ID}"
# The launch row's HEAD, index digest and working-tree fingerprint, read together before anything is written for
# the job: the same reader the dispatcher checks against (scripts/review_candidate.py), leaving out the reviews
# directory the detached wrapper is given below, so both digests cover one scope; a commit or an index change
# during the read refuses, so the three name one candidate.
READ_RC=0
tell "step reading the candidate"
TREE_LAUNCH="$(python3 "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/review_candidate.py" launch --exclude "$PWD/$MANIFEST_DIR")" || READ_RC=$?
if [[ "$READ_RC" != 0 ]]; then
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
# The directory, the hash and then the file. The name is this dispatch's own (its process id, stamp and kind),
# created exclusively; the supervisor removes it when the dispatch ends before a unit takes it.
PROMPT_FILE="$PWD/$MANIFEST_DIR/.prompt.${STAMP}-${BROKER_ID}-${KIND}"
tell "step writing the prompt"
PREP=""
if mkdir -p "$MANIFEST_DIR" && PREP="$(printf '%s' "$PROMPT" | sha256sum)" \
    && ( set -C; printf '%s' "$PROMPT" > "$PROMPT_FILE" ); then
  PREP="${PREP%% *}"
fi
if [[ ! "$PREP" =~ ^[0-9a-f]{64}$ ]]; then
  echo "review-broker: the prompt could not be written to $MANIFEST_DIR; nothing was launched" >&2
  exit 2
fi
PROMPT_SHA="$PREP"
PROMPT_LEN="$(LC_ALL=C; printf '%s' "${#PROMPT}")"
if [[ "$PASS_KIND" == "1" ]]; then
  # The wrapper's own messages (a refusal, an incomplete pass) go to the log too.
  # The detached job gets a fresh environment, so everything the wrapper reads is named here.
  # CODEX_EXPECT_CANDIDATE: the candidate the launch row below names; the wrapper refuses another.
  RUN="_review_prompt=\"\$(cat /dev/fd/\$REVIEW_PROMPT_FD && printf x)\" && CODEX_BROKER_JOB=$(q "detached:$UNIT") CODEX_BROKER_LOG=$(q "$LOG_FILE") CODEX_EXPECT_CANDIDATE=$(q "$TREE_HEAD $TREE_INDEX $TREE_WORK") REVIEW_BASE=$(q "${REVIEW_BASE:-}") CODEX_REVIEWS_DIR=$(q "$PWD/$MANIFEST_DIR") CODEX_COMPANION_PATH=$(q "$COMPANION") CODEX_COMPANION_ACCEPTS_EFFORT=$(q "${CODEX_COMPANION_ACCEPTS_EFFORT:-}") CODEX_REVIEW_MAX_SECS=$(q "${CODEX_REVIEW_MAX_SECS:-}") bash $(q "$PASS_WRAPPER") \"\${_review_prompt%x}\""
else
  # An older kind runs the companion here, but in its own data directory with no inherited broker endpoint, as
  # every pinned pass does (codex-dispatch.sh run_companion): passes in one checkout never share the companion's
  # unlocked job state or its broker. The job's supervisor reaps whatever it starts; the directory goes after
  # (or, after a SIGKILL that runs no trap, with the next pass: it is named after this shell, .cp.<pid>.*). With
  # no CODEX_REVIEW_MAX_SECS the job has no deadline of its own, so the companion gets the dispatcher's default.
  RUN="_review_prompt=\"\$(cat /dev/fd/\$REVIEW_PROMPT_FD && printf x)\" && . $(q "$PASS_DIR_LIB") && _d=\"\$(new_pass_dir $(q "$PWD/$MANIFEST_DIR"))\" && trap 'rm -rf -- \"\$_d\"; exit 143' TERM INT HUP && { env -u CODEX_COMPANION_APP_SERVER_ENDPOINT -u CODEX_COMPANION_APP_SERVER_PID_FILE -u CODEX_COMPANION_APP_SERVER_LOG_FILE CLAUDE_PLUGIN_DATA=\"\$_d\" TMPDIR=\"\$_d\" timeout --foreground -k 10 $(q "${CODEX_REVIEW_MAX_SECS:-1800}") node $(q "$COMPANION") adversarial-review -- \"\${_review_prompt%x}\"; _rc=\$?; rm -rf -- \"\$_d\"; exit \$_rc; }"
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
# ONE OWNER FOR A JOB'S RESULT. The log file is created exclusively by whichever comes first: the detached job,
# at its start, before it reads the prompt or runs anything; or the broker's supervisor, when the dispatch failed
# or was ended after the job's name was given out. The other leaves the result alone, so a unit that starts late
# never writes over a published result, and a launch cut short still ends with exactly one. Both publish through
# review_result.py, which reports a publication that failed (the job then exits 1).
tell "owns"
tell "step registering the dispatch"
REGISTER=(python3 "$MANIFEST_LINE" --append "$MANIFEST")
for field in "int:ts=$(printf '%(%s)T' -1)" "kind=$KIND" "todo=$TODO_PATH" "opt:int:section=$SECTION" "jobId=$DETACH" \
    "logFile=$LOG_FILE" "prompt_sha256=$PROMPT_SHA" "head=$TREE_HEAD" "index_tree=$TREE_INDEX" \
    "worktree_sha256=$TREE_WORK" "bool:done_file=true"; do
  REGISTER+=("$field")
done
if ! "${REGISTER[@]}"; then
  echo "[review-broker] BLOCK -- the dispatch could not be registered in $MANIFEST; nothing was launched" >&2
  exit 1
fi
# The job is ONE supervisor (review_runtime_limit.py --job): it claims the result, runs the review with its
# output appended to the log (opened by the supervisor as a regular file, never reopened by the command), ends the WHOLE review by the steps' deadline (TERM, then KILL, the grace inside
# the budget) when CODEX_REVIEW_MAX_SECS gives one, and publishes its status by the final deadline. One, not
# one nested in another: an outer limiter's grace would end the inner one before it could publish.
if [[ -n "$LIMIT_END" ]]; then
  JOB_LIMIT="--publish-by $LIMIT_FINAL --until-cs $LIMIT_END"
else
  JOB_LIMIT="--no-deadline"
fi
JOB="python3 $(q "$LIMITER") --job $Q_LOG $Q_PROMPT --prompt-sha $PROMPT_SHA --prompt-len $PROMPT_LEN $JOB_LIMIT $(q "{ $RUN; }")"
# The broker's answer, made before the submission.
tell "step making the answer"
RESPONSE_RC=0
RESPONSE="$(python3 "$MANIFEST_LINE" "kind=$KIND" "jobId=$DETACH" "logFile=$LOG_FILE" "manifest=$MANIFEST")" || RESPONSE_RC=$?
if [[ "$RESPONSE_RC" != 0 ]]; then
  echo "review-broker: the answer could not be made (exit $RESPONSE_RC); nothing was launched" >&2
  exit 2
fi
tell "response $RESPONSE"
tell "step the submission"
SUBMIT_RC=0
# LIFECYCLE-WAIVER: detaches a REVIEW into its own scope (the reasons are above PROMPT_FILE); it does not relaunch or watch the session.
systemd-run --user --collect "--unit=$UNIT" "--setenv=CODEX_REVIEWER_DISPATCH=1" "--setenv=HOME=$HOME" \
  "--setenv=PATH=$PATH" --same-dir /bin/bash -c "$JOB" >/dev/null 2>&1 || SUBMIT_RC=$?
if [[ "$SUBMIT_RC" -ge 128 ]]; then
  # The service manager ended by a signal: whether the unit was created is unknown, so no second copy is
  # started. The supervisor records the result with its one owner.
  echo "review-broker: the submission was cancelled (exit $SUBMIT_RC); no fallback was started" >&2
  exit "$SUBMIT_RC"
elif [[ "$SUBMIT_RC" != 0 ]]; then
  # Non-systemd fallback: a session of its own (survives plain shells; NOT a systemd cgroup -- callers inside
  # a service should have systemd-run), started by the supervisor once this dispatch is torn down.
  tell "job $JOB"
fi
tell "step answered"                         # the supervisor delivers the answer and checks the write
