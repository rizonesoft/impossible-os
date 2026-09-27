#!/usr/bin/env bash
# ============================================================================
# convert-icons.sh -- Render the colour icon set to PNG at every IRES size
#
# Usage:
#   bash scripts/convert-icons.sh [svg_dir]
#
# Default svg_dir: resources/icons/src/ (the original Impossible OS icon
# sources, GPL-3.0-only; spec in docs/design/icons.md).
# Output: resources/icons/color/{size}/*.png, which are COMMITTED so a build
# never needs rsvg-convert. Re-run this after editing any source SVG and commit
# the regenerated PNGs with it.
#
# Requires: rsvg-convert (apt install librsvg2-bin), only when re-rendering.
# ============================================================================

set -euo pipefail

SVG_DIR="${1:-resources/icons/src}"
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

# Record which sources these PNGs were rendered from. scripts/site/build.py
# --check (lint Check 30) fails when a source SVG changes without a re-render.
# The stamp binds sources AND outputs: "<sha256>  src/<name>.svg" per source and
# "<sha256>  <size>/<name>.png" per rendered PNG (SIZES must match ICON_SIZES in
# scripts/site/build.py).
{
    for svg in "$SVG_DIR"/*.svg; do
        printf '%s  src/%s\n' "$(sha256sum < "$svg" | cut -d' ' -f1)" "$(basename "$svg")"
    done
    for svg in "$SVG_DIR"/*.svg; do
        n=$(basename "$svg" .svg)
        for size in "${SIZES[@]}"; do
            printf '%s  %s/%s.png\n' "$(sha256sum < "$OUT_BASE/$size/$n.png" | cut -d' ' -f1)" "$size" "$n"
        done
    done
} > "$OUT_BASE/SOURCES.sha256"

echo ""
echo "[OK] Converted $count icons × ${#SIZES[@]} sizes → $OUT_BASE/"
echo "Run 'bash scripts/build.sh clean' to pack into icons.ires"
