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
if read -r _up _ < /proc/uptime 2>/dev/null && [[ "$_up" =~ ^[0-9]+\.[0-9]{2}$ ]]; then
    _now_us() { local up; read -r up _ < /proc/uptime; echo $(( ${up%.*} * 1000000 + 10#${up#*.} * 10000 )); }
else
    _now_us() { local t="$EPOCHREALTIME"; echo $(( ${t%.*} * 1000000 + 10#${t#*.} )); }
fi
START_US=$(_now_us)
# The caller's streams, kept for the report: a forked writer reaches them through these descriptors.
exec {OUT_FD}>&1 {ERR_FD}>&2

# Setup messages are kept and written at the end through the same bounded writer as the report (a caller
# whose stderr pipe is full never holds this call), or by `_refuse` for a usage error.
SETUP_ERR=""
_refuse() {
    printf '%s\n' "$1" | timeout -k 1 1 cat >&2 2>/dev/null
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
# ONE OBSERVER DOES EVERY FILESYSTEM READ. The shell that owns this call's deadline touches no file: a
# background observer probes each log (its status through review_result.py status, the one bounded status
# reader the recovery and the envelope also use; its owner lock; its size and last growth), recovers a result
# whose owner is gone, and streams what it saw down a pipe, one line per observation. This shell reads that
# pipe with `read -t` bounded by what is left of --max, so a probe stalled on the filesystem delays an
# observation, never the answer, and the report at the deadline is made from the latest observations
# without reading anything again.
#
# Recovery: the owner holds <log>.owner locked for its life, so review_result.py recover publishes 1 only for
# an owner that is gone, and replaces a damaged status (an empty or unreadable .done) the same way. It runs
# at most once per log per call; its exit status is read once it ends, and a recovery that could not publish
# (a full filesystem) while the owner is still gone leaves a review that has ended and will never publish,
# reported UNPUBLISHED (exit 4, re-dispatch that log), never as a review still running.
# Beside this script, named by expansion alone (no lookup: the observer is the first to touch the filesystem).
case "${BASH_SOURCE[0]}" in
    */*) RESULT_TOOL="${BASH_SOURCE[0]%/*}/review_result.py" ;;
    *)   RESULT_TOOL="./review_result.py" ;;
esac

# B2 activity. An existing log's silence is measured from its own mtime (when
# it last grew), so a review silent since launch is STALE as soon as the
# threshold passes, within this call. A path that does not exist yet has its
# first sighting recorded in a sidecar, which survives re-invocations.
_first_seen() {
    # Made only for a log that is missing, so a published status is read before any of this bookkeeping.
    mkdir -p "$ACT_DIR" 2>/dev/null || true
    local side; side="$ACT_DIR/$(printf '%s' "$1" | md5sum 2>/dev/null | cut -c1-16)"
    # A sighting that cannot be kept reads as the epoch, so a missing log is
    # reported MISSING at once rather than looking newly launched forever.
    local t=""
    [ -f "$side" ] || { date +%s > "$side.tmp.$$" && mv -f "$side.tmp.$$" "$side"; } 2>/dev/null || { echo 0; return; }
    read -r t < "$side" 2>/dev/null || t=""
    case "$t" in ''|*[!0-9]*) echo 0 ;; *) echo "$t" ;; esac
}

# The observer: runs in the background, writes `done RC LOG`, `unpublished LOG`, `seen EXISTS SIZE SILENT
# LOG` (the path last, so it is read whole) and, once every log is done, the last log's tail (`tail TEXT`) and `alldone`; a `cycle` line ends
# each round of probes.
_observe() {
    local -A rec_pid=() unpublished=() status=()
    local f st rc pid pst now size silent exists
    local soon left
    while :; do
        local all=1
        soon=0
        for f in "${LOGS[@]}"; do
            if [ -n "${status[$f]:-}" ]; then continue; fi
            src=0
            st="$(python3 "$RESULT_TOOL" status "$f" 2>/dev/null)" || src=$?
            if [ "$src" -eq 0 ]; then
                status[$f]="$st"
                echo "done $st $f"
                continue
            fi
            all=0
            if [ "$src" -eq 3 ]; then
                # The status is there but could not be read: its content is unknown, so it is neither
                # recovered nor reported as unpublished; it is read again next round.
                echo "unreadable $f"
                continue
            fi
            if [ -n "${unpublished[$f]:-}" ]; then echo "unpublished $f"; continue; fi
            pid="${rec_pid[$f]:-}"
            if [ -n "$pid" ] && [ "$pid" != ended ]; then
                # A recovery started this call: read its status once it has ended (a zombie of this shell
                # until waited on, so a still-running one is a live, non-zombie /proc entry).
                pst=""
                [ -r "/proc/$pid/stat" ] && read -r _ _ pst _ < "/proc/$pid/stat" 2>/dev/null
                if [ -z "$pst" ] || [ "$pst" = Z ]; then
                    rc=0
                    wait "$pid" 2>/dev/null || rc=$?
                    rec_pid[$f]=ended
                    src=0
                    python3 "$RESULT_TOOL" status "$f" >/dev/null 2>&1 || src=$?
                    if [ "$rc" -ne 0 ] && [ "$src" -eq 1 ] && flock -n "$f.owner" true 2>/dev/null; then
                        unpublished[$f]=1
                        echo "unpublished $f"
                        continue
                    fi
                fi
            elif [ -z "$pid" ] && [ -e "$f.owner" ] && [ -e "$f" ] && flock -n "$f.owner" true 2>/dev/null; then
                # Only a free owner lock (flock -n, which never waits) is worth a recovery; a live owner holds it.
                # LIFECYCLE-WAIVER: publishes one REVIEW attempt's result; it does not relaunch or watch the session.
                python3 "$RESULT_TOOL" recover "$f" </dev/null >/dev/null 2>&1 &
                rec_pid[$f]=$!
                soon=1
            fi
            now=$(date +%s)
            # Metadata only, from one stat (the file is never opened: a FIFO that took the path cannot hold
            # this probe): a regular file's size and last growth.
            meta=$(stat -c '%f %s %Y' -- "$f" 2>/dev/null) || meta=""
            read -r mode size mtime <<< "$meta"
            if [ -n "$meta" ] && [ $(( 0x$mode & 0xF000 )) -eq $(( 0x8000 )) ]; then
                exists=1
                silent=$(( now - mtime ))
            else
                exists=0
                size=0
                silent=$(( now - $(_first_seen "$f") ))
            fi
            echo "seen $exists ${size:-0} $silent $f"
        done
        if [ "$all" -eq 1 ]; then
            # Every status is in: the wave has finished whatever the tail read below costs. The tail is
            # diagnostic, read last and bounded by bytes (one long line never reads a whole transcript).
            echo "statuses"
            # Read through review_result.py tail: a regular file opened without blocking, so a FIFO that
            # replaced the transcript fails at once instead of holding this observer.
            python3 "$RESULT_TOOL" tail "$LAST" </dev/null 2>/dev/null | while IFS= read -r line || [ -n "$line" ]; do
                printf 'tail %s\n' "$line"
            done
            echo "alldone"
            return 0
        fi
        echo "cycle"
        # The next round comes after POLL, or sooner: a second after a recovery started (its status is then
        # seen at once), and in any case a second before the deadline, so a status published during the
        # last interval is still observed in time.
        left=$(( (END_US - $(_now_us)) / 1000000 - 1 ))
        dur=$POLL
        [ "$soon" -eq 1 ] && dur=1
        [ "$left" -lt "$dur" ] && dur=$left
        [ "$dur" -lt 1 ] && dur=1
        sleep "$dur"
    done
}

# One deadline for the whole call, counted from the clock taken at the top.
END_US=$(( START_US + MAX * 1000000 ))
declare -A GOT=() UNPUB=() UNREAD=() EXISTS=() SIZE=() SILENT=()
TAIL=()
# The observer's streams are its own (exec, so no saved copy of the caller's stderr stays open in it): a
# caller capturing this call's output sees EOF when this shell exits, not when the observer's sleep ends.
# The observer holds none of the caller's streams (its stalled children would keep the caller's pipe open).
exec {OBS}< <(exec </dev/null 2>/dev/null {OUT_FD}>&- {ERR_FD}>&-; _observe)
OBS_PID=$!
finished=0
tail_read=0
while :; do
    left_us=$(( END_US - $(_now_us) ))
    [ "$left_us" -le 0 ] && break
    left=$(printf '%d.%06d' $(( left_us / 1000000 )) $(( left_us % 1000000 )))
    IFS= read -r -t "$left" -u "$OBS" line || break       # out of time, or the observer has gone
    case "$line" in
        "done "*)        read -r _ rc key <<< "$line"; GOT[$key]="$rc" ;;
        "unpublished "*) UNPUB["${line#unpublished }"]=1 ;;
        "unreadable "*)  UNREAD["${line#unreadable }"]=1 ;;
        "seen "*)        read -r _ ex sz sil key <<< "$line"
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
elapsed=$(( ($(_now_us) - START_US) / 1000000 ))

# The report is assembled here and written once, by a child bounded by what is left of --max (at least a
# second): a caller whose stdout pipe is full, with its reader not draining, never holds this exit.
REPORT=""
say() { REPORT+="$*"$'\n'; }
# shellcheck disable=SC2059  # every caller passes a literal format, as printf itself would take it
sayf() { local line; printf -v line "$@"; REPORT+="$line"; }
# Writes $1 to descriptor $2 by $3 (microseconds, _now_us). The write is made by a forked copy of this
# shell, which starts no program (nothing to load from a stalled disk), and this shell waits for its word
# only until the deadline: a write blocked on a full pipe is then ended, never waited for.
_write_until() {
    local done_fd pid left_us
    left_us=$(( $3 - $(_now_us) ))
    [ "$left_us" -ge 100000 ] || return 0
    exec {done_fd}< <(exec 2>/dev/null; printf '%s' "$1" >&"$2"; echo written)
    pid=$!
    left_us=$(( $3 - $(_now_us) ))
    [ "$left_us" -gt 0 ] || left_us=1
    IFS= read -r -t "$(printf '%d.%06d' $(( left_us / 1000000 )) $(( left_us % 1000000 )))" -u "$done_fd" _ \
        || kill -KILL "$pid" 2>/dev/null
    exec {done_fd}<&-
}

deliver() {
    # The observer is this call's own and ends with it: whatever probe it is in, nothing outlives the call
    # for longer than that one probe.
    kill -TERM "$OBS_PID" 2>/dev/null
    # One absolute deadline for both writes (what is left of --max, at least a second): the report first,
    # then the setup warning with whatever time remains, so a blocked stderr never takes the report's time.
    local now_us until_us
    now_us=$(_now_us)
    until_us=$(( END_US > now_us + 1000000 ? END_US : now_us + 1000000 ))
    _write_until "$REPORT" "$OUT_FD" "$until_us"
    [ -n "$SETUP_ERR" ] && _write_until "$SETUP_ERR" "$ERR_FD" $(( until_us - 100000 ))
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
