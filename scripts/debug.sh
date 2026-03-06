#!/bin/bash
# =============================================================================
# scripts/debug.sh — Launch QEMU paused + connect GDB to the kernel
#
# Usage:
#   ./scripts/debug.sh
#
# This will:
#   1. Launch QEMU paused (waiting for GDB)
#   2. Open GDB in a second terminal, connected to localhost:1234
#   3. Load kernel symbols from build/kernel.elf
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
KERNEL_ELF="${PROJECT_DIR}/build/kernel.elf"

# Verify kernel ELF exists
if [ ! -f "$KERNEL_ELF" ]; then
    echo "ERROR: $KERNEL_ELF not found. Run 'make all' first."
    exit 1
fi

echo "=== Impossible OS Debugger ==="
echo ""
echo "Starting QEMU in debug mode (paused)..."
echo "GDB will connect to localhost:1234"
echo ""
echo "Useful GDB commands:"
echo "  break kernel_main    — breakpoint at kernel entry"
echo "  continue             — resume execution"
echo "  info registers       — show CPU registers"
echo "  x/10i \$eip           — disassemble at current instruction"
echo "  bt                   — backtrace"
echo ""

# Launch QEMU in background
"${SCRIPT_DIR}/run-qemu.sh" --debug &
QEMU_PID=$!

# Give QEMU a moment to start
sleep 1

# Connect GDB
echo "[GDB] Connecting to QEMU..."
gdb -q "$KERNEL_ELF" \
    -ex "target remote localhost:1234" \
    -ex "break kernel_main" \
    -ex "echo \n[GDB] Connected. Type 'continue' to start booting.\n"

# Clean up QEMU when GDB exits
kill $QEMU_PID 2>/dev/null || true
wait $QEMU_PID 2>/dev/null || true
echo "[DEBUG] Session ended."
