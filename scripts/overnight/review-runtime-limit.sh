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
# read from /proc/uptime, which a wall-clock step does not move. Cancelling
# this script ends the attempt the same way and exits 128 + the signal.
set -u
# Hundredths of a second since boot (/proc/uptime), so a deadline is kept to a fraction of a second. Set in
# NOW rather than printed, so a poll forks nothing to read the clock.
now_cs() { local u; read -r u _ < /proc/uptime; NOW=$(( ${u%.*} * 100 + 10#${u#*.} )); }
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
    now_cs
    left=$(( end - NOW ))
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
    now_cs
    end=$(( NOW + max * 100 ))
    left=$(( max * 100 ))
fi
# TERM a grace before the end (30 s, or a quarter of a limit up to 120 s, at least a tenth of a second), KILL at it.
grace=3000
[ "$left" -le 12000 ] && grace=$(( left / 4 ))
[ "$grace" -lt 10 ] && grace=10

# Cancelling this script (TERM, INT, HUP) ends the attempt the same way the deadline does, then reports the
# signal: a caller that gives up leaves nothing of the attempt running.
cancelled=0
trap 'cancelled=143' TERM
trap 'cancelled=130' INT
trap 'cancelled=129' HUP

# A background child of a non-interactive shell is not a group leader, so
# setsid makes it one in place: its pid is its process group id.
# LIFECYCLE-WAIVER: groups one REVIEW attempt so it can be ended whole; it
# does not detach, relaunch or watch the session (this script waits for it).
setsid bash -c "$cmd" &
pid=$!

# The state is the first field after the name, and the name (in parentheses) can hold spaces and ')', so
# it is read after the LAST ')'.
alive() {
    local stat st
    read -r stat 2>/dev/null < "/proc/$pid/stat" || return 1        # gone: nothing to read
    st="${stat##*) }"
    [ "${st%% *}" != "Z" ]
}

term_at=$(( end - grace ))
limited=0
# A pipe nothing writes to: a timed read of it is the poll's pause, a shell builtin with no process per poll.
exec {pause_fd}<> <(:)
# The attempt is its whole process tree, not only its group: a step inside it can start its own session (the
# candidate reader runs git that way, so its own timeout can end git's group), and a KILL that reached only
# this group would orphan that session. So each signal goes to every process descended from the child, read
# from /proc while the tree is still linked, and to every group those processes lead or belong to; groups
# seen at an earlier signal are signalled again, since their leader may have gone since. Until the child
# has run setsid its group does not exist; the child itself is then signalled, which ends it before it can
# start anything.
declare -A groups=()
own_group=""
read_stat() {   # STAT_PPID and STAT_PGRP of the /proc stat file $1, read after the name's last ')'
    local stat fields
    read -r stat 2>/dev/null < "$1" || return 1
    read -r -a fields <<< "${stat##*) }"
    STAT_PPID=${fields[1]} STAT_PGRP=${fields[2]}
}
read_stat "/proc/$$/stat" && own_group=$STAT_PGRP
signal_job() {
    local -A kids=() pgrp_of=()
    local f p cur queue=("$pid") tree=()
    for f in /proc/[0-9]*/stat; do
        read_stat "$f" || continue
        p=${f#/proc/}
        p=${p%/stat}
        kids[$STAT_PPID]+=" $p"
        [ "$STAT_PGRP" = "$pid" ] || pgrp_of[$p]=$STAT_PGRP
    done
    while [ ${#queue[@]} -gt 0 ]; do
        cur=${queue[0]}
        queue=("${queue[@]:1}")
        tree+=("$cur")
        [ -n "${pgrp_of[$cur]:-}" ] && [ "${pgrp_of[$cur]}" != "$own_group" ] && groups[${pgrp_of[$cur]}]=1
        for p in ${kids[$cur]:-}; do queue+=("$p"); done
    done
    kill "-$1" -- "-$pid" 2>/dev/null || kill "-$1" "$pid" 2>/dev/null
    for p in "${!groups[@]}"; do kill "-$1" -- "-$p" 2>/dev/null; done
    [ ${#tree[@]} -gt 1 ] && kill "-$1" -- "${tree[@]:1}" 2>/dev/null
    return 0
}
while alive; do
    if [ "$cancelled" -ne 0 ]; then
        signal_job KILL
        break
    fi
    now_cs
    now=$NOW
    if [ "$now" -ge "$end" ]; then
        limited=1          # the limit ended it, even when TERM's window was missed
        signal_job KILL
        break
    fi
    if [ "$now" -ge "$term_at" ] && [ "$limited" -eq 0 ]; then
        limited=1
        signal_job TERM
    fi
    read -r -t 0.1 -u "$pause_fd" _       # the limit is kept to a tenth of a second; it times out by design
done
wait "$pid" 2>/dev/null
status=$?
if [ "$cancelled" -ne 0 ]; then
    signal_job KILL                                    # a cancellation that arrived as the child ended
    wait "$pid" 2>/dev/null
fi
# Seen ending only after the deadline (this supervisor was not scheduled in between): the limit's, not a pass.
now_cs
[ "$NOW" -ge "$end" ] && limited=1
# The leader is gone; whatever it left in its group goes with it, and so does what an ended attempt left in
# a session of its own.
kill -KILL -- "-$pid" 2>/dev/null
for p in "${!groups[@]}"; do kill -KILL -- "-$p" 2>/dev/null; done
[ "$cancelled" -ne 0 ] && exit "$cancelled"
[ "$limited" -eq 1 ] && exit 124
exit "$status"
