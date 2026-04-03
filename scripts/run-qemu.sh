#!/bin/bash
# =============================================================================
# scripts/run-qemu.sh -- Launch Impossible OS in QEMU (UEFI boot via OVMF)
#
# Usage:
#   ./scripts/run-qemu.sh                  # Normal boot (2 CPUs)
#   ./scripts/run-qemu.sh --debug-tests    # Boot with debug=1 (unit + boot tests)
#   ./scripts/run-qemu.sh --test-only      # Boot with test=1 (unit tests, then shutdown)
#   ./scripts/run-qemu.sh --single-cpu     # 1 CPU (for bisecting SMP bugs)
#   ./scripts/run-qemu.sh --debug          # Paused boot waiting for GDB on :1234
#   ./scripts/run-qemu.sh --headless       # No display, serial only
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

# Default SMP: 2 CPUs for KVM (test SMP), 1 for TCG (too slow)
SMP=0  # 0 = auto

# Check for KVM support
HAS_KVM=false
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    HAS_KVM=true
    echo "[QEMU] KVM acceleration enabled"
else
    echo "[QEMU] KVM not available, using TCG (slower)"
fi

# Parse arguments
DEBUG=false
HEADLESS=false
PATCH_CONF=""

for arg in "$@"; do
    case "$arg" in
        --debug)
            DEBUG=true
            ;;
        --headless)
            HEADLESS=true
            ;;
        --single-cpu)
            SMP=1
            ;;
        --debug-tests)
            PATCH_CONF="debug=1"
            ;;
        --test-only)
            PATCH_CONF="test=1"
            ;;
        *)
            echo "Unknown argument: $arg"
            echo "Usage: $0 [--debug] [--headless] [--single-cpu] [--debug-tests] [--test-only]"
            exit 1
            ;;
    esac
done

# Patch boot.conf if requested
if [ -n "$PATCH_CONF" ]; then
    KEY="${PATCH_CONF%%=*}"
    VAL="${PATCH_CONF##*=}"
    bash "$SCRIPT_DIR/patch-boot-conf.sh" "$KEY" "$VAL"
    trap 'bash "$SCRIPT_DIR/patch-boot-conf.sh" reset' EXIT
fi

# Auto SMP: 2 for KVM, 1 for TCG
if [ "$SMP" -eq 0 ]; then
    if [ "$HAS_KVM" = true ]; then
        SMP=2
    else
        SMP=1
    fi
fi
QEMU_FLAGS+=(-smp "$SMP")
echo "[QEMU] CPUs: $SMP"

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
