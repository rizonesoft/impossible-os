#!/usr/bin/env bash
# ============================================================================
# run-vbox.sh — Launch Impossible OS in VirtualBox (Linux/WSL)
#
# Prerequisites:
#   1. Install VirtualBox: https://www.virtualbox.org/
#   2. Build first: bash scripts/build.sh clean
#
# Usage:
#   bash scripts/machines/run-vbox.sh           # GUI mode
#   bash scripts/machines/run-vbox.sh --headless  # Headless (CI)
#   bash scripts/machines/run-vbox.sh --debug     # Debug boot (no splash)
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="$REPO_ROOT/build"

DISK_RAW="$BUILD/system-disk.img"
DISK_VDI="$BUILD/system-disk.vdi"
OVMF_CODE="$BUILD/OVMF_CODE_4M.fd"
OVMF_VARS="$BUILD/OVMF_VARS_4M.fd"

VM_NAME="ImpossibleOS"
LOG_OFFSET=68157440  # Must match Makefile LOG_OFFSET

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Parse flags ----
HEADLESS=false
DEBUG_BOOT=false

for arg in "$@"; do
    case "$arg" in
        --headless) HEADLESS=true ;;
        --debug)    DEBUG_BOOT=true ;;
        -h|--help)
            echo "Usage: bash scripts/machines/run-vbox.sh [--headless] [--debug]"
            echo "  --headless  Start VM without GUI (for CI)"
            echo "  --debug     Inject DEBUG flag (skip boot splash)"
            exit 0
            ;;
        *) echo -e "${RED}Unknown flag: $arg${NC}"; exit 1 ;;
    esac
done

# ---- Preflight checks ----
if ! command -v VBoxManage >/dev/null 2>&1; then
    echo -e "${RED}✗ VBoxManage not found. Install VirtualBox.${NC}"
    echo "  https://www.virtualbox.org/wiki/Downloads"
    exit 1
fi

if [ ! -f "$DISK_RAW" ]; then
    echo -e "${RED}✗ Missing: $DISK_RAW${NC}"
    echo -e "${YELLOW}  Run 'bash scripts/build.sh clean' first.${NC}"
    exit 1
fi

# ---- Copy OVMF firmware if missing ----
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo -e "${YELLOW}Copying OVMF firmware to build/...${NC}"
    cp /usr/share/OVMF/OVMF_CODE_4M.fd "$BUILD/" 2>/dev/null || true
    cp /usr/share/OVMF/OVMF_VARS_4M.fd "$BUILD/" 2>/dev/null || true
fi

# ---- Debug boot flag ----
if [ "$DEBUG_BOOT" = true ]; then
    echo -n "debug" | mcopy -i "$DISK_RAW@@$LOG_OFFSET" - ::DEBUG 2>/dev/null || \
    echo -n "debug" | mcopy -o -i "$DISK_RAW@@$LOG_OFFSET" - ::DEBUG
    echo -e "  ${GREEN}✓${NC} DEBUG flag injected into Logs partition"
else
    mdel -i "$DISK_RAW@@$LOG_OFFSET" ::DEBUG 2>/dev/null || true
fi

# ---- Convert raw disk to VDI ----
echo -e "${CYAN}Converting disk image to VDI...${NC}"

# Power off VM if running
VBoxManage controlvm "$VM_NAME" poweroff 2>/dev/null || true
sleep 0.5

# Detach disk from VM
VBoxManage storageattach "$VM_NAME" --storagectl "AHCI" --port 0 --medium none 2>/dev/null || true

# Close medium in VirtualBox registry
VBoxManage closemedium disk "$DISK_VDI" 2>/dev/null || true

# Delete old VDI and convert
rm -f "$DISK_VDI"
VBoxManage convertfromraw "$DISK_RAW" "$DISK_VDI" --format VDI

if [ ! -f "$DISK_VDI" ]; then
    echo -e "${RED}✗ Failed to convert disk image.${NC}"
    exit 1
fi
echo -e "  ${DIM}VDI: $DISK_VDI${NC}"

# ---- Create or update VM ----
VM_EXISTS=false
if VBoxManage showvminfo "$VM_NAME" >/dev/null 2>&1; then
    VM_EXISTS=true
fi

if [ "$VM_EXISTS" = true ]; then
    echo -e "${YELLOW}Updating existing VM '$VM_NAME'...${NC}"
    VBoxManage controlvm "$VM_NAME" poweroff 2>/dev/null || true
    sleep 1
else
    echo -e "${GREEN}Creating VM '$VM_NAME'...${NC}"
    VBoxManage createvm --name "$VM_NAME" --ostype "Other_64" --register

    # Storage controller: AHCI (matches our AHCI driver)
    VBoxManage storagectl "$VM_NAME" --name "AHCI" --add sata --controller IntelAhci --portcount 2
fi

# ---- VM Settings ----
# VMSVGA is required for UEFI guests (VBoxVGA has no EFI GOP support)
VBoxManage modifyvm "$VM_NAME" \
    --memory 2048 \
    --cpus 4 \
    --ioapic on \
    --firmware efi \
    --graphicscontroller vmsvga \
    --vram 128 \
    --mouse ps2 \
    --keyboard ps2 \
    --audio-driver none \
    --uart1 "0x3F8" "4" \
    --uart-mode1 file "$BUILD/serial.log" \
    --boot1 disk \
    --boot2 none \
    --boot3 none \
    --boot4 none

# Disable mouse integration (no Guest Additions)
VBoxManage setextradata "$VM_NAME" "VBoxInternal/Devices/pckbd/0/Config/DisableMouseIntegration" "" 2>/dev/null || true
VBoxManage setextradata "$VM_NAME" "GUI/Input/MachineMouseIntegration" "false"

# Set resolution hint for VMSVGA adapter
VBoxManage setextradata "$VM_NAME" "CustomVideoMode1" "1280x720x32"
VBoxManage setextradata "$VM_NAME" "VBoxInternal2/EfiGraphicsResolution" "1280x720"

# Attach boot disk
VBoxManage storageattach "$VM_NAME" \
    --storagectl "AHCI" \
    --port 0 \
    --type hdd \
    --medium "$DISK_VDI"

# Detach any stale test disks from ports 1+
for p in $(seq 1 15); do
    VBoxManage storageattach "$VM_NAME" --storagectl "AHCI" --port "$p" --medium none 2>/dev/null || true
done

# ---- Launch ----
echo ""
echo -e "${GREEN}Launching Impossible OS in VirtualBox...${NC}"
echo -e "  ${DIM}VM: $VM_NAME${NC}"
echo -e "  ${DIM}Disk: $DISK_VDI${NC}"
echo -e "  ${DIM}Display: VMSVGA 1280x720 (UEFI)${NC}"
echo -e "  ${DIM}Serial log: $BUILD/serial.log${NC}"

if [ "$DEBUG_BOOT" = true ]; then
    echo -e "  ${YELLOW}Mode: DEBUG (splash disabled, live text on screen)${NC}"
fi
echo ""

if [ "$HEADLESS" = true ]; then
    echo -e "  ${CYAN}Starting headless...${NC}"
    VBoxManage startvm "$VM_NAME" --type headless
else
    VBoxManage startvm "$VM_NAME"
fi
