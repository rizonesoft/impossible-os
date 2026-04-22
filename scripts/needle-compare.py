#!/usr/bin/env python3
# needle-compare.py -- perceptual diff + openQA-style needle comparator.
#
# Implements TODO-05-desktop-ui-test-framework.md the perceptual diff +
# structured-needles section. Upgrades section 6's flat pixel-percent
# comparator with:
#   * SSIM (scikit-image structural_similarity) as the default metric,
#   * SSIMULACRA2 as an opt-in high-quality perceptual metric,
#   * per-region tolerance via a JSON needle sidecar (strict / relaxed /
#     ignored), and
#   * region OCR assertions via tesseract.
#
# Invoked directly or via `scripts/compare-screenshot.sh --mode ssim` /
# `--needle <path>`. The bash wrapper keeps the pixel-mode callers of
# section 6 unchanged.
#
# Exit codes match scripts/compare-screenshot.sh:
#   0  pass (or every region passes its tolerance)
#   1  preflight failure (missing tool / bad arg)
#   2  input file missing / unreadable
#   3  reference vs current dimension mismatch
#   4  decode / metric failure (PIL / ImageMagick / skimage error,
#      malformed needle JSON, OCR binary missing when OCR requested and
#      --allow-missing-ocr not set)
#   5  visual regression (score below threshold, region failed, OCR
#      string mismatch)

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

try:
    from PIL import Image, ImageDraw, ImageChops
except ImportError:
    print("[needle-compare] missing Pillow: install `python3-pil`", file=sys.stderr)
    sys.exit(1)


def die(code, msg):
    print(f"[needle-compare] {msg}", file=sys.stderr)
    sys.exit(code)


def info(msg):
    print(f"[needle-compare] {msg}")


# ---- Metric implementations ---------------------------------------------

def ssim_score(ref_img, cur_img):
    """Return SSIM in [0, 1]. Requires scikit-image. Transparent over
    crops too small for the library default win_size (7): we compute an
    adaptive odd win_size <= min(width, height) so thin needle regions
    don't raise ValueError and abort the whole run. Codex [H] review
    flagged the crash path explicitly. A crop smaller than 2x2 cannot be
    compared meaningfully and exits via die(4)."""
    try:
        import numpy as np
        from skimage.metrics import structural_similarity
    except ImportError:
        die(1, "SSIM mode needs scikit-image + numpy "
               "(apt: python3-skimage python3-numpy)")
    a = np.asarray(ref_img.convert("RGB"))
    b = np.asarray(cur_img.convert("RGB"))
    h, w = a.shape[:2]
    smaller = min(h, w)
    if smaller < 2:
        die(4, f"SSIM region too small: {w}x{h} (need >= 2 on each side)")
    if smaller < 7:
        # Pick the largest odd value <= smaller. win_size must be odd
        # per scikit-image; 3, 5, 7 are the only useful small choices.
        win = smaller if (smaller % 2) == 1 else smaller - 1
        return float(structural_similarity(a, b, channel_axis=2, win_size=win))
    return float(structural_similarity(a, b, channel_axis=2))


def pixel_score(ref_img, cur_img, fuzz_pct):
    """Return percent-identical in [0, 100] with per-channel fuzz budget."""
    # Fuzz: treat two pixels as identical when every channel's absolute
    # delta is <= (fuzz_pct / 100) * 255. Implement via Pillow's
    # ImageChops.difference, then threshold.
    if ref_img.size != cur_img.size:
        return 0.0
    diff = ImageChops.difference(ref_img.convert("RGB"), cur_img.convert("RGB"))
    # Convert to single-channel max-delta per pixel.
    max_band = diff.split()
    # Pillow's 'point' lets us threshold per channel, then we union.
    # Build a threshold mask: channel > limit -> 255, else 0.
    limit = int(round(fuzz_pct / 100.0 * 255))
    mask = None
    for b in max_band:
        t = b.point(lambda v, L=limit: 255 if v > L else 0)
        mask = t if mask is None else ImageChops.lighter(mask, t)
    # Count the non-zero bytes in the mask. Use a histogram lookup when
    # available (Pillow 10+ keeps .histogram() stable, and `getdata()` is
    # deprecated in 13+ in favor of `get_flattened_data()` which isn't
    # everywhere yet).
    hist = mask.histogram()
    diff_px = sum(hist[1:]) if len(hist) > 1 else 0
    total = mask.size[0] * mask.size[1]
    if total == 0:
        return 0.0
    return (1.0 - diff_px / total) * 100.0


def ssimulacra2_score(ref_path, cur_path):
    """Return SSIMULACRA2 in [0, 100]. Requires the `ssimulacra2`
    external binary (libjxl project). Exit 4 when missing."""
    bin_path = shutil.which("ssimulacra2")
    if not bin_path:
        die(4, "--mode ssimulacra2 needs the `ssimulacra2` binary "
               "(build from https://github.com/libjxl/libjxl). "
               "Fall back to --mode ssim on hosts without it.")
    try:
        out = subprocess.check_output(
            [bin_path, ref_path, cur_path],
            stderr=subprocess.STDOUT, timeout=30)
    except subprocess.CalledProcessError as e:
        die(4, f"ssimulacra2 failed: {e.output!r}")
    except subprocess.TimeoutExpired:
        die(4, "ssimulacra2 timed out after 30s")
    # ssimulacra2 prints a single float to stdout; be defensive against
    # tool-version output variance.
    m = re.search(rb"(-?\d+(\.\d+)?)", out)
    if not m:
        die(4, f"could not parse ssimulacra2 output: {out!r}")
    return float(m.group(1))


# ---- Region cropping + tolerance ----------------------------------------

TOL_STRICT = "strict"
TOL_RELAXED = "relaxed"
TOL_IGNORED = "ignored"
VALID_TOL = (TOL_STRICT, TOL_RELAXED, TOL_IGNORED)


def validate_needle(needle, path):
    """Schema-check a loaded needle and route every violation through
    die(4), matching the documented exit-code contract. Codex [M] review
    flagged that the earlier draft let malformed needles raise uncaught
    KeyError / TypeError from the main loop."""
    if not isinstance(needle, dict):
        die(4, f"needle {path}: top-level must be a JSON object")
    if needle.get("version") != 1:
        die(4, f"needle {path}: unsupported version {needle.get('version')} (expected 1)")
    regions = needle.get("regions")
    if not isinstance(regions, list) or not regions:
        die(4, f"needle {path}: 'regions' must be a non-empty array")
    for idx, r in enumerate(regions):
        if not isinstance(r, dict):
            die(4, f"needle {path}: regions[{idx}] must be an object")
        for key in ("x", "y", "w", "h"):
            if key not in r:
                die(4, f"needle {path}: regions[{idx}] missing '{key}'")
            if not isinstance(r[key], int) or (key in ("w", "h") and r[key] <= 0) \
                    or (key in ("x", "y") and r[key] < 0):
                die(4, f"needle {path}: regions[{idx}].{key} must be a "
                       f"non-negative integer ({key} in ['w','h'] must be > 0); got {r[key]!r}")
        tol = r.get("tolerance", TOL_RELAXED)
        if tol not in VALID_TOL:
            die(4, f"needle {path}: regions[{idx}].tolerance must be one of "
                   f"{VALID_TOL}, got {tol!r}")
        if "name" in r and not isinstance(r["name"], str):
            die(4, f"needle {path}: regions[{idx}].name must be a string")
        if "ocr" in r and not isinstance(r["ocr"], str):
            die(4, f"needle {path}: regions[{idx}].ocr must be a string")


def crop_region(img, r):
    """Crop `img` to the (x, y, w, h) region. Returns the crop, or
    raises ValueError when the region escapes the image bounds."""
    x, y, w, h = int(r["x"]), int(r["y"]), int(r["w"]), int(r["h"])
    if x < 0 or y < 0 or w <= 0 or h <= 0:
        raise ValueError(f"region '{r.get('name', '<unnamed>')}' has non-positive rect")
    if x + w > img.size[0] or y + h > img.size[1]:
        raise ValueError(
            f"region '{r.get('name', '<unnamed>')}' ({x},{y} {w}x{h}) "
            f"escapes {img.size[0]}x{img.size[1]} image")
    return img.crop((x, y, x + w, y + h))


def score_region(ref_crop, cur_crop, mode, fuzz, ref_path, cur_path):
    """Return a (score, normalized_score_in_0_1) for the crop under the
    chosen global metric. SSIMULACRA2 needs file paths, so the crops are
    written to temp files when that mode is selected."""
    if mode == "ssim":
        s = ssim_score(ref_crop, cur_crop)
        return (f"{s:.4f}", s)
    if mode == "pixel":
        s = pixel_score(ref_crop, cur_crop, fuzz)
        return (f"{s:.2f}%", s / 100.0)
    if mode == "ssimulacra2":
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as rf, \
             tempfile.NamedTemporaryFile(suffix=".png", delete=False) as cf:
            ref_crop.save(rf.name)
            cur_crop.save(cf.name)
            try:
                s = ssimulacra2_score(rf.name, cf.name)
            finally:
                os.unlink(rf.name)
                os.unlink(cf.name)
        return (f"{s:.2f}", s / 100.0)
    die(1, f"unknown --mode: {mode}")


def tolerance_threshold(tolerance, mode, user_threshold):
    """Map a region's tolerance label to a metric-specific normalized
    threshold in [0, 1]. `user_threshold` is the global flag (e.g. 0.95
    for SSIM or 95 for pixel), used as the `relaxed` value."""
    if tolerance == TOL_IGNORED:
        return None  # skip
    if tolerance == TOL_STRICT:
        # Strict: require virtually identical content.
        if mode == "ssim":
            return 0.99
        if mode == "pixel":
            return 1.0  # 100% identical
        return 0.95     # ssimulacra2
    # relaxed: use the user threshold (converted to 0..1).
    if mode == "pixel":
        return user_threshold / 100.0
    return user_threshold  # ssim / ssimulacra2 are already 0..1


# ---- OCR ---------------------------------------------------------------

def ocr_region(img, expected, allow_missing):
    """Return (ok, actual_text) for an OCR assertion. When tesseract is
    missing and `allow_missing` is True, returns (True, '<ocr-skipped>')
    with an info log; else exits 4."""
    bin_path = shutil.which("tesseract")
    if not bin_path:
        if allow_missing:
            info(f"tesseract not installed; skipping OCR assertion for '{expected}'")
            return (True, "<ocr-skipped>")
        die(4, "OCR assertion needs `tesseract` (apt: tesseract-ocr). "
               "Pass --allow-missing-ocr to soft-skip.")
    with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tf:
        img.save(tf.name)
        try:
            out = subprocess.check_output(
                [bin_path, tf.name, "-", "-l", "eng", "--psm", "6"],
                stderr=subprocess.DEVNULL, timeout=30).decode("utf-8", errors="replace")
        except subprocess.CalledProcessError as e:
            os.unlink(tf.name)
            die(4, f"tesseract failed: {e}")
        except subprocess.TimeoutExpired:
            os.unlink(tf.name)
            die(4, "tesseract timed out")
        finally:
            try:
                os.unlink(tf.name)
            except FileNotFoundError:
                pass
    actual = out.strip()
    # Normalize whitespace on both sides so "C:\>" renders-with-padding
    # still matches the literal in the needle.
    norm_actual = re.sub(r"\s+", " ", actual)
    norm_expected = re.sub(r"\s+", " ", expected.strip())
    return (norm_expected in norm_actual, actual)


# ---- Diff image writer --------------------------------------------------

def write_diff(ref_img, cur_img, regions_result, diff_path):
    """Write a debug diff image:
       * base = pixel difference between ref and cur,
       * red outline around regions that failed,
       * gray overlay on regions marked 'ignored',
       * green outline around regions that passed."""
    diff = ImageChops.difference(ref_img.convert("RGB"),
                                  cur_img.convert("RGB"))
    overlay = diff.convert("RGBA").copy()
    draw = ImageDraw.Draw(overlay, "RGBA")
    for name, r, status in regions_result:
        rect = (r["x"], r["y"], r["x"] + r["w"] - 1, r["y"] + r["h"] - 1)
        if status == "ignored":
            draw.rectangle(rect, fill=(128, 128, 128, 96))
        elif status == "pass":
            draw.rectangle(rect, outline=(0, 200, 0, 255), width=2)
        else:  # fail / ocr-fail
            draw.rectangle(rect, outline=(255, 0, 0, 255), width=3)
        # Label
        draw.text((r["x"] + 2, r["y"] + 2), f"{name}:{status}",
                  fill=(255, 255, 255, 255))
    # Atomic write: .tmp + rename so a half-written diff never overwrites
    # a prior good one. Matches compare-screenshot.sh's pattern. Pillow
    # can't auto-infer the format from a `.tmp.NNNN` suffix, so pass
    # format="PNG" explicitly.
    tmp_path = diff_path + f".tmp.{os.getpid()}"
    overlay.convert("RGB").save(tmp_path, format="PNG")
    os.replace(tmp_path, diff_path)


# ---- Main ---------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Perceptual + structured-needle screenshot comparator.")
    ap.add_argument("reference", help="Reference PNG")
    ap.add_argument("current",   help="Current / candidate PNG")
    ap.add_argument("diff", nargs="?", default=None,
                    help="Diff image output (default: <current>.diff.png)")
    ap.add_argument("--mode", choices=("pixel", "ssim", "ssimulacra2"),
                    default="ssim",
                    help="Comparison mode (default: ssim)")
    ap.add_argument("--threshold", type=float, default=None,
                    help="Pass threshold (ssim: 0..1 default 0.95; "
                         "pixel: 0..100 default 95; ssimulacra2: 0..100 default 80)")
    ap.add_argument("--fuzz", type=float, default=2.0,
                    help="Pixel-mode per-channel fuzz percent (default 2.0)")
    ap.add_argument("--needle", default=None,
                    help="Path to needle JSON sidecar")
    ap.add_argument("--allow-missing-ocr", action="store_true",
                    help="Soft-skip OCR assertions when tesseract is absent")
    args = ap.parse_args()

    # Normalize thresholds per mode.
    if args.threshold is None:
        args.threshold = {"ssim": 0.95, "pixel": 95.0, "ssimulacra2": 80.0}[args.mode]

    if args.fuzz < 0 or args.fuzz > 100:
        die(1, f"--fuzz must be in [0, 100], got {args.fuzz}")

    if args.mode == "pixel" and not (0 <= args.threshold <= 100):
        die(1, f"pixel threshold must be in [0, 100], got {args.threshold}")
    if args.mode == "ssim" and not (0 <= args.threshold <= 1):
        die(1, f"ssim threshold must be in [0, 1], got {args.threshold}")
    if args.mode == "ssimulacra2" and not (0 <= args.threshold <= 100):
        die(1, f"ssimulacra2 threshold must be in [0, 100], got {args.threshold}")

    # Separate existence/readability (exit 2) from decode failure (exit 4)
    # so callers that bucket fixture errors independently from broken PNGs
    # can tell them apart. Codex [M] quality review.
    for role, path in (("reference", args.reference), ("current", args.current)):
        if not os.path.isfile(path):
            die(2, f"{role} image missing: {path}")
        if not os.access(path, os.R_OK):
            die(2, f"{role} image not readable: {path} (permissions?)")

    diff_path = args.diff or (os.path.splitext(args.current)[0] + ".diff.png")
    os.makedirs(os.path.dirname(os.path.abspath(diff_path)) or ".", exist_ok=True)

    try:
        ref_img = Image.open(args.reference)
        cur_img = Image.open(args.current)
        ref_img.load(); cur_img.load()
    except (PermissionError, IsADirectoryError) as e:
        # These are still "readability" failures -- exit 2 -- even though
        # the earlier os.access() check passed: races, TOCTOU, or a
        # directory named like a file.
        die(2, f"failed to open inputs: {e}")
    except Exception as e:
        die(4, f"failed to decode inputs: {e}")

    if ref_img.size != cur_img.size:
        die(3, f"dimension mismatch: ref={ref_img.size}, cur={cur_img.size}")

    # Threshold for pass/fail on the 0..1 normalized scale.
    if args.mode == "pixel":
        norm_thresh = args.threshold / 100.0
    elif args.mode == "ssim":
        norm_thresh = args.threshold
    else:
        norm_thresh = args.threshold / 100.0

    # Auto-discover a sibling `<reference>.needle.json` when --needle was
    # not passed. Without this, a committed sidecar was silently ignored
    # and clock/OCR regions never applied -- Codex [H] quality review.
    # Explicit --needle always wins. When the caller really wants the
    # full-frame fallback even with a sibling present, they can pass
    # --needle /dev/null (fails schema validation cleanly), or we can
    # add an --no-needle flag later if anyone needs it.
    resolved_needle = args.needle
    if resolved_needle is None:
        sibling = os.path.splitext(args.reference)[0] + ".needle.json"
        if os.path.isfile(sibling):
            info(f"auto-discovered sibling needle: {sibling}")
            resolved_needle = sibling

    needle = None
    if resolved_needle:
        try:
            with open(resolved_needle, "r", encoding="utf-8") as f:
                needle = json.load(f)
        except Exception as e:
            die(4, f"failed to parse needle JSON {resolved_needle}: {e}")
        validate_needle(needle, resolved_needle)

    # No needle: one global region covering the whole image.
    if needle is None:
        needle = {"version": 1, "regions": [{
            "name": "full-frame",
            "x": 0, "y": 0,
            "w": ref_img.size[0], "h": ref_img.size[1],
            "tolerance": TOL_RELAXED,
        }]}

    # SSIMULACRA2 mode scores via an external subprocess + two PNG
    # temp-writes per region. Multi-region needles therefore multiply
    # wall time by region count (subprocess startup + encode cost),
    # exactly where structured needles are supposed to reduce work.
    # Reject upfront with a clear pointer to the alternative modes
    # that DO support regions in-process. Codex [M] quality review.
    if args.mode == "ssimulacra2":
        scored = [r for r in needle["regions"]
                  if r.get("tolerance", TOL_RELAXED) != TOL_IGNORED]
        if len(scored) > 1:
            die(1, f"--mode ssimulacra2 supports only one scored region "
                   f"per run (got {len(scored)}); use --mode ssim for "
                   f"multi-region needles, or split into per-region runs")

    info(f"mode={args.mode} threshold={args.threshold} "
         f"regions={len(needle['regions'])} size={ref_img.size[0]}x{ref_img.size[1]}")

    overall_pass = True
    regions_result = []
    fail_detail = []

    for r in needle["regions"]:
        name = r.get("name", f"region@{r.get('x', '?')},{r.get('y', '?')}")
        tolerance = r.get("tolerance", TOL_RELAXED)
        if tolerance not in VALID_TOL:
            die(4, f"region '{name}' has invalid tolerance '{tolerance}'")

        try:
            ref_crop = crop_region(ref_img, r)
            cur_crop = crop_region(cur_img, r)
        except ValueError as e:
            die(4, str(e))

        if tolerance == TOL_IGNORED:
            info(f"  {name}: IGNORED (tolerance=ignored)")
            regions_result.append((name, r, "ignored"))
            continue

        score_str, score01 = score_region(
            ref_crop, cur_crop, args.mode, args.fuzz,
            args.reference, args.current)
        reg_thresh = tolerance_threshold(tolerance, args.mode, args.threshold)

        if score01 + 1e-9 >= reg_thresh:
            status = "pass"
            info(f"  {name}: PASS score={score_str} tolerance={tolerance}")
        else:
            status = "fail"
            overall_pass = False
            fail_detail.append(
                f"{name} score={score_str} (needed >= {reg_thresh:.4f} "
                f"under tolerance={tolerance})")
            info(f"  {name}: FAIL score={score_str} "
                 f"< {reg_thresh:.4f} ({tolerance})")

        # OCR, when requested. OCR failure promotes the result to
        # "ocr-fail" regardless of the pixel/SSIM status -- a visually
        # matching region with the wrong text is still a regression.
        if "ocr" in r:
            ok, actual = ocr_region(cur_crop, r["ocr"], args.allow_missing_ocr)
            if not ok:
                status = "ocr-fail"
                overall_pass = False
                fail_detail.append(
                    f"{name} OCR expected '{r['ocr']}', got '{actual}'")
                info(f"  {name}: OCR FAIL expected='{r['ocr']}' actual='{actual[:60]}'")
            else:
                info(f"  {name}: OCR PASS")

        regions_result.append((name, r, status))

    write_diff(ref_img, cur_img, regions_result, diff_path)

    if overall_pass:
        print(f"[needle-compare] Visual match: {args.mode} threshold={args.threshold} "
              f"({len(regions_result)} region(s)) -- diff: {diff_path}")
        sys.exit(0)
    print(f"[needle-compare] Visual MISMATCH: {args.mode} "
          f"({len(fail_detail)} region(s) failed of {len(regions_result)}) -- diff: {diff_path}",
          file=sys.stderr)
    for d in fail_detail:
        print(f"  - {d}", file=sys.stderr)
    sys.exit(5)


if __name__ == "__main__":
    main()
