#!/usr/bin/env bash
# Install apt packages on a CI runner, surviving a stalled mirror.
#
#   bash scripts/ci/apt-install.sh <package>...
#
# The runner's Ubuntu mirror sometimes stops sending mid-download: no error,
# no progress, and apt's own retry and per-request timeout never fire because
# the connection stays open. On 2026-11-10 that used a whole step timeout four
# times in one day, across three jobs (once 16 minutes inside a single
# download), and each time the only cure was re-running the job by hand.
#
# So the network part is separated from the rest. Each attempt runs the index
# update and a download-only install under a wall-clock bound; a stalled one
# is abandoned and retried, which is safe because nothing has been unpacked.
# Only when every package is in the local archive does the real install run,
# with no network left to stall on.
#
# Worst case with the defaults: three stalled attempts (40 s update + 120 s
# download, plus 10 s kill grace each), 15 s of pauses, then a 240 s install:
# about 13.5 minutes. Callers give the step 15 minutes, and
# test_ci_toolchain_install.py recomputes this from the caps below.
#
# Needs GNU timeout (the workflows install it first: uutils `timeout` leaves a
# stalled child running).
#
# Exit: 0 installed, 1 every download attempt failed or the install failed.
set -uo pipefail

[ $# -gt 0 ] || { echo "usage: apt-install.sh <package>..." >&2; exit 1; }
export DEBIAN_FRONTEND=noninteractive
OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30)
ATTEMPTS="${APT_INSTALL_ATTEMPTS:-3}"
UPDATE_CAP="${APT_UPDATE_SECONDS:-40}"
DOWNLOAD_CAP="${APT_DOWNLOAD_SECONDS:-120}"
INSTALL_CAP="${APT_INSTALL_SECONDS:-240}"
MIRROR="${APT_MIRROR:-azure.archive.ubuntu.com}"
FALLBACK_MIRROR="${APT_FALLBACK_MIRROR:-archive.ubuntu.com}"

for n in $(seq 1 "$ATTEMPTS"); do
    echo "apt-install: download attempt $n of $ATTEMPTS: $*"
    # -k: a stalled apt that ignores TERM is killed, not waited on.
    # One network budget per attempt (update cap + download cap), spent in
    # order: an index update that has to fetch every list again, as it does
    # right after a mirror switch, may run into the download's share.
    t_start=$(date +%s)
    if sudo -E timeout -k 10 "$((UPDATE_CAP + DOWNLOAD_CAP - 10))" apt-get update "${OPTS[@]}" \
        && left=$((UPDATE_CAP + DOWNLOAD_CAP - ($(date +%s) - t_start))) && [ "$left" -ge 10 ] \
        && sudo -E timeout -k 10 "$left" apt-get install -y --download-only "${OPTS[@]}" "$@"; then
        echo "apt-install: packages downloaded; installing"
        if sudo -E timeout -k 10 "$INSTALL_CAP" apt-get install -y --no-download "${OPTS[@]}" "$@"; then
            exit 0
        fi
        echo "apt-install: the install failed after a complete download: $*" >&2
        exit 1
    fi
    echo "apt-install: download attempt $n failed or stalled" >&2
    sudo killall -q apt-get 2>/dev/null || true
    sudo rm -f /var/lib/apt/lists/lock /var/cache/apt/archives/lock
    # A retry against the same mirror does not help when this runner's route to
    # it is what is broken: on 2026-11-10 one job failed all three attempts on
    # azure.archive.ubuntu.com while two jobs beside it downloaded in seconds.
    # After the first failure, point apt at the primary archive instead.
    # GitHub's runners name the mirror in /etc/apt/apt-mirrors.txt (the sources
    # point at that list), other hosts in the sources themselves.
    if [ "$n" = "1" ] && sudo grep -rqsF "$MIRROR" /etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d; then
        echo "apt-install: switching from $MIRROR to $FALLBACK_MIRROR" >&2
        for f in /etc/apt/apt-mirrors.txt /etc/apt/sources.list /etc/apt/sources.list.d/*; do
            [ -f "$f" ] && sudo sed -i "s|//$MIRROR/|//$FALLBACK_MIRROR/|g" "$f"
        done
    fi
    [ "$n" -lt "$ATTEMPTS" ] && sleep $((n * 5))
done
echo "apt-install: every download attempt failed: $*" >&2
exit 1
