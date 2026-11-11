#!/usr/bin/env python3
"""Fixtures for scripts/smoke-screen.py, the crash leg's screen verdict.

Every frame is drawn with the kernel's own font table, the way fb_draw_char
draws it, so the positive case proves the decoder reads real glyphs and each
negative case is a way a broken stop screen used to pass (a partly blue frame
with no text, a truncated capture) or could pass.

    python3 scripts/tests/test_smoke_screen.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("smoke_screen", HERE.parent / "smoke-screen.py")
ss = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ss)

GLYPHS = ss.load_font()
BY_CHAR = {c: rows for rows, c in GLYPHS.items()}
W, H = 640, 480
BG = ss.PANIC_BG
FG = bytes((0xFF, 0xFF, 0xFF))
DIM = bytes((0xA0, 0xB0, 0xD0))
NEEDLES = ["Description: (unreadable: the panic string pointer faulted)",
           "Source:  (unreadable: the panic string pointer faulted):443"]

failures = 0


def frame(fill: bytes = BG) -> bytearray:
    return bytearray(fill * (W * H))


def draw(px: bytearray, col: int, row: int, text: str, fg: bytes) -> None:
    for i, ch in enumerate(text):
        glyph = BY_CHAR[ch]
        for py in range(ss.FONT_H):
            for x in range(ss.FONT_W):
                if glyph[py] & (0x80 >> x):
                    off = ((row * ss.FONT_H + py) * W + (col + i) * ss.FONT_W + x) * 3
                    px[off:off + 3] = fg


def ppm(px: bytes, w: int = W, h: int = H) -> bytes:
    return b"P6\n# fixture\n%d %d\n255\n" % (w, h) + bytes(px)


def expect(name: str, data: bytes, ok: bool, needles: list[str] = NEEDLES) -> None:
    global failures
    try:
        ss.check(data, needles, GLYPHS)
        got, why = True, ""
    except ss.ScreenError as exc:
        got, why = False, str(exc)
    status = "PASS" if got == ok else "FAIL"
    if got != ok:
        failures += 1
    print("%s  %s%s" % (status, name, ("  (" + why + ")") if why else ""))


stop = frame()
draw(stop, 4, 14, "Your Impossible OS ran into a problem and needs to restart.", FG)
draw(stop, 4, 17, "Stop code:  KERNEL_PANIC", DIM)
draw(stop, 4, 18, NEEDLES[0], FG)
draw(stop, 4, 19, NEEDLES[1], DIM)
expect("a painted stop screen with both placeholders passes", ppm(stop), True)

# The review's counterexample: 400 blue rows over 80 black ones, no glyphs.
half = frame()
for y in range(400, H):
    half[y * W * 3:(y + 1) * W * 3] = b"\x00\x00\x00" * W
expect("a partly blue frame with no text fails", ppm(half), False)

expect("a blue frame with no text fails", ppm(frame()), False)

one = frame()
draw(one, 4, 18, NEEDLES[0], FG)
expect("a screen missing the Source placeholder fails", ppm(one), False)

off_grid = frame()
draw(off_grid, 4, 18, NEEDLES[0], FG)
draw(off_grid, 4, 19, NEEDLES[1], FG)
shifted = bytearray(BG * 3) + off_grid[:-9]  # every glyph 3 px right of its cell
expect("text off the console grid does not decode", ppm(shifted), False)

expect("a black frame fails", ppm(frame(b"\x00\x00\x00")), False)

faint = frame()
draw(faint, 4, 18, NEEDLES[0], bytes((0x00, 0x33, 0x81)))
draw(faint, 4, 19, NEEDLES[1], bytes((0x00, 0x33, 0x81)))
expect("glyphs one colour step off the background fail", ppm(faint), False)

full = ppm(stop)
expect("a truncated raster fails", full[:-W * 3 * 30], False)
expect("an over-long raster fails", full + b"\x00\x00\x00", False)
expect("a non-P6 magic fails", b"P5" + full[2:], False)
expect("a 16-bit maxval fails", ppm(stop).replace(b"\n255\n", b"\n65535\n", 1), False)
expect("a zero width fails", b"P6\n0 480\n255\n", False)

# A header cut short mid-field must refuse, not spin: each runs under a clock.
for cut in (b"P6\n640 480\n25", b"P6\n640", b"P6\n# no newline", b""):
    t0 = time.monotonic()
    expect("a truncated header %r fails" % cut, cut, False)
    if time.monotonic() - t0 > 2.0:
        failures += 1
        print("FAIL  truncated header %r took over 2 s" % cut)

print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
