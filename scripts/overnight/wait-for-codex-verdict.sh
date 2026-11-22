#!/usr/bin/env bash
# wait-for-codex-verdict.sh [--max SECONDS] [--stale-secs SECONDS | --no-stale] <logfile> [<logfile>...]
#
# Wait until EVERY given DETACHED Codex review log has its <log>.done sidecar
# (written by review-broker-codex-dispatch.sh / codex-dispatch.sh after the review
# exits), then print each verdict line + the last log's tail.
# Pass several logs to wait on a whole multi-kind round in ONE call (the
# combined-completion condition the wait-discipline requires).
#
# WHY THIS EXISTS (B1): the hand-rolled `for i in $(seq 1 N); do ...; sleep S;
# done` polls are 540-900s, but the Bash TOOL kills any call at its ~120s default
# unless the caller remembers a `timeout:` arg -- so the FIRST poll of nearly
# every session died at 2m ("Exit code 143") and had to be re-issued. This
# wrapper bounds ITSELF (default 100s) to finish under that ceiling, so a BARE
# call is never killed: it returns promptly with a clear status and the caller
# re-invokes if any review has not finished. No `timeout:` arg to remember, no
# per-session re-learning, no wasted killed call.
#
# WHY THE STALE SIGNAL (B2): the completion sentinel is BINARY (present / absent),
# so a slow-but-ALIVE review (Codex still streaming `rg`/analysis output) was
# indistinguishable from a HUNG one. In the 2026-07-13 watched canary that gap
# made the runner abandon + re-dispatch a genuinely-live 14-min review, wasting
# it. Fix: on each still-running return, report per-log byte SIZE + how long since
# it last GREW (activity), tracked across re-invocations in a sidecar. A log that
# has not grown for --stale-secs (default 300s = 5 min) is flagged STALE (exit 4)
# so the caller re-dispatches ONLY a genuinely-silent review and keeps WAITING on
# one still producing output.
#
# Exit status:
#   0  -- ALL logs have <log>.done; each verdict + the last tail printed.
#   3  -- bounded wait elapsed, >=1 review still running AND producing output;
#         re-invoke to keep going (nothing is lost, reviews run detached).
#   5  -- bounded wait elapsed AND >=1 pending log path DOES NOT EXIST after
#         --stale-secs, and no existing log is stale. That is NOT a hung
#         review (v17 close-out, 2026-08-29: a path reconstructed from the
#         jobId instead of the broker's logFile was reported as "0 bytes,
#         likely hung; re-dispatch", and the re-dispatch cost a duplicate
#         Codex run). Check the path against the broker's `logFile` /
#         .claude/overnight/reviews/manifest.jsonl before doing anything.
#   4  -- bounded wait elapsed AND >=1 pending log has not grown for --stale-secs;
#         that review is likely hung -- consider re-dispatching THAT log only.
#         Also exit 4 (UNPUBLISHED, before STALE) for a review whose owner ended and whose recovery could not
#         publish its status (a full filesystem): it has ended and will never publish, so re-dispatch THAT log.
#   2  -- usage error.
# Zero model tokens while it sleeps (one blocking Bash call, idle in `sleep`).
#
# One turn per bounded wait. For a single LONG wait in one turn, raise the bound
# AND pass an explicit Bash timeout, e.g. `--max 540 <log>` with tool
# `timeout: 600000`. Otherwise just re-invoke on exit 3 (each call is < 2 min).
#
# In the unattended run prefer the long form (`--max >= 300` + a matching tool
# timeout). Rationale: every re-invoke is a full model turn re-reading a
# ~350K-token cached prefix; run-20260719-022200 spent 51 poll turns (~18M
# cache-read tokens) on one review convergence. The default 100s bound below is
# the INTERACTIVE-safe shape and stays.
set -u

MAX=100          # default < the Bash tool's ~120s ceiling, so a bare call is safe
POLL=10
STALE=300        # no byte growth for this long => STALE (likely hung), exit 4
NO_STALE=0       # 1: never report STALE (a missing log is still exit 5)
LOGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --max)          [ $# -ge 2 ] || { echo "wait-for-codex-verdict.sh: --max needs a value" >&2; exit 2; }
                        MAX="$2"; shift 2 ;;
        --max=*)        MAX="${1#--max=}"; shift ;;
        --stale-secs)   [ $# -ge 2 ] || { echo "wait-for-codex-verdict.sh: --stale-secs needs a value" >&2; exit 2; }
                        STALE="$2"; shift 2 ;;
        --stale-secs=*) STALE="${1#--stale-secs=}"; shift ;;
        # The caller's review has its own runtime limit (CODEX_REVIEW_MAX_SECS),
        # which ends a hung attempt and publishes its status: silence is then
        # not a signal, and every wait sleeps its full --max until .done.
        --no-stale)     NO_STALE=1; shift ;;
        *)
            # J2a: a BARE number that is not a file is almost always a misused
            # `--max` (`... .out 300` intending `--max 300`); adding it as a
            # phantom log makes the wait never complete. Warn + skip it.
            if printf '%s' "$1" | grep -qE '^[0-9]+$' && [ ! -f "$1" ]; then
                echo "wait-for-codex-verdict.sh: ignoring bare number '$1' (not a log file) -- did you mean '--max $1'?" >&2
            else
                LOGS+=("$1")
            fi
            shift ;;
    esac
done
if [ "${#LOGS[@]}" -lt 1 ] || ! [ "$MAX" -gt 0 ] 2>/dev/null \
   || ! [ "$STALE" -gt 0 ] 2>/dev/null; then
    echo "usage: wait-for-codex-verdict.sh [--max SECONDS] [--stale-secs SECONDS] <logfile> [<logfile>...]" >&2
    exit 2
fi
LAST="${LOGS[$(( ${#LOGS[@]} - 1 ))]}"
ACT_DIR=".claude/state/waiter-first-seen"

# A review ends only when its dispatcher has written <log>.done (the exit
# status, written atomically after the review process exited). Log text never
# ends a wait: the companion prints "[codex] Turn completed." before its
# verdict, and a live review can print a line that looks like any sentinel.
# A result whose owner ended without publishing it (out of memory, a crash) is recovered here: the owner holds
# <log>.owner locked for its life, so review_result.py recover publishes 1 only for an owner that is gone.
# It runs in the background with streams of its own, at most once per log per call, so a stalled filesystem
# delays the recovery, never this wait: the status it publishes is read on a later poll, inside this call's
# deadline. Its own exit status is read too once it ends: a recovery that could not publish (a full
# filesystem) while the owner is still gone leaves a review that has ended and will never publish, which is
# reported UNPUBLISHED (exit 4, re-dispatch that log), never as a review still running.
RESULT_TOOL="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/review_result.py"
declare -A REC_PID=() UNPUBLISHED=()
_log_done() {
    [ -f "$1.done" ] && return 0
    [ -n "${UNPUBLISHED[$1]:-}" ] && return 1
    local pid="${REC_PID[$1]:-}"
    [ "$pid" = ended ] && return 1                  # its one recovery this call has ended
    if [ -n "$pid" ]; then
        # A recovery already started this call: read its status once it has ended (a zombie of this shell
        # until waited on, so a still-running one is a live, non-zombie /proc entry).
        local st=""
        [ -r "/proc/$pid/stat" ] && read -r _ _ st _ < "/proc/$pid/stat" 2>/dev/null
        if [ -z "$st" ] || [ "$st" = Z ]; then
            local rc=0
            wait "$pid" 2>/dev/null || rc=$?
            if [ "$rc" -ne 0 ] && [ ! -f "$1.done" ] && flock -n "$1.owner" true 2>/dev/null; then
                UNPUBLISHED[$1]=1
            fi
            REC_PID[$1]=ended
        fi
        return 1
    fi
    # Only a free owner lock (flock -n, which never waits) is worth a recovery; a live owner holds it.
    if [ -e "$1.owner" ] && [ -e "$1" ] && flock -n "$1.owner" true 2>/dev/null; then
        # LIFECYCLE-WAIVER: publishes one REVIEW attempt's result; it does not relaunch or watch the session.
        python3 "$RESULT_TOOL" recover "$1" </dev/null >/dev/null 2>&1 &
        REC_PID[$1]=$!
    fi
    return 1
}

_all_done() {
    local f
    for f in "${LOGS[@]}"; do
        _log_done "$f" || return 1
    done
    return 0
}

# B2 activity. An existing log's silence is measured from its own mtime (when
# it last grew), so a review silent since launch is STALE as soon as the
# threshold passes, within this call. A path that does not exist yet has its
# first sighting recorded in a sidecar, which survives re-invocations.
mkdir -p "$ACT_DIR" 2>/dev/null || true
_first_seen() {
    local side; side="$ACT_DIR/$(printf '%s' "$1" | md5sum 2>/dev/null | cut -c1-16)"
    # A sighting that cannot be kept reads as the epoch, so a missing log is
    # reported MISSING at once rather than looking newly launched forever.
    local t=""
    [ -f "$side" ] || { date +%s > "$side.tmp.$$" && mv -f "$side.tmp.$$" "$side"; } 2>/dev/null || { echo 0; return; }
    read -r t < "$side" 2>/dev/null || t=""
    case "$t" in ''|*[!0-9]*) echo 0 ;; *) echo "$t" ;; esac
}
_silent_for() {
    local now; now=$(date +%s)
    if [ -f "$1" ]; then
        echo $(( now - $(stat -c %Y "$1" 2>/dev/null || echo "$now") ))
    else
        echo $(( now - $(_first_seen "$1") ))
    fi
}
_any_silent() {
    local f
    for f in "${LOGS[@]}"; do
        _log_done "$f" && continue
        [ -n "${UNPUBLISHED[$f]:-}" ] && return 0      # ended without a status that can ever be published
        [ "$NO_STALE" -eq 1 ] && [ -f "$f" ] && continue
        [ "$(_silent_for "$f")" -ge "$STALE" ] && return 0
    done
    return 1
}
# One deadline for the whole call, read from the shell's clock and taken before any bookkeeping: time spent
# outside sleep (the first sightings, a slow stat, a recovery started, the silence scan) counts against it.
started=$SECONDS
for f in "${LOGS[@]}"; do _first_seen "$f" >/dev/null; done
elapsed=0
while :; do
    # Checked once more when the deadline comes, so a status published during the last sleep is seen.
    if _all_done; then
        for f in "${LOGS[@]}"; do
            echo "DONE: $f -- Turn completed (rc=$(cat "$f.done" 2>/dev/null))"
        done
        echo "--- verdict tail ($LAST) ---"
        tail -n 25 "$LAST"
        # The next step, stated where the verdicts are read (2026-09-28: 85 of 309
        # refused calls in 21 run logs were an edit made right here, before the
        # wave was received).
        echo "NEXT: python3 scripts/overnight/review-envelope.py for the combined wave, then Skill(superpowers:receiving-code-review) BEFORE any Edit, Write or fix."
        exit 0
    fi
    elapsed=$((SECONDS - started))        # the polls above spent time too
    [ "$elapsed" -ge "$MAX" ] && break
    _any_silent && break
    elapsed=$((SECONDS - started))        # and so did the silence scan: the sleep is what is left after it
    [ "$elapsed" -ge "$MAX" ] && break
    dur=$POLL
    [ $((MAX - elapsed)) -lt "$POLL" ] && dur=$((MAX - elapsed))
    [ "$dur" -gt 0 ] && sleep "$dur"
    elapsed=$((SECONDS - started))
done

any_stale=0
any_missing=0
any_unpublished=0
pending=()
for f in "${LOGS[@]}"; do
    _log_done "$f" && continue
    pending+=("$f")
    size=0
    [ -f "$f" ] && size=$(wc -c < "$f" 2>/dev/null | tr -d ' ')
    stalled=$(_silent_for "$f")
    if [ -n "${UNPUBLISHED[$f]:-}" ]; then
        any_unpublished=1
        printf '  UNPUBLISHED: %s (its owner ended and the recovery could not publish its status -- the review has ended; re-dispatch THIS log)\n' "$f"
    elif [ "$stalled" -ge "$STALE" ] && [ ! -f "$f" ]; then
        any_missing=1
        printf '  MISSING: %s (no such file after %ss -- NOT a hung review: verify the path against the broker'"'"'s logFile / .claude/overnight/reviews/manifest.jsonl before re-dispatching anything)\n' \
               "$f" "$stalled"
    elif [ "$stalled" -ge "$STALE" ] && [ "$NO_STALE" -eq 0 ]; then
        any_stale=1
        printf '  STALE: %s (%s bytes, no growth for %ss >= %ss -- likely hung; re-dispatch THIS log, keep waiting on the rest)\n' \
               "$f" "$size" "$stalled" "$STALE"
    else
        printf '  running: %s (%s bytes, last growth %ss ago)\n' "$f" "$size" "$stalled"
    fi
done

if [ "$any_unpublished" -eq 1 ]; then
    echo "UNPUBLISHED after ${elapsed}s -- >=1 review ended with no status that can be published (see above)."
    exit 4
fi
if [ "$any_stale" -eq 1 ]; then
    echo "STALE after ${elapsed}s -- >=1 pending review produced no new output for >= ${STALE}s (see above)."
    exit 4
fi
if [ "$any_missing" -eq 1 ]; then
    echo "MISSING after ${elapsed}s -- >=1 log path never appeared (see above). A missing file is a wrong path or a review that never started, never a hung one."
    exit 5
fi
echo "STILL RUNNING after ${elapsed}s -- pending: ${pending[*]} -- re-invoke to" \
     "keep waiting (reviews run detached; nothing is lost)."
exit 3
