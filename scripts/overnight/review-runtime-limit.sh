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
max="${1:-}"
cmd="${2:-}"
if ! [[ "$max" =~ ^[1-9][0-9]*$ ]] || [ -z "$cmd" ] || [ $# -ne 2 ]; then
    echo "usage: review-runtime-limit.sh SECONDS COMMAND" >&2
    exit 2
fi
grace=30
[ "$max" -le 120 ] && grace=$(( max / 4 ))
[ "$grace" -lt 1 ] && grace=1

uptime_s() { local u; read -r u _ < /proc/uptime; echo "${u%.*}"; }

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

end=$(( $(uptime_s) + max ))
term_at=$(( end - grace ))
limited=0
while alive; do
    now=$(uptime_s)
    if [ "$now" -ge "$end" ]; then
        limited=1          # the limit ended it, even when TERM's window was missed
        kill -KILL -- "-$pid" 2>/dev/null
        break
    fi
    if [ "$now" -ge "$term_at" ] && [ "$limited" -eq 0 ]; then
        limited=1
        kill -TERM -- "-$pid" 2>/dev/null
    fi
    sleep 1
done
wait "$pid" 2>/dev/null
status=$?
# The leader is gone; whatever it left in its group goes with it.
kill -KILL -- "-$pid" 2>/dev/null
[ "$limited" -eq 1 ] && exit 124
exit "$status"
