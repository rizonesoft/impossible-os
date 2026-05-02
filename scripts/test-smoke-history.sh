#!/usr/bin/env bash
# ============================================================================
# test-smoke-history.sh -- cascade-failure fixture for the boot-error
#                          history ring (consumer half).
#
# Intentional sequence:
#   1. 3x boot a corrupt-kernel image (each boot fatals at load_kernel).
#   2. 1x boot the canonical clean image.
#   3. Assert the clean boot's serial log contains:
#        - "Recent boot history (4 attempts):"
#        - 3 lines with src=bl-kernel  err=0x0003  (BOOT_ERR_KERNEL_NOT_FOUND
#          or its closest sibling for the deleted kernel.exe path)
#        - 1 line with src=ebs-success or src=kernel-Phase3 (the clean boot)
#
# The OVMF_VARS pflash file is preserved across all 4 boots so the
# NVRAM ring + cookie accumulate correctly.  Each fatal boot increments
# the cookie by 1 (boot_fatal append) but does NOT add the EBS-success
# sentinel because it never reaches that path.  The final clean boot
# adds the EBS-success + kernel-Phase3 entries.
#
# Usage:  bash scripts/test-smoke-history.sh
#
# Exit codes:
#   0 = PASS (cascade visible in final boot serial)
#   1 = FAIL (build error, QEMU error, or expected log line missing)
#
# Requirements:
#   mtools (mcopy/mdel)  -- corrupts the kernel.exe on a copied disk
#   qemu-system-x86_64
#   OVMF firmware at /usr/share/OVMF/OVMF_{CODE,VARS}_4M.fd
# ============================================================================

set -euo pipefail

case "${1:-}" in
    -h|--help)
        cat <<'EOF'
test-smoke-history.sh -- cascade-failure boot-error history fixture

Usage:
  bash scripts/test-smoke-history.sh        Boot 3x corrupt + 1x clean, assert ring

Behavior:
  Builds the OS, makes a corrupt copy of system-disk.img, runs 3 QEMU
  boots with the corrupt image (each expected to fatal), then 1 boot
  with the canonical clean image, and asserts the cascade ring is
  visible in the final boot's klog block.

Environment:
  TIMEOUT_SEC    QEMU per-boot wait budget (default: 25; corrupt boots
                 typically fatal within ~3s, clean boot within ~3s).

Artifacts:
  build/smoke-history.{1,2,3}.log    raw serial captures of corrupt boots
  build/smoke-history.clean.log      serial capture of the final clean boot
  build/smoke-history.OVMF_VARS.fd   shared NVRAM pflash (accumulates across boots)
  build/smoke-history.corrupt.img    corrupt-kernel disk copy
EOF
        exit 0
        ;;
esac

GREEN='\033[0;32m'
RED='\033[0;31m'
DIM='\033[0;90m'
CYAN='\033[0;36m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"

CLEAN_DISK="$BUILD/system-disk.img"
CORRUPT_DISK="$BUILD/smoke-history.corrupt.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_RUN="$BUILD/smoke-history.OVMF_VARS.fd"
TIMEOUT_SEC="${TIMEOUT_SEC:-25}"

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Boot-Error History Cascade Smoke Fixture${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"

# ---- Preflight ----
if ! command -v mcopy >/dev/null || ! command -v mdel >/dev/null; then
    echo -e "${RED}FAIL: mtools not installed (need mcopy + mdel)${NC}"
    exit 1
fi
if ! command -v qemu-system-x86_64 >/dev/null; then
    echo -e "${RED}FAIL: qemu-system-x86_64 not on PATH${NC}"
    exit 1
fi
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
    echo -e "${RED}FAIL: OVMF firmware missing at /usr/share/OVMF/${NC}"
    exit 1
fi

# Build if disk image is absent.  Don't auto-rebuild on every run --
# the producer-side build is the smoke test's job; this fixture is
# additive and assumes a fresh build/system-disk.img already exists.
if [ ! -f "$CLEAN_DISK" ]; then
    echo -e "${DIM}  build/system-disk.img not found; running build...${NC}"
    bash "$SCRIPT_DIR/build.sh"
fi

# ---- Corrupt-disk preparation ----
echo -e "${CYAN}[1/5]${NC} Preparing corrupt-kernel disk image..."
cp "$CLEAN_DISK" "$CORRUPT_DISK"
# Delete \boot\kernel.exe on the EFI partition (offset 1 MiB = LBA
# 0x800 * 512 B).  load_kernel falls through the 3-path search and
# fatals with BOOT_ERR_KERNEL_NOT_FOUND.
if ! mdel -i "$CORRUPT_DISK@@1M" ::/boot/kernel.exe 2>&1; then
    echo -e "${RED}  FAIL: mdel could not delete /boot/kernel.exe${NC}"
    exit 1
fi
echo -e "  ${DIM}deleted /boot/kernel.exe on $CORRUPT_DISK${NC}"

# ---- Shared NVRAM pflash for all 4 boots ----
cp "$OVMF_VARS_SRC" "$OVMF_VARS_RUN"
echo -e "  ${DIM}NVRAM pflash: $OVMF_VARS_RUN (shared across all 4 boots)${NC}"

# ---- Helper: run one boot, return immediately when serial shows
#               either fatal or success markers ----
run_one_boot() {
    local disk="$1"
    local serial_log="$2"
    local kvm_flags=()
    if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
        kvm_flags=(-enable-kvm -cpu host)
    fi

    > "$serial_log"
    qemu-system-x86_64 \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$OVMF_VARS_RUN" \
        -drive "id=disk0,file=$disk,format=raw,if=none" \
        -device "ich9-ahci,id=ahci0" \
        -device "ide-hd,drive=disk0,bus=ahci0.0" \
        -m 2G \
        -serial file:"$serial_log" \
        -no-reboot \
        -no-shutdown \
        -display none \
        "${kvm_flags[@]}" \
        &
    local qpid=$!

    local i
    for i in $(seq 1 "$TIMEOUT_SEC"); do
        sleep 1
        # Stop early on fatal-path or successful-boot markers.
        if grep -q "Boot complete in\|BOOT FATAL\|Kernel ELF\|kernel.exe.*not found\|BOOT_ERR_" "$serial_log" 2>/dev/null; then
            break
        fi
        if ! kill -0 "$qpid" 2>/dev/null; then
            break
        fi
    done
    if kill -0 "$qpid" 2>/dev/null; then
        kill "$qpid" 2>/dev/null || true
        wait "$qpid" 2>/dev/null || true
    fi
}

# ---- 3 corrupt boots ----
for i in 1 2 3; do
    echo -e "${CYAN}[2/5]${NC} Corrupt boot $i/3..."
    run_one_boot "$CORRUPT_DISK" "$BUILD/smoke-history.$i.log"
    if grep -q "BOOT FATAL\|Kernel ELF\|kernel.exe.*not found" "$BUILD/smoke-history.$i.log"; then
        echo -e "  ${GREEN}fatal-path observed (expected)${NC}"
    else
        echo -e "  ${RED}FAIL: corrupt boot $i did not fatal -- aborting${NC}"
        exit 1
    fi
done

# ---- 1 clean boot ----
echo -e "${CYAN}[3/5]${NC} Clean boot (final)..."
run_one_boot "$CLEAN_DISK" "$BUILD/smoke-history.clean.log"
if ! grep -q "Boot complete in\|C:\\\\>" "$BUILD/smoke-history.clean.log"; then
    echo -e "  ${RED}FAIL: clean boot did not complete${NC}"
    echo -e "${DIM}  Serial log: $BUILD/smoke-history.clean.log${NC}"
    exit 1
fi
echo -e "  ${GREEN}clean boot completed${NC}"

# ---- Assertions on the final boot's serial log ----
echo -e "${CYAN}[4/5]${NC} Asserting cascade ring is visible..."
LOG="$BUILD/smoke-history.clean.log"
fail=0

if ! grep -q "Recent boot history" "$LOG"; then
    echo -e "  ${RED}MISSING: \"Recent boot history\" header${NC}"
    fail=1
fi

# Count entries in the ring block (lines after the header).
n_attempts=$(grep -E "Recent boot history \([0-9]+ attempts\)" "$LOG" | sed -E 's/.*\(([0-9]+) attempts\).*/\1/' | tail -1 || true)
if [ -z "$n_attempts" ] || [ "$n_attempts" -lt 4 ]; then
    echo -e "  ${RED}MISSING: at least 4 attempts in ring (got: ${n_attempts:-0})${NC}"
    fail=1
fi

# Three corrupt boots should each have a bl-kernel fatal entry; the
# clean boot should add either ebs-success or kernel-Phase3.
n_bl_kernel=$(grep -cE "src=bl-kernel" "$LOG" || true)
if [ "$n_bl_kernel" -lt 3 ]; then
    echo -e "  ${RED}MISSING: 3 src=bl-kernel entries (got: $n_bl_kernel)${NC}"
    fail=1
fi

n_clean_sentinel=$(grep -cE "src=ebs-success|src=kernel-Phase3" "$LOG" || true)
if [ "$n_clean_sentinel" -lt 1 ]; then
    echo -e "  ${RED}MISSING: clean-boot sentinel entry${NC}"
    fail=1
fi

# ---- Verdict ----
echo -e "${CYAN}[5/5]${NC} Verdict..."
if [ "$fail" -ne 0 ]; then
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  CASCADE FIXTURE FAILED${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${DIM}  Final clean-boot log: $LOG${NC}"
    echo -e "${DIM}  Corrupt-boot logs: $BUILD/smoke-history.{1,2,3}.log${NC}"
    exit 1
fi
echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  CASCADE FIXTURE PASSED${NC}"
echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
echo -e "  ${DIM}$n_attempts attempts in ring; $n_bl_kernel bl-kernel fatals + $n_clean_sentinel clean sentinel${NC}"
exit 0
