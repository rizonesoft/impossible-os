#!/bin/bash
# =============================================================================
# test.sh — Build, boot QEMU headless, run kernel unit tests, report results
#
# Usage:
#   bash scripts/test.sh              # run all test suites
#   bash scripts/test.sh SUITE=pmm    # run only suites matching "pmm"
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed or timeout
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT/build"
DISK="$BUILD_DIR/system-disk.img"
TEST_LOG="$BUILD_DIR/test.log"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD_DIR/OVMF_VARS_4M.fd"
TIMEOUT="${TIMEOUT:-60}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

# Parse optional SUITE= argument
SUITE_FILTER=""
for arg in "$@"; do
    case "$arg" in
        SUITE=*) SUITE_FILTER="${arg#SUITE=}" ;;
    esac
done

# --- Step 1: Build ---
echo -e "${CYAN}${BOLD}[TEST]${RESET} Building kernel..."
if ! bash "$PROJECT/scripts/build.sh" > /dev/null 2>&1; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} Build failed. Check build/build.log"
    exit 1
fi
echo -e "${GREEN}[TEST]${RESET} Build OK"

# --- Step 2: Patch boot.conf for test mode ---
bash "$PROJECT/scripts/patch-boot-conf.sh" test 1 > /dev/null

# --- Step 3: Boot QEMU headless ---
cp -n "$OVMF_VARS" "$OVMF_VARS_CP" 2>/dev/null || true
rm -f "$TEST_LOG"

# Detect acceleration: KVM > TCG
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_ARGS="-accel kvm -cpu host"
    ACCEL_NAME="KVM"
else
    ACCEL_ARGS="-accel tcg -cpu Haswell"
    ACCEL_NAME="TCG (slow)"
    echo -e "${YELLOW}[TEST]${RESET} KVM not available — using TCG (add user to kvm group for 10x speedup)"
fi

echo -e "${CYAN}[TEST]${RESET} Booting QEMU headless (${ACCEL_NAME}, ${TIMEOUT}s timeout)..."
timeout "$TIMEOUT" qemu-system-x86_64 \
    $ACCEL_ARGS \
    -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS_CP" \
    -drive id=disk0,file="$DISK",format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -m 2G \
    -serial file:"$TEST_LOG" \
    -display none \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -device virtio-tablet-pci \
    -rtc base=localtime \
    -no-reboot 2>/dev/null
QEMU_EXIT=$?

# --- Step 4: Restore boot.conf ---
bash "$PROJECT/scripts/patch-boot-conf.sh" reset > /dev/null

# --- Step 5: Parse results ---
if [ ! -f "$TEST_LOG" ]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} No test output (QEMU did not produce serial log)"
    exit 1
fi

# Check for timeout (exit code 124)
if [ "$QEMU_EXIT" -eq 124 ]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} QEMU timed out after ${TIMEOUT}s"
    echo ""
    echo -e "${YELLOW}Last 10 lines of serial output:${RESET}"
    tail -10 "$TEST_LOG" 2>/dev/null || true
    exit 1
fi

# Look for the summary line
SUMMARY=$(grep -E '=== [0-9]+ tests? passed' "$TEST_LOG" 2>/dev/null | tail -1 || true)

if [ -z "$SUMMARY" ]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} No test summary found in output"
    echo ""
    echo -e "${YELLOW}Last 20 lines of serial output:${RESET}"
    tail -20 "$TEST_LOG" 2>/dev/null || true
    exit 1
fi

# Extract numbers
PASSED=$(echo "$SUMMARY" | grep -oP '\d+(?= tests? passed)' || echo "0")
FAILED=$(echo "$SUMMARY" | grep -oP '\d+(?= FAILED)' || echo "0")

# Show individual test results (filtered by suite if requested)
echo ""
if [ -n "$SUITE_FILTER" ]; then
    echo -e "${CYAN}[TEST]${RESET} Filter: SUITE=${SUITE_FILTER}"
    echo ""
fi

# Show suite results
grep -E '\[ OK \].*TEST:.*::|\[FAIL\].*TEST:.*::' "$TEST_LOG" 2>/dev/null | while IFS= read -r line; do
    if [ -n "$SUITE_FILTER" ]; then
        echo "$line" | grep -qi "$SUITE_FILTER" || continue
    fi
    if echo "$line" | grep -q '\[FAIL\]'; then
        echo -e "  ${RED}FAIL${RESET}  $(echo "$line" | sed 's/.*TEST: //')"
    else
        echo -e "  ${GREEN} OK ${RESET}  $(echo "$line" | sed 's/.*TEST: \[ OK \] //')"
    fi
done

# Show any FAIL lines that aren't suite results (boot test failures)
grep -E '\[FAIL\]' "$TEST_LOG" 2>/dev/null | grep -v '::' | while IFS= read -r line; do
    echo -e "  ${RED}FAIL${RESET}  $(echo "$line" | sed 's/.*TEST: //')"
done

# Final verdict
echo ""
if [ "${FAILED:-0}" = "0" ] || [ -z "$FAILED" ]; then
    echo -e "${GREEN}${BOLD}PASS: ${PASSED} tests passed${RESET}"
    exit 0
else
    TOTAL=$((PASSED + FAILED))
    echo -e "${RED}${BOLD}FAIL: ${FAILED} of ${TOTAL} failed${RESET}"
    exit 1
fi
