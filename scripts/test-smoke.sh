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

DISK="$BUILD/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD/OVMF_VARS_4M.fd"
SERIAL_LOG="$BUILD/smoke-test.log"
STRIPPED_LOG="$BUILD/smoke-test.stripped.log"
TIMEOUT_SEC="${TIMEOUT_SEC:-30}"

# Legacy smoke test: superseded by `bash scripts/test.sh`. Retained as a light
# boot-only sanity check (no unit-test suite). Boots the canonical GPT disk
# image via AHCI, matching Makefile's run: target. Do not re-introduce the
# grub-mkrescue ISO path -- that is retired.

# Strip ANSI color and control sequences from the serial log into a derived
# file before pattern matching. The kernel emits `\x1b[31m[FAIL]...` etc., and
# OVMF emits screen-clear sequences that collide with any bracketed substring.
# Matching the stripped copy keeps patterns robust against future color edits.
strip_ansi() {
    sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b[=>]//g' "$SERIAL_LOG" > "$STRIPPED_LOG" 2>/dev/null || :
}

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Pass/Fail criteria ----
# PASS requires BOTH signals: the kernel's "Boot complete in" sentinel AND the
# shell's `C:\>` prompt. Either alone is insufficient (a kernel can print the
# sentinel and then panic in userland). All matches run against STRIPPED_LOG,
# so bracketed patterns are literal and ANSI colors never confuse grep.
PASS_PATTERNS_ALL=(
    "Boot complete in"
    'C:\>'
)
FAIL_PATTERNS=(
    "KERNEL PANIC"
    "ASSERT FAILED"
    "triple fault"
    "General Protection Fault"
    "Page Fault"
    "Double Fault"
    "[CRIT] ExitBootServices failed"
    "[FAIL] Kernel ELF corrupt"
    "[BOOT HALT]"
)

# ---- Bootloader + kernel presence checks (verified after boot) ----
# These MUST appear in a healthy boot. Matched as fixed strings against the
# ANSI-stripped log.
BOOT_REQUIRED_PATTERNS=(
    "[BOOT] ELF segment"
    "[BOOT] Kernel found at"
    "[BOOT] Watchdog: armed"
    "[BOOT] Watchdog: disarmed"
    "[BOOT] ExitBootServices OK"
    "[PHASE0] BOOT_INFO"
    "Boot complete in"
    'C:\>'
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
if [ ! -f "$DISK" ]; then
    echo -e "${RED}SMOKE TEST FAILED: $DISK not found${NC}"
    echo -e "${DIM}  Run 'bash scripts/build.sh' first.${NC}"
    exit 1
fi
if [ ! -f "$OVMF_CODE" ]; then
    echo -e "${RED}SMOKE TEST FAILED: OVMF not found at $OVMF_CODE${NC}"
    exit 1
fi

cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# Boot the canonical GPT image via AHCI (matches Makefile run: target).
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -drive "id=disk0,file=$DISK,format=raw,if=none"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
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

# Cleanup trap: kill QEMU on normal exit, SIGINT, SIGTERM. Idempotent so the
# existing post-polling kill (if it still runs) is harmless.
cleanup_qemu() {
    local ec=$?
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    exit "$ec"
}
trap cleanup_qemu EXIT INT TERM

# Wait for boot with timeout, checking serial log periodically
BOOT_PASSED=false
BOOT_FAILED=false
FAIL_REASON=""

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    strip_ansi

    # Check if QEMU crashed
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        BOOT_FAILED=true
        FAIL_REASON="QEMU exited unexpectedly"
        break
    fi

    # Check for fail patterns (match against stripped log)
    for pattern in "${FAIL_PATTERNS[@]}"; do
        if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            BOOT_FAILED=true
            FAIL_REASON="Detected: $pattern"
            break 2
        fi
    done

    # PASS requires ALL patterns in PASS_PATTERNS_ALL (AND-set, not OR)
    all_found=true
    for pattern in "${PASS_PATTERNS_ALL[@]}"; do
        if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            all_found=false
            break
        fi
    done
    if [ "$all_found" = true ]; then
        BOOT_PASSED=true
        break
    fi

    # Progress indicator
    printf "\r  ${DIM}Waiting... %d/${TIMEOUT_SEC}s${NC}  " "$i"
done
printf "\r"

# Kill QEMU (trap also does this; harmless repeat)
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# Final strip in case the loop exited before the last pass
strip_ansi

# ---- Step 3: Results ----
echo -e "${CYAN}[3/3]${NC} Analyzing results..."
echo ""

LOG_LINES=$(wc -l < "$SERIAL_LOG" 2>/dev/null || echo 0)
echo -e "  ${DIM}Serial log: $SERIAL_LOG ($LOG_LINES lines; ANSI-stripped: $STRIPPED_LOG)${NC}"

# ---- Bootloader + kernel presence checks ----
PATTERN_FAIL=false
for pattern in "${BOOT_REQUIRED_PATTERNS[@]}"; do
    if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}MISSING:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done
for pattern in "${BOOT_ABSENT_PATTERNS[@]}"; do
    if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
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
    tail -10 "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  /'
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
    tail -10 "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
fi
