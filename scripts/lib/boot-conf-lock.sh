#!/bin/bash
# =============================================================================
# boot-conf-lock.sh -- hold the system disk's boot.conf for one whole run
#
# Sourced by every script that boots build/system-disk.img or patches its
# boot.conf: run-with-boot-conf.sh, with-boot-conf-lock.sh (every Makefile
# QEMU recipe), run-qemu.sh, test.sh, test-swtpm.sh, test-smoke.sh,
# test-smoke-matrix.sh, test-fs.sh, test-desktop.sh, test-visual-regression.sh and scripts/machines/{run-qemu-kvm,
# run-qemu-tcg,boot-test-qemu}.sh and the rollback / stale-ABI fixtures under
# scripts/debug/. Building or cleaning the image checks it first (Makefile).
# The caller
# holds .boot-conf.lock (at the worktree root, never under build/, which
# `build.sh clean` deletes) from before its patch until it exits, and
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
    # A run started by a run that holds it (test-smoke-matrix -> test-smoke,
    # run-with-boot-conf -> make run-only) shares its parent's hold.
    [ "${BOOT_CONF_LOCK_HELD:-}" = "1" ] && return 0
    command -v flock >/dev/null 2>&1 || return 0
    eval "exec ${fd}>\"\$project/.boot-conf.lock\"" || return 1
    if ! flock -n "$fd"; then
        eval "exec ${fd}>&-"
        echo "another run is using $project/build/system-disk.img (.boot-conf.lock); wait for it to finish" >&2
        return 1
    fi
    export BOOT_CONF_LOCK_HELD=1
}

# boot_conf_lock_if_under_build <project> <fd> <path>...: hold the lock (as
# boot_conf_lock_hold) when any path lies under <project>/build/, which
# `make clean` deletes under this lock; a run on images elsewhere needs none.
boot_conf_lock_if_under_build() {
    local project="$1" fd="$2" path build
    shift 2
    build="$(realpath -m -- "$project/build")"
    for path in "$@"; do
        case "$(realpath -m -- "$path")" in
            "$build"/*) boot_conf_lock_hold "$project" "$fd"; return ;;
        esac
    done
    return 0
}
