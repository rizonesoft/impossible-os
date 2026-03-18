#!/usr/bin/env bash
# ============================================================================
# convert-icons.sh — Convert SVG icons to PNG at all required sizes
#
# Usage:
#   bash scripts/convert-icons.sh [svg_dir]
#
# Default svg_dir: resources/icons/svg/
# Output: resources/icons/color/{size}/*.png
#
# Requires: rsvg-convert (apt install librsvg2-bin)
# ============================================================================

set -euo pipefail

SVG_DIR="${1:-resources/icons/color/svg}"
OUT_BASE="resources/icons/color"
SIZES=(16 24 32 48 64 72 96 128 256)

if ! command -v rsvg-convert &>/dev/null; then
    echo "ERROR: rsvg-convert not found. Install with: sudo apt install librsvg2-bin"
    exit 1
fi

if [ ! -d "$SVG_DIR" ]; then
    echo "ERROR: SVG directory not found: $SVG_DIR"
    echo "Place your SVG files there and re-run."
    exit 1
fi

count=0
for svg in "$SVG_DIR"/*.svg; do
    [ -f "$svg" ] || continue
    name=$(basename "$svg" .svg)

    for size in "${SIZES[@]}"; do
        mkdir -p "$OUT_BASE/$size"
        out="$OUT_BASE/$size/${name}.png"
        rsvg-convert -w "$size" -h "$size" "$svg" -o "$out"
        echo "  [OK] ${name}.png @ ${size}px"
    done
    count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
    echo "WARNING: No SVG files found in $SVG_DIR"
    echo "Expected files like: folder_closed.svg, file_default.svg, etc."
    exit 1
fi

echo ""
echo "[OK] Converted $count icons × ${#SIZES[@]} sizes → $OUT_BASE/"
echo "Run 'bash scripts/build.sh clean' to pack into icons.ires"
