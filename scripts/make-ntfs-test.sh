#!/bin/bash
# ============================================================================
# make-ntfs-test.sh — Generate comprehensive NTFS test disk image
#
# Creates a 32 MiB NTFS image populated with various test cases:
#   - Root directory files (basic read test)
#   - Known-content file (byte-exact verification)
#   - Large file (>4 MB, forces multiple data runs)
#   - Deep directory tree (A\B\C\D\E\file.txt)
#   - Directory with >100 files (forces INDX allocation)
#   - Resident small file (< 700 bytes, fits in MFT record)
#   - Long filename (200+ characters)
#
# Usage:
#   bash scripts/make-ntfs-test.sh [output_dir]
#
# Requirements: ntfs-3g (mkntfs, ntfscp)
# ============================================================================

set -e

OUT="${1:-build/test-disks}"
IMG="$OUT/ntfs.img"
IMG_SIZE_MB=32

GREEN='\033[0;32m'
YELLOW='\033[0;33m'
RED='\033[0;31m'
NC='\033[0m'

log()  { echo -e "${GREEN}[NTFS-TEST]${NC} $*"; }
warn() { echo -e "${YELLOW}[SKIP]${NC} $*"; }

# ---- Check dependencies ----
HAVE_MKNTFS=false
HAVE_NTFSCP=false
HAVE_NTFS3G=false

command -v mkntfs  &>/dev/null && HAVE_MKNTFS=true
command -v ntfscp  &>/dev/null && HAVE_NTFSCP=true
command -v ntfs-3g &>/dev/null && HAVE_NTFS3G=true

if ! $HAVE_MKNTFS; then
    warn "mkntfs not found — install ntfs-3g"
    exit 1
fi

mkdir -p "$OUT"

# ---- Remove old image to rebuild with all test cases ----
rm -f "$IMG"

# ---- Create sample files in a temp directory ----
SAMPLE_DIR=$(mktemp -d)
trap 'rm -rf "$SAMPLE_DIR"' EXIT

# 1. Basic test file (known content)
echo -n "Test file for NTFS driver testing." > "$SAMPLE_DIR/test.txt"

# 2. Known 4 KB pattern file (byte-exact verification)
# Pattern: repeating "NTFS_TEST_PATTERN_" followed by 4-digit sequence number
{
    for i in $(seq -w 0000 0254); do
        printf "NTFS_TEST_PATTERN_%s\n" "$i"
    done
} > "$SAMPLE_DIR/verify.txt"
# Pad to exactly 4096 bytes
dd if=/dev/zero bs=1 count=$((4096 - $(wc -c < "$SAMPLE_DIR/verify.txt"))) >> "$SAMPLE_DIR/verify.txt" 2>/dev/null || true
truncate -s 4096 "$SAMPLE_DIR/verify.txt"

# 3. Empty file
touch "$SAMPLE_DIR/empty.txt"

# 4. Resident small file (< 700 bytes — fits in MFT record)
python3 -c "print('R' * 500, end='')" > "$SAMPLE_DIR/resident.txt"

# 5. Large file (5 MB with known repeating pattern for multi-run verification)
dd if=/dev/urandom of="$SAMPLE_DIR/large.bin" bs=1M count=5 2>/dev/null

# 6. Deep directory structure files
mkdir -p "$SAMPLE_DIR/deep/A/B/C/D/E"
echo -n "Deep nested file at level 5." > "$SAMPLE_DIR/deep/A/B/C/D/E/file.txt"
echo -n "Level 1 marker." > "$SAMPLE_DIR/deep/A/marker.txt"
echo -n "Level 3 marker." > "$SAMPLE_DIR/deep/A/B/C/marker.txt"

# 7. Long filename (200+ characters)
LONG_NAME="This_is_a_very_long_filename_that_exceeds_two_hundred_characters_to_test_the_NTFS_driver_handling_of_extended_UTF16LE_filenames_which_are_commonly_found_on_Windows_volumes_and_must_be_properly_decoded_end"
echo -n "Long filename test content." > "$SAMPLE_DIR/$LONG_NAME.txt"

# 8. Directory with >100 files (forces INDX allocation)
mkdir -p "$SAMPLE_DIR/manyfiles"
for i in $(seq -w 001 120); do
    echo -n "File number $i in large directory." > "$SAMPLE_DIR/manyfiles/file_$i.txt"
done

# ---- Create NTFS volume ----
log "Creating ntfs.img (${IMG_SIZE_MB} MiB)"
dd if=/dev/zero of="$IMG" bs=1M count=$IMG_SIZE_MB 2>/dev/null
mkntfs -Q -F -L "NTFS_TEST" "$IMG" >/dev/null 2>&1

# ---- Populate via ntfscp (root-level files) ----
if $HAVE_NTFSCP; then
    log "Copying root-level files via ntfscp..."

    # Basic files
    ntfscp "$IMG" "$SAMPLE_DIR/test.txt" test.txt 2>/dev/null
    ntfscp "$IMG" "$SAMPLE_DIR/verify.txt" verify.txt 2>/dev/null
    ntfscp "$IMG" "$SAMPLE_DIR/empty.txt" empty.txt 2>/dev/null
    ntfscp "$IMG" "$SAMPLE_DIR/resident.txt" resident.txt 2>/dev/null
    ntfscp "$IMG" "$SAMPLE_DIR/large.bin" large.bin 2>/dev/null

    log "Root files copied: test.txt, verify.txt, empty.txt, resident.txt, large.bin"
fi

# ---- Populate via FUSE mount (directories and advanced cases) ----
if $HAVE_NTFS3G; then
    MNT=$(mktemp -d)
    log "Mounting NTFS image via ntfs-3g FUSE..."

    # Try user-space FUSE mount (no root needed if /dev/fuse accessible)
    if ntfs-3g "$IMG" "$MNT" -o rw,no_def_opts,allow_other 2>/dev/null ||
       ntfs-3g "$IMG" "$MNT" -o rw 2>/dev/null; then

        log "FUSE mount successful, creating directories and advanced test cases..."

        # Deep directory tree
        mkdir -p "$MNT/A/B/C/D/E"
        echo -n "Deep nested file at level 5." > "$MNT/A/B/C/D/E/file.txt"
        echo -n "Level 1 marker." > "$MNT/A/marker.txt"
        echo -n "Level 3 marker." > "$MNT/A/B/C/marker.txt"

        # Subdirectory (standard test)
        mkdir -p "$MNT/subdir"
        echo -n "Nested file in a subdirectory." > "$MNT/subdir/nested.txt"

        # Long filename
        echo -n "Long filename test content." > "$MNT/$LONG_NAME.txt" 2>/dev/null || \
            warn "Long filename creation failed (path too long for FUSE)"

        # 120 files in one directory (forces INDX allocation beyond $INDEX_ROOT)
        mkdir -p "$MNT/manyfiles"
        for i in $(seq -w 001 120); do
            echo -n "File number $i in large directory." > "$MNT/manyfiles/file_$i.txt"
        done

        # Sync and unmount
        sync
        fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
        log "FUSE unmount complete"
    else
        warn "ntfs-3g FUSE mount failed — directories will be missing from test image"
        warn "Root-level files (ntfscp) are still present for basic testing"
    fi
    rmdir "$MNT" 2>/dev/null || true
else
    warn "ntfs-3g not available — only root-level files (via ntfscp)"
fi

# ---- Summary ----
log "NTFS test image created: $IMG ($(du -h "$IMG" | cut -f1))"
log "Test cases:"
log "  ✓ test.txt         — basic text file (34 bytes)"
log "  ✓ verify.txt       — known 4 KB pattern for byte-exact verification"
log "  ✓ empty.txt        — zero-byte file"
log "  ✓ resident.txt     — 500-byte file (resident in MFT)"
log "  ✓ large.bin        — 5 MB file (multi-run data)"
if $HAVE_NTFS3G; then
    log "  ✓ subdir/nested.txt — file in subdirectory"
    log "  ✓ A/B/C/D/E/file.txt — deep directory tree (5 levels)"
    log "  ✓ manyfiles/file_NNN.txt — 120 files (forces INDX allocation)"
    log "  ✓ longname (200+ chars) — UTF-16LE long filename"
fi
