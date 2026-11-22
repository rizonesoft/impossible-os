#!/usr/bin/env bash
# review-runtime-limit.sh SECONDS COMMAND
#
# Runs COMMAND (a bash command string) in its own process group and ends the
# whole group within SECONDS: TERM at the limit minus a grace, KILL at the
# limit. Exits with COMMAND's status, or 124 when the limit ended it. Whatever
# way COMMAND ends, nothing left in its group survives this script, so a caller
# that publishes a status afterwards (the review broker's job) knows no process
# of the attempt can still write to its log.
#
# GNU timeout is not enough for that: once its direct child exits on TERM it
# stops supervising, and a descendant that ignores TERM keeps running. Time is
# read from /proc/uptime, which a wall-clock step does not move.
set -u
# Hundredths of a second since boot (/proc/uptime), so a deadline is kept to a fraction of a second.
uptime_cs() { local u; read -r u _ < /proc/uptime; echo $(( ${u%.*} * 100 + 10#${u#*.} )); }
usage() {
    echo "usage: review-runtime-limit.sh SECONDS COMMAND | --until-cs UPTIME_CENTISECONDS COMMAND" >&2
    exit 2
}
# --until-cs: a caller that started its clock earlier (the review broker) passes the absolute deadline, so time
# spent before this script started (registration, the service manager) is not given back. A deadline already
# passed starts nothing and reports the limit (124).
if [ "${1:-}" = "--until-cs" ]; then
    [ $# -eq 3 ] && [[ "${2:-}" =~ ^[0-9]+$ ]] && [ -n "${3:-}" ] || usage
    end=$2
    cmd=$3
    left=$(( end - $(uptime_cs) ))
    if [ "$left" -le 0 ]; then
        echo "review-runtime-limit: the deadline had passed before the command started; nothing was run" >&2
        exit 124
    fi
else
    max="${1:-}"
    cmd="${2:-}"
    if ! [[ "$max" =~ ^[1-9][0-9]*$ ]] || [ -z "$cmd" ] || [ $# -ne 2 ]; then
        usage
    fi
    end=$(( $(uptime_cs) + max * 100 ))
    left=$(( max * 100 ))
fi
# TERM a grace before the end (30 s, or a quarter of a limit up to 120 s, at least a tenth of a second), KILL at it.
grace=3000
[ "$left" -le 12000 ] && grace=$(( left / 4 ))
[ "$grace" -lt 10 ] && grace=10

# A background child of a non-interactive shell is not a group leader, so
# setsid makes it one in place: its pid is its process group id.
# LIFECYCLE-WAIVER: groups one REVIEW attempt so it can be ended whole; it
# does not detach, relaunch or watch the session (this script waits for it).
setsid bash -c "$cmd" &
pid=$!

alive() {
    local st
    read -r _ _ st _ < "/proc/$pid/stat" 2>/dev/null || return 1
    [ "$st" != "Z" ]
}

term_at=$(( end - grace ))
limited=0
while alive; do
    now=$(uptime_cs)
    if [ "$now" -ge "$end" ]; then
        limited=1          # the limit ended it, even when TERM's window was missed
        kill -KILL -- "-$pid" 2>/dev/null
        break
    fi
    if [ "$now" -ge "$term_at" ] && [ "$limited" -eq 0 ]; then
        limited=1
        kill -TERM -- "-$pid" 2>/dev/null
    fi
    sleep 0.1                # the limit is kept to a tenth of a second
done
wait "$pid" 2>/dev/null
status=$?
# The leader is gone; whatever it left in its group goes with it.
kill -KILL -- "-$pid" 2>/dev/null
[ "$limited" -eq 1 ] && exit 124
exit "$status"
