#!/usr/bin/env bash
# run-qemu-tcg.sh — Launch Impossible OS under QEMU TCG (software emulation)
#
# Purpose: Forces QEMU to use TCG (software CPU emulation) instead of
#          KVM/WHPX hardware acceleration. This makes the OS select PIT
#          as g_system_timer (via platform_is_tcg() detection), matching
#          the QEMU TCG timer path in timer_hal_init().
#
# UTS Behavior:
#   - CPUID hypervisor leaf → "TCGTCGTCGTCG" → PLATFORM_QEMU_TCG
#   - timer_hal_init() → pit_init() → g_system_timer = &pit_driver
#   - PIT ticks at wall-clock rate (host-backed)
#   - LAPIC timer is NOT used for timekeeping
#
# Usage:
#   bash scripts/vm/run-qemu-tcg.sh           # default 1280×720
#   bash scripts/vm/run-qemu-tcg.sh 1920 1080 # custom resolution
#
# Compare with `bash scripts/build.sh run` which auto-detects KVM/WHPX
# and selects LAPIC timer instead.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$PROJECT/build"

XRES="${1:-1280}"
YRES="${2:-720}"

DISK="$BUILD/system-disk.img"
OVMF_CODE="$BUILD/OVMF_CODE_4M.fd"
OVMF_VARS="$BUILD/OVMF_VARS_4M.fd"
OVMF_VARS_CP="/tmp/OVMF_VARS_4M_tcg.fd"

# Preflight
if [ ! -f "$DISK" ]; then
    echo "ERROR: Missing $DISK"
    echo "Run: bash scripts/build.sh"
    exit 1
fi

# Copy OVMF firmware if missing
if [ ! -f "$OVMF_CODE" ]; then
    cp /usr/share/OVMF/OVMF_CODE_4M.fd "$BUILD/" 2>/dev/null || true
fi
if [ ! -f "$OVMF_VARS" ]; then
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$BUILD/" 2>/dev/null || true
fi

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "ERROR: OVMF firmware not found in $BUILD/"
    echo "Install: sudo apt install ovmf"
    exit 1
fi

# OVMF_VARS needs a writable copy
cp "$OVMF_VARS" "$OVMF_VARS_CP"

echo "══════════════════════════════════════════════════"
echo "  Impossible OS — QEMU TCG (Software Emulation)"
echo "══════════════════════════════════════════════════"
echo "  Accelerator : TCG (no KVM/WHPX)"
echo "  Resolution  : ${XRES}×${YRES}"
echo "  Timer Source: PIT (via platform_is_tcg())"
echo "  Disk        : $DISK"
echo "══════════════════════════════════════════════════"
echo ""
echo "  Expected UTS log lines:"
echo "    [platform] Detected: QEMU-TCG (CPUID 0x40000000)"
echo "    [timer] UTS: using PIT timer"
echo ""

qemu-system-x86_64 \
    -accel tcg \
    -cpu qemu64 \
    -smp 1 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS_CP" \
    -drive id=disk0,file="$DISK",format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -m 2G \
    -serial stdio \
    -vga none \
    -device "VGA,xres=$XRES,yres=$YRES" \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -device virtio-tablet-pci \
    -rtc base=localtime \
    -no-reboot
