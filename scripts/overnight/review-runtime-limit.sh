#!/usr/bin/env bash
# review-runtime-limit.sh SECONDS COMMAND | --until-cs UPTIME_CENTISECONDS COMMAND
#
# Runs COMMAND (a bash command string) and ends the whole attempt within the limit: TERM a grace before it,
# KILL at it. Exits with COMMAND's status, 124 when the limit ended it, or 128 + the signal when this script
# was cancelled, which ends the attempt the same way. Whatever way COMMAND ends, no process descended from it
# survives this script (the supervisor is the attempt's child subreaper, so a step that starts its own session
# or leaves an orphan cannot escape), so a caller that publishes a status afterwards (the review broker's job)
# knows no process of the attempt can still write to its log. The supervisor and the reasons are in
# review_runtime_limit.py beside this script.
exec python3 "$(dirname "${BASH_SOURCE[0]}")/review_runtime_limit.py" "$@"
