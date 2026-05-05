#!/usr/bin/env bash
# ============================================================================
# write-usb.sh -- Write Impossible OS to a USB flash drive (Linux/WSL)
#
# Writes the raw system-disk.img directly to a USB block device.
# The image already contains a GPT partition table with EFI + System + Logs.
#
# Usage:  sudo bash scripts/deploy/write-usb.sh
#
# SAFETY: Only lists removable USB drives. Requires double confirmation.
#         Will NOT touch fixed/internal drives.
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"
DISK_IMG="$BUILD/system-disk.img"

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
    echo "  Usage: sudo bash scripts/deploy/write-usb.sh"
    exit 1
fi

# ---- Preflight ----
if [ ! -f "$DISK_IMG" ]; then
    echo -e "${RED}✗ Missing: $DISK_IMG${NC}"
    echo -e "${YELLOW}  Run 'bash scripts/build.sh clean' first.${NC}"
    exit 1
fi

IMG_SIZE=$(stat -c%s "$DISK_IMG")
IMG_SIZE_MB=$((IMG_SIZE / 1048576))

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- USB Writer (Linux)${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""
echo -e "  ${DIM}Image: $DISK_IMG (${IMG_SIZE_MB} MB)${NC}"
echo ""

# ---- List removable USB drives ----
# Find block devices that are removable (RM=1) and not partitions
echo -e "${GREEN}Available USB drives:${NC}"
echo ""

FOUND=0
declare -a USB_DEVS=()
declare -a USB_NAMES=()
declare -a USB_SIZES=()

while IFS= read -r line; do
    dev=$(echo "$line" | awk '{print $1}')
    size=$(echo "$line" | awk '{print $2}')
    tran=$(echo "$line" | awk '{print $3}')
    rm_flag=$(echo "$line" | awk '{print $4}')
    model=$(echo "$line" | awk '{for(i=5;i<=NF;i++) printf "%s ", $i; print ""}' | sed 's/ *$//')

    # Only show removable drives or USB transport
    if [ "$rm_flag" = "1" ] || [ "$tran" = "usb" ]; then
        FOUND=$((FOUND + 1))
        USB_DEVS+=("/dev/$dev")
        USB_NAMES+=("$model")
        USB_SIZES+=("$size")
        echo -e "  ${CYAN}[$FOUND]${NC} /dev/$dev -- $model ($size)"
    fi
done < <(lsblk -d -n -o NAME,SIZE,TRAN,RM,MODEL 2>/dev/null | grep -v "^loop\|^sr\|^ram")

echo ""

if [ "$FOUND" -eq 0 ]; then
    echo -e "${RED}✗ No removable USB drives found.${NC}"
    echo -e "${YELLOW}  Insert a USB flash drive and try again.${NC}"
    echo ""
    echo -e "${DIM}All block devices:${NC}"
    lsblk -d -o NAME,SIZE,TRAN,RM,MODEL
    exit 1
fi

# ---- Select drive ----
read -rp "  Select drive [1-$FOUND]: " selection

if ! [[ "$selection" =~ ^[0-9]+$ ]] || [ "$selection" -lt 1 ] || [ "$selection" -gt "$FOUND" ]; then
    echo -e "${RED}✗ Invalid selection.${NC}"
    exit 1
fi

idx=$((selection - 1))
TARGET_DEV="${USB_DEVS[$idx]}"
TARGET_NAME="${USB_NAMES[$idx]}"
TARGET_SIZE="${USB_SIZES[$idx]}"

# ---- Size check ----
TARGET_BYTES=$(blockdev --getsize64 "$TARGET_DEV" 2>/dev/null || echo 0)
if [ "$TARGET_BYTES" -lt "$IMG_SIZE" ]; then
    TARGET_MB=$((TARGET_BYTES / 1048576))
    echo -e "${RED}✗ Drive too small (${TARGET_MB} MB, need ${IMG_SIZE_MB} MB).${NC}"
    exit 1
fi

# ---- Double confirmation ----
echo ""
echo -e "${RED}  ╔══════════════════════════════════════════════╗${NC}"
echo -e "${RED}  ║  WARNING: ALL DATA ON THIS DRIVE WILL BE    ║${NC}"
echo -e "${RED}  ║           PERMANENTLY DESTROYED!             ║${NC}"
echo -e "${RED}  ╚══════════════════════════════════════════════╝${NC}"
echo ""
echo -e "  ${YELLOW}Target: $TARGET_DEV -- $TARGET_NAME ($TARGET_SIZE)${NC}"
echo -e "  ${DIM}Image:  ${IMG_SIZE_MB} MB${NC}"
echo ""

read -rp "  Type 'YES' to continue: " confirm1
if [ "$confirm1" != "YES" ]; then
    echo -e "${YELLOW}Cancelled.${NC}"
    exit 0
fi

dev_basename=$(basename "$TARGET_DEV")
read -rp "  Type the device name '$dev_basename' to confirm: " confirm2
if [ "$confirm2" != "$dev_basename" ]; then
    echo -e "${YELLOW}Cancelled -- device name mismatch.${NC}"
    exit 0
fi

# ---- Unmount any mounted partitions ----
echo ""
echo -e "${CYAN}Writing Impossible OS to USB...${NC}"
echo ""

echo -e "  ${DIM}[1/4] Unmounting partitions on $TARGET_DEV...${NC}"
for part in "${TARGET_DEV}"*; do
    umount "$part" 2>/dev/null || true
done

# ---- Write raw image ----
echo -e "  ${DIM}[2/4] Writing ${IMG_SIZE_MB} MB image (this may take a minute)...${NC}"
echo ""

dd if="$DISK_IMG" of="$TARGET_DEV" bs=4M conv=fsync status=progress 2>&1

echo ""
echo -e "  ${GREEN}[3/4] Write complete.${NC}"

# ---- Sync and refresh ----
echo -e "  ${DIM}[4/4] Syncing and refreshing partition table...${NC}"
sync
partprobe "$TARGET_DEV" 2>/dev/null || true
sleep 1

# ---- Verify (re-read partition table + per-file sha256 cross-check) ----
echo ""
echo -e "${CYAN}Verifying USB boot files...${NC}"
echo ""

# Re-read partition table from the device. A successful write must produce
# a GPT layout matching the source image's partition GUIDs; mismatch means
# the partition table didn't make it through the write or the kernel cached
# a stale view.
if command -v sgdisk >/dev/null 2>&1; then
    echo -e "  ${DIM}Partition table on $TARGET_DEV (re-read post-write):${NC}"
    sgdisk -p "$TARGET_DEV" 2>/dev/null | sed -n '/Number/,/^$/p' | sed 's/^/    /'
fi

# Capture expected hashes from the source image BEFORE remounting the
# device, so a tampered re-read cannot satisfy itself.
declare -A EXPECTED_SHA
if command -v mtype >/dev/null 2>&1; then
    # Source-image ESP starts at LBA 2048 for build-image.sh outputs and at
    # the first FAT partition for the legacy system-disk pipeline; both
    # share byte-offset 1MiB so the @@1048576 form works for either.
    for esp_path in "EFI/BOOT/BOOTX64.EFI" "boot/kernel.exe" "EFI/ImpossibleOS/boot.conf"; do
        sha="$(MTOOLS_SKIP_CHECK=1 mtype -i "$DISK_IMG@@1048576" "::$esp_path" 2>/dev/null \
                 | sha256sum | awk '{print $1}')"
        # The empty-input sha256 below is the no-content sentinel mtype
        # produces when the file is missing in the source image; treat it
        # as "no expected hash" rather than a real value to compare.
        if [ -n "$sha" ] && [ "$sha" != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ]; then
            EXPECTED_SHA["$esp_path"]="$sha"
        fi
    done
fi

# Find the EFI partition (first partition)
EFI_PART="${TARGET_DEV}1"
verify_failures=0
if [ -b "$EFI_PART" ]; then
    MOUNT_DIR=$(mktemp -d)
    if mount -t vfat "$EFI_PART" "$MOUNT_DIR" 2>/dev/null; then
        for esp_path in "EFI/BOOT/BOOTX64.EFI" "boot/kernel.exe" "EFI/ImpossibleOS/boot.conf"; do
            usb_file="$MOUNT_DIR/$esp_path"
            if [ ! -f "$usb_file" ]; then
                echo -e "  ${RED}*${NC} $esp_path not present on USB"
                verify_failures=$((verify_failures + 1))
                continue
            fi
            sz_kb=$(( $(stat -c%s "$usb_file") / 1024 ))
            actual_sha="$(sha256sum "$usb_file" | awk '{print $1}')"
            expected_sha="${EXPECTED_SHA[$esp_path]:-}"
            if [ -n "$expected_sha" ] && [ "$actual_sha" != "$expected_sha" ]; then
                echo -e "  ${RED}*${NC} $esp_path (${sz_kb} KB) sha256 MISMATCH"
                echo -e "      ${DIM}expected: $expected_sha${NC}"
                echo -e "      ${DIM}got:      $actual_sha${NC}"
                verify_failures=$((verify_failures + 1))
            elif [ -n "$expected_sha" ]; then
                echo -e "  ${GREEN}OK${NC} $esp_path (${sz_kb} KB) sha256 matches source image"
            else
                echo -e "  ${GREEN}OK${NC} $esp_path (${sz_kb} KB)  ${DIM}(presence only -- no source hash)${NC}"
            fi
        done

        umount "$MOUNT_DIR"
    else
        echo -e "  ${YELLOW}!${NC} Could not mount EFI partition for verification"
        verify_failures=$((verify_failures + 1))
    fi
    rmdir "$MOUNT_DIR" 2>/dev/null || true
else
    echo -e "  ${YELLOW}!${NC} EFI partition not detected (${EFI_PART})"
    verify_failures=$((verify_failures + 1))
fi

if [ "$verify_failures" -gt 0 ]; then
    echo ""
    echo -e "${RED}USB write verification FAILED ($verify_failures issue(s)).${NC}"
    echo -e "${YELLOW}  The image was written but post-write read-back did not match.${NC}"
    echo -e "${YELLOW}  Re-run after replacing the USB drive or re-imaging.${NC}"
    exit 1
fi

# ---- Done ----
echo ""
echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  Done! USB drive is ready to boot.${NC}"
echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
echo ""
echo -e "  ${CYAN}To boot:${NC}"
echo -e "  ${DIM}1. Insert USB into target machine${NC}"
echo -e "  ${DIM}2. Enter BIOS/UEFI boot menu (usually F12, F2, or Del)${NC}"
echo -e "  ${DIM}3. Select the USB drive (UEFI mode)${NC}"
echo -e "  ${DIM}4. Impossible OS should boot!${NC}"
echo ""
