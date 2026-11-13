#!/bin/bash
# =============================================================================
# with-boot-conf-lock.sh -- run one boot of the system disk under its lock
#
# Every Makefile recipe that boots build/system-disk.img runs QEMU through
# this script, so a run that patches boot.conf (run-with-boot-conf.sh,
# run-qemu.sh, test.sh, test-swtpm.sh) cannot patch or reset it while the
# boot reads it, and this boot does not start while such a run holds it.
# Under a caller that already holds the lock (BOOT_CONF_LOCK_HELD=1, as
# run-with-boot-conf.sh exports for `make run-only`) it runs the command as is.
#
# Usage: bash scripts/with-boot-conf-lock.sh <command> [args...]
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "${BOOT_CONF_LOCK_HELD:-}" != "1" ]; then
    . "$SCRIPT_DIR/lib/boot-conf-lock.sh"
    boot_conf_lock_hold "$(dirname "$SCRIPT_DIR")" 9 || exit 1
fi
exec "$@"
