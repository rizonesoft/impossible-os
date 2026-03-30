#!/usr/bin/env bash
# extract-logs.sh -- Mount IXFS from USB or disk image and extract logs to debug/
#
# Usage:
#   ./extract-logs.sh                          Auto-detect USB drive
#   ./extract-logs.sh build/system-disk.img    Extract from disk image
#
# Output: debug/logs/ directory with all log files from C:\Impossible\System\Logs\

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_DIR="$(dirname "$SCRIPT_DIR")"
REPO_DIR="$(dirname "$SDK_DIR")"
IXFS_MOUNT="$SDK_DIR/tools/ixfs-mount"
MOUNT_POINT="/tmp/ixfs-extract-logs-$$"
DEBUG_DIR="$REPO_DIR/debug/logs"
LOGS_PATH="Impossible/System/Logs"

# Colors
GREEN='\033[32m'
RED='\033[31m'
CYAN='\033[36m'
DIM='\033[2m'
BOLD='\033[1m'
RESET='\033[0m'

cleanup() {
    fusermount -u "$MOUNT_POINT" 2>/dev/null
    rmdir "$MOUNT_POINT" 2>/dev/null
}
trap cleanup EXIT

# Check ixfs-mount exists
if [ ! -x "$IXFS_MOUNT" ]; then
    echo -e "${RED}ERROR${RESET} ixfs-mount not found at $IXFS_MOUNT"
    echo "       Run: bash sdk/build.sh"
    exit 1
fi

echo ""
echo -e "${BOLD}  Extract Impossible OS Logs${RESET}"
echo "========================================"
echo ""

# Determine source: argument or USB auto-detect
if [ $# -ge 1 ]; then
    IMAGE="$1"
    PART=2
    # Support image:partition syntax
    if [[ "$IMAGE" == *:* ]]; then
        PART="${IMAGE##*:}"
        IMAGE="${IMAGE%:*}"
    fi
    if [ ! -f "$IMAGE" ]; then
        echo -e "${RED}ERROR${RESET} File not found: $IMAGE"
        exit 1
    fi
    echo -e "  Source: ${CYAN}$IMAGE${RESET} partition $PART"
    MOUNT_ARG="$IMAGE:$PART"
else
    # Auto-detect USB
    echo -e "  Scanning for USB drives with IXFS partitions..."
    IXFS_MAGIC="49584653"
    FOUND=""

    for dev in /sys/block/sd* /sys/block/nvme*; do
        [ -e "$dev" ] || continue
        name=$(basename "$dev")

        # Check if USB
        if ! readlink -f "$dev/device" 2>/dev/null | grep -q "usb"; then
            removable=$(cat "$dev/removable" 2>/dev/null || echo 0)
            [ "$removable" = "1" ] || continue
        fi

        echo -e "  ${DIM}Scanning /dev/$name...${RESET}"

        for part in "/dev/${name}"[0-9]* "/dev/${name}p"[0-9]*; do
            [ -b "$part" ] || continue
            magic=$(xxd -l 4 -p "$part" 2>/dev/null)
            if [ "$magic" = "$IXFS_MAGIC" ]; then
                # Extract partition number
                partnum=$(echo "$part" | grep -o '[0-9]*$')
                echo -e "  ${GREEN}FOUND${RESET} IXFS on $part"
                FOUND="/dev/$name:$partnum"
                break 2
            fi
        done
    done

    if [ -z "$FOUND" ]; then
        echo -e "  ${RED}No IXFS USB drives found${RESET}"
        echo "  Usage: $0 build/system-disk.img"
        exit 1
    fi
    MOUNT_ARG="$FOUND"
    echo -e "  Source: ${CYAN}$FOUND${RESET}"
fi

# Mount
mkdir -p "$MOUNT_POINT"
echo -e "  Mounting IXFS..."

"$IXFS_MOUNT" "$MOUNT_ARG" "$MOUNT_POINT" 2>&1 &
MOUNT_PID=$!
sleep 1

if ! mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
    echo -e "  ${RED}Mount failed${RESET}"
    kill $MOUNT_PID 2>/dev/null
    exit 1
fi

# Check logs directory exists
if [ ! -d "$MOUNT_POINT/$LOGS_PATH" ]; then
    echo -e "  ${RED}Logs directory not found at $LOGS_PATH${RESET}"
    cleanup
    exit 1
fi

# Extract logs
mkdir -p "$DEBUG_DIR"
echo -e "  Extracting logs to ${CYAN}debug/logs/${RESET}..."
echo ""

# Copy all log files and subdirectories
cp -r "$MOUNT_POINT/$LOGS_PATH/"* "$DEBUG_DIR/" 2>/dev/null

# Also grab serial log from build directory if it exists
if [ -f "$REPO_DIR/build/serial.log" ]; then
    cp "$REPO_DIR/build/serial.log" "$DEBUG_DIR/serial-qemu.log"
fi

# List what we got
echo -e "  ${GREEN}Extracted:${RESET}"
find "$DEBUG_DIR" -type f -printf "    %P (%s bytes)\n" | sort
echo ""

# Show summary of each log
for log in "$DEBUG_DIR"/*.log; do
    [ -f "$log" ] || continue
    name=$(basename "$log")
    lines=$(wc -l < "$log")
    size=$(stat -c%s "$log")
    echo -e "  ${CYAN}$name${RESET} -- $lines lines, $size bytes"
done

echo ""
echo -e "  ${GREEN}Logs saved to debug/logs/${RESET}"
echo "  View: cat debug/logs/kernel.log"
echo ""

# Unmount
cleanup
