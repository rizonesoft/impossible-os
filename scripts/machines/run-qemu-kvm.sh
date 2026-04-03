#!/usr/bin/env bash
# run-qemu-kvm.sh -- Launch Impossible OS under QEMU with KVM acceleration
#
# Purpose: Forces QEMU to use KVM hardware acceleration. This makes the
#          OS detect a KVM hypervisor via CPUID and select LAPIC as
#          g_system_timer (via timer_hal_init() non-TCG path).
#
# UTS Behavior:
#   - CPUID hypervisor leaf → "KVMKVMKVM" → PLATFORM_QEMU_KVM
#   - timer_hal_init() → lapic_timer_calibrate() → g_system_timer = &lapic_driver
#   - LAPIC timer calibrated via Tier 1 (CPUID 0x40000010 paravirt freq)
#   - PIT IRQ0 is masked via ioapic_mask_irq(0)
#
# Usage:
#   bash scripts/machines/run-qemu-kvm.sh           # default 1280×720
#   bash scripts/machines/run-qemu-kvm.sh 1920 1080 # custom resolution
#
# Prerequisites:
#   - /dev/kvm must exist (KVM kernel modules loaded)
#   - User must be in the 'kvm' group: sudo usermod -aG kvm $USER

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$PROJECT/build"

XRES="${1:-1280}"
YRES="${2:-720}"

DISK="$BUILD/system-disk.img"
OVMF_CODE="$BUILD/OVMF_CODE_4M.fd"
OVMF_VARS="$BUILD/OVMF_VARS_4M.fd"
OVMF_VARS_CP="/tmp/OVMF_VARS_4M_kvm.fd"

# Preflight: check KVM availability
if [ ! -e /dev/kvm ]; then
    echo "ERROR: /dev/kvm not found -- KVM is not available"
    echo ""
    echo "Possible fixes:"
    echo "  1. Load KVM module: sudo modprobe kvm-intel (or kvm-amd)"
    echo "  2. Enable VT-x/AMD-V in BIOS"
    echo "  3. WSL2 note: KVM requires nested virtualization"
    echo ""
    echo "To test without KVM, use: bash scripts/machines/run-qemu-tcg.sh"
    exit 1
fi

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
echo "  Impossible OS -- QEMU KVM (Hardware Accelerated)"
echo "══════════════════════════════════════════════════"
echo "  Accelerator : KVM"
echo "  Resolution  : ${XRES}×${YRES}"
echo "  Timer Source: LAPIC (via timer_hal_init non-TCG)"
echo "  Disk        : $DISK"
echo "══════════════════════════════════════════════════"
echo ""
echo "  Expected UTS log lines:"
echo "    [platform] Detected: QEMU-KVM (CPUID 0x40000000)"
echo "    [timer] UTS: using LAPIC timer"
echo ""

qemu-system-x86_64 \
    -accel kvm \
    -cpu host \
    -smp 2 \
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
