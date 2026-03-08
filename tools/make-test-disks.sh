#!/bin/bash
# ============================================================================
# make-test-disks.sh — Generate test disk images for filesystem driver testing
#
# Creates small (4–16 MiB) disk images with various filesystems, each
# populated with sample files for kernel FS driver development.
#
# Usage:
#   ./tools/make-test-disks.sh <output_dir> <build_dir>
#
# Each image is populated with:
#   test.txt         — small text file
#   subdir/nested.txt — file in a subdirectory
#   empty.txt        — zero-byte file
#   large.bin        — 64 KiB file with pattern data
# ============================================================================

set -e

OUT="${1:-build/test-disks}"
BUILD="${2:-build}"
MKFS_IXFS="${BUILD}/tools/mkfs-ixfs"
MAKE_MBR="${BUILD}/tools/make-mbr"
MAKE_GPT="${BUILD}/tools/make-gpt"

GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

log()  { echo -e "${GREEN}[TEST-DISK]${NC} $*"; }
warn() { echo -e "${YELLOW}[SKIP]${NC} $*"; }
fail() { echo -e "${RED}[FAIL]${NC} $*"; }

# Create output directory
mkdir -p "$OUT"

# --- Generate sample files in a temp directory ---
SAMPLE_DIR=$(mktemp -d)
trap 'rm -rf "$SAMPLE_DIR"' EXIT

echo -n "Test file for filesystem driver testing." > "$SAMPLE_DIR/test.txt"
mkdir -p "$SAMPLE_DIR/subdir"
echo -n "Nested file in a subdirectory." > "$SAMPLE_DIR/subdir/nested.txt"
touch "$SAMPLE_DIR/empty.txt"
# 64 KiB large file with repeating pattern
dd if=/dev/urandom of="$SAMPLE_DIR/large.bin" bs=1024 count=64 2>/dev/null

CREATED=0
SKIPPED=0

# ============================================================================
# 1. FAT32 (8 MiB)
# ============================================================================
IMG="$OUT/fat32.img"
if [ ! -f "$IMG" ]; then
    if command -v mkfs.fat &>/dev/null && command -v mcopy &>/dev/null; then
        log "Creating fat32.img (8 MiB)"
        dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
        mkfs.fat -F 32 "$IMG" >/dev/null 2>&1
        mcopy -i "$IMG" "$SAMPLE_DIR/test.txt" ::test.txt
        mcopy -i "$IMG" "$SAMPLE_DIR/empty.txt" ::empty.txt
        mcopy -i "$IMG" "$SAMPLE_DIR/large.bin" ::large.bin
        mmd -i "$IMG" ::subdir
        mcopy -i "$IMG" "$SAMPLE_DIR/subdir/nested.txt" ::subdir/nested.txt
        CREATED=$((CREATED + 1))
    else
        warn "fat32.img — mkfs.fat or mcopy not found"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "fat32.img already exists, skipping"
fi

# ============================================================================
# 2. exFAT (8 MiB)
# ============================================================================
IMG="$OUT/exfat.img"
if [ ! -f "$IMG" ]; then
    if command -v mkfs.exfat &>/dev/null; then
        log "Creating exfat.img (8 MiB)"
        dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
        mkfs.exfat -n "TESTEXFAT" "$IMG" >/dev/null 2>&1
        # exFAT doesn't have mtools support; create the image formatted only
        # (file population requires mount, which needs root)
        CREATED=$((CREATED + 1))
    else
        warn "exfat.img — mkfs.exfat not found (install exfatprogs)"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "exfat.img already exists, skipping"
fi

# ============================================================================
# 3. ext2 (4 MiB)
# ============================================================================
IMG="$OUT/ext2.img"
if [ ! -f "$IMG" ]; then
    if command -v mkfs.ext2 &>/dev/null && command -v debugfs &>/dev/null; then
        log "Creating ext2.img (4 MiB)"
        dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
        mkfs.ext2 -q -F -L "test_ext2" "$IMG" 2>/dev/null
        # Populate using debugfs (no root/mount needed)
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/test.txt test.txt" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/empty.txt empty.txt" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/large.bin large.bin" 2>/dev/null
        debugfs -w "$IMG" -R "mkdir subdir" 2>/dev/null
        debugfs -w "$IMG" -R "cd subdir" -R "write $SAMPLE_DIR/subdir/nested.txt nested.txt" 2>/dev/null
        CREATED=$((CREATED + 1))
    else
        warn "ext2.img — mkfs.ext2 or debugfs not found"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "ext2.img already exists, skipping"
fi

# ============================================================================
# 4. ext3 (4 MiB)
# ============================================================================
IMG="$OUT/ext3.img"
if [ ! -f "$IMG" ]; then
    if command -v mkfs.ext3 &>/dev/null && command -v debugfs &>/dev/null; then
        log "Creating ext3.img (4 MiB)"
        dd if=/dev/zero of="$IMG" bs=1M count=4 2>/dev/null
        mkfs.ext3 -q -F -L "test_ext3" "$IMG" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/test.txt test.txt" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/empty.txt empty.txt" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/large.bin large.bin" 2>/dev/null
        debugfs -w "$IMG" -R "mkdir subdir" 2>/dev/null
        debugfs -w "$IMG" -R "cd subdir" -R "write $SAMPLE_DIR/subdir/nested.txt nested.txt" 2>/dev/null
        CREATED=$((CREATED + 1))
    else
        warn "ext3.img — mkfs.ext3 or debugfs not found"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "ext3.img already exists, skipping"
fi

# ============================================================================
# 5. ext4 (8 MiB)
# ============================================================================
IMG="$OUT/ext4.img"
if [ ! -f "$IMG" ]; then
    if command -v mkfs.ext4 &>/dev/null && command -v debugfs &>/dev/null; then
        log "Creating ext4.img (8 MiB)"
        dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
        mkfs.ext4 -q -F -L "test_ext4" "$IMG" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/test.txt test.txt" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/empty.txt empty.txt" 2>/dev/null
        debugfs -w "$IMG" -R "write $SAMPLE_DIR/large.bin large.bin" 2>/dev/null
        debugfs -w "$IMG" -R "mkdir subdir" 2>/dev/null
        debugfs -w "$IMG" -R "cd subdir" -R "write $SAMPLE_DIR/subdir/nested.txt nested.txt" 2>/dev/null
        CREATED=$((CREATED + 1))
    else
        warn "ext4.img — mkfs.ext4 or debugfs not found"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "ext4.img already exists, skipping"
fi

# ============================================================================
# 6. NTFS (16 MiB)
# ============================================================================
IMG="$OUT/ntfs.img"
if [ ! -f "$IMG" ]; then
    if command -v mkntfs &>/dev/null && command -v ntfscp &>/dev/null; then
        log "Creating ntfs.img (16 MiB)"
        dd if=/dev/zero of="$IMG" bs=1M count=16 2>/dev/null
        mkntfs -Q -F -L "test_ntfs" "$IMG" >/dev/null 2>&1
        ntfscp "$IMG" "$SAMPLE_DIR/test.txt" test.txt 2>/dev/null
        ntfscp "$IMG" "$SAMPLE_DIR/empty.txt" empty.txt 2>/dev/null
        ntfscp "$IMG" "$SAMPLE_DIR/large.bin" large.bin 2>/dev/null
        # ntfscp doesn't support mkdir; create formatted-only for subdir test
        CREATED=$((CREATED + 1))
    else
        warn "ntfs.img — mkntfs or ntfscp not found (install ntfs-3g)"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "ntfs.img already exists, skipping"
fi

# ============================================================================
# 7. IXFS v2 (8 MiB)
# ============================================================================
IMG="$OUT/ixfs.img"
if [ ! -f "$IMG" ]; then
    if [ -x "$MKFS_IXFS" ]; then
        log "Creating ixfs.img (8 MiB)"
        # Create a temp populate directory with our sample files
        IXFS_POP=$(mktemp -d)
        cp "$SAMPLE_DIR/test.txt" "$IXFS_POP/"
        cp "$SAMPLE_DIR/empty.txt" "$IXFS_POP/"
        cp "$SAMPLE_DIR/large.bin" "$IXFS_POP/"
        # Note: mkfs-ixfs --populate is flat (no subdirs), so we add what we can
        "$MKFS_IXFS" -o "$IMG" -s 8388608 -l "test_ixfs" --populate "$IXFS_POP"
        rm -rf "$IXFS_POP"
        CREATED=$((CREATED + 1))
    else
        warn "ixfs.img — $MKFS_IXFS not found (build first: make system-disk)"
        SKIPPED=$((SKIPPED + 1))
    fi
else
    log "ixfs.img already exists, skipping"
fi

# ============================================================================
# 8. MBR partition table (8 MiB)
# ============================================================================
IMG="$OUT/mbr.img"
if [ ! -f "$IMG" ]; then
    if [ -x "$MAKE_MBR" ]; then
        log "Creating mbr.img (8 MiB)"
        "$MAKE_MBR" -o "$IMG" -s 8M 2>/dev/null || {
            # Fallback: create a simple MBR disk with dd + manual partition entry
            dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
            # Write MBR signature
            printf '\x55\xAA' | dd of="$IMG" bs=1 seek=510 conv=notrunc 2>/dev/null
            # Write a FAT32 partition entry at offset 446 (16 bytes)
            # Type 0x0C (FAT32 LBA), start LBA 2048, size ~14336 sectors
            printf '\x80\x00\x00\x00\x0C\x00\x00\x00' | dd of="$IMG" bs=1 seek=446 conv=notrunc 2>/dev/null
            printf '\x00\x08\x00\x00\x00\x38\x00\x00' | dd of="$IMG" bs=1 seek=454 conv=notrunc 2>/dev/null
        }
        CREATED=$((CREATED + 1))
    else
        warn "mbr.img — $MAKE_MBR not found (build first: make system-disk)"
        # Create minimal MBR image as fallback
        log "Creating mbr.img (8 MiB, minimal fallback)"
        dd if=/dev/zero of="$IMG" bs=1M count=8 2>/dev/null
        printf '\x55\xAA' | dd of="$IMG" bs=1 seek=510 conv=notrunc 2>/dev/null
        printf '\x80\x00\x00\x00\x0C\x00\x00\x00' | dd of="$IMG" bs=1 seek=446 conv=notrunc 2>/dev/null
        printf '\x00\x08\x00\x00\x00\x38\x00\x00' | dd of="$IMG" bs=1 seek=454 conv=notrunc 2>/dev/null
        CREATED=$((CREATED + 1))
    fi
else
    log "mbr.img already exists, skipping"
fi

# ============================================================================
# 9. GPT partition table (16 MiB)
# ============================================================================
IMG="$OUT/gpt.img"
if [ ! -f "$IMG" ]; then
    if [ -x "$MAKE_GPT" ]; then
        log "Creating gpt.img (16 MiB)"
        "$MAKE_GPT" -o "$IMG" -s 16M 2>/dev/null || {
            warn "gpt.img — make-gpt failed, creating minimal GPT"
            dd if=/dev/zero of="$IMG" bs=1M count=16 2>/dev/null
            # Write protective MBR signature
            printf '\x55\xAA' | dd of="$IMG" bs=1 seek=510 conv=notrunc 2>/dev/null
            # Write EFI PART signature at LBA 1
            printf 'EFI PART' | dd of="$IMG" bs=1 seek=512 conv=notrunc 2>/dev/null
        }
        CREATED=$((CREATED + 1))
    else
        warn "gpt.img — $MAKE_GPT not found (build first: make system-disk)"
        # Create minimal GPT image
        log "Creating gpt.img (16 MiB, minimal fallback)"
        dd if=/dev/zero of="$IMG" bs=1M count=16 2>/dev/null
        printf '\x55\xAA' | dd of="$IMG" bs=1 seek=510 conv=notrunc 2>/dev/null
        printf 'EFI PART' | dd of="$IMG" bs=1 seek=512 conv=notrunc 2>/dev/null
        CREATED=$((CREATED + 1))
    fi
else
    log "gpt.img already exists, skipping"
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
log "Done: $CREATED created, $SKIPPED skipped"
ls -lh "$OUT"/*.img 2>/dev/null | awk '{printf "  %-20s %s\n", $NF, $5}'
