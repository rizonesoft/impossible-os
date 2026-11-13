#!/bin/bash
# =============================================================================
# run-with-boot-conf.sh -- Boot the built image once with one boot.conf key set
#
# Patches KEY=VALUE into the system disk's boot.conf, launches QEMU through
# `make run-only` (which has no prerequisites, so nothing rebuilds the disk
# and overwrites the patch), and restores boot.conf when QEMU exits, fails or
# is interrupted. A failing boot fails this script (make reports a failed
# recipe as status 2, whatever QEMU returned). Used by `make run-unit-tests`
# and `make run-boot-tests`.
#
# It is a script rather than a recipe line because GNU make runs any recipe
# line that names $(MAKE) even under `make -n`, and the patch must not.
#
# Usage: bash scripts/run-with-boot-conf.sh <key> <value>
# =============================================================================

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <key> <value>" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"

# One run at a time owns the disk's boot.conf (the lock patch-boot-conf.sh
# defines): a second run (make -j run-unit-tests run-boot-tests, run-qemu.sh,
# another shell) would patch or reset the image under this one. It is taken
# BEFORE the restore trap, so a run that does not get it leaves the other
# run's boot.conf alone, and held until this script exits.
. "$SCRIPT_DIR/lib/boot-conf-lock.sh"
boot_conf_lock_hold "$PROJECT" 9 || exit 1

# The restore is installed BEFORE the patch, so a patch that fails or is
# interrupted half way still ends with the default boot.conf (reset rewrites
# the default whatever the disk holds). Ctrl-C reaches QEMU's whole process
# group, so QEMU stops and the reset runs. A TERM sent to this script alone is
# held by bash until QEMU exits; the boot is interactive (serial on stdio), so
# QEMU stays in the foreground rather than being backgrounded to forward it.
# A failed restore fails the run (boot_conf_restore reports it).
trap '_ec=$?; boot_conf_restore "$PROJECT" || { [ "$_ec" -eq 0 ] && _ec=1; }; exit "$_ec"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

bash "$SCRIPT_DIR/patch-boot-conf.sh" "$1" "$2"
make -C "$PROJECT" --no-print-directory run-only
