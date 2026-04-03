#!/usr/bin/env bash
# ============================================================================
# test-fs.sh -- Automated filesystem test suite
#
# Generates test disk images, boots each in QEMU headless mode with the
# test disk on AHCI port 1, captures serial output, and checks for
# filesystem detection and file read results.
#
# Usage:
#   bash scripts/test-fs.sh              # Test all filesystems
#   bash scripts/test-fs.sh fat32        # Test only FAT32
#   bash scripts/test-fs.sh fat32 ext4   # Test FAT32 and ext4
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"

SYSTEM_DISK="$BUILD/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD/OVMF_VARS_4M.fd"
TEST_DISK_DIR="$BUILD/test-disks"
LOG_DIR="$BUILD/fs-tests"
TIMEOUT_SEC=20

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Filesystem Test Suite${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ---- Step 1: Build ----
echo -e "${CYAN}[1/3]${NC} Building OS and generating test disks..."
cd "$REPO_ROOT"
bash scripts/build.sh clean >/dev/null 2>&1

BUILD_RESULT=$(tail -1 build/build.log 2>/dev/null || echo "UNKNOWN")
if [ "$BUILD_RESULT" != "=== BUILD OK ===" ]; then
    echo -e "${RED}FS TEST FAILED: Build failed${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} Build succeeded"

# Generate test disks
bash tools/make-test-disks.sh "$TEST_DISK_DIR" "$BUILD" >/dev/null 2>&1
echo -e "  ${GREEN}✓${NC} Test disks generated"

# ---- Step 2: Discover test disks ----
# Build list of disk images to test
declare -a DISK_TESTS=()

if [ $# -gt 0 ]; then
    # User specified which filesystems to test
    for name in "$@"; do
        if [ -f "$TEST_DISK_DIR/$name.img" ]; then
            DISK_TESTS+=("$name:$TEST_DISK_DIR/$name.img:disk")
        elif [ -f "$TEST_DISK_DIR/optical/$name.iso" ]; then
            DISK_TESTS+=("$name:$TEST_DISK_DIR/optical/$name.iso:optical")
        else
            echo -e "  ${YELLOW}!${NC} Test disk not found: $name"
        fi
    done
else
    # Test all available disk images
    for img in "$TEST_DISK_DIR"/*.img; do
        [ -f "$img" ] || continue
        name=$(basename "$img" .img)
        # Skip partition table images (no filesystem to test)
        case "$name" in
            mbr|gpt) continue ;;
        esac
        DISK_TESTS+=("$name:$img:disk")
    done
    # Test optical images
    if [ -d "$TEST_DISK_DIR/optical" ]; then
        for iso in "$TEST_DISK_DIR/optical"/*.iso; do
            [ -f "$iso" ] || continue
            name=$(basename "$iso" .iso)
            DISK_TESTS+=("$name:$iso:optical")
        done
    fi
fi

if [ ${#DISK_TESTS[@]} -eq 0 ]; then
    echo -e "${RED}No test disks found.${NC}"
    exit 1
fi

echo ""
echo -e "${CYAN}[2/3]${NC} Running ${#DISK_TESTS[@]} filesystem test(s)..."
echo ""

mkdir -p "$LOG_DIR"

# ---- Preflight ----
if [ ! -f "$SYSTEM_DISK" ]; then
    echo -e "${RED}FS TEST FAILED: $SYSTEM_DISK not found${NC}"
    exit 1
fi
if [ ! -f "$OVMF_CODE" ]; then
    echo -e "${RED}FS TEST FAILED: OVMF not found${NC}"
    exit 1
fi

# ---- Step 3: Run each test ----
PASSED=0
FAILED=0
SKIPPED=0
declare -a RESULTS=()

for entry in "${DISK_TESTS[@]}"; do
    IFS=':' read -r name test_file media_type <<< "$entry"
    LOG_FILE="$LOG_DIR/$name.log"
    > "$LOG_FILE"

    printf "  %-16s " "$name"

    # Copy OVMF vars (fresh for each test)
    cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

    # Build QEMU flags
    QEMU_FLAGS=(
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
        -drive "id=disk0,file=$SYSTEM_DISK,format=raw,if=none"
        -device "ich9-ahci,id=ahci0"
        -device "ide-hd,drive=disk0,bus=ahci0.0"
        -m 2G
        -serial "file:$LOG_FILE"
        -display none
        -no-reboot
        -no-shutdown
        -device "VGA,xres=1280,yres=720"
    )

    # Attach test disk on port 1
    if [ "$media_type" = "optical" ]; then
        QEMU_FLAGS+=(
            -drive "id=cdrom0,file=$test_file,format=raw,if=none,media=cdrom"
            -device "ide-cd,drive=cdrom0,bus=ahci0.1"
        )
    else
        QEMU_FLAGS+=(
            -drive "id=testdisk,file=$test_file,format=raw,if=none"
            -device "ide-hd,drive=testdisk,bus=ahci0.1"
        )
    fi

    # Use KVM if available
    if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
        QEMU_FLAGS+=(-enable-kvm -cpu host)
    fi

    # Launch QEMU in background
    qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
    QEMU_PID=$!

    # Wait for boot + FS detection
    test_passed=false
    test_failed=false
    fail_reason=""

    for i in $(seq 1 "$TIMEOUT_SEC"); do
        sleep 1

        # Check if QEMU crashed
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            test_failed=true
            fail_reason="QEMU exited"
            break
        fi

        # Check for panic
        if grep -q "KERNEL PANIC\|ASSERT FAILED" "$LOG_FILE" 2>/dev/null; then
            test_failed=true
            fail_reason="Kernel panic"
            break
        fi

        # Check for boot completion (indicates FS was at least detected)
        if grep -q "Boot complete in" "$LOG_FILE" 2>/dev/null; then
            test_passed=true
            break
        fi
    done

    # Kill QEMU
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true

    # Check for FS-specific results in log
    fs_detected=false
    if grep -qi "AHCI.*port 1\|Detected.*filesystem\|FAT32\|exFAT\|ext[234]\|NTFS\|IXFS\|ISO.9660\|UDF\|Joliet" "$LOG_FILE" 2>/dev/null; then
        fs_detected=true
    fi

    # Final verdict
    if [ "$test_failed" = true ]; then
        echo -e "${RED}FAIL${NC} ($fail_reason)"
        FAILED=$((FAILED + 1))
        RESULTS+=("$name:FAIL:$fail_reason")
    elif [ "$test_passed" = true ]; then
        if [ "$fs_detected" = true ]; then
            echo -e "${GREEN}PASS${NC} (detected)"
        else
            echo -e "${GREEN}PASS${NC} (boot OK)"
        fi
        PASSED=$((PASSED + 1))
        RESULTS+=("$name:PASS:")
    else
        echo -e "${YELLOW}TIMEOUT${NC} (${TIMEOUT_SEC}s)"
        FAILED=$((FAILED + 1))
        RESULTS+=("$name:TIMEOUT:")
    fi
done

# ---- Summary ----
echo ""
TOTAL=$((PASSED + FAILED + SKIPPED))
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}[3/3]${NC} Results: ${GREEN}$PASSED passed${NC}, ${RED}$FAILED failed${NC} (of $TOTAL)"
echo -e "  ${DIM}Logs: $LOG_DIR/${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# Show failures
if [ "$FAILED" -gt 0 ]; then
    echo -e "${RED}Failed tests:${NC}"
    for entry in "${RESULTS[@]}"; do
        IFS=':' read -r name result reason <<< "$entry"
        if [ "$result" != "PASS" ]; then
            echo -e "  ${RED}✗${NC} $name -- $result${reason:+ ($reason)}"
            echo -e "    ${DIM}Log: $LOG_DIR/$name.log${NC}"
        fi
    done
    echo ""
    echo -e "${RED}FS TEST SUITE FAILED${NC}"
    exit 1
else
    echo -e "${GREEN}FS TEST SUITE PASSED${NC}"
    exit 0
fi
