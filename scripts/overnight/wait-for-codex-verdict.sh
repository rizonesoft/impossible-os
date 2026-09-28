#!/usr/bin/env bash
# wait-for-codex-verdict.sh [--max SECONDS] [--stale-secs SECONDS] <logfile> [<logfile>...]
#
# Wait until EVERY given DETACHED Codex review log contains the "Turn completed"
# sentinel (written by review-broker-codex-dispatch.sh / codex-bg-dispatch.sh
# when a review finishes), then print each verdict line + the last log's tail.
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
#   0  -- ALL logs show "Turn completed"; each verdict + the last tail printed.
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
#   2  -- usage error.
# Zero model tokens while it sleeps (one blocking Bash call, idle in `sleep`).
#
# One turn per bounded wait. For a single LONG wait in one turn, raise the bound
# AND pass an explicit Bash timeout, e.g. `--max 540 <log>` with tool
# `timeout: 600000`. Otherwise just re-invoke on exit 3 (each call is < 2 min).
#
# R3 (2026-07-19): in the HEADLESS run the long form is MANDATORY --
# codex_wait_discipline.py BLOCKs a call without `--max >= 300` + a matching
# tool timeout. Rationale: every re-invoke is a full model turn re-reading a
# ~350K-token cached prefix; run-20260719-022200 spent 51 poll turns (~18M
# cache-read tokens) on one review convergence. The default 100s bound below is
# the INTERACTIVE-safe shape and stays.
set -u

MAX=100          # default < the Bash tool's ~120s ceiling, so a bare call is safe
POLL=10
STALE=300        # no byte growth for this long => STALE (likely hung), exit 4
LOGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --max)          MAX="${2:-}"; shift 2 ;;
        --max=*)        MAX="${1#--max=}"; shift ;;
        --stale-secs)   STALE="${2:-}"; shift 2 ;;
        --stale-secs=*) STALE="${1#--stale-secs=}"; shift ;;
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
ACT_DIR=".claude/state/waiter-activity"

_all_done() {
    local f
    for f in "${LOGS[@]}"; do
        [ -f "$f" ] && grep -q "Turn completed" "$f" 2>/dev/null || return 1
    done
    return 0
}

elapsed=0
while [ "$elapsed" -lt "$MAX" ]; do
    if _all_done; then
        for f in "${LOGS[@]}"; do
            echo "DONE: $f -- $(grep 'Turn completed' "$f" | tail -1)"
        done
        echo "--- verdict tail ($LAST) ---"
        tail -n 25 "$LAST"
        # The next step, stated where the verdicts are read (2026-09-28: 85 of 309
        # refused calls in 21 run logs were an edit made right here, before the
        # wave was received).
        echo "NEXT: python3 scripts/overnight/review-envelope.py for the combined wave, then Skill(superpowers:receiving-code-review) BEFORE any Edit, Write or fix."
        exit 0
    fi
    dur=$POLL
    [ $((MAX - elapsed)) -lt "$POLL" ] && dur=$((MAX - elapsed))
    sleep "$dur"
    elapsed=$((elapsed + dur))
done

# B2 activity report: for each still-pending log, track byte size across
# re-invocations in a sidecar and report how long since it last grew.
mkdir -p "$ACT_DIR" 2>/dev/null || true
now=$(date +%s)
any_stale=0
any_missing=0
pending=()
for f in "${LOGS[@]}"; do
    if { [ -f "$f" ] && grep -q "Turn completed" "$f" 2>/dev/null; }; then
        continue
    fi
    pending+=("$f")
    size=0
    [ -f "$f" ] && size=$(wc -c < "$f" 2>/dev/null | tr -d ' ') || size=0
    key=$(printf '%s' "$f" | md5sum 2>/dev/null | cut -c1-16)
    side="$ACT_DIR/$key"
    prev_size=-1
    growth_ts="$now"
    if [ -f "$side" ]; then
        read -r prev_size growth_ts < "$side" 2>/dev/null || { prev_size=-1; growth_ts="$now"; }
    fi
    [ -n "${prev_size:-}" ] || prev_size=-1
    [ -n "${growth_ts:-}" ] || growth_ts="$now"
    if [ "$size" -gt "$prev_size" ] 2>/dev/null; then
        growth_ts="$now"          # grew since last check -> reset the stall clock
    fi
    printf '%s %s\n' "$size" "$growth_ts" > "$side" 2>/dev/null || true
    stalled=$(( now - growth_ts ))
    if [ "$stalled" -ge "$STALE" ] && [ ! -f "$f" ]; then
        any_missing=1
        printf '  MISSING: %s (no such file after %ss -- NOT a hung review: verify the path against the broker'"'"'s logFile / .claude/overnight/reviews/manifest.jsonl before re-dispatching anything)\n' \
               "$f" "$stalled"
    elif [ "$stalled" -ge "$STALE" ]; then
        any_stale=1
        printf '  STALE: %s (%s bytes, no growth for %ss >= %ss -- likely hung; re-dispatch THIS log, keep waiting on the rest)\n' \
               "$f" "$size" "$stalled" "$STALE"
    else
        printf '  running: %s (%s bytes, last growth %ss ago)\n' "$f" "$size" "$stalled"
    fi
done

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
