#!/usr/bin/env bash
# build.sh — Build wrapper with progress display, timing, and error extraction.
#
# Usage:
#   bash scripts/build.sh              Incremental build (only changed files)
#   bash scripts/build.sh clean        Full clean build (rm build/ + rebuild)
#   bash scripts/build.sh run          Incremental build + launch QEMU
#   bash scripts/build.sh clean run    Full clean build + launch QEMU
#
# Output is tee'd to build/build.log.  The last line is always one of:
#   === BUILD OK ===
#   === BUILD FAILED ===
#
# Verify completion: tail -1 build/build.log

set -uo pipefail

# ── Config ──────────────────────────────────────────────────────────────────
LOG="build/build.log"
mkdir -p build

# ── Parse arguments ─────────────────────────────────────────────────────────
DO_CLEAN=false
DO_RUN=false

if [[ $# -eq 0 ]]; then
    : # default: incremental build only
else
    for arg in "$@"; do
        case "$arg" in
            clean) DO_CLEAN=true ;;
            run)   DO_RUN=true ;;
            *)     echo "Unknown argument: $arg"; echo "Usage: build.sh [clean] [run]"; exit 1 ;;
        esac
    done
fi

# ── Helpers ─────────────────────────────────────────────────────────────────
BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[32m'
RED='\033[31m'
YELLOW='\033[33m'
CYAN='\033[36m'
RESET='\033[0m'

divider()  { printf '%b──────────────────────────────────────────────────%b\n' "$DIM" "$RESET"; }
header()   { printf '%b══════════════════════════════════════════════════%b\n' "$BOLD" "$RESET"; }

# Elapsed time since $1 (epoch seconds with nanoseconds)
elapsed() {
    local start=$1
    local now
    now=$(date +%s.%N)
    # Use awk for floating-point subtraction
    awk "BEGIN { printf \"%.1f\", $now - $start }"
}

# Run a make target with progress banner
# Usage: run_step <step_number> <total_steps> <label> <make_target>
STEP_TIMES=()
STEP_NAMES=()

run_step() {
    local num=$1 total=$2 label=$3
    shift 3
    local targets=("$@")
    local step_start
    step_start=$(date +%s.%N)

    divider | tee -a "$LOG"
    printf ' %b[%d/%d]%b %b%s%b\n' "$CYAN" "$num" "$total" "$RESET" "$BOLD" "$label" "$RESET" | tee -a "$LOG"
    divider | tee -a "$LOG"

    make "${targets[@]}" 2>&1 | tee -a "$LOG"
    local rc=${PIPESTATUS[0]}

    local secs
    secs=$(elapsed "$step_start")

    if [[ $rc -ne 0 ]]; then
        printf ' %b✗ %s FAILED%b (%ss)\n' "$RED" "$label" "$RESET" "$secs" | tee -a "$LOG"
        return $rc
    fi

    printf ' %b✓ %s%b (%ss)\n' "$GREEN" "$label" "$RESET" "$secs" | tee -a "$LOG"
    STEP_TIMES+=("$secs")
    STEP_NAMES+=("$label")
    return 0
}

# Print error summary from the log
print_errors() {
    header | tee -a "$LOG"
    printf ' %b BUILD FAILED%b\n' "${RED}${BOLD}" "$RESET" | tee -a "$LOG"
    header | tee -a "$LOG"

    local errors
    errors=$(grep -iE 'error:|undefined reference|fatal error' "$LOG" | grep -v '=== BUILD' | head -15)
    if [[ -n "$errors" ]]; then
        printf ' %bErrors:%b\n' "$YELLOW" "$RESET" | tee -a "$LOG"
        echo "$errors" | sed 's/^/   /' | tee -a "$LOG"
        divider | tee -a "$LOG"
    fi
}

# Print success summary with per-step times
print_summary() {
    local total_secs=$1
    header | tee -a "$LOG"
    printf ' %b BUILD OK%b (%ss total)\n' "${GREEN}${BOLD}" "$RESET" "$total_secs" | tee -a "$LOG"
    header | tee -a "$LOG"

    for i in "${!STEP_NAMES[@]}"; do
        printf ' %-16s %6ss\n' "${STEP_NAMES[$i]}" "${STEP_TIMES[$i]}" | tee -a "$LOG"
    done
    divider | tee -a "$LOG"
}

# ── Main ────────────────────────────────────────────────────────────────────
BUILD_START=$(date +%s.%N)

# Header
echo "" > "$LOG"
printf '\n%b Impossible OS — Build System%b\n' "${BOLD}${CYAN}" "$RESET" | tee -a "$LOG"
printf ' %b%s%b\n\n' "$DIM" "$(date '+%Y-%m-%d %H:%M:%S')" "$RESET" | tee -a "$LOG"

# Step counter
STEP=0
if $DO_CLEAN; then
    TOTAL=5  # clean + kernel + userland + efi + disk
else
    TOTAL=4  # kernel + userland + efi + disk
fi

# Clean (optional)
if $DO_CLEAN; then
    STEP=$((STEP + 1))
    run_step $STEP $TOTAL "Clean" "clean" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }
fi

# Kernel
STEP=$((STEP + 1))
run_step $STEP $TOTAL "Kernel" _increment_build kernel || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# Userland
STEP=$((STEP + 1))
run_step $STEP $TOTAL "Userland" "userland" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# EFI
STEP=$((STEP + 1))
run_step $STEP $TOTAL "EFI Boot" "grub-efi" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# System Disk
STEP=$((STEP + 1))
run_step $STEP $TOTAL "System Disk" "system-disk" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# Summary
TOTAL_SECS=$(elapsed "$BUILD_START")
print_summary "$TOTAL_SECS"
echo "=== BUILD OK ===" >> "$LOG"

# Run QEMU (optional)
if $DO_RUN; then
    divider | tee -a "$LOG"
    printf ' %b▶ Launching QEMU%b\n' "${CYAN}${BOLD}" "$RESET" | tee -a "$LOG"
    divider | tee -a "$LOG"
    make run 2>&1
fi
