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

# The --max clock starts here, before anything else runs, in microseconds (EPOCHREALTIME): SECONDS counts
# wall-clock second boundaries, so a call started late in a second lost up to a second of --max. Nothing
# before the observer starts touches the filesystem (no command lookup, no stat), so setup cannot hold it.
# The clock is the kernel's elapsed-time clock (/proc/uptime: CLOCK_BOOTTIME, held in memory, never on
# storage), so a wall-clock step back or forward never lengthens or shortens --max; EPOCHREALTIME only where
# /proc is unreadable, and then for the whole call.
# _now sets NOW_US in this shell (no command substitution, so reading the clock never needs a new process),
# and the first read is the start itself.
if read -r _up _ < /proc/uptime 2>/dev/null && [[ "$_up" =~ ^[0-9]+\.[0-9]{2}$ ]]; then
    START_US=$(( ${_up%.*} * 1000000 + 10#${_up#*.} * 10000 ))
    _now() { local up; read -r up _ < /proc/uptime; NOW_US=$(( ${up%.*} * 1000000 + 10#${up#*.} * 10000 )); }
else
    _now() { local t="$EPOCHREALTIME"; NOW_US=$(( ${t%.*} * 1000000 + 10#${t#*.} )); }
    _now
    START_US=$NOW_US
fi
# The caller's streams, kept for the report: a forked writer reaches them through these descriptors.
exec {OUT_FD}>&1 {ERR_FD}>&2

# Setup messages are kept and written at the end through the same bounded writer as the report (a caller
# whose stderr pipe is full never holds this call), or by `_refuse` for a usage error.
SETUP_ERR=""
# Writes $1 to descriptor $2 by $3 (microseconds, as NOW_US). The write is made by a forked copy of this
# shell, which starts no program (nothing to load from a stalled disk), and this shell waits for its word
# only until the deadline: a write blocked on a full pipe is then ended, never waited for.
_write_until() {
    local done_fd pid left_us
    _now
    left_us=$(( $3 - NOW_US ))
    [ "$left_us" -gt 0 ] || return 0
    exec {done_fd}< <(exec 2>/dev/null; printf '%s' "$1" >&"$2"; echo written)
    pid=$!
    _now
    left_us=$(( $3 - NOW_US ))
    [ "$left_us" -gt 0 ] || left_us=1
    local left
    printf -v left '%d.%06d' $(( left_us / 1000000 )) $(( left_us % 1000000 ))
    IFS= read -r -t "$left" -u "$done_fd" _ \
        || kill -KILL "$pid" 2>/dev/null
    exec {done_fd}<&-
}

_refuse() {
    # Bounded from the start of the call: --max when it is already known and valid, else a second.
    local secs=1
    [[ "${MAX:-}" =~ ^[1-9][0-9]{0,5}$ ]] && secs=$MAX
    _write_until "$1"$'\n' "$ERR_FD" $(( START_US + secs * 1000000 ))
    exit 2
}

MAX=100          # default < the Bash tool's ~120s ceiling, so a bare call is safe
POLL=10
STALE=300        # no byte growth for this long => STALE (likely hung), exit 4
NO_STALE=0       # 1: never report STALE (a missing log is still exit 5)
LOGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --max)          [ $# -ge 2 ] || _refuse "wait-for-codex-verdict.sh: --max needs a value"
                        MAX="$2"; shift 2 ;;
        --max=*)        MAX="${1#--max=}"; shift ;;
        --stale-secs)   [ $# -ge 2 ] || _refuse "wait-for-codex-verdict.sh: --stale-secs needs a value"
                        STALE="$2"; shift 2 ;;
        --stale-secs=*) STALE="${1#--stale-secs=}"; shift ;;
        # The caller's review has its own runtime limit (CODEX_REVIEW_MAX_SECS),
        # which ends a hung attempt and publishes its status: silence is then
        # not a signal, and every wait sleeps its full --max until .done.
        --no-stale)     NO_STALE=1; shift ;;
        *)
            # J2a: a BARE number is a misused `--max` (`... .out 300` intending
            # `--max 300`); adding it as a phantom log makes the wait never
            # complete. Warn + skip it. It is recognised by its shape alone (a
            # log is a path, never a bare number), so no file is looked at here.
            if [[ "$1" =~ ^[0-9]+$ ]]; then
                SETUP_ERR+="wait-for-codex-verdict.sh: ignoring bare number '$1' (not a log file) -- did you mean '--max $1'?"$'\n'
            else
                LOGS+=("$1")
            fi
            shift ;;
    esac
done
if [ "${#LOGS[@]}" -lt 1 ] || ! [ "$MAX" -gt 0 ] 2>/dev/null \
   || ! [ "$STALE" -gt 0 ] 2>/dev/null; then
    _refuse "${SETUP_ERR}usage: wait-for-codex-verdict.sh [--max SECONDS] [--stale-secs SECONDS] <logfile> [<logfile>...]"
fi
LAST="${LOGS[$(( ${#LOGS[@]} - 1 ))]}"
ACT_DIR=".claude/state/waiter-first-seen"

# A review ends only when its dispatcher has written <log>.done (the exit
# status, written atomically after the review process exited). Log text never
# ends a wait: the companion prints "[codex] Turn completed." before its
# verdict, and a live review can print a line that looks like any sentinel.
#
# ONE OBSERVER DOES EVERY FILESYSTEM READ. The shell that owns this call's deadline touches no file: one
# observer process (verdict_observer.py) probes each log (its status through review_result's one status
# parser, its owner lock, its size and last growth), recovers a result whose owner is gone, and streams what
# it saw down a pipe, one line per observation. This shell reads that pipe with `read -t` bounded by what is
# left of --max, so a probe stalled on the filesystem delays an observation, never the answer, and the
# report at the deadline is made from the latest observations without reading anything again. Every probe
# runs inside that one process, so ending it ends them all.
#
# Recovery: the owner holds <log>.owner locked for its life, so review_result.py recover publishes 1 only for
# an owner that is gone, and replaces a damaged status (an empty or unreadable .done) the same way. The
# observer starts it at most once per log per call, in a session of its own (it outlives the observer and
# publishes); its exit status is judged once a later probe reads "no status", and a recovery that could not
# publish (a full filesystem) while the owner is still gone leaves a review that has ended and will never
# publish, reported UNPUBLISHED (exit 4, re-dispatch that log), never as a review still running.
# Beside this script, named by expansion alone (no lookup: the observer is the first to touch the filesystem).
case "${BASH_SOURCE[0]}" in
    */*) OBSERVER="${BASH_SOURCE[0]%/*}/verdict_observer.py" ;;
    *)   OBSERVER="./verdict_observer.py" ;;
esac

# One deadline for the whole call, counted from the clock taken at the top.
END_US=$(( START_US + MAX * 1000000 ))
# The report's own time is inside --max: observation ends this much earlier, so writing the answer never
# takes the call past the deadline its caller set.
REPORT_US=$(( MAX * 1000000 / 4 < 300000 ? MAX * 1000000 / 4 : 300000 ))
OBS_END_US=$(( END_US - REPORT_US ))
declare -A GOT=() UNPUB=() UNREAD=() EXISTS=() SIZE=() SILENT=()
TAIL=()
# The observer holds none of the caller's streams, so a caller capturing this call's output sees EOF when
# this shell exits, not when the observer ends. Its time is the rest of --max, handed over as seconds.
_now
obs_us=$(( END_US - NOW_US > 1000 ? END_US - NOW_US : 1000 ))
printf -v OBS_SECS '%d.%06d' $(( obs_us / 1000000 )) $(( obs_us % 1000000 ))
exec {OBS}< <(exec </dev/null 2>/dev/null {OUT_FD}>&- {ERR_FD}>&-
              exec python3 "$OBSERVER" --secs "$OBS_SECS" --poll "$POLL" --act-dir "$ACT_DIR" -- "${LOGS[@]}")
OBS_PID=$!
finished=0
tail_read=0
while :; do
    _now
    left_us=$(( OBS_END_US - NOW_US ))
    [ "$left_us" -le 0 ] && break
    printf -v left '%d.%06d' $(( left_us / 1000000 )) $(( left_us % 1000000 ))
    IFS= read -r -t "$left" -u "$OBS" line || break       # out of time, or the observer has gone
    case "$line" in
        "done "*)        read -r _ rc key <<< "$line"; GOT[$key]="$rc" ;;
        # Each probe of a log replaces what the previous one said about it: the latest observation decides.
        "unpublished "*) key="${line#unpublished }"; UNPUB[$key]=1; unset "UNREAD[$key]" ;;
        "unreadable "*)  key="${line#unreadable }"; UNREAD[$key]=1
                         unset "UNPUB[$key]" "EXISTS[$key]" "SIZE[$key]" "SILENT[$key]" ;;
        "seen "*)        read -r _ ex sz sil key <<< "$line"
                         unset "UNREAD[$key]" "UNPUB[$key]"
                         EXISTS[$key]="$ex"; SIZE[$key]="$sz"; SILENT[$key]="$sil" ;;
        "tail "*)        TAIL+=("${line#tail }") ;;
        statuses)        finished=1 ;;                      # done; the tail lines follow until alldone
        alldone)         tail_read=1; break ;;
        cycle)
            # A round of probes is complete: a log silent past --stale-secs, or one that has ended without a
            # status, ends the wait now rather than at --max.
            stop=0
            for f in "${LOGS[@]}"; do
                [ -n "${GOT[$f]:-}" ] && continue
                [ -n "${UNPUB[$f]:-}" ] && stop=1
                [ -z "${SILENT[$f]:-}" ] && continue
                [ "$NO_STALE" -eq 1 ] && [ "${EXISTS[$f]:-0}" = 1 ] && continue
                [ "${SILENT[$f]}" -ge "$STALE" ] && stop=1
            done
            [ "$stop" -eq 1 ] && break ;;
    esac
done
_now
elapsed=$(( (NOW_US - START_US) / 1000000 ))

# The report is assembled here and written once, by a child bounded by what is left of --max (at least a
# second): a caller whose stdout pipe is full, with its reader not draining, never holds this exit.
REPORT=""
say() { REPORT+="$*"$'\n'; }
# shellcheck disable=SC2059  # every caller passes a literal format, as printf itself would take it
sayf() { local line; printf -v line "$@"; REPORT+="$line"; }
deliver() {
    # The observer is this call's own and ends with it. Every probe runs inside that one process, so one
    # signal ends them all (a read stalled on storage included); a recovery it started runs in a session of
    # its own and is left to publish.
    kill -KILL "$OBS_PID" 2>/dev/null
    # One absolute deadline for both writes, the end of --max (its last REPORT_US left for them, and never
    # extended): the report first, then the setup warning with whatever time remains, so a blocked stderr
    # never takes the report's time. A write whose time has gone is not made; the exit status still answers.
    _write_until "$REPORT" "$OUT_FD" "$END_US"
    [ -n "$SETUP_ERR" ] && _write_until "$SETUP_ERR" "$ERR_FD" "$END_US"
    exit "$1"
}

if [ "$finished" -eq 1 ]; then
    for f in "${LOGS[@]}"; do
        say "DONE: $f -- Turn completed (rc=${GOT[$f]:-})"
    done
    say "--- verdict tail ($LAST) ---"
    for line in "${TAIL[@]}"; do say "$line"; done
    [ "$tail_read" -eq 1 ] || say "(the transcript's tail could not be read within --max; every status above is published)"
    # The next step, stated where the verdicts are read (2026-09-28: 85 of 309
    # refused calls in 21 run logs were an edit made right here, before the
    # wave was received).
    say "NEXT: python3 scripts/overnight/review-envelope.py for the combined wave, then Skill(superpowers:receiving-code-review) BEFORE any Edit, Write or fix."
    deliver 0
fi

# The report is made from the latest observations; nothing is read again.
any_stale=0
any_missing=0
any_unpublished=0
pending=()
for f in "${LOGS[@]}"; do
    [ -n "${GOT[$f]:-}" ] && continue
    pending+=("$f")
    size="${SIZE[$f]:-0}"
    stalled="${SILENT[$f]:-}"
    if [ -n "${UNREAD[$f]:-}" ]; then
        sayf '  UNREADABLE: %s (a status is there but its storage could not be read; NOT a reason to re-dispatch -- wait again)\n' "$f"
    elif [ -n "${UNPUB[$f]:-}" ]; then
        any_unpublished=1
        sayf '  UNPUBLISHED: %s (its owner ended and the recovery could not publish its status -- the review has ended; re-dispatch THIS log)\n' "$f"
    elif [ -z "$stalled" ]; then
        sayf '  running: %s (no observation yet: its filesystem has not answered within --max)\n' "$f"
    elif [ "$stalled" -ge "$STALE" ] && [ "${EXISTS[$f]:-0}" = 0 ]; then
        any_missing=1
        sayf '  MISSING: %s (no such file after %ss -- NOT a hung review: verify the path against the broker'"'"'s logFile / .claude/overnight/reviews/manifest.jsonl before re-dispatching anything)\n' \
               "$f" "$stalled"
    elif [ "$stalled" -ge "$STALE" ] && [ "$NO_STALE" -eq 0 ]; then
        any_stale=1
        sayf '  STALE: %s (%s bytes, no growth for %ss >= %ss -- likely hung; re-dispatch THIS log, keep waiting on the rest)\n' \
               "$f" "$size" "$stalled" "$STALE"
    else
        sayf '  running: %s (%s bytes, last growth %ss ago)\n' "$f" "$size" "$stalled"
    fi
done

if [ "$any_unpublished" -eq 1 ]; then
    say "UNPUBLISHED after ${elapsed}s -- >=1 review ended with no status that can be published (see above)."
    deliver 4
fi
if [ "$any_stale" -eq 1 ]; then
    say "STALE after ${elapsed}s -- >=1 pending review produced no new output for >= ${STALE}s (see above)."
    deliver 4
fi
if [ "$any_missing" -eq 1 ]; then
    say "MISSING after ${elapsed}s -- >=1 log path never appeared (see above). A missing file is a wrong path or a review that never started, never a hung one."
    deliver 5
fi
say "STILL RUNNING after ${elapsed}s -- pending: ${pending[*]} -- re-invoke to" \
     "keep waiting (reviews run detached; nothing is lost)."
deliver 3
