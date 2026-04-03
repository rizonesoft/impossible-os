#!/usr/bin/env bash
# ============================================================================
# size-report.sh -- Kernel and build artifact size tracking
#
# Reports sizes of the kernel binary, top object files, and system disk image.
# Tracks history in build/size-history.csv for regression detection.
#
# Usage:  bash scripts/size-report.sh
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"

KERNEL="$BUILD/kernel.exe"
SYSTEM_DISK="$BUILD/system-disk.img"
HISTORY="$BUILD/size-history.csv"
SYMMAP="$BUILD/kernel.sym"

# ---- Thresholds ----
KERNEL_WARN_KB=8192      # 8 MB -- kernel shouldn't exceed this
DISK_WARN_MB=1024        # 1 GB -- system disk shouldn't exceed this

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Helpers ----
human_size() {
    local bytes=$1
    if [ "$bytes" -ge 1073741824 ]; then
        printf "%.1f GB" "$(echo "$bytes / 1073741824" | bc -l)"
    elif [ "$bytes" -ge 1048576 ]; then
        printf "%.1f MB" "$(echo "$bytes / 1048576" | bc -l)"
    elif [ "$bytes" -ge 1024 ]; then
        printf "%.1f KB" "$(echo "$bytes / 1024" | bc -l)"
    else
        printf "%d B" "$bytes"
    fi
}

delta_str() {
    local cur=$1 prev=$2
    if [ "$prev" -eq 0 ]; then
        echo "(new)"
        return
    fi
    local diff=$((cur - prev))
    if [ "$diff" -eq 0 ]; then
        echo "(unchanged)"
    elif [ "$diff" -gt 0 ]; then
        echo -e "${RED}+$(human_size $diff)${NC}"
    else
        local abs=$(( -diff ))
        echo -e "${GREEN}-$(human_size $abs)${NC}"
    fi
}

# ---- Preflight ----
if [ ! -f "$KERNEL" ]; then
    echo -e "${RED}ERROR: $KERNEL not found. Run 'bash scripts/build.sh' first.${NC}"
    exit 1
fi

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Size Report${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ---- Get current sizes ----
KERNEL_SIZE=$(stat --printf="%s" "$KERNEL" 2>/dev/null || echo 0)
DISK_SIZE=0
if [ -f "$SYSTEM_DISK" ]; then
    DISK_SIZE=$(stat --printf="%s" "$SYSTEM_DISK" 2>/dev/null || echo 0)
fi
SYMMAP_SIZE=0
if [ -f "$SYMMAP" ]; then
    SYMMAP_SIZE=$(stat --printf="%s" "$SYMMAP" 2>/dev/null || echo 0)
fi
COMMIT=$(cd "$REPO_ROOT" && git rev-parse --short HEAD 2>/dev/null || echo "unknown")
DATE=$(date '+%Y-%m-%d %H:%M')

# ---- Read previous sizes from history ----
PREV_KERNEL=0
PREV_DISK=0
if [ -f "$HISTORY" ]; then
    LAST_LINE=$(tail -1 "$HISTORY" 2>/dev/null || echo "")
    if [ -n "$LAST_LINE" ]; then
        PREV_KERNEL=$(echo "$LAST_LINE" | cut -d',' -f3)
        PREV_DISK=$(echo "$LAST_LINE" | cut -d',' -f4)
    fi
fi

# ---- Kernel binary ----
echo -e "${CYAN}Kernel Binary${NC}"
echo -e "  kernel.exe:  $(human_size "$KERNEL_SIZE")  $(delta_str "$KERNEL_SIZE" "$PREV_KERNEL")"
if [ "$SYMMAP_SIZE" -gt 0 ]; then
    echo -e "  kernel.sym:  $(human_size "$SYMMAP_SIZE")  ${DIM}(symbol map)${NC}"
fi

# Check threshold
KERNEL_KB=$(( KERNEL_SIZE / 1024 ))
if [ "$KERNEL_KB" -gt "$KERNEL_WARN_KB" ]; then
    echo -e "  ${YELLOW}⚠ WARNING: Kernel exceeds $(( KERNEL_WARN_KB / 1024 )) MB threshold!${NC}"
fi
echo ""

# ---- System disk ----
if [ "$DISK_SIZE" -gt 0 ]; then
    echo -e "${CYAN}System Disk${NC}"
    echo -e "  system-disk.img:  $(human_size "$DISK_SIZE")  $(delta_str "$DISK_SIZE" "$PREV_DISK")"
    DISK_MB=$(( DISK_SIZE / 1048576 ))
    if [ "$DISK_MB" -gt "$DISK_WARN_MB" ]; then
        echo -e "  ${YELLOW}⚠ WARNING: System disk exceeds ${DISK_WARN_MB} MB threshold!${NC}"
    fi
    echo ""
fi

# ---- Top 10 largest object files ----
echo -e "${CYAN}Top 10 Largest Object Files${NC}"
if [ -d "$BUILD" ]; then
    find "$BUILD" -name '*.o' -type f -printf '%s %p\n' 2>/dev/null \
        | sort -rn \
        | head -10 \
        | while read -r size path; do
            relpath="${path#"$BUILD"/}"
            printf "  %-50s %s\n" "$relpath" "$(human_size "$size")"
        done
fi
echo ""

# ---- Section sizes (via llvm-size) ----
if command -v llvm-size-19 &>/dev/null; then
    echo -e "${CYAN}Section Breakdown${NC}"
    llvm-size-19 "$KERNEL" 2>/dev/null | while IFS= read -r line; do
        echo "  $line"
    done
    echo ""
fi

# ---- Append to history ----
if [ ! -f "$HISTORY" ]; then
    echo "date,commit,kernel_bytes,disk_bytes" > "$HISTORY"
fi
echo "$DATE,$COMMIT,$KERNEL_SIZE,$DISK_SIZE" >> "$HISTORY"
echo -e "${DIM}History appended to $HISTORY${NC}"

# ---- Summary line ----
HIST_ENTRIES=$(( $(wc -l < "$HISTORY") - 1 ))  # subtract header
echo -e "${DIM}$HIST_ENTRIES builds tracked${NC}"
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
