#!/usr/bin/env bash
# test-fs.sh — Filesystem test harness (separate from main build flow).
#
# Usage:
#   bash scripts/test-fs.sh                  List available test disks
#   bash scripts/test-fs.sh gen              Generate all test disk images
#   bash scripts/test-fs.sh fat32            Build + launch QEMU with FAT32 on AHCI port 1
#   bash scripts/test-fs.sh ext4             Build + launch QEMU with ext4 on AHCI port 1
#   bash scripts/test-fs.sh ntfs             Build + launch QEMU with NTFS on AHCI port 1
#   bash scripts/test-fs.sh optical/iso9660  Build + launch QEMU with ISO as CD-ROM
#   bash scripts/test-fs.sh optical/joliet   Build + launch QEMU with Joliet ISO as CD-ROM
#   bash scripts/test-fs.sh optical/udf      Build + launch QEMU with UDF as CD-ROM
#   bash scripts/test-fs.sh clean            Remove all test disk images
#
# Test disk images live in build/test-disks/ and are NOT part of the normal
# build pipeline. The main build system is never touched by this script.

set -uo pipefail

# ── Config ──────────────────────────────────────────────────────────────────
BUILD_DIR="build"
TEST_DIR="$BUILD_DIR/test-disks"
LOG="$BUILD_DIR/test-fs.log"

BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[32m'
RED='\033[31m'
YELLOW='\033[33m'
CYAN='\033[36m'
RESET='\033[0m'

divider() { printf '%b──────────────────────────────────────────────────%b\n' "$DIM" "$RESET"; }
header()  { printf '%b══════════════════════════════════════════════════%b\n' "$BOLD" "$RESET"; }

# ── Functions ───────────────────────────────────────────────────────────────

list_disks() {
    header
    printf ' %bAvailable test disks:%b\n' "${BOLD}${CYAN}" "$RESET"
    header
    echo ""

    printf ' %bBlock devices (AHCI port 1):%b\n' "$BOLD" "$RESET"
    local found=0
    for img in "$TEST_DIR"/*.img 2>/dev/null; do
        [ -f "$img" ] || continue
        local name
        name=$(basename "$img" .img)
        local size
        size=$(du -h "$img" | cut -f1)
        printf '   %-16s %s\n' "$name" "$size"
        found=1
    done
    [ $found -eq 0 ] && printf '   %b(none — run: bash scripts/test-fs.sh gen)%b\n' "$DIM" "$RESET"

    echo ""
    printf ' %bOptical media (CD-ROM):%b\n' "$BOLD" "$RESET"
    found=0
    for iso in "$TEST_DIR"/optical/*.iso 2>/dev/null; do
        [ -f "$iso" ] || continue
        local name
        name="optical/$(basename "$iso" .iso)"
        local size
        size=$(du -h "$iso" | cut -f1)
        printf '   %-24s %s\n' "$name" "$size"
        found=1
    done
    [ $found -eq 0 ] && printf '   %b(none — run: bash scripts/test-fs.sh gen)%b\n' "$DIM" "$RESET"

    echo ""
    divider
    printf ' %bUsage:%b  bash scripts/test-fs.sh <disk_name>\n' "$BOLD" "$RESET"
    printf ' %bExample:%b bash scripts/test-fs.sh fat32\n' "$BOLD" "$RESET"
    printf '          bash scripts/test-fs.sh optical/iso9660\n'
    divider
}

generate_disks() {
    header
    printf ' %bGenerating test disk images%b\n' "${BOLD}${CYAN}" "$RESET"
    header

    # Ensure the kernel is built first (needed for mkfs-ixfs)
    printf ' %bBuilding kernel (if needed)...%b\n' "$DIM" "$RESET"
    bash scripts/build.sh 2>&1 | tail -3

    echo ""
    bash tools/make-test-disks.sh "$TEST_DIR" "$BUILD_DIR"
}

clean_disks() {
    if [ -d "$TEST_DIR" ]; then
        rm -rf "$TEST_DIR"
        printf ' %b✓ Removed %s%b\n' "$GREEN" "$TEST_DIR" "$RESET"
    else
        printf ' %b(nothing to clean)%b\n' "$DIM" "$RESET"
    fi
}

run_test() {
    local disk="$1"
    local test_file

    # Determine the test file path
    if echo "$disk" | grep -q "optical/"; then
        test_file="$TEST_DIR/${disk}.iso"
    else
        test_file="$TEST_DIR/${disk}.img"
    fi

    # Check if test disk exists
    if [ ! -f "$test_file" ]; then
        printf ' %b✗ Test disk not found: %s%b\n' "$RED" "$test_file" "$RESET"
        printf '   Run: %bbash scripts/test-fs.sh gen%b\n' "$BOLD" "$RESET"
        echo ""
        list_disks
        exit 1
    fi

    local size
    size=$(du -h "$test_file" | cut -f1)

    header
    printf ' %bFilesystem Test Run%b\n' "${BOLD}${CYAN}" "$RESET"
    header
    printf ' Disk:   %b%s%b (%s)\n' "$BOLD" "$disk" "$RESET" "$size"

    # Build first
    printf ' %bBuilding OS...%b\n' "$DIM" "$RESET"
    bash scripts/build.sh 2>&1 | tail -3

    if [ $? -ne 0 ]; then
        printf ' %b✗ Build failed — aborting test run%b\n' "$RED" "$RESET"
        exit 1
    fi

    divider
    if echo "$disk" | grep -q "optical/"; then
        printf ' Mode:   %bCD-ROM%b (QEMU -cdrom)\n' "$YELLOW" "$RESET"
    else
        printf ' Mode:   %bAHCI port 1%b (secondary hard drive)\n' "$YELLOW" "$RESET"
    fi
    divider

    # Launch QEMU with the test disk
    printf ' %b▶ Launching QEMU%b\n' "${CYAN}${BOLD}" "$RESET"
    divider

    make run-test DISK="$disk" 2>&1
}

# ── Main ────────────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"
echo "" > "$LOG"

if [ $# -eq 0 ]; then
    list_disks
    exit 0
fi

case "$1" in
    gen|generate)
        generate_disks
        ;;
    clean)
        clean_disks
        ;;
    list)
        list_disks
        ;;
    help|-h|--help)
        echo "Usage: bash scripts/test-fs.sh [gen|clean|list|<disk_name>]"
        echo ""
        echo "Commands:"
        echo "  gen              Generate all test disk images"
        echo "  clean            Remove all test disk images"
        echo "  list             List available test disks"
        echo "  <disk_name>      Build + launch QEMU with test disk"
        echo ""
        echo "Examples:"
        echo "  bash scripts/test-fs.sh fat32"
        echo "  bash scripts/test-fs.sh optical/iso9660"
        ;;
    *)
        run_test "$1"
        ;;
esac
