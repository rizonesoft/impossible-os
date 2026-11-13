#!/bin/bash
# =============================================================================
# patch-boot-conf.sh -- Patch boot.conf in the system disk image
#
# Modifies a specific key=value in the EFI partition's boot.conf without
# rebuilding the entire disk image. Used by scripts/run-with-boot-conf.sh
# (`make run-unit-tests`, `make run-boot-tests`), scripts/run-qemu.sh and the
# test runners.
#
# Usage:
#   bash scripts/patch-boot-conf.sh <key> <value>
#   bash scripts/patch-boot-conf.sh debug 1
#   bash scripts/patch-boot-conf.sh test 1
#   bash scripts/patch-boot-conf.sh reset     # restore original boot.conf
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
DISK="$PROJECT/build/system-disk.img"
BOOT_CONF="$PROJECT/resources/boot/boot.conf"
TEMP_CONF="$PROJECT/build/boot.conf.tmp"

if [ ! -f "$DISK" ]; then
    echo "ERROR: $DISK not found. Run 'bash scripts/build.sh' first."
    exit 1
fi

# EFI partition offset, as make-system-disk recorded it for this image.
EFI_OFFSET="$(grep -E '^EFI_OFFSET=[0-9]+$' "$DISK.info" 2>/dev/null | cut -d= -f2)" || true
if [ -z "$EFI_OFFSET" ]; then
    echo "ERROR: no EFI_OFFSET in $DISK.info. Run 'bash scripts/build.sh' first."
    exit 1
fi

# One run at a time owns this image's boot.conf. A run that patches, boots and
# resets holds build/.boot-conf.lock for its whole life through
# scripts/lib/boot-conf-lock.sh and exports BOOT_CONF_LOCK_HELD=1 to its own
# calls here; any other caller takes the lock for its one write and is
# refused while a launcher holds it, so it cannot patch or reset under a boot.
if [ "${BOOT_CONF_LOCK_HELD:-}" != "1" ] && command -v flock >/dev/null 2>&1; then
    exec 9>"$PROJECT/build/.boot-conf.lock"
    if ! flock -n 9; then
        echo "ERROR: another run is booting $DISK with a patched boot.conf (build/.boot-conf.lock)." >&2
        exit 1
    fi
fi

if [ "${1:-}" = "reset" ]; then
    # Restore original boot.conf
    mcopy -o -i "${DISK}@@${EFI_OFFSET}" "$BOOT_CONF" ::/EFI/ImpossibleOS/boot.conf
    echo "[PATCH] boot.conf reset to defaults"
    exit 0
fi

if [ $# -lt 2 ] || [ $(( $# % 2 )) -ne 0 ]; then
    echo "Usage: $0 <key> <value> [<key2> <value2> ...]   or   $0 reset"
    exit 1
fi

# Copy original, patch all key-value pairs
cp "$BOOT_CONF" "$TEMP_CONF"
while [ $# -ge 2 ]; do
    KEY="$1"
    VALUE="$2"
    shift 2

    if grep -q "^${KEY}=" "$TEMP_CONF"; then
        sed -i "s/^${KEY}=.*/${KEY}=${VALUE}/" "$TEMP_CONF"
    else
        echo "${KEY}=${VALUE}" >> "$TEMP_CONF"
    fi

    echo "[PATCH] boot.conf: ${KEY}=${VALUE}"
done

# Write patched config back into disk image EFI partition
mcopy -o -i "${DISK}@@${EFI_OFFSET}" "$TEMP_CONF" ::/EFI/ImpossibleOS/boot.conf
rm -f "$TEMP_CONF"
