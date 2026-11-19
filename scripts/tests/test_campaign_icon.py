"""Tests of `scripts/campaign-icon.py`: a launcher's icon, the project's icon with a runner's badge or a link's overlay."""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import unittest.mock

SCRIPT = Path(__file__).resolve().parents[1] / "campaign-icon.py"
_spec = importlib.util.spec_from_file_location("campaign_icon", SCRIPT)
assert _spec is not None and _spec.loader is not None
icon = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(icon)
TILE = (16, 34, 26)  # a dark green tile, like the SDK's own


def windowless():
    """No console window of its own for any process a test starts on Windows (a console child of a parent with
    none opens one over the owner's work: 2026-11-12, Office's launcher tests flashed 5 to 10); 0 elsewhere."""
    return subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def solid(size, colour=TILE):
    return bytearray(bytes((*colour, 255)) * (size * size))


def pixel(rgba, size, x, y):
    o = (y * size + x) * 4
    return tuple(rgba[o:o + 4])


def close(a, b, tolerance=6):
    return all(abs(p - q) <= tolerance for p, q in zip(a, b))


class PngAndIcoTest(unittest.TestCase):
    def test_a_png_reads_back_as_written(self):
        rgba = bytearray(range(256)) * (8 * 8 * 4 // 256)
        width, height, back = icon.read_png(icon.write_png(8, 8, rgba))
        self.assertEqual((width, height, back), (8, 8, rgba))

    def test_an_icon_holds_every_frame_smallest_first(self):
        frames = [(size, icon.write_png(size, size, solid(size))) for size in (256, 16, 48)]
        data = icon.pack_ico(frames)
        read = icon.read_ico(data)
        self.assertEqual(sorted(read), [16, 48, 256])
        self.assertEqual(read[48][2], solid(48))
        self.assertEqual(data[6], 16, "the first entry is the smallest frame")
        self.assertEqual(data[6 + 32], 0, "a 256-pixel frame is written as 0")

    def test_area_resizing_keeps_a_solid_colour_and_brings_no_dark_fringe(self):
        self.assertEqual(icon.resize(64, 64, solid(64), 16), solid(16))
        half = bytearray(bytes((255, 255, 255, 255)) * 32 + bytes((0, 0, 0, 0)) * 32) * 64  # white left, clear right
        edge = pixel(icon.resize(64, 64, half, 16), 16, 7, 8)
        self.assertEqual(edge[:3], (255, 255, 255), "a transparent neighbour darkened the edge")


class PathTest(unittest.TestCase):
    def bounds(self, name):
        points = [p for poly in icon.flatten_path(icon.MARKS[name][1]) for p in poly]
        xs, ys = [p[0] for p in points], [p[1] for p in points]
        return min(xs), min(ys), max(xs), max(ys)

    def test_each_mark_fills_its_own_box(self):
        for name, size in (("claude", 100), ("gemini", 65)):
            with self.subTest(mark=name):
                left, top, right, bottom = self.bounds(name)
                self.assertGreaterEqual(min(left, top), -0.5)
                self.assertLessEqual(max(right, bottom), size + 0.5)
                self.assertGreater(right - left, size * 0.9)

    def test_arcs_with_packed_flags_are_read(self):
        # "a1 1 0 001 1" is a quarter circle whose flags (0 0) run into the next number.
        polys = icon.flatten_path("M0 0a1 1 0 001 1z")
        self.assertAlmostEqual(polys[0][-1][0], 1.0, places=6)
        self.assertAlmostEqual(polys[0][-1][1], 1.0, places=6)

    def test_a_square_covers_its_pixels_exactly(self):
        cover = icon.coverage([[(1, 1), (3, 1), (3, 3), (1, 3)]], 4, 1, 0, 0)
        self.assertEqual([round(c, 3) for c in cover[4:8]], [0, 1, 1, 0])
        self.assertEqual(round(sum(cover), 3), 4)


class BadgeTest(unittest.TestCase):
    def badge(self, runner, size):
        fill, mark = icon.BADGES[runner]
        return icon.badge_frame(size, solid(size), fill, mark)

    def test_the_badge_sits_bottom_right_in_the_runners_colour_and_leaves_the_rest(self):
        for runner, colour in (("claude", (0xD9, 0x77, 0x57)), ("agy", (0x4C, 0x8D, 0xF6)),
                               ("codex", (0x10, 0xA3, 0x7F)), ("watch", (0x6B, 0x5B, 0xD6)),
                               ("stop", (0xD9, 0x3A, 0x3A))):
            with self.subTest(runner=runner):
                out = self.badge(runner, 64)
                self.assertTrue(close(pixel(out, 64, 4, 4), (*TILE, 255)), "the badge reached the top left")
                radius = 64 * 0.24
                centre = 64 - radius - 0.64
                # Between the mark (a box 1.15 radii wide) and the white ring: the runner's colour alone.
                inner = pixel(out, 64, round(centre - radius * 0.8), round(centre))
                self.assertTrue(close(inner, (*colour, 255), 12), (runner, inner))
                ring = pixel(out, 64, round(centre), round(centre + radius - 0.9))
                self.assertTrue(close(ring[:3], (255, 255, 255), 60), ("the white outline", ring))

    def test_the_run_controls_carry_their_marks(self):
        # The Watch and Stop shortcuts: at the badge's centre the stop square is white, and the eye's pupil is white
        # inside an iris cut back to the badge's colour.
        radius = 256 * 0.24
        centre = 256 - radius - 2.56
        unit = radius * 1.15 / 24  # one unit of the marks' 24-unit box
        stop = self.badge("stop", 256)
        self.assertTrue(close(pixel(stop, 256, round(centre), round(centre))[:3], (255, 255, 255), 20))
        eye = self.badge("watch", 256)
        self.assertTrue(close(pixel(eye, 256, round(centre), round(centre))[:3], (255, 255, 255), 20), "no pupil")
        iris = pixel(eye, 256, round(centre + 3.3 * unit), round(centre))
        self.assertTrue(close(iris[:3], (0x6B, 0x5B, 0xD6), 20), ("the iris was not cut out", iris))
        lens = pixel(eye, 256, round(centre + 8 * unit), round(centre))
        self.assertTrue(close(lens[:3], (255, 255, 255), 20), ("no lens", lens))

    def test_a_mark_is_drawn_from_32_pixels_up_and_not_below(self):
        def whites(out, size):
            return sum(1 for i in range(size * size) if out[i * 4:i * 4 + 3] == bytearray((255, 255, 255)))
        plain_16, mark_16 = icon.badge_frame(16, solid(16), "#D97757", None), self.badge("claude", 16)
        self.assertEqual(plain_16, mark_16, "a mark was drawn at 16 pixels")
        plain_64, mark_64 = icon.badge_frame(64, solid(64), "#D97757", None), self.badge("claude", 64)
        self.assertGreater(whites(mark_64, 64), whites(plain_64, 64) + 20, "no mark at 64 pixels")
        codex = self.badge("codex", 64)
        plain = icon.badge_frame(64, solid(64), "#10A37F", None)
        self.assertGreater(whites(codex, 64), whites(plain, 64) + 20, "Codex must carry its logo")

    def test_the_command_writes_an_icon_with_every_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "base.png"
            base.write_bytes(icon.write_png(256, 256, solid(256)))
            target = Path(tmp) / "out" / "claude.ico"
            done = subprocess.run([sys.executable, "-B", str(SCRIPT), "badge", str(base), "claude", str(target)],
                                  capture_output=True, text=True, timeout=600, creationflags=windowless())
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(sorted(icon.read_ico(target.read_bytes())), list(icon.SIZES))
            self.assertEqual([p.name for p in target.parent.iterdir()], ["claude.ico"], "a temporary file was left")
            bad = subprocess.run([sys.executable, "-B", str(SCRIPT), "badge", str(base), "gemini", str(target)],
                                 capture_output=True, text=True, timeout=60, creationflags=windowless())
            self.assertEqual(bad.returncode, 2)
            self.assertIn("usage: campaign-icon.py", bad.stderr)


class OverlayTest(unittest.TestCase):
    """An everyday link's icon: the project's icon with a rounded square in the corner."""

    @staticmethod
    def frame(mark, size=256):
        return icon.overlay_frame(size, solid(size), mark)

    def test_the_rounded_square_sits_bottom_right_on_a_white_outline_and_leaves_the_rest(self):
        out = self.frame("claude")
        orange = icon.hex_colour("#D97757")
        # 256 is half the 512 geometry: the outline square from 111, the filled one from 118 to 254, corners of 30.
        self.assertTrue(close(pixel(out, 256, 124, 186), (*orange, 255)), "no fill at the square's centre-left")
        self.assertTrue(close(pixel(out, 256, 114, 186)[:3], (255, 255, 255), 30), "no white outline on the left")
        self.assertEqual(pixel(out, 256, 0, 0), (*TILE, 255), "the base icon changed outside the overlay")
        self.assertEqual(pixel(out, 256, 100, 186), (*TILE, 255), "the overlay reaches past its outline")
        # Inside the rounded corner and outside any circle in the same box: a round badge would leave the tile here.
        self.assertTrue(close(pixel(out, 256, 128, 128), (*orange, 255)), "the corner is not square")

    def test_each_mark_has_its_colour_and_a_white_mark_inside_from_32_pixels(self):
        def whites(out, size):
            return sum(1 for i in range(size * size) if out[i * 4:i * 4 + 3] == bytearray((255, 255, 255)))
        colours = {"claude": "#D97757", "codex": "#10A37F", "agy": "#4C8DF6", "wordpress": "#21759B",
                   "site": "#2563EB"}
        self.assertEqual(set(icon.OVERLAYS), set(colours))
        for mark, colour in colours.items():
            with self.subTest(mark=mark):
                self.assertEqual(icon.OVERLAYS[mark][0], colour)
                out = self.frame(mark)
                self.assertTrue(close(pixel(out, 256, 124, 186), (*icon.hex_colour(colour), 255)), mark)
                # The mark: white inside the filled square (118 to 254 at 256 pixels), beyond the outline's own.
                inside = sum(1 for y in range(140, 232) for x in range(140, 232)
                             if out[(y * 256 + x) * 4:(y * 256 + x) * 4 + 3] == bytearray((255, 255, 255)))
                self.assertGreater(inside, 400, f"no {mark} mark in white")
        plain = icon.overlay_frame(16, solid(16), "site")
        self.assertLessEqual(whites(plain, 16), whites(icon.overlay_frame(16, solid(16), "claude"), 16) + 2,
                             "a mark was drawn below 32 pixels")

    def run_overlay(self, base, mark, target):
        return subprocess.run([sys.executable, "-B", str(SCRIPT), "overlay", str(base), mark, str(target)],
                              capture_output=True, text=True, timeout=600, creationflags=windowless())

    def test_the_command_writes_an_icon_with_every_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "base.png"
            base.write_bytes(icon.write_png(256, 256, solid(256)))
            target = Path(tmp) / "out" / "launcher-site.ico"
            done = self.run_overlay(base, "site", target)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertEqual(sorted(icon.read_ico(target.read_bytes())), list(icon.SIZES))
            self.assertEqual([p.name for p in target.parent.iterdir()], ["launcher-site.ico"], "a temporary file")
            self.assertIn("campaign-icon.py overlay <base.png or .ico>", icon.USAGE)

    def test_every_saved_icon_carries_its_own_mark_from_a_png_or_an_ico_base(self):
        # Proven on the command's decoded output, not on overlay_frame alone.
        def whites(rgba, size, box=None):
            x0, y0, x1, y1 = box or (0, 0, size, size)
            return {(x, y) for y in range(y0, y1) for x in range(x0, x1)
                    if rgba[(y * size + x) * 4:(y * size + x) * 4 + 3] == bytearray((255, 255, 255))}
        with tempfile.TemporaryDirectory() as tmp:
            png = Path(tmp) / "base.png"
            png.write_bytes(icon.write_png(256, 256, solid(256)))
            ico = Path(tmp) / "base.ico"
            ico.write_bytes(icon.pack_ico([(s, icon.write_png(s, s, solid(s))) for s in (16, 48, 256)]))
            patterns = {}
            for base in (png, ico):
                for mark, (colour, _drawn) in icon.OVERLAYS.items():
                    with self.subTest(base=base.suffix, mark=mark):
                        target = Path(tmp) / f"{base.suffix[1:]}-{mark}.ico"
                        done = self.run_overlay(base, mark, target)
                        self.assertEqual(done.returncode, 0, done.stderr)
                        frames = icon.read_ico(target.read_bytes())
                        self.assertEqual(sorted(frames), list(icon.SIZES))
                        big = frames[256][2]
                        self.assertTrue(close(pixel(big, 256, 124, 186), (*icon.hex_colour(colour), 255)), mark)
                        self.assertEqual(pixel(big, 256, 0, 0), (*TILE, 255))
                        mark_box = whites(big, 256, (140, 140, 232, 232))   # the 186 box at 512, inside the fill
                        self.assertGreater(len(mark_box), 400, f"no {mark} mark")
                        patterns.setdefault(mark, mark_box)
                        self.assertEqual(patterns[mark], mark_box, "a PNG and an ICO base drew different marks")
                        with unittest.mock.patch.object(icon, "MARK_FROM", 10 ** 6):
                            plain = {s: icon.overlay_frame(s, solid(s), mark) for s in (24, 32)}
                        # At 32 pixels a mark is anti-aliased into few pure whites: count the pixels it lightens.
                        lighter = sum(1 for i in range(32 * 32)
                                      if min(frames[32][2][i * 4:i * 4 + 3]) >= min(plain[32][i * 4:i * 4 + 3]) + 40)
                        self.assertGreater(lighter, 15, f"no {mark} mark at MARK_FROM")
                        self.assertEqual(frames[24][2], plain[24], f"a {mark} mark below MARK_FROM")
            marks = list(patterns)
            for i, one in enumerate(marks):
                for other in marks[i + 1:]:
                    self.assertGreater(len(patterns[one] ^ patterns[other]), 300, f"{one} and {other} look alike")

    def test_a_bad_mark_base_or_target_is_refused_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "base.png"
            base.write_bytes(icon.write_png(64, 64, solid(64)))
            text = Path(tmp) / "base.txt"
            text.write_text("not an image\n", encoding="utf-8")
            cases = {"an unknown mark": (base, "drupal", "a.ico", 2, "usage: campaign-icon.py"),
                     "a base that is neither PNG nor ICO": (text, "site", "b.ico", 1, "campaign-icon: not a PNG"),
                     "a target that is not .ico": (base, "site", "c.png", 2, "usage: campaign-icon.py")}
            for case, (source, mark, name, code, said) in cases.items():
                for existing in (None, b"the project's own icon"):
                    with self.subTest(case=case, existing=existing is not None):
                        folder = Path(tmp) / f"{name[0]}-{'kept' if existing else 'new'}"
                        target = folder / name
                        if existing:
                            folder.mkdir(exist_ok=True)
                            target.write_bytes(existing)
                        done = self.run_overlay(source, mark, target)
                        self.assertEqual(done.returncode, code, done.stderr)
                        self.assertIn(said, done.stderr)
                        self.assertNotIn("Traceback", done.stderr)
                        if existing:
                            self.assertEqual(target.read_bytes(), existing, f"{case} changed the target")
                        else:
                            self.assertFalse(target.exists(), f"{case} wrote {target}")
                        left = sorted(p.name for p in folder.iterdir()) if folder.exists() else []
                        self.assertEqual(left, [name] if existing else [], f"{case} left a file")

    def test_a_failed_write_leaves_the_old_icon_and_no_temporary_file(self):
        def half_written(path, data):
            with open(path, "wb") as handle:
                handle.write(data[:2])   # some bytes reach the disk before the failure
            raise OSError("disk full")
        for case, patch in (("the rename fails", unittest.mock.patch.object(icon.os, "replace",
                                                                             side_effect=OSError("disk full"))),
                            ("the write fails part way", unittest.mock.patch.object(Path, "write_bytes",
                                                                                    half_written))):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                target = Path(tmp) / "launcher.ico"
                target.write_bytes(b"old")
                with patch, self.assertRaises(OSError):
                    icon.write_atomic(target, b"new icon")
                self.assertEqual(target.read_bytes(), b"old")
                self.assertEqual([p.name for p in Path(tmp).iterdir()], ["launcher.ico"])

if __name__ == "__main__":
    unittest.main()
