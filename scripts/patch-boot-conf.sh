#!/bin/bash
# =============================================================================
# patch-boot-conf.sh — Patch boot.conf in the system disk image
#
# Modifies a specific key=value in the EFI partition's boot.conf without
# rebuilding the entire disk image. Used by `make run-debug` and `make test`.
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

# EFI partition offset — must match Makefile EFI_OFFSET
EFI_OFFSET=$(grep -oP 'EFI_OFFSET\s*:=\s*\K\d+' "$PROJECT/Makefile" 2>/dev/null || echo "1048576")

if [ ! -f "$DISK" ]; then
    echo "ERROR: $DISK not found. Run 'bash scripts/build.sh' first."
    exit 1
fi

if [ "${1:-}" = "reset" ]; then
    # Restore original boot.conf
    mcopy -o -i "${DISK}@@${EFI_OFFSET}" "$BOOT_CONF" ::/EFI/ImpossibleOS/boot.conf
    echo "[PATCH] boot.conf reset to defaults"
    exit 0
fi

if [ $# -lt 2 ]; then
    echo "Usage: $0 <key> <value>   or   $0 reset"
    exit 1
fi

KEY="$1"
VALUE="$2"

# Copy original, patch the key
cp "$BOOT_CONF" "$TEMP_CONF"
if grep -q "^${KEY}=" "$TEMP_CONF"; then
    sed -i "s/^${KEY}=.*/${KEY}=${VALUE}/" "$TEMP_CONF"
else
    # Key doesn't exist — append it
    echo "${KEY}=${VALUE}" >> "$TEMP_CONF"
fi

# Write patched config back into disk image EFI partition
mcopy -o -i "${DISK}@@${EFI_OFFSET}" "$TEMP_CONF" ::/EFI/ImpossibleOS/boot.conf
rm -f "$TEMP_CONF"

echo "[PATCH] boot.conf: ${KEY}=${VALUE}"
