#!/bin/bash
# =============================================================================
# scripts/run-qemu.sh — Launch Impossible OS in QEMU (UEFI boot via OVMF)
#
# Usage:
#   ./scripts/run-qemu.sh          # Normal boot
#   ./scripts/run-qemu.sh --debug  # Paused boot waiting for GDB on :1234
#   ./scripts/run-qemu.sh --headless  # No display, serial only
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Paths
ISO="${PROJECT_DIR}/build/os-build.iso"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="${PROJECT_DIR}/build/OVMF_VARS_4M.fd"

# Verify ISO exists
if [ ! -f "$ISO" ]; then
    echo "ERROR: $ISO not found. Run 'make all' first."
    exit 1
fi

# Verify OVMF exists
if [ ! -f "$OVMF_CODE" ]; then
    echo "ERROR: OVMF not found at $OVMF_CODE"
    echo "Install with: sudo apt install -y ovmf"
    exit 1
fi

# Copy OVMF VARS (writable EFI variable store)
cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# Base QEMU flags
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -cdrom "$ISO"
    -m 2G
    -serial stdio
    -no-reboot
    -no-shutdown
)

# Check for KVM support
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    echo "[QEMU] KVM acceleration enabled"
else
    echo "[QEMU] KVM not available, using TCG (slower)"
fi

# Parse arguments
DEBUG=false
HEADLESS=false

for arg in "$@"; do
    case "$arg" in
        --debug)
            DEBUG=true
            ;;
        --headless)
            HEADLESS=true
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [--debug] [--headless]"
            exit 1
            ;;
    esac
done

# Debug mode: pause CPU, open GDB port
if [ "$DEBUG" = true ]; then
    QEMU_FLAGS+=(-s -S)
    echo "[QEMU] Debug mode: CPU paused, connect GDB to localhost:1234"
fi

# Headless mode: no display window
if [ "$HEADLESS" = true ]; then
    QEMU_FLAGS+=(-display none)
    echo "[QEMU] Headless mode: no display window"
fi

echo "[QEMU] Booting $ISO via UEFI..."
echo ""

qemu-system-x86_64 "${QEMU_FLAGS[@]}"
