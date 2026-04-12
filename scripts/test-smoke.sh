#!/usr/bin/env bash
# ============================================================================
# test-smoke.sh -- Automated QEMU smoke test for CI/CD
#
# Builds the OS, boots in QEMU headless mode, captures serial output,
# and checks for expected boot messages / absence of panics.
#
# Usage:  bash scripts/test-smoke.sh
#
# Exit codes:
#   0 = PASS (boot completed successfully)
#   1 = FAIL (panic, timeout, or build failure)
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"

ISO="$BUILD/os-build.iso"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD/OVMF_VARS_4M.fd"
SERIAL_LOG="$BUILD/smoke-test.log"
TIMEOUT_SEC=30

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Pass/Fail criteria ----
PASS_PATTERNS=(
    "Boot complete in"
)
FAIL_PATTERNS=(
    "KERNEL PANIC"
    "ASSERT FAILED"
    "triple fault"
    "General Protection Fault"
    "Page Fault"
    "[CRIT] ExitBootServices failed"
    "[FAIL] Kernel ELF corrupt"
    "[BOOT HALT]"
)

# ---- Bootloader presence checks (verified after boot) ----
# These patterns MUST appear in a healthy boot serial log.
BOOT_REQUIRED_PATTERNS=(
    "[BOOT] ELF segment"
    "[BOOT] Kernel found at"
    "[BOOT] Watchdog: armed"
    "[BOOT] Watchdog: disarmed"
    "[BOOT] ExitBootServices OK"
    "Boot info v1"
    "Boot device:"
)
# These patterns must NOT appear on a clean firmware boot.
BOOT_ABSENT_PATTERNS=(
    "[BOOT] mmap: overlap resolved"
    "[WARN] mmap: carve at cap"
    "[WARN] Memory map entry"
    "[FAIL] Kernel ELF corrupt"
    "[CRIT] ExitBootServices failed"
)

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Smoke Test${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ---- Step 1: Build ----
echo -e "${CYAN}[1/3]${NC} Building OS..."
cd "$REPO_ROOT"
bash scripts/build.sh clean >/dev/null 2>&1

BUILD_RESULT=$(tail -1 build/build.log 2>/dev/null || echo "UNKNOWN")
if [ "$BUILD_RESULT" != "=== BUILD OK ===" ]; then
    echo -e "${RED}SMOKE TEST FAILED: Build failed${NC}"
    echo -e "${DIM}  Check build/build.log for details${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} Build succeeded"

# ---- Step 2: Boot in QEMU ----
echo -e "${CYAN}[2/3]${NC} Booting in QEMU (headless, ${TIMEOUT_SEC}s timeout)..."

# Preflight
if [ ! -f "$ISO" ]; then
    echo -e "${RED}SMOKE TEST FAILED: $ISO not found${NC}"
    exit 1
fi
if [ ! -f "$OVMF_CODE" ]; then
    echo -e "${RED}SMOKE TEST FAILED: OVMF not found at $OVMF_CODE${NC}"
    exit 1
fi

cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# Build QEMU flags
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -cdrom "$ISO"
    -m 2G
    -serial file:"$SERIAL_LOG"
    -no-reboot
    -no-shutdown
    -display none
)

# Use KVM if available
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    echo -e "  ${DIM}KVM acceleration enabled${NC}"
fi

# Clear previous log
> "$SERIAL_LOG"

# Launch QEMU in background
qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
QEMU_PID=$!

# Wait for boot with timeout, checking serial log periodically
BOOT_PASSED=false
BOOT_FAILED=false
FAIL_REASON=""

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1

    # Check if QEMU crashed
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        BOOT_FAILED=true
        FAIL_REASON="QEMU exited unexpectedly"
        break
    fi

    # Check for fail patterns
    for pattern in "${FAIL_PATTERNS[@]}"; do
        if grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; then
            BOOT_FAILED=true
            FAIL_REASON="Detected: $pattern"
            break 2
        fi
    done

    # Check for pass patterns
    for pattern in "${PASS_PATTERNS[@]}"; do
        if grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; then
            BOOT_PASSED=true
            break 2
        fi
    done

    # Progress indicator
    printf "\r  ${DIM}Waiting... %d/${TIMEOUT_SEC}s${NC}  " "$i"
done
printf "\r"

# Kill QEMU
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# ---- Step 3: Results ----
echo -e "${CYAN}[3/3]${NC} Analyzing results..."
echo ""

LOG_LINES=$(wc -l < "$SERIAL_LOG" 2>/dev/null || echo 0)
echo -e "  ${DIM}Serial log: $SERIAL_LOG ($LOG_LINES lines)${NC}"

# ---- Bootloader pattern checks (TODO-02 §1-§18) ----
PATTERN_FAIL=false
for pattern in "${BOOT_REQUIRED_PATTERNS[@]}"; do
    if ! grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; then
        echo -e "  ${RED}MISSING:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done
for pattern in "${BOOT_ABSENT_PATTERNS[@]}"; do
    if grep -q "$pattern" "$SERIAL_LOG" 2>/dev/null; then
        echo -e "  ${RED}UNEXPECTED:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done
if [ "$PATTERN_FAIL" = true ]; then
    BOOT_FAILED=true
    FAIL_REASON="Bootloader pattern check failed (see MISSING/UNEXPECTED above)"
fi

if [ "$BOOT_FAILED" = true ]; then
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  SMOKE TEST FAILED: $FAIL_REASON${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo ""
    # Show relevant log lines
    echo -e "${DIM}Last 10 lines of serial output:${NC}"
    tail -10 "$SERIAL_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
elif [ "$BOOT_PASSED" = true ]; then
    # Extract boot time
    BOOT_TIME=$(grep -o "Boot complete in [0-9.]*s" "$SERIAL_LOG" 2>/dev/null | head -1 || echo "")
    echo ""
    echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}  SMOKE TEST PASSED${NC}"
    if [ -n "$BOOT_TIME" ]; then
        echo -e "  ${DIM}$BOOT_TIME${NC}"
    fi
    echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
    echo ""
    exit 0
else
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  SMOKE TEST FAILED: Timeout (${TIMEOUT_SEC}s)${NC}"
    echo -e "${RED}  Boot did not complete within the time limit.${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "${DIM}Last 10 lines of serial output:${NC}"
    tail -10 "$SERIAL_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
fi
