#!/usr/bin/env bash
# mount-ixfs-usb.sh -- Detect USB drives with IXFS partitions and mount them.
#
# Usage:
#   ./mount-ixfs-usb.sh              Auto-detect and mount
#   ./mount-ixfs-usb.sh /dev/sdb     Mount specific device
#
# Unmount: fusermount -u /mnt/ixfs

set -uo pipefail

MOUNT_POINT="/mnt/ixfs"
IXFS_MAGIC="49584653"  # "IXFS" in hex (little-endian at offset 0 of partition)

# Find ixfs-mount binary
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IXFS_MOUNT="$SCRIPT_DIR/../tools/ixfs-mount"

if [ ! -x "$IXFS_MOUNT" ]; then
    echo "ERROR: ixfs-mount not found at $IXFS_MOUNT"
    echo "       Run: bash sdk/build.sh"
    exit 1
fi

# Colors
GREEN='\033[32m'
RED='\033[31m'
CYAN='\033[36m'
DIM='\033[2m'
RESET='\033[0m'

# Check if already mounted
if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
    echo -e "${RED}ERROR${RESET} $MOUNT_POINT is already mounted"
    echo "       Unmount first: fusermount -u $MOUNT_POINT"
    exit 1
fi

# Find USB/removable block devices
find_usb_devices() {
    local devices=()
    for dev in /sys/block/sd* /sys/block/nvme*; do
        [ -e "$dev" ] || continue
        local name=$(basename "$dev")
        local removable=$(cat "$dev/removable" 2>/dev/null || echo 0)
        local transport=""

        # Check if USB via device path
        if readlink -f "$dev/device" 2>/dev/null | grep -q "usb"; then
            transport="USB"
        elif [ "$removable" = "1" ]; then
            transport="removable"
        fi

        if [ -n "$transport" ]; then
            devices+=("/dev/$name")
        fi
    done
    echo "${devices[@]}"
}

# Check if a partition has IXFS magic at offset 0
check_ixfs_partition() {
    local dev="$1"
    local magic
    magic=$(xxd -l 4 -p "$dev" 2>/dev/null)
    [ "$magic" = "$IXFS_MAGIC" ] && return 0
    # Try reversed endianness
    magic=$(dd if="$dev" bs=4 count=1 2>/dev/null | od -An -tx4 | tr -d ' ')
    [ "$magic" = "$IXFS_MAGIC" ] && return 0
    return 1
}

# Scan a device for IXFS partitions
scan_device() {
    local dev="$1"
    local part_num=0

    echo -e "${DIM}  Scanning $dev...${RESET}"

    # List partitions
    for part in "${dev}"[0-9]* "${dev}p"[0-9]*; do
        [ -b "$part" ] || continue
        part_num=$((part_num + 1))

        if check_ixfs_partition "$part"; then
            echo -e "${GREEN}  FOUND${RESET} IXFS partition: $part (partition $part_num)"
            echo "$part:$part_num"
            return 0
        fi
    done

    return 1
}

# Main
echo "Scanning for USB drives with IXFS partitions..."
echo ""

# If device specified on command line, scan only that
if [ $# -ge 1 ]; then
    DEVICES=("$1")
else
    read -ra DEVICES <<< "$(find_usb_devices)"
fi

if [ ${#DEVICES[@]} -eq 0 ]; then
    echo -e "${RED}No USB/removable drives found${RESET}"
    exit 1
fi

echo -e "  Found ${CYAN}${#DEVICES[@]}${RESET} removable device(s)"

FOUND=""
for dev in "${DEVICES[@]}"; do
    result=$(scan_device "$dev")
    if [ $? -eq 0 ] && [ -n "$result" ]; then
        FOUND="$result"
        break
    fi
done

if [ -z "$FOUND" ]; then
    echo ""
    echo -e "${RED}No IXFS partitions found on USB drives${RESET}"
    exit 1
fi

# Parse result: /dev/sdb2:2
PART_DEV=$(echo "$FOUND" | tail -1 | cut -d: -f1)
PART_NUM=$(echo "$FOUND" | tail -1 | cut -d: -f2)
BASE_DEV=$(echo "$PART_DEV" | sed 's/[0-9]*$//' | sed 's/p$//')

# Create mount point
mkdir -p "$MOUNT_POINT"

echo ""
echo -e "  Mounting ${CYAN}$PART_DEV${RESET} at ${CYAN}$MOUNT_POINT${RESET}..."

# Mount using ixfs-mount with device:partition syntax
"$IXFS_MOUNT" "${BASE_DEV}:${PART_NUM}" "$MOUNT_POINT" &
sleep 1

if mountpoint -q "$MOUNT_POINT" 2>/dev/null; then
    echo -e "${GREEN}  Mounted IXFS from $PART_DEV at $MOUNT_POINT${RESET}"
    echo ""
    echo "  Unmount: fusermount -u $MOUNT_POINT"
else
    echo -e "${RED}  Mount failed${RESET}"
    exit 1
fi
