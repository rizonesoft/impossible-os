#!/usr/bin/env bash
# wait-for-codex-verdict.sh [--max SECONDS] <logfile> [<logfile>...]
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
# Exit status:
#   0  -- ALL logs show "Turn completed"; each verdict + the last tail printed.
#   3  -- bounded wait elapsed, >=1 review still running; re-invoke to keep going.
#   2  -- usage error.
# Zero model tokens while it sleeps (one blocking Bash call, idle in `sleep`).
#
# One turn per bounded wait. For a single LONG wait in one turn, raise the bound
# AND pass an explicit Bash timeout, e.g. `--max 540 <log>` with tool
# `timeout: 600000`. Otherwise just re-invoke on exit 3 (each call is < 2 min).
set -u

MAX=100          # default < the Bash tool's ~120s ceiling, so a bare call is safe
POLL=10
LOGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --max)    MAX="${2:-}"; shift 2 ;;
        --max=*)  MAX="${1#--max=}"; shift ;;
        *)        LOGS+=("$1"); shift ;;
    esac
done
if [ "${#LOGS[@]}" -lt 1 ] || ! [ "$MAX" -gt 0 ] 2>/dev/null; then
    echo "usage: wait-for-codex-verdict.sh [--max SECONDS] <logfile> [<logfile>...]" >&2
    exit 2
fi
LAST="${LOGS[$(( ${#LOGS[@]} - 1 ))]}"

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
        exit 0
    fi
    dur=$POLL
    [ $((MAX - elapsed)) -lt "$POLL" ] && dur=$((MAX - elapsed))
    sleep "$dur"
    elapsed=$((elapsed + dur))
done

pending=()
for f in "${LOGS[@]}"; do
    { [ -f "$f" ] && grep -q "Turn completed" "$f" 2>/dev/null; } || pending+=("$f")
done
echo "STILL RUNNING after ${elapsed}s -- pending: ${pending[*]} -- re-invoke to" \
     "keep waiting (reviews run detached; nothing is lost)."
exit 3
