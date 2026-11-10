#!/usr/bin/env bash
# Install apt packages on a CI runner, surviving a stalled mirror.
#
#   bash scripts/ci/apt-install.sh <package>...
#
# The runner's Ubuntu mirror sometimes stops sending mid-download: no error,
# no progress, and apt's own retry and per-request timeout never fire because
# the connection stays open. On 2026-11-10 that used the LLVM step's whole
# 10 minutes twice in one day, in two different jobs, and each time the only
# cure was re-running the job by hand. So each attempt gets a wall-clock bound
# (45 s to update, 140 s to install; three attempts fit the 10-minute step)
# and a stalled one is abandoned and retried; a healthy install takes well
# under a minute of download.
#
# Exit: 0 installed, 1 every attempt failed.
set -uo pipefail

[ $# -gt 0 ] || { echo "usage: apt-install.sh <package>..." >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30)
ATTEMPTS="${APT_INSTALL_ATTEMPTS:-3}"
UPDATE_CAP="${APT_UPDATE_SECONDS:-45}"
INSTALL_CAP="${APT_INSTALL_SECONDS:-140}"

for n in $(seq 1 "$ATTEMPTS"); do
    echo "apt-install: attempt $n of $ATTEMPTS: $*"
    # -k: a stalled apt that ignores TERM is killed, not waited on.
    if sudo -E timeout -k 10 "$UPDATE_CAP" apt-get update "${OPTS[@]}" \
        && sudo -E timeout -k 10 "$INSTALL_CAP" apt-get install -y "${OPTS[@]}" "$@"; then
        exit 0
    fi
    echo "apt-install: attempt $n failed or stalled" >&2
    # An interrupted install can leave dpkg half-configured and its lock held.
    sudo killall -q apt-get dpkg 2>/dev/null || true
    sudo rm -f /var/lib/dpkg/lock-frontend /var/lib/dpkg/lock /var/cache/apt/archives/lock
    sudo dpkg --configure -a || true
    sleep $((n * 5))
done
echo "apt-install: every attempt failed: $*" >&2
exit 1
