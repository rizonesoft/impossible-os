#!/usr/bin/env bash
# build.sh — Build wrapper with progress bar, timing, and error extraction.
#
# Usage:
#   bash scripts/build.sh              Incremental build (only changed files)
#   bash scripts/build.sh clean        Full clean build (rm build/ + rebuild)
#   bash scripts/build.sh run          Incremental build + launch QEMU
#   bash scripts/build.sh run-usb      Build + launch QEMU with xHCI USB disk
#   bash scripts/build.sh clean run    Full clean build + launch QEMU
#   bash scripts/build.sh --jobs=4     Build with 4 parallel jobs (default: nproc)
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
DO_RUN_USB=false
JOBS=$(nproc 2>/dev/null || echo 4)

if [[ $# -eq 0 ]]; then
    : # default: incremental build only
else
    for arg in "$@"; do
        case "$arg" in
            clean) DO_CLEAN=true ;;
            run)     DO_RUN=true ;;
            run-usb) DO_RUN_USB=true ;;
            --jobs=*) JOBS="${arg#--jobs=}" ;;
            *)     echo "Unknown argument: $arg"; echo "Usage: build.sh [clean] [run|run-usb] [--jobs=N]"; exit 1 ;;
        esac
    done
fi

# Make flags for parallel compilation
MAKE_FLAGS="-j${JOBS}"

# Bear wraps make on clean builds to generate compile_commands.json for clangd
BEAR_PREFIX=""
if command -v bear &>/dev/null && [ "$DO_CLEAN" = true ]; then
    BEAR_PREFIX="bear --append --"
    rm -f compile_commands.json
    echo "[BEAR] Will generate compile_commands.json" | tee -a "$LOG"
fi

# ── Helpers ─────────────────────────────────────────────────────────────────
BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[32m'
RED='\033[31m'
YELLOW='\033[33m'
CYAN='\033[36m'
WHITE='\033[37m'
RESET='\033[0m'
CLEAR_LINE='\033[2K'

divider()  { printf '%b──────────────────────────────────────────────────%b\n' "$DIM" "$RESET"; }
header()   { printf '%b══════════════════════════════════════════════════%b\n' "$BOLD" "$RESET"; }

# Elapsed time since $1 (epoch seconds with nanoseconds)
elapsed() {
    local start=$1
    local now
    now=$(date +%s.%N)
    awk "BEGIN { t = $now - $start; if (t < 0) t = 0; printf \"%.1f\", t }"
}

# Count source files that make will compile (for progress bar)
count_sources() {
    find src/ -name '*.c' -o -name '*.asm' 2>/dev/null | wc -l
}

# ── Progress bar renderer ──────────────────────────────────────────────────
# Draws: ██████████░░░░░░░░░░  42/76  55%  [CC] gfx_core.c
# Only shown on terminal (stderr), not in log file.
draw_progress() {
    local current=$1 total=$2 filename=$3
    local bar_width=24

    if [[ $total -eq 0 ]]; then return; fi

    local pct=$(( current * 100 / total ))
    local filled=$(( current * bar_width / total ))
    local empty=$(( bar_width - filled ))

    # Build the bar
    local bar=""
    for ((i=0; i<filled; i++)); do bar+="█"; done
    for ((i=0; i<empty; i++));  do bar+="░"; done

    # Color transitions: cyan → green as we approach 100%
    local bar_color
    if   [[ $pct -ge 90 ]]; then bar_color="$GREEN"
    elif [[ $pct -ge 50 ]]; then bar_color="$CYAN"
    else                         bar_color="$WHITE"
    fi

    # Render on stderr (terminal only) with carriage return
    printf '\r%b%b %s %b%3d/%d  %3d%%%b  %s%b' \
        "$CLEAR_LINE" "$bar_color" "$bar" \
        "$BOLD" "$current" "$total" "$pct" "$RESET" \
        "$filename" "$RESET" >&2
}

clear_progress() {
    printf '\r%b' "$CLEAR_LINE" >&2
}

# ── Step runners ───────────────────────────────────────────────────────────
STEP_TIMES=()
STEP_NAMES=()

# Run a make target with progress banner (no progress bar)
run_step() {
    local num=$1 total=$2 label=$3
    shift 3
    local targets=("$@")
    local step_start
    step_start=$(date +%s.%N)

    divider | tee -a "$LOG"
    printf ' %b[%d/%d]%b %b%s%b\n' "$CYAN" "$num" "$total" "$RESET" "$BOLD" "$label" "$RESET" | tee -a "$LOG"
    divider | tee -a "$LOG"

    make $MAKE_FLAGS "${targets[@]}" 2>&1 | tee -a "$LOG"
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

# Run kernel build with per-file progress bar
run_kernel_step() {
    local num=$1 total=$2
    local step_start
    step_start=$(date +%s.%N)

    local src_total
    src_total=$(count_sources)

    divider | tee -a "$LOG"
    printf ' %b[%d/%d]%b %bKernel%b  (%d source files)\n' \
        "$CYAN" "$num" "$total" "$RESET" "$BOLD" "$RESET" "$src_total" | tee -a "$LOG"
    divider | tee -a "$LOG"

    local compiled=0
    $BEAR_PREFIX make $MAKE_FLAGS _increment_build kernel 2>&1 | while IFS= read -r line; do
        # Log every line
        echo "$line" >> "$LOG"

        # Check for compilation markers
        case "$line" in
            "[CC]"*|"[AS]"*|"[CC/SSE2]"*)
                compiled=$((compiled + 1))
                # Extract just the filename from e.g. "[CC] src/kernel/gfx/gfx_core.c"
                local fname
                fname=$(echo "$line" | sed 's/^\[.*\] //' | xargs basename 2>/dev/null || echo "$line")
                draw_progress "$compiled" "$src_total" "$fname"
                ;;
            "[LD]"*|"[KERNEL]"*)
                # Linker/kernel steps — show at 100% without incrementing count
                draw_progress "$src_total" "$src_total" "Linking..."
                ;;
            *)
                # Non-compilation lines: print normally
                echo "$line"
                ;;
        esac
    done
    local rc=${PIPESTATUS[0]}

    clear_progress

    local secs
    secs=$(elapsed "$step_start")

    if [[ $rc -ne 0 ]]; then
        printf ' %b✗ Kernel FAILED%b (%ss)\n' "$RED" "$RESET" "$secs" | tee -a "$LOG"
        return $rc
    fi

    printf ' %b✓ Kernel%b (%ss)\n' "$GREEN" "$RESET" "$secs" | tee -a "$LOG"
    STEP_TIMES+=("$secs")
    STEP_NAMES+=("Kernel")
    return 0
}

# ── Error and summary display ──────────────────────────────────────────────

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
printf ' %b%s  (%d parallel jobs)%b\n\n' "$DIM" "$(date '+%Y-%m-%d %H:%M:%S')" "$JOBS" "$RESET" | tee -a "$LOG"

# Step counter
STEP=0
if $DO_CLEAN; then
    TOTAL=6  # clean + kernel + userland + efi + sign + disk
else
    TOTAL=5  # kernel + userland + efi + sign + disk
fi

# Clean (optional)
if $DO_CLEAN; then
    STEP=$((STEP + 1))
    run_step $STEP $TOTAL "Clean" "clean" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }
    # make clean removes build/ — re-create it for the log file
    mkdir -p build
    echo "" > "$LOG"
fi

# Kernel (with progress bar)
STEP=$((STEP + 1))
run_kernel_step $STEP $TOTAL || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# ── BSS / user-mode address collision check ─────────────────────────────────
# Kernel BSS must not overlap the user-mode ELF base address (user/user.ld).
# If BSS grows past USER_BASE, user programs overwrite kernel data at runtime.
USER_BASE=0x800000
if [[ -f build/kernel.map ]]; then
    BSS_END_HEX=$(grep ' [bB] ' build/kernel.map | awk '{print $1}' | sort | tail -1)
    if [[ -n "$BSS_END_HEX" ]]; then
        BSS_END=$((16#${BSS_END_HEX}))
        if [[ $BSS_END -ge $USER_BASE ]]; then
            printf '\n %b✗ BSS COLLISION:%b Kernel BSS end (0x%s) >= user base (0x%X)\n' \
                "$RED" "$RESET" "$BSS_END_HEX" "$USER_BASE" | tee -a "$LOG"
            printf '   Increase USER_BASE in user/user.ld or reduce kernel static allocations.\n' | tee -a "$LOG"
            echo "=== BUILD FAILED ===" >> "$LOG"
            exit 1
        fi
        printf ' %b✓ BSS check:%b kernel BSS end 0x%s < user base 0x%X\n' \
            "$GREEN" "$RESET" "$BSS_END_HEX" "$USER_BASE" | tee -a "$LOG"
    fi
fi

# Userland
STEP=$((STEP + 1))
run_step $STEP $TOTAL "Userland" "userland" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# EFI
STEP=$((STEP + 1))
run_step $STEP $TOTAL "EFI Boot" "uefi-boot" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# EFI Signing (skipped silently if keys/MOK.key is absent)
STEP=$((STEP + 1))
run_step $STEP $TOTAL "EFI Signing" "sign-efi" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

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
    make $MAKE_FLAGS run 2>&1
fi

# Run QEMU with USB (optional)
if $DO_RUN_USB; then
    divider | tee -a "$LOG"
    printf ' %b▶ Launching QEMU with xHCI + USB storage%b\n' "${CYAN}${BOLD}" "$RESET" | tee -a "$LOG"
    divider | tee -a "$LOG"
    make $MAKE_FLAGS run-usb 2>&1
fi
