#!/usr/bin/env bash
# ============================================================================
# assemble-esp.sh -- build a stale-variant system-disk.img for the
#                    stale-ABI fixture harness.
#
# Copies build/system-disk.img to build/fixtures/stale-<variant>-disk.img,
# then uses mtools to overwrite EITHER /EFI/BOOT/BOOTX64.EFI OR
# /boot/kernel.exe inside the FAT32 ESP partition (starts at 1 MiB) with
# the stale binary pre-built by build-stale-bootloader.sh or
# build-stale-kernel.sh. Emits the output disk-image path on stdout.
#
# Usage:
#   assemble-esp.sh --stale-bootloader PATH
#   assemble-esp.sh --stale-kernel PATH
#
# Exit 0 on success; non-zero on argument errors or mtools failures.
# ============================================================================
set -euo pipefail

# Reserve fd 3 for the fixture-disk path; everything else to stderr.
exec 3>&1
exec 1>&2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
DISK_SRC="$REPO_ROOT/build/system-disk.img"
FIXTURES_DIR="$REPO_ROOT/build/fixtures"

if [ $# -ne 2 ]; then
    echo "usage: $0 --stale-bootloader PATH | --stale-kernel PATH" >&2
    exit 2
fi

VARIANT=""
STALE_PATH=""
case "$1" in
    --stale-bootloader) VARIANT="bootloader"; STALE_PATH="$2" ;;
    --stale-kernel)     VARIANT="kernel";     STALE_PATH="$2" ;;
    *)
        echo "$0: unknown flag '$1'" >&2
        exit 2
        ;;
esac

if [ ! -f "$DISK_SRC" ]; then
    echo "$0: source disk not found: $DISK_SRC (run bash scripts/build.sh)" >&2
    exit 2
fi
if [ ! -f "$STALE_PATH" ]; then
    echo "$0: stale binary not found: $STALE_PATH" >&2
    exit 2
fi
if ! command -v mcopy >/dev/null; then
    echo "$0: mtools not installed (sudo apt-get install -y mtools)" >&2
    exit 2
fi

mkdir -p "$FIXTURES_DIR"
OUT="$FIXTURES_DIR/stale-${VARIANT}-disk.img"
cp "$DISK_SRC" "$OUT"

# ESP partition starts at 1 MiB per the GPT layout produced by the
# system-disk build. mtools `@@1M` offset notation works as long as
# the partition is FAT32 and starts at a round byte offset.
if [ "$VARIANT" = "bootloader" ]; then
    TARGET="::/EFI/BOOT/BOOTX64.EFI"
else
    TARGET="::/boot/kernel.exe"
fi

# `-o` tells mcopy to overwrite without prompting. `-i` selects the
# disk image and FAT offset.
if ! mcopy -o -i "$OUT@@1M" "$STALE_PATH" "$TARGET"; then
    echo "$0: mcopy failed writing $STALE_PATH to $TARGET" >&2
    rm -f "$OUT"
    exit 1
fi

# Sanity-check the write: mdir should list the new file size.
NEW_SIZE="$(wc -c < "$STALE_PATH" | tr -d ' ')"
DIR_OUT="$(mdir -i "$OUT@@1M" "$(dirname "$TARGET")" 2>/dev/null || true)"
if ! echo "$DIR_OUT" | grep -qi "$(basename "$TARGET" .EFI | tr '[:lower:]' '[:upper:]')\|$(basename "$TARGET" .exe)"; then
    echo "$0: WARNING: mdir did not list target after mcopy -- disk may be corrupt" >&2
fi

echo "[assemble-esp] wrote $STALE_PATH (${NEW_SIZE} bytes) to ${TARGET} in $OUT"
echo "$OUT" >&3
