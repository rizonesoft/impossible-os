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
# Requires: mtools (mdir, mcopy, mlabel)
#
# BlackBox partition layout (GPT):
#   Partition: FAT32 volume labelled "BLACKBOX", ~128 MiB.
#   The realized byte offset is emitted by the disk build to
#   "<disk-image>.info" (key BB_OFFSET); this tool sources it from there so
#   it never drifts from the build. The Makefile sources the same sidecar.
#
# To mount on Linux instead (substitute the .info BB_OFFSET):
#   BB_OFFSET=$(sed -n 's/^BB_OFFSET=\([0-9]*\)$/\1/p' build/system-disk.img.info)
#   sudo mount -o loop,offset="$BB_OFFSET",ro build/system-disk.img /mnt/blackbox
# ============================================================================

set -e

DISK="${1:-build/system-disk.img}"
OUTDIR="${2:-build/blackbox-extract}"

if [ ! -f "$DISK" ]; then
    echo "Error: disk image not found: $DISK" >&2
    echo "Usage: bash scripts/tools/read-blackbox.sh [disk-image] [output-dir]" >&2
    exit 1
fi

# Resolve the BlackBox byte offset from the build-emitted sidecar. The
# "<disk>.info" file is the realized-offset source of truth (make-system-disk
# emits it and the Makefile sources it); a hardcoded offset would silently
# read the wrong partition after any layout change. Fall back to the historic
# default only when the sidecar is absent, and say so loudly.
INFO="$DISK.info"
if [ -f "$INFO" ]; then
    # Parse BB_OFFSET as DATA, never source the sidecar: this tool accepts an
    # arbitrary disk path, so a hostile adjacent .info must not be able to
    # execute shell code. Accept only a plain numeric assignment.
    BB_OFFSET=$(sed -n 's/^BB_OFFSET=\([0-9]\{1,\}\)$/\1/p' "$INFO" | head -1)
fi
if [ -z "${BB_OFFSET:-}" ]; then
    BB_OFFSET=68157440
    echo "Warning: $INFO not found or has no BB_OFFSET; using default offset $BB_OFFSET" >&2
fi

# Verify a FAT filesystem is readable at that offset.
if ! mdir -i "$DISK@@$BB_OFFSET" ::/ > /dev/null 2>&1; then
    echo "Error: cannot read a FAT filesystem at offset $BB_OFFSET" >&2
    echo "Is the disk image built with the GPT layout + .info offsets?" >&2
    exit 1
fi

# Validate the FAT volume label is exactly BLACKBOX before copying anything.
# Without this, a stale offset could silently extract the ESP / recovery /
# IXFS volume and present it as diagnostics.
LABEL=$(mlabel -s -i "$DISK@@$BB_OFFSET" 2>/dev/null | sed -n 's/.*Volume label is *//p' | tr -d ' \r')
if [ "$LABEL" != "BLACKBOX" ]; then
    echo "Error: FAT volume at offset $BB_OFFSET has label '$LABEL', expected BLACKBOX." >&2
    echo "Refusing to extract -- the offset may be stale or point at the wrong partition." >&2
    exit 1
fi

echo "=== BlackBox Partition Reader ==="
echo "  Disk:   $DISK"
echo "  Offset: $BB_OFFSET (from $INFO)"
echo "  Output: $OUTDIR"
echo ""

# Show partition contents
echo "--- Partition contents ---"
mdir -i "$DISK@@$BB_OFFSET" ::/
echo ""

mkdir -p "$OUTDIR"

# Recursively extract the entire BlackBox tree in one pass. mcopy -s copies
# the nested subtrees (Logs/Serial, Crash/WER) too, so no directory is
# silently dropped. set -e is active and there is no "|| true": a real mtools
# error (corrupt FAT, I/O failure, parse error) aborts with a nonzero status
# instead of being swallowed and reported as a clean extraction.
mcopy -s -n -i "$DISK@@$BB_OFFSET" "::/*" "$OUTDIR/"

# Per-directory top-level file counts for the operator.
for dir in Logs Logs/Serial Boot Crash Crash/WER Perf Diag Tools; do
    if [ -d "$OUTDIR/$dir" ]; then
        count=$(find "$OUTDIR/$dir" -maxdepth 1 -type f 2>/dev/null | wc -l)
        echo "  $dir/: $count file(s) extracted"
    fi
done

echo ""
echo "=== Done: files extracted to $OUTDIR ==="
