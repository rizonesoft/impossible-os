#!/usr/bin/env bash
# ============================================================================
# run-qemu.sh -- Launch Impossible OS in QEMU (UEFI boot via OVMF + AHCI)
#
# Usage:
#   bash scripts/run-qemu.sh                # Default boot (2 CPUs, normal mode)
#   bash scripts/run-qemu.sh --debug-tests  # debug=1 in boot.conf (unit + boot tests)
#   bash scripts/run-qemu.sh --test-only    # test=1 in boot.conf (tests, then shutdown)
#   bash scripts/run-qemu.sh --single-cpu   # 1 CPU (for bisecting SMP bugs)
#   bash scripts/run-qemu.sh --debug        # Pause CPU, open GDB stub on :1234
#   bash scripts/run-qemu.sh --headless     # No display, serial only
#   bash scripts/run-qemu.sh --help         # Print usage and exit
#
# Boots the canonical GPT disk image (build/system-disk.img) via AHCI,
# produced by `bash scripts/build.sh`. The legacy GRUB+ISO path has been
# retired; use `bash scripts/build.sh run` for the standard developer flow.
#
# Machine-specific launchers (VirtualBox, QEMU-KVM, QEMU-TCG, NVMe, USB,
# secure-boot, 1cpu) live under scripts/machines/.
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Paths (canonical GPT disk produced by scripts/build.sh)
DISK="${PROJECT_DIR}/build/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="${PROJECT_DIR}/build/OVMF_VARS_4M.fd"

print_help() {
    cat <<'EOF'
Impossible OS -- QEMU launcher

Usage:
  bash scripts/run-qemu.sh                Default boot (2 CPUs)
  bash scripts/run-qemu.sh --debug-tests  debug=1 in boot.conf
  bash scripts/run-qemu.sh --test-only    test=1 in boot.conf
  bash scripts/run-qemu.sh --single-cpu   1 CPU
  bash scripts/run-qemu.sh --debug        Paused with GDB stub on :1234
  bash scripts/run-qemu.sh --headless     No display, serial only
  bash scripts/run-qemu.sh --help         Show this help

Boots:
  build/system-disk.img via AHCI; UEFI via /usr/share/OVMF/OVMF_CODE_4M.fd.

Inputs:
  Expects a prior `bash scripts/build.sh` that produced build/system-disk.img.
  If the image is missing, run that first.

Related wrappers:
  bash scripts/build.sh run            Canonical build + launch.
  bash scripts/debug.sh                QEMU + GDB with kernel symbols.
  scripts/machines/run-qemu-kvm.sh     KVM-forced launch.
  scripts/machines/run-qemu-tcg.sh     TCG-forced launch.
  scripts/machines/run-vbox.sh         VirtualBox.

EOF
}

# --- Parse arguments first (before any side effects) ---
DEBUG=false
HEADLESS=false
SMP=0  # 0 = auto (2 on KVM, 1 on TCG)
PATCH_CONF=""

for arg in "$@"; do
    case "$arg" in
        -h|--help)
            print_help
            exit 0
            ;;
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
            echo ""
            print_help
            exit 1
            ;;
    esac
done

# --- Preflight: disk and firmware ---
if [ ! -f "$DISK" ]; then
    echo "ERROR: $DISK not found."
    echo "Run 'bash scripts/build.sh' first to produce the canonical disk image."
    exit 1
fi

if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
    echo "ERROR: OVMF firmware not found (expected at $OVMF_CODE and $OVMF_VARS_SRC)."
    echo "Install with: bash scripts/setup.sh"
    exit 1
fi

# Copy OVMF_VARS to a writable location (EFI variable store)
cp -n "$OVMF_VARS_SRC" "$OVMF_VARS_CP" 2>/dev/null || true

# --- Base QEMU flags (match Makefile's run: target) ---
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -drive "id=disk0,file=$DISK,format=raw,if=none"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
    -m 2G
    -serial stdio
    -vga none
    -device "VGA,xres=1280,yres=720"
    -device "rtl8139,netdev=net0"
    -netdev "user,id=net0"
    -device virtio-tablet-pci
    -rtc base=localtime
    -no-reboot
)

# --- Accelerator selection ---
HAS_KVM=false
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    HAS_KVM=true
    echo "[QEMU] KVM acceleration enabled"
else
    QEMU_FLAGS+=(-cpu Haswell)
    echo "[QEMU] KVM not available, using TCG (slower)"
fi

# --- Boot.conf patch (reverted on exit) ---
if [ -n "$PATCH_CONF" ]; then
    KEY="${PATCH_CONF%%=*}"
    VAL="${PATCH_CONF##*=}"
    bash "$SCRIPT_DIR/patch-boot-conf.sh" "$KEY" "$VAL"
    trap 'bash "$SCRIPT_DIR/patch-boot-conf.sh" reset' EXIT
fi

# --- Auto SMP: 2 for KVM, 1 for TCG (unless overridden by --single-cpu) ---
if [ "$SMP" -eq 0 ]; then
    if [ "$HAS_KVM" = true ]; then
        SMP=2
    else
        SMP=1
    fi
fi
QEMU_FLAGS+=(-smp "$SMP")
echo "[QEMU] CPUs: $SMP"

# --- Debug mode: pause CPU, open GDB port ---
if [ "$DEBUG" = true ]; then
    QEMU_FLAGS+=(-s -S)
    echo "[QEMU] Debug mode: CPU paused, connect GDB to localhost:1234"
fi

# --- Headless mode: no display window ---
if [ "$HEADLESS" = true ]; then
    QEMU_FLAGS+=(-display none)
    echo "[QEMU] Headless mode: no display window"
fi

echo "[QEMU] Booting $DISK via UEFI + AHCI..."
echo ""

qemu-system-x86_64 "${QEMU_FLAGS[@]}"
