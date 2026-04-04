#!/bin/bash
# ============================================================================
# read-blackbox.sh -- Extract BlackBox partition contents from a disk image
#
# Usage:
#   bash scripts/tools/read-blackbox.sh [disk-image] [output-dir]
#
# Defaults:
#   disk-image = build/system-disk.img
#   output-dir = build/blackbox-extract
#
# Requires: mtools (mdir, mcopy)
#
# BlackBox partition layout (GPT):
#   Partition 2: FAT32 "BLACKBOX"
#   LBA start:   133120
#   Byte offset:  68157440  (133120 * 512)
#   Size:         128 MiB
#
# To mount on Linux instead:
#   sudo mount -o loop,offset=68157440,ro build/system-disk.img /mnt/blackbox
# ============================================================================

set -e

DISK="${1:-build/system-disk.img}"
OUTDIR="${2:-build/blackbox-extract}"
BB_OFFSET=68157440

if [ ! -f "$DISK" ]; then
    echo "Error: disk image not found: $DISK"
    echo "Usage: bash scripts/tools/read-blackbox.sh [disk-image] [output-dir]"
    exit 1
fi

# Verify BlackBox partition is readable
if ! mdir -i "$DISK@@$BB_OFFSET" ::/ > /dev/null 2>&1; then
    echo "Error: cannot read BlackBox partition at offset $BB_OFFSET"
    echo "Is the disk image formatted with the 3-partition GPT layout?"
    exit 1
fi

echo "=== BlackBox Partition Reader ==="
echo "  Disk:   $DISK"
echo "  Offset: $BB_OFFSET (LBA 133120)"
echo "  Output: $OUTDIR"
echo ""

# Show partition contents
echo "--- Partition contents ---"
mdir -i "$DISK@@$BB_OFFSET" ::/
echo ""

# Create output directory structure
mkdir -p "$OUTDIR"

# Extract each BlackBox directory
for dir in Logs Boot Crash Perf Diag Tools; do
    # Check if directory has files
    if mdir -i "$DISK@@$BB_OFFSET" "::/$dir" > /dev/null 2>&1; then
        mkdir -p "$OUTDIR/$dir"
        # Copy all files from this directory (non-recursive for now)
        mcopy -i "$DISK@@$BB_OFFSET" -n "::/$dir/*" "$OUTDIR/$dir/" 2>/dev/null || true
        count=$(find "$OUTDIR/$dir" -type f 2>/dev/null | wc -l)
        echo "  $dir/: $count file(s) extracted"
    else
        echo "  $dir/: (empty or missing)"
    fi
done

# Also extract Serial subdirectory if it exists
if mdir -i "$DISK@@$BB_OFFSET" ::/Logs/Serial > /dev/null 2>&1; then
    mkdir -p "$OUTDIR/Logs/Serial"
    mcopy -i "$DISK@@$BB_OFFSET" -n ::/Logs/Serial/* "$OUTDIR/Logs/Serial/" 2>/dev/null || true
    count=$(find "$OUTDIR/Logs/Serial" -type f 2>/dev/null | wc -l)
    echo "  Logs/Serial/: $count file(s) extracted"
fi

echo ""
echo "=== Done: files extracted to $OUTDIR ==="
