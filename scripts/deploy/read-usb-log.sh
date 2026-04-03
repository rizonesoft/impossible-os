#!/usr/bin/env bash
# ============================================================================
# read-usb-log.sh -- Read boot logs from Impossible OS USB drive
#
# After booting on real hardware, the Logs partition (3rd partition)
# contains debug.log and hardware information. This script mounts it
# read-only, copies the logs, and displays a summary.
#
# Usage:  sudo bash scripts/deploy/read-usb-log.sh [/dev/sdX]
#         If no device specified, auto-detects removable USB drives.
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Root check ----
if [ "$(id -u)" -ne 0 ]; then
    echo -e "${RED}✗ This script must be run as root (sudo).${NC}"
    echo "  Usage: sudo bash scripts/deploy/read-usb-log.sh [/dev/sdX]"
    exit 1
fi

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- USB Log Reader${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ---- Determine target device ----
TARGET_DEV=""

if [ $# -ge 1 ]; then
    # Device specified on command line
    TARGET_DEV="$1"
    if [ ! -b "$TARGET_DEV" ]; then
        echo -e "${RED}✗ Not a block device: $TARGET_DEV${NC}"
        exit 1
    fi
else
    # Auto-detect removable USB drives
    echo -e "${GREEN}Available USB drives:${NC}"
    echo ""

    FOUND=0
    declare -a USB_DEVS=()

    while IFS= read -r line; do
        dev=$(echo "$line" | awk '{print $1}')
        size=$(echo "$line" | awk '{print $2}')
        tran=$(echo "$line" | awk '{print $3}')
        rm_flag=$(echo "$line" | awk '{print $4}')
        model=$(echo "$line" | awk '{for(i=5;i<=NF;i++) printf "%s ", $i; print ""}' | sed 's/ *$//')

        if [ "$rm_flag" = "1" ] || [ "$tran" = "usb" ]; then
            FOUND=$((FOUND + 1))
            USB_DEVS+=("/dev/$dev")
            echo -e "  ${CYAN}[$FOUND]${NC} /dev/$dev -- $model ($size)"
        fi
    done < <(lsblk -d -n -o NAME,SIZE,TRAN,RM,MODEL 2>/dev/null | grep -v "^loop\|^sr\|^ram")

    echo ""

    if [ "$FOUND" -eq 0 ]; then
        echo -e "${RED}✗ No removable USB drives found.${NC}"
        exit 1
    fi

    read -rp "  Select drive [1-$FOUND]: " selection
    if ! [[ "$selection" =~ ^[0-9]+$ ]] || [ "$selection" -lt 1 ] || [ "$selection" -gt "$FOUND" ]; then
        echo -e "${RED}✗ Invalid selection.${NC}"
        exit 1
    fi

    TARGET_DEV="${USB_DEVS[$((selection - 1))]}"
fi

# ---- Find logs partition (3rd partition) ----
LOGS_PART="${TARGET_DEV}3"

# Handle NVMe-style naming (e.g., /dev/nvme0n1p3)
if [[ "$TARGET_DEV" =~ [0-9]$ ]]; then
    LOGS_PART="${TARGET_DEV}p3"
fi

if [ ! -b "$LOGS_PART" ]; then
    echo -e "${RED}✗ Logs partition not found: $LOGS_PART${NC}"
    echo -e "${DIM}  Expected 3rd partition on $TARGET_DEV${NC}"
    echo ""
    echo -e "${DIM}Partitions on $TARGET_DEV:${NC}"
    lsblk "$TARGET_DEV" -o NAME,SIZE,FSTYPE,LABEL
    exit 1
fi

echo -e "  ${DIM}Logs partition: $LOGS_PART${NC}"

# ---- Mount read-only ----
MOUNT_DIR=$(mktemp -d)
echo -e "  ${DIM}Mounting read-only...${NC}"

if ! mount -t vfat -o ro "$LOGS_PART" "$MOUNT_DIR" 2>/dev/null; then
    echo -e "${RED}✗ Failed to mount $LOGS_PART${NC}"
    echo -e "${YELLOW}  Is this an Impossible OS USB drive?${NC}"
    rmdir "$MOUNT_DIR" 2>/dev/null || true
    exit 1
fi

# ---- Check for log files ----
LOG_COUNT=$(find "$MOUNT_DIR" -name "*.log" -o -name "*.txt" 2>/dev/null | wc -l)

if [ "$LOG_COUNT" -eq 0 ]; then
    echo ""
    echo -e "${YELLOW}! No log files found on the Logs partition.${NC}"
    echo -e "${DIM}  The OS may not have written logs yet (boot once on real hardware first).${NC}"
    umount "$MOUNT_DIR"
    rmdir "$MOUNT_DIR" 2>/dev/null || true
    exit 0
fi

# ---- Copy logs ----
TIMESTAMP=$(date '+%Y-%m-%d_%H%M%S')
LOG_DIR="$BUILD/logs/$TIMESTAMP"
mkdir -p "$LOG_DIR"

echo ""
echo -e "${CYAN}Copying log files...${NC}"
echo ""

TOTAL_SIZE=0
while IFS= read -r -d '' logfile; do
    basename=$(basename "$logfile")
    cp "$logfile" "$LOG_DIR/$basename"
    fsize=$(stat -c%s "$logfile")
    fsize_kb=$((fsize / 1024))
    TOTAL_SIZE=$((TOTAL_SIZE + fsize))
    echo -e "  ${GREEN}✓${NC} $basename (${fsize_kb} KB)"
done < <(find "$MOUNT_DIR" -maxdepth 2 \( -name "*.log" -o -name "*.txt" -o -name "crashdump*" \) -print0 2>/dev/null)

# ---- Unmount ----
umount "$MOUNT_DIR"
rmdir "$MOUNT_DIR" 2>/dev/null || true

TOTAL_KB=$((TOTAL_SIZE / 1024))

# ---- Summary ----
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "  ${GREEN}Logs saved to:${NC} $LOG_DIR"
echo -e "  ${DIM}Total: $LOG_COUNT file(s), ${TOTAL_KB} KB${NC}"
echo ""

# Check for panic indicators
if grep -qi "panic\|KERNEL PANIC\|BSOD\|fault\|triple" "$LOG_DIR"/*.log 2>/dev/null; then
    echo -e "  ${RED}⚠  PANIC/FAULT detected in logs!${NC}"
    echo ""
    grep -i "panic\|KERNEL PANIC\|BSOD\|fault\|triple" "$LOG_DIR"/*.log | head -5 | while read -r line; do
        echo -e "  ${RED}  $line${NC}"
    done
    echo ""
fi

# Show first and last lines of debug.log if present
if [ -f "$LOG_DIR/debug.log" ]; then
    echo -e "  ${CYAN}First log line:${NC}"
    echo -e "  ${DIM}$(head -1 "$LOG_DIR/debug.log")${NC}"
    echo -e "  ${CYAN}Last log line:${NC}"
    echo -e "  ${DIM}$(tail -1 "$LOG_DIR/debug.log")${NC}"
fi

echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""
