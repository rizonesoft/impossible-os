#!/usr/bin/env python3
"""Validate all embedded assets at build time.

Usage: python3 tools/validate-assets.py
Exit:  0 = all assets valid, 1 = one or more failures

Checks:
  - PNG icons:   dimensions match filename, RGBA/RGB format, file size < 1 MB
  - TTF fonts:   valid header (sfVersion), numTables > 0, maxp glyph count > 0
  - XCursors:    valid Xcur magic, image dimensions ≤ 256, hotspot within bounds
  - Wallpapers:  JPEG decodeable, dimensions < 8K
"""
import os
import re
import struct
import sys
import zlib

# ── Configuration ──────────────────────────────────────────────────────────

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOGO_DIR     = os.path.join(ROOT, 'resources', 'logo')
SYSTEM_DIR   = os.path.join(ROOT, 'resources', 'system')
FONT_DIR     = os.path.join(ROOT, 'resources', 'fonts')
CURSOR_DIR   = os.path.join(ROOT, 'resources', 'cursors')
BG_DIR       = os.path.join(ROOT, 'resources', 'backgrounds')

# Cursors actually used by the kernel (from Makefile CURSOR_SRCS)
REQUIRED_CURSORS = [
    'default', 'pointer', 'text', 'fleur',
    'sb_v_double_arrow', 'sb_h_double_arrow',
    'bd_double_arrow', 'fd_double_arrow',
    'progress', 'crosshair', 'not-allowed',
]

PNG_MAX_SIZE = 1 * 1024 * 1024   # 1 MB
MAX_CURSOR_DIM = 256             # XCursor images should be ≤ 256px
MAX_WALLPAPER_DIM = 7680         # < 8K (7680×4320)

# ── Counters ───────────────────────────────────────────────────────────────

pass_count = 0
fail_count = 0


def ok(msg):
    global pass_count
    pass_count += 1
    print(f"  \033[32m✓\033[0m {msg}")


def fail(msg):
    global fail_count
    fail_count += 1
    print(f"  \033[31m✗\033[0m {msg}")


# ── PNG Validation ─────────────────────────────────────────────────────────

def validate_png(path, expected_w=None, expected_h=None):
    """Validate a PNG file: header, dimensions, color type, file size."""
    basename = os.path.basename(path)
    size = os.path.getsize(path)

    if size > PNG_MAX_SIZE:
        fail(f"{basename}: file size {size} bytes exceeds {PNG_MAX_SIZE // 1024} KB limit")
        return

    with open(path, 'rb') as f:
        data = f.read(32)  # Only need header + IHDR

    if data[:8] != b'\x89PNG\r\n\x1a\n':
        fail(f"{basename}: invalid PNG magic")
        return

    # Parse IHDR chunk (always first after signature)
    chunk_len = struct.unpack('>I', data[8:12])[0]
    chunk_type = data[12:16]

    if chunk_type != b'IHDR' or chunk_len < 13:
        fail(f"{basename}: missing or malformed IHDR chunk")
        return

    w, h, bit_depth, color_type = struct.unpack('>IIBB', data[16:26])

    # Color type 6 = RGBA, 2 = RGB (both acceptable)
    if color_type not in (2, 6):
        fail(f"{basename}: unsupported color type {color_type} (need RGB=2 or RGBA=6)")
        return

    if expected_w and w != expected_w:
        fail(f"{basename}: width {w} != expected {expected_w}")
        return

    if expected_h and h != expected_h:
        fail(f"{basename}: height {h} != expected {expected_h}")
        return

    ct_name = "RGBA" if color_type == 6 else "RGB"
    ok(f"{basename}: {w}×{h} {ct_name} ({size} bytes)")


def validate_png_icons():
    """Validate all OS logo PNGs in resources/logo/."""
    print("\n── PNG Icons ──")

    if not os.path.isdir(LOGO_DIR):
        fail(f"Logo directory missing: {LOGO_DIR}")
        return

    pattern = re.compile(r'^os_logo_(\d+)\.png$')
    found = 0

    for fname in sorted(os.listdir(LOGO_DIR)):
        m = pattern.match(fname)
        if m:
            expected_size = int(m.group(1))
            validate_png(
                os.path.join(LOGO_DIR, fname),
                expected_w=expected_size,
                expected_h=expected_size,
            )
            found += 1

    if found == 0:
        fail(f"No os_logo_*.png files found in {LOGO_DIR}")

    # BSOD icon
    bsod_path = os.path.join(SYSTEM_DIR, 'bsod.png')
    if os.path.isfile(bsod_path):
        validate_png(bsod_path)
    else:
        fail(f"BSOD icon missing: {bsod_path}")


# ── TTF Validation ─────────────────────────────────────────────────────────

def validate_ttf(path):
    """Validate a TrueType font: header, table count, glyph count."""
    basename = os.path.basename(path)

    with open(path, 'rb') as f:
        data = f.read()

    if len(data) < 12:
        fail(f"{basename}: file too small ({len(data)} bytes)")
        return

    sf_version = struct.unpack('>I', data[0:4])[0]

    # 0x00010000 = TrueType, 0x4F54544F = 'OTTO' (OpenType/CFF)
    if sf_version not in (0x00010000, 0x4F54544F):
        fail(f"{basename}: invalid sfVersion {sf_version:#010x}"
             f" (expected 0x00010000 or 0x4F54544F)")
        return

    num_tables = struct.unpack('>H', data[4:6])[0]
    if num_tables == 0:
        fail(f"{basename}: numTables = 0 (no font tables)")
        return

    # Find the 'maxp' table to get glyph count
    glyph_count = None
    for i in range(num_tables):
        offset = 12 + i * 16
        if offset + 16 > len(data):
            break
        tag = data[offset:offset + 4]
        if tag == b'maxp':
            tbl_offset = struct.unpack('>I', data[offset + 8:offset + 12])[0]
            tbl_length = struct.unpack('>I', data[offset + 12:offset + 16])[0]
            if tbl_offset + 6 <= len(data):
                # maxp version (4 bytes) + numGlyphs (2 bytes)
                glyph_count = struct.unpack('>H', data[tbl_offset + 4:tbl_offset + 6])[0]
            break

    if glyph_count is not None and glyph_count == 0:
        fail(f"{basename}: glyph count = 0")
        return

    kind = "TrueType" if sf_version == 0x00010000 else "OpenType/CFF"
    glyphs_str = f", {glyph_count} glyphs" if glyph_count else ""
    ok(f"{basename}: {kind}, {num_tables} tables{glyphs_str}"
       f" ({len(data) // 1024} KB)")


def validate_ttf_fonts():
    """Validate all TTF files in resources/fonts/."""
    print("\n── TTF Fonts ──")

    if not os.path.isdir(FONT_DIR):
        fail(f"Font directory missing: {FONT_DIR}")
        return

    found = 0
    for fname in sorted(os.listdir(FONT_DIR)):
        if fname.endswith('.ttf'):
            validate_ttf(os.path.join(FONT_DIR, fname))
            found += 1

    if found == 0:
        fail(f"No .ttf files found in {FONT_DIR}")


# ── XCursor Validation ────────────────────────────────────────────────────

XCUR_MAGIC = b'Xcur'
XCUR_IMAGE_TYPE = 0xFFFD0002

def validate_xcursor(path):
    """Validate an Adwaita XCursor file: magic, image dims, hotspot."""
    basename = os.path.basename(path)

    with open(path, 'rb') as f:
        data = f.read()

    if len(data) < 16:
        fail(f"{basename}: file too small ({len(data)} bytes)")
        return

    magic = data[0:4]
    if magic != XCUR_MAGIC:
        fail(f"{basename}: invalid magic {magic!r} (expected {XCUR_MAGIC!r})")
        return

    header_size, version, ntoc = struct.unpack('<III', data[4:16])

    if ntoc == 0:
        fail(f"{basename}: no TOC entries (empty cursor)")
        return

    image_count = 0

    for i in range(ntoc):
        toc_off = header_size + i * 12
        if toc_off + 12 > len(data):
            fail(f"{basename}: TOC entry {i} truncated")
            return

        chunk_type, chunk_subtype, chunk_pos = struct.unpack(
            '<III', data[toc_off:toc_off + 12])

        if chunk_type != XCUR_IMAGE_TYPE:
            continue  # Skip non-image chunks (comments, etc.)

        # Image chunk header: header_size(4) + type(4) + subtype(4) +
        #                     version(4) + width(4) + height(4) +
        #                     xhot(4) + yhot(4)
        img_off = chunk_pos
        if img_off + 36 > len(data):
            fail(f"{basename}: image chunk {i} truncated")
            return

        (img_header_size, img_type, img_subtype, img_version,
         width, height, xhot, yhot) = struct.unpack(
            '<IIIIIIII', data[img_off:img_off + 32])

        if width > MAX_CURSOR_DIM or height > MAX_CURSOR_DIM:
            fail(f"{basename}: image {width}×{height} exceeds"
                 f" {MAX_CURSOR_DIM}×{MAX_CURSOR_DIM} limit")
            return

        if xhot > width or yhot > height:
            fail(f"{basename}: hotspot ({xhot},{yhot}) out of bounds"
                 f" ({width}×{height})")
            return

        image_count += 1

    if image_count == 0:
        fail(f"{basename}: no image chunks found")
        return

    ok(f"{basename}: {image_count} image(s), {ntoc} TOC entries"
       f" ({len(data)} bytes)")


def validate_cursors():
    """Validate all required Adwaita XCursor files."""
    print("\n── Cursors (Adwaita XCursor) ──")

    if not os.path.isdir(CURSOR_DIR):
        fail(f"Cursor directory missing: {CURSOR_DIR}")
        return

    for name in REQUIRED_CURSORS:
        path = os.path.join(CURSOR_DIR, name)
        if not os.path.isfile(path):
            fail(f"{name}: required cursor file missing")
        else:
            validate_xcursor(path)


# ── JPEG Wallpaper Validation ──────────────────────────────────────────────

def validate_jpeg(path):
    """Validate a JPEG file: decodeable, reasonable dimensions."""
    basename = os.path.basename(path)

    # Check JPEG SOI marker
    with open(path, 'rb') as f:
        soi = f.read(2)

    if soi != b'\xFF\xD8':
        fail(f"{basename}: invalid JPEG SOI marker")
        return

    # Use Pillow for full decode validation
    try:
        from PIL import Image
        img = Image.open(path)
        img.verify()  # Verify file integrity without full decode
        # Re-open to get dimensions (verify() leaves file in bad state)
        img = Image.open(path)
        w, h = img.size

        if w > MAX_WALLPAPER_DIM or h > MAX_WALLPAPER_DIM:
            fail(f"{basename}: {w}×{h} exceeds {MAX_WALLPAPER_DIM}px limit")
            return

        size_kb = os.path.getsize(path) // 1024
        ok(f"{basename}: {w}×{h} JPEG ({size_kb} KB)")

    except ImportError:
        # Pillow not available — fall back to basic SOI/EOI check
        with open(path, 'rb') as f:
            data = f.read()

        if data[-2:] != b'\xFF\xD9':
            fail(f"{basename}: missing JPEG EOI marker (may be truncated)")
            return

        size_kb = len(data) // 1024
        ok(f"{basename}: JPEG basic check OK ({size_kb} KB)"
           " — install Pillow for full validation")

    except Exception as e:
        fail(f"{basename}: JPEG decode failed: {e}")


def validate_wallpapers():
    """Validate wallpaper files in resources/backgrounds/."""
    print("\n── Wallpapers ──")

    if not os.path.isdir(BG_DIR):
        fail(f"Backgrounds directory missing: {BG_DIR}")
        return

    found = 0
    for fname in sorted(os.listdir(BG_DIR)):
        if fname.lower().endswith(('.jpg', '.jpeg')):
            validate_jpeg(os.path.join(BG_DIR, fname))
            found += 1

    if found == 0:
        fail(f"No JPEG wallpapers found in {BG_DIR}")


# ── Main ───────────────────────────────────────────────────────────────────

def main():
    print("═══ Asset Validation ═══")

    validate_png_icons()
    validate_ttf_fonts()
    validate_cursors()
    validate_wallpapers()

    total = pass_count + fail_count
    print()

    if fail_count > 0:
        print(f"\033[31m[FAIL] {fail_count}/{total} assets failed validation\033[0m")
        sys.exit(1)
    else:
        print(f"\033[32m[OK] {total} assets validated\033[0m")
        sys.exit(0)


if __name__ == '__main__':
    main()
