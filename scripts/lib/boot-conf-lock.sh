#!/bin/bash
# =============================================================================
# boot-conf-lock.sh -- hold the system disk's boot.conf for one whole run
#
# Sourced by every script that patches boot.conf, boots the image and resets
# it (run-with-boot-conf.sh, run-qemu.sh, test.sh, test-swtpm.sh). The caller
# holds build/.boot-conf.lock from before its patch until it exits, and
# exports BOOT_CONF_LOCK_HELD=1 so its own patch-boot-conf.sh calls do not
# wait on it; every other patch or reset is refused while it is held, so no
# run patches or resets boot.conf under another run's boot.
#
# Usage: . scripts/lib/boot-conf-lock.sh; boot_conf_lock_hold <project> <fd>
# Returns 0 held, 1 another run holds it. A host without flock holds nothing
# and returns 0, as patch-boot-conf.sh does.
# =============================================================================

boot_conf_lock_hold() {
    local project="$1" fd="$2"
    command -v flock >/dev/null 2>&1 || return 0
    mkdir -p "$project/build" || return 1
    eval "exec ${fd}>\"\$project/build/.boot-conf.lock\"" || return 1
    if ! flock -n "$fd"; then
        eval "exec ${fd}>&-"
        echo "another run is booting $project/build/system-disk.img with a patched boot.conf (build/.boot-conf.lock)" >&2
        return 1
    fi
    export BOOT_CONF_LOCK_HELD=1
}
