#!/usr/bin/env python3
"""Read the panic stop screen back off a QEMU display capture.

The smoke test judges a crash leg by what reached the SCREEN, not only by
serial: printk writes serial before the framebuffer, so a serial marker cannot
show the stop screen painted. The kernel console draws an 8x16 bitmap font on
a fixed grid in exact colours (fb_draw_char in src/kernel/drivers/framebuffer.c),
so the text can be decoded cell by cell against that same font table.

    smoke-screen.py capture <monitor-socket> <out.ppm>
        screendump through the QEMU HMP monitor, wait for the command's prompt,
        then wait until the file is a complete PPM (bounded by a deadline)
    smoke-screen.py check <ppm> <needle> [<needle> ...]
        exit 0 only when most of the frame is the panic blue AND every needle
        appears in the decoded console text

    python3 scripts/tests/test_smoke_screen.py   (fixtures)
"""
from __future__ import annotations

import pathlib
import re
import socket
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent
FONT_SOURCE = REPO / "src" / "kernel" / "drivers" / "framebuffer.c"
FONT_W, FONT_H = 8, 16
# PANIC_BG_COLOR in src/kernel/panic.c, as the RGB bytes a PPM stores.
PANIC_BG = bytes((0x00, 0x33, 0x80))
# The colours stop-screen text is drawn in (PANIC_FG_COLOR, PANIC_DIM_COLOR and
# PANIC_ACCENT in src/kernel/panic.c). A glyph pixel in any other colour, even
# one step off the background, makes its cell unreadable rather than text.
PANIC_TEXT = (bytes((0xFF, 0xFF, 0xFF)), bytes((0xA0, 0xB0, 0xD0)), bytes((0x44, 0x88, 0xFF)))
MIN_BLUE_SHARE = 0.5
MAX_DIM = 16384
PROMPT = b"(qemu) "


class ScreenError(Exception):
    """A capture that cannot be judged: missing, truncated or malformed."""


def load_font(path: pathlib.Path = FONT_SOURCE) -> dict[bytes, str]:
    """Map each glyph's 16 row bytes to its character.

    Reads the rows of kernel_font_data, one `/* NN 'c' */ {0x..,...}` line per
    glyph (this repo's own table format), keyed by the decimal code NN.
    """
    glyphs: dict[bytes, str] = {}
    # parser-allow: one fixed line shape of this repo's own C table
    row = re.compile(r"^\s*/\*\s*(\d+)\s.*?\*/\s*\{([^}]*)\}")
    for line in path.read_text(encoding="utf-8").splitlines():
        m = row.match(line)
        if not m:
            continue
        values = [int(v, 16) for v in m.group(2).split(",") if v.strip()]
        if len(values) != FONT_H:
            raise ScreenError("font row for code %s has %d bytes" % (m.group(1), len(values)))
        glyphs.setdefault(bytes(values), chr(int(m.group(1))))
    if len(glyphs) < 90:
        raise ScreenError("font table in %s not found (%d glyphs)" % (path, len(glyphs)))
    return glyphs


def read_ppm(data: bytes) -> tuple[int, int, bytes]:
    """Parse a binary 8-bit P6 PPM strictly; any short or odd input raises."""
    fields: list[bytes] = []
    pos, n = 0, len(data)
    while len(fields) < 4:
        if pos >= n:
            raise ScreenError("truncated PPM header")
        ch = data[pos:pos + 1]
        if ch.isspace():
            pos += 1
            continue
        if ch == b"#":
            nl = data.find(b"\n", pos)
            if nl < 0:
                raise ScreenError("truncated PPM header comment")
            pos = nl + 1
            continue
        end = pos
        while end < n and not data[end:end + 1].isspace():
            end += 1
        if end >= n:
            raise ScreenError("truncated PPM header")
        fields.append(data[pos:end])
        pos = end
    pos += 1  # the single whitespace byte that ends the header
    magic, raw_w, raw_h, raw_max = fields
    if magic != b"P6":
        raise ScreenError("not a binary PPM (magic %r)" % magic)
    if not (raw_w.isdigit() and raw_h.isdigit() and raw_max.isdigit()):
        raise ScreenError("non-numeric PPM header field")
    w, h, maxval = int(raw_w), int(raw_h), int(raw_max)
    if not (0 < w <= MAX_DIM and 0 < h <= MAX_DIM):
        raise ScreenError("PPM dimensions %dx%d out of range" % (w, h))
    if maxval != 255:
        raise ScreenError("PPM maxval %d, expected 255" % maxval)
    px = data[pos:]
    if len(px) != w * h * 3:
        raise ScreenError("PPM raster is %d bytes, expected %d" % (len(px), w * h * 3))
    return w, h, px


def decode_text(w: int, h: int, px: bytes, glyphs: dict[bytes, str],
                bg: bytes = PANIC_BG, fg: tuple[bytes, ...] = PANIC_TEXT) -> list[str]:
    """Decode the console grid: a pixel in `bg` is clear, one in `fg` is set,
    and any other colour makes the whole cell undecodable."""
    stride = w * 3
    lines = []
    for cy in range(h // FONT_H):
        out = []
        for cx in range(w // FONT_W):
            rows = bytearray(FONT_H)
            readable = True
            for py in range(FONT_H):
                base = (cy * FONT_H + py) * stride + cx * FONT_W * 3
                bits = 0
                for x in range(FONT_W):
                    pixel = px[base + x * 3:base + x * 3 + 3]
                    if pixel == bg:
                        continue
                    if pixel not in fg:
                        readable = False
                    bits |= 0x80 >> x
                rows[py] = bits
            out.append(glyphs.get(bytes(rows), "\x7f") if readable else "\x7f")
        lines.append("".join(out).rstrip())
    return lines


def blue_share(w: int, h: int, px: bytes, bg: bytes = PANIC_BG) -> float:
    blue = sum(1 for i in range(0, len(px), 3) if px[i:i + 3] == bg)
    return blue / float(w * h)


def check(data: bytes, needles: list[str], glyphs: dict[bytes, str]) -> str:
    """Return a one-line summary, or raise ScreenError naming what is missing."""
    w, h, px = read_ppm(data)
    share = blue_share(w, h, px)
    if share < MIN_BLUE_SHARE:
        raise ScreenError("%dx%d frame is %.0f%% panic blue" % (w, h, share * 100))
    text = "\n".join(decode_text(w, h, px, glyphs))
    missing = [n for n in needles if n not in text]
    if missing:
        raise ScreenError("%dx%d, %.0f%% panic blue, but the screen text lacks %r"
                          % (w, h, share * 100, missing[0]))
    return "%dx%d, %.0f%% panic blue, %d line(s) read back from the glyphs" % (
        w, h, share * 100, len(needles))


def _read_until(sock: socket.socket, buf: bytearray, count: int, deadline: float) -> None:
    """Read until `buf` holds `count` monitor prompts or the deadline passes."""
    while buf.count(PROMPT) < count:
        left = deadline - time.monotonic()
        if left <= 0:
            raise ScreenError("QEMU monitor gave no prompt in time")
        sock.settimeout(left)
        try:
            chunk = sock.recv(4096)
        except socket.timeout as exc:
            raise ScreenError("QEMU monitor gave no prompt in time") from exc
        if not chunk:
            raise ScreenError("QEMU monitor closed the connection")
        buf.extend(chunk)


def capture(sock_path: str, out: str, timeout: float = 15.0) -> None:
    out_path = pathlib.Path(out)
    out_path.unlink(missing_ok=True)
    deadline = time.monotonic() + timeout
    buf = bytearray()
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        sock.settimeout(timeout)
        sock.connect(sock_path)
        _read_until(sock, buf, 1, deadline)  # the greeting's prompt
        sock.sendall(("screendump %s\n" % out).encode())
        _read_until(sock, buf, 2, deadline)  # the prompt after the command
    finally:
        sock.close()
    # The prompt follows the command, but confirm the file is whole rather
    # than trusting the order: judge only a capture that parses completely.
    last = "no file written"
    while time.monotonic() < deadline:
        try:
            read_ppm(out_path.read_bytes())
            return
        except FileNotFoundError:
            last = "no file written"
        except ScreenError as exc:
            last = str(exc)
        time.sleep(0.2)
    raise ScreenError("screendump incomplete: %s" % last)


def main(argv: list[str]) -> int:
    try:
        if len(argv) == 3 and argv[0] == "capture":
            capture(argv[1], argv[2])
            return 0
        if len(argv) >= 3 and argv[0] == "check":
            path = pathlib.Path(argv[1])
            if not path.is_file():
                raise ScreenError("no screen capture at %s" % path)
            print(check(path.read_bytes(), argv[2:], load_font()))
            return 0
    except (ScreenError, OSError) as exc:
        print(exc)
        return 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
