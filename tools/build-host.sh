#!/usr/bin/env bash
# tools/build-host.sh -- Build all host tools with progress bar, timing, and error extraction.
#
# Usage:
#   bash tools/build-host.sh          Build all host tools (incremental)
#   bash tools/build-host.sh clean    Clean all host tool build artifacts
#
# A host tool is any directory under tools/ with a Makefile. Binaries go to build/host-tools/.
#
# Output is printed to stdout. The last line is always one of:
#   === HOST TOOLS BUILD OK ===
#   === HOST TOOLS BUILD FAILED ===

set -uo pipefail

# ── Resolve paths ──────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="$SCRIPT_DIR"
OUT_DIR="$(dirname "$SCRIPT_DIR")/build/host-tools"

mkdir -p "$OUT_DIR"

# ── Parse arguments ────────────────────────────────────────────────────────
DO_CLEAN=false

for arg in "$@"; do
    case "$arg" in
        clean) DO_CLEAN=true ;;
        *)     echo "Unknown argument: $arg"; echo "Usage: tools/build-host.sh [clean]"; exit 1 ;;
    esac
done

# ── Colors ─────────────────────────────────────────────────────────────────
BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[32m'
RED='\033[31m'
YELLOW='\033[33m'
CYAN='\033[36m'
RESET='\033[0m'

divider()  { printf '%b──────────────────────────────────────────────────%b\n' "$DIM" "$RESET"; }
header()   { printf '%b══════════════════════════════════════════════════%b\n' "$BOLD" "$RESET"; }

elapsed() {
    local start=$1
    local now
    now=$(date +%s.%N)
    awk "BEGIN { t = $now - $start; if (t < 0) t = 0; printf \"%.1f\", t }"
}

# ── Detect host compiler ──────────────────────────────────────────────────
CC=""
if command -v gcc &>/dev/null; then
    CC="gcc"
elif command -v clang &>/dev/null; then
    CC="clang"
else
    printf '%b  FAIL %b No C compiler found (need gcc or clang)\n' "$RED" "$RESET"
    echo "=== HOST TOOLS BUILD FAILED ==="
    exit 1
fi

# ── Check optional dependencies ───────────────────────────────────────────
check_dependencies() {
    # libfuse3 -- needed by ixfs-mount on Linux
    if pkg-config --exists fuse3 2>/dev/null; then
        printf '%b  INFO %b libfuse3 found (pkg-config)\n' "$DIM" "$RESET"
    elif dpkg -s libfuse3-dev &>/dev/null 2>&1; then
        printf '%b  INFO %b libfuse3-dev found (dpkg)\n' "$DIM" "$RESET"
    else
        printf '%b  WARN %b MISSING: libfuse3-dev -- install with: sudo apt install libfuse3-dev\n' "$YELLOW" "$RESET"
        printf '%b       %b Tools requiring libfuse3 may fail to build\n' "$DIM" "$RESET"
    fi
}

# ── Discover host tool directories ─────────────────────────────────────────
discover_tools() {
    local tools=()
    for dir in "$SRC_DIR"/*/; do
        [ -d "$dir" ] || continue
        if [ -f "$dir/Makefile" ]; then
            tools+=("$dir")
        fi
    done
    echo "${tools[@]}"
}

# ── Clean ──────────────────────────────────────────────────────────────────
if [ "$DO_CLEAN" = true ]; then
    header
    printf '%b  HOST TOOLS CLEAN%b\n' "$BOLD" "$RESET"
    header

    TOOL_DIRS=$(discover_tools)
    if [ -z "$TOOL_DIRS" ]; then
        printf '%b  INFO %b No host tools with Makefiles found\n' "$DIM" "$RESET"
    else
        for dir in $TOOL_DIRS; do
            name=$(basename "$dir")
            printf '  Cleaning %s...\n' "$name"
            make -C "$dir" clean OUTDIR="$OUT_DIR" --no-print-directory 2>/dev/null || true
        done
    fi

    printf '%b  OK   %b Clean complete\n' "$GREEN" "$RESET"
    echo "=== HOST TOOLS BUILD OK ==="
    exit 0
fi

# ── Build ──────────────────────────────────────────────────────────────────
BUILD_START=$(date +%s.%N)

header
printf '%b  HOST TOOLS BUILD%b\n' "$BOLD" "$RESET"
header

printf '  Compiler: %b%s%b\n' "$CYAN" "$CC" "$RESET"
divider

# Check dependencies
check_dependencies
divider

# Discover tools
read -ra TOOL_DIRS <<< "$(discover_tools)"
TOOL_COUNT=${#TOOL_DIRS[@]}

if [ "$TOOL_COUNT" -eq 0 ]; then
    printf '%b  WARN %b No host tools found in %s\n' "$YELLOW" "$RESET" "$SRC_DIR"
    printf '       Each tool needs a Makefile in its directory\n'
    divider
    printf '%b  OK   %b Nothing to build\n' "$GREEN" "$RESET"
    echo "=== HOST TOOLS BUILD OK ==="
    exit 0
fi

# Sort tool dirs alphabetically for deterministic order
IFS=$'\n' TOOL_DIRS=($(sort <<< "${TOOL_DIRS[*]}")); unset IFS

# Report discovered tools
TOOL_NAMES=""
for dir in "${TOOL_DIRS[@]}"; do
    name=$(basename "$dir")
    if [ -n "$TOOL_NAMES" ]; then
        TOOL_NAMES="$TOOL_NAMES, $name"
    else
        TOOL_NAMES="$name"
    fi
done
printf '  Found %b%d%b host tools: %s\n' "$CYAN" "$TOOL_COUNT" "$RESET" "$TOOL_NAMES"
divider

# Build each tool
FAIL_COUNT=0
IDX=0

for dir in "${TOOL_DIRS[@]}"; do
    IDX=$((IDX + 1))
    name=$(basename "$dir")
    TOOL_START=$(date +%s.%N)

    printf '  %b[%d/%d]%b Building %b%s%b...' "$BOLD" "$IDX" "$TOOL_COUNT" "$RESET" "$CYAN" "$name" "$RESET"

    # Capture make output for error extraction
    BUILD_OUTPUT=$(make -C "$dir" CC="$CC" OUTDIR="$OUT_DIR" --no-print-directory 2>&1)
    BUILD_RC=$?

    TOOL_ELAPSED=$(elapsed "$TOOL_START")

    if [ $BUILD_RC -eq 0 ]; then
        printf ' %b OK %b (%ss)\n' "$GREEN" "$RESET" "$TOOL_ELAPSED"
    else
        printf ' %bFAILED%b (%ss)\n' "$RED" "$RESET" "$TOOL_ELAPSED"
        FAIL_COUNT=$((FAIL_COUNT + 1))

        # Extract relevant compiler errors (lines with "error:" or "undefined reference")
        divider
        echo "$BUILD_OUTPUT" | grep -E '(error:|undefined reference|fatal error|cannot find)' | head -20
        divider
    fi
done

# ── Summary ────────────────────────────────────────────────────────────────
divider
TOTAL_ELAPSED=$(elapsed "$BUILD_START")

if [ "$FAIL_COUNT" -eq 0 ]; then
    printf '  %b HOST TOOLS BUILD OK %b -- %d tools built in %ss\n' "$GREEN" "$RESET" "$TOOL_COUNT" "$TOTAL_ELAPSED"
    header
    echo "=== HOST TOOLS BUILD OK ==="
    exit 0
else
    PASS_COUNT=$((TOOL_COUNT - FAIL_COUNT))
    printf '  %b HOST TOOLS BUILD FAILED %b -- %d/%d tools failed (%ss)\n' "$RED" "$RESET" "$FAIL_COUNT" "$TOOL_COUNT" "$TOTAL_ELAPSED"
    header
    echo "=== HOST TOOLS BUILD FAILED ==="
    exit 1
fi
