#!/bin/bash
# ============================================================================
# make-ntfs-test.sh -- Generate comprehensive NTFS test disk image
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
    warn "mkntfs not found -- install ntfs-3g"
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

# 4. Resident small file (< 700 bytes -- fits in MFT record)
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

    # Use -f (force) to bypass the exclusive-open volume lock check.
    # This is safe: we just created the image and nothing else has it mounted.
    # Without -f, ntfscp fails on some systems (WSL2, certain ntfs-3g builds)
    # with "Access denied because the NTFS volume is already exclusively opened".
    ntfscp -f "$IMG" "$SAMPLE_DIR/test.txt" test.txt
    ntfscp -f "$IMG" "$SAMPLE_DIR/verify.txt" verify.txt
    ntfscp -f "$IMG" "$SAMPLE_DIR/empty.txt" empty.txt
    ntfscp -f "$IMG" "$SAMPLE_DIR/resident.txt" resident.txt
    ntfscp -f "$IMG" "$SAMPLE_DIR/large.bin" large.bin

    log "Root files copied: test.txt, verify.txt, empty.txt, resident.txt, large.bin"
fi

# ---- Populate via FUSE mount (directories and advanced cases) ----
# ntfs-3g requires root for loop-device mounts.
# We try sudo -- if passwordless sudo is available, it works automatically.
# If not, fall back to ntfscp-only (root-level files).

if $HAVE_NTFS3G; then
    MNT=$(mktemp -d)
    log "Mounting NTFS image via ntfs-3g (sudo)..."

    MOUNT_OK=false

    if sudo -n ntfs-3g "$IMG" "$MNT" -o rw 2>/dev/null; then
        MOUNT_OK=true
    elif ntfs-3g "$IMG" "$MNT" -o rw 2>/dev/null; then
        MOUNT_OK=true
    fi

    if $MOUNT_OK; then
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

        # ---- LZNT1 Compression Test Files (NTFS compression tests) ----
        # ntfs-3g compresses files when the directory has the compressed flag
        COMPRESS_OK=false
        mkdir -p "$MNT/compressed" 2>/dev/null

        # Try to set compressed attribute on the directory via setfattr or chattr
        if command -v setfattr &>/dev/null; then
            # Set NTFS compressed attribute flag (FILE_ATTRIBUTE_COMPRESSED = 0x800)
            # system.ntfs_attrib_be is big-endian, so 0x800 → 00 00 08 00
            # Also include DIRECTORY flag (0x10), so: 0x810 → 00 00 08 10
            setfattr -n system.ntfs_attrib_be -v 0x00000810 "$MNT/compressed" 2>/dev/null && COMPRESS_OK=true
        fi

        # Fallback: try chattr +c (works on some ntfs-3g builds)
        if ! $COMPRESS_OK && command -v chattr &>/dev/null; then
            chattr +c "$MNT/compressed" 2>/dev/null && COMPRESS_OK=true
        fi

        if $COMPRESS_OK; then
            log "Creating LZNT1 compressed test files..."

            # 1. Known-content file (highly compressible -- repeating pattern)
            # 8192 bytes of repeating "COMPRESS_TEST_" (14 chars × 585 + padding)
            {
                for _ in $(seq 1 585); do
                    printf "COMPRESS_TEST_"
                done
            } | head -c 8192 > "$MNT/compressed/known.txt"

            # 2. Sparse/zero file (all zeros -- should become sparse CU)
            dd if=/dev/zero of="$MNT/compressed/zeros.bin" bs=1K count=64 2>/dev/null

            # 3. Incompressible file (random data -- stored uncompressed)
            dd if=/dev/urandom of="$MNT/compressed/random.bin" bs=1K count=8 2>/dev/null

            # 4. Mixed file: compressible header + random middle + compressible tail
            # Total: 128 KB (2 compression units at 64 KB each)
            {
                # First 32 KB: highly compressible (repeating 'A')
                dd if=/dev/zero bs=1K count=32 2>/dev/null | tr '\0' 'A'
                # Middle 64 KB: random (incompressible)
                dd if=/dev/urandom bs=1K count=64 2>/dev/null
                # Last 32 KB: compressible (repeating 'Z')
                dd if=/dev/zero bs=1K count=32 2>/dev/null | tr '\0' 'Z'
            } > "$MNT/compressed/mixed.bin"

            log "  ✓ compressed/known.txt    -- 8 KB compressible pattern"
            log "  ✓ compressed/zeros.bin    -- 64 KB all-zeros (sparse)"
            log "  ✓ compressed/random.bin   -- 8 KB random (incompressible)"
            log "  ✓ compressed/mixed.bin    -- 128 KB mixed (compress+random+compress)"
        else
            warn "Cannot set compressed attribute (setfattr unavailable or failed)"
            warn "LZNT1 tests will be skipped at runtime"
        fi

        # Sync and unmount
        sync
        sudo -n umount "$MNT" 2>/dev/null || \
            fusermount3 -u "$MNT" 2>/dev/null || \
            fusermount -u "$MNT" 2>/dev/null || \
            umount "$MNT" 2>/dev/null || true
        log "FUSE unmount complete"
    else
        warn "ntfs-3g mount failed (needs sudo)"
        warn "Fix: echo '$USER ALL=(root) NOPASSWD: /usr/bin/ntfs-3g, /usr/bin/umount' | sudo tee /etc/sudoers.d/ntfs-test"
        warn "Root-level files (ntfscp) are still present for basic testing"
    fi
    rmdir "$MNT" 2>/dev/null || true
else
    warn "ntfs-3g not available -- only root-level files (via ntfscp)"
fi

# ---- Summary ----
log "NTFS test image created: $IMG ($(du -h "$IMG" | cut -f1))"
log "Test cases:"
log "  ✓ test.txt         -- basic text file (34 bytes)"
log "  ✓ verify.txt       -- known 4 KB pattern for byte-exact verification"
log "  ✓ empty.txt        -- zero-byte file"
log "  ✓ resident.txt     -- 500-byte file (resident in MFT)"
log "  ✓ large.bin        -- 5 MB file (multi-run data)"
if $HAVE_NTFS3G; then
    log "  ✓ subdir/nested.txt -- file in subdirectory"
    log "  ✓ A/B/C/D/E/file.txt -- deep directory tree (5 levels)"
    log "  ✓ manyfiles/file_NNN.txt -- 120 files (forces INDX allocation)"
    log "  ✓ longname (200+ chars) -- UTF-16LE long filename"
fi
