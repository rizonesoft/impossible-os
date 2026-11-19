#!/usr/bin/env python3
"""Render a launcher's icon: the project's own icon with a runner's badge or a link's overlay in the corner.

    python campaign-icon.py badge <base.png or .ico> <runner> <target.ico>
    python campaign-icon.py overlay <base.png or .ico> <mark> <target.ico>
    python campaign-icon.py frames <large.png> <small.png> <target.ico>

`badge` draws, on every frame of the project's icon, a circle in the runner's colour with a white outline at the
bottom right, and from 32 pixels up the runner's mark in white inside it: Claude Code's orange spark for `claude`,
Antigravity's blue sparkle for `agy` (the Carbon runner), Codex's green for `codex`. The unattended run's Watch and
Stop links carry the same badge as `watch` (a white eye on violet) and `stop` (a white stop square on red). `frames`
builds a Windows icon
from two large renders of a project's icon, the detailed one for 96 pixels and up and the heavy one below.
`overlay` marks an everyday link (one a person opens, where a campaign link starts a run): a rounded square where the
badge is a circle, in the mark's colour on a white outline, with the mark in white from 32 pixels up. Its marks are the
three runners' (`claude`, `codex`, `agy`), `wordpress` and `site` (a globe drawn here).

The marks are the official Claude symbol and Google Gemini sparkle (path data from their Wikimedia Commons files,
Claude_AI_symbol.svg and Google_Gemini_icon_2025.svg), trademarks of Anthropic and Google, drawn only into a
launcher icon on the operator's own machine; the Codex badge carries the OpenAI blossom logo. The `wordpress`
mark is the WordPress logo (Wikimedia Commons, WordPress_blue_logo.svg), the WordPress Foundation's trademark, drawn
the same way and only there. Everything here is the Python
standard library: PNG is read and written with zlib, the marks are rasterised from their path data, and every edge
is supersampled.
"""
from __future__ import annotations

import math
import os
from pathlib import Path
import re
import struct
import sys
import zlib

USAGE = ("usage: campaign-icon.py badge <base.png or .ico> <claude|agy|codex|watch|stop> <target.ico>\n"
         "       campaign-icon.py overlay <base.png or .ico> <claude|agy|codex|wordpress|site> <target.ico>\n"
         "       campaign-icon.py frames <large.png> <small.png> <target.ico>")
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
SMALL_UP_TO = 64  # frames up to this size come from the heavier small drawing, where thin lines would close up
MARK_FROM = 32  # below this a mark is a smudge: the colour alone tells the runners apart
OVERSAMPLE = 4
RUNNERS = {
    "claude": ("#D97757", "claude"),
    "agy": ("#4C8DF6", "gemini"),
    "codex": ("#10A37F", "codex"),
}
# The unattended run's own controls, badged like a runner: its Watch and Stop links.
CONTROLS = {
    "watch": ("#6B5BD6", "eye"),
    "stop": ("#D93A3A", "stop"),
}
BADGES = {**RUNNERS, **CONTROLS}
MARKS = {
    # An eye: a lens, the iris cut out of it (wound the other way), and the pupil inside.
    "eye": ("0 0 24 24", "M1 12A13 13 0 0 1 23 12A13 13 0 0 1 1 12Z"
                         "M16.2 12A4.2 4.2 0 1 0 7.8 12A4.2 4.2 0 1 0 16.2 12Z"
                         "M14.4 12A2.4 2.4 0 1 1 9.6 12A2.4 2.4 0 1 1 14.4 12Z"),
    # A stop square with rounded corners.
    "stop": ("0 0 24 24", "M7 4H17A3 3 0 0 1 20 7V17A3 3 0 0 1 17 20H7A3 3 0 0 1 4 17V7A3 3 0 0 1 7 4Z"),
    # OpenAI blossom: Wikimedia Commons, OpenAI_logo_2025_(symbol).svg.
    "codex": ('1.68 1.75 16.65 16.5', 'M11.248 18.25q-.825 0-1.568-.314a4.3 4.3 0 0 1-1.32-.874 4 4 0 0 1-1.304.214 4 4 0 0 1-2.046-.544 4.27 4.27 0 0 1-1.518-1.485 4 4 0 0 1-.56-2.095q0-.48.131-1.04A4.4 4.4 0 0 1 2.04 10.71a4.07 4.07 0 0 1 .017-3.4 4.2 4.2 0 0 1 1.056-1.418 3.8 3.8 0 0 1 1.6-.842 3.9 3.9 0 0 1 .76-1.683q.593-.759 1.451-1.188a4.04 4.04 0 0 1 1.832-.429q.825 0 1.567.313.742.314 1.32.875a4 4 0 0 1 1.304-.215q1.106 0 2.046.545a4.14 4.14 0 0 1 1.501 1.485q.578.941.578 2.095 0 .48-.132 1.04.66.61 1.023 1.419.363.792.363 1.666 0 .892-.38 1.717a4.3 4.3 0 0 1-1.072 1.435 3.8 3.8 0 0 1-1.584.825 3.8 3.8 0 0 1-.775 1.683 4.06 4.06 0 0 1-1.436 1.188 4.04 4.04 0 0 1-1.832.429m-4.076-2.062q.825 0 1.435-.347l3.103-1.782a.36.36 0 0 0 .164-.313v-1.42L7.881 14.62a.67.67 0 0 1-.726 0l-3.118-1.798a.5.5 0 0 1-.017.115v.198q0 .841.396 1.551.413.693 1.139 1.089a3.2 3.2 0 0 0 1.617.412m.165-2.69a.4.4 0 0 0 .181.05q.083 0 .165-.05l1.238-.71-3.977-2.31a.7.7 0 0 1-.363-.643v-3.58q-.825.362-1.32 1.122a2.9 2.9 0 0 0-.495 1.65q0 .809.413 1.55.412.743 1.072 1.123zm3.91 3.663q.875 0 1.585-.396a2.96 2.96 0 0 0 1.534-2.64v-3.564a.32.32 0 0 0-.165-.297l-1.254-.726v4.604a.7.7 0 0 1-.363.643l-3.119 1.799a3 3 0 0 0 1.783.577m.627-6.039V8.878L10.01 7.822 8.129 8.878v2.244l1.881 1.056zM7.057 5.859a.7.7 0 0 1 .363-.644l3.119-1.798a3 3 0 0 0-1.782-.578q-.874 0-1.584.396A2.96 2.96 0 0 0 6.05 4.324a3.07 3.07 0 0 0-.396 1.551v3.547q0 .199.165.314l1.237.726zm8.383 7.887q.825-.364 1.303-1.123.495-.758.495-1.65a3.15 3.15 0 0 0-.412-1.55q-.413-.743-1.073-1.123l-3.086-1.782q-.099-.065-.181-.049a.3.3 0 0 0-.165.05l-1.238.692 3.993 2.327a.6.6 0 0 1 .264.264.64.64 0 0 1 .1.363zm-3.317-8.382a.63.63 0 0 1 .726 0l3.135 1.831v-.297q0-.792-.396-1.501a2.86 2.86 0 0 0-1.105-1.155q-.71-.43-1.65-.43-.825 0-1.436.347L8.294 5.941a.36.36 0 0 0-.165.314v1.418z'),
    "claude": ("0 0 100 100", "m19.6 66.5 19.7-11 .3-1-.3-.5h-1l-3.3-.2-11.2-.3L14 53l-9.5-.5-2.4-.5L0 49l.2-1.5 2-1.3 2.9.2 6.3.5 9.5.6 6.9.4L38 49.1h1.6l.2-.7-.5-.4-.4-.4L29 41l-10.6-7-5.6-4.1-3-2-1.5-2-.6-4.2 2.7-3 3.7.3.9.2 3.7 2.9 8 6.1L37 36l1.5 1.2.6-.4.1-.3-.7-1.1L33 25l-6-10.4-2.7-4.3-.7-2.6c-.3-1-.4-2-.4-3l3.1-4.2L28 0l4.2.6L33.8 2l2.6 6 4.1 9.3L47 29.9l2 3.8 1 3.4.3 1h.7v-.5l.5-7.2 1-8.7 1-11.2.3-3.2 1.6-3.8 3-2L61 2.6l2 2.9-.3 1.8-1.1 7.7L59 27.1l-1.5 8.2h.9l1-1.1 4.1-5.4 6.9-8.6 3-3.5L77 13l2.3-1.8h4.3l3.1 4.7-1.4 4.9-4.4 5.6-3.7 4.7-5.3 7.1-3.2 5.7.3.4h.7l12-2.6 6.4-1.1 7.6-1.3 3.5 1.6.4 1.6-1.4 3.4-8.2 2-9.6 2-14.3 3.3-.2.1.2.3 6.4.6 2.8.2h6.8l12.6 1 3.3 2 1.9 2.7-.3 2-5.1 2.6-6.8-1.6-16-3.8-5.4-1.3h-.8v.4l4.6 4.5 8.3 7.5L89 80.1l.5 2.4-1.3 2-1.4-.2-9.2-7-3.6-3-8-6.8h-.5v.7l1.8 2.7 9.8 14.7.5 4.5-.7 1.4-2.6 1-2.7-.6-5.8-8-6-9-4.7-8.2-.5.4-2.9 30.2-1.3 1.5-3 1.2-2.5-2-1.4-3 1.4-6.2 1.6-8 1.3-6.4 1.2-7.9.7-2.6v-.2H49L43 72l-9 12.3-7.2 7.6-1.7.7-3-1.5.3-2.8L24 86l10-12.8 6-7.9 4-4.6-.1-.5h-.3L17.2 77.4l-4.7.6-2-2 .2-3 1-1 8-5.5Z"),
    "gemini": ("0 0 65 65", "M32.447 0c.68 0 1.273.465 1.439 1.125a38.904 38.904 0 001.999 5.905c2.152 5 5.105 9.376 8.854 13.125 3.751 3.75 8.126 6.703 13.125 8.855a38.98 38.98 0 005.906 1.999c.66.166 1.124.758 1.124 1.438 0 .68-.464 1.273-1.125 1.439a38.902 38.902 0 00-5.905 1.999c-5 2.152-9.375 5.105-13.125 8.854-3.749 3.751-6.702 8.126-8.854 13.125a38.973 38.973 0 00-2 5.906 1.485 1.485 0 01-1.438 1.124c-.68 0-1.272-.464-1.438-1.125a38.913 38.913 0 00-2-5.905c-2.151-5-5.103-9.375-8.854-13.125-3.75-3.749-8.125-6.702-13.125-8.854a38.973 38.973 0 00-5.905-2A1.485 1.485 0 010 32.448c0-.68.465-1.272 1.125-1.438a38.903 38.903 0 005.905-2c5-2.151 9.376-5.104 13.125-8.854 3.75-3.749 6.703-8.125 8.855-13.125a38.972 38.972 0 001.999-5.905A1.485 1.485 0 0132.447 0z"),
}



def _globe() -> str:
    """The `site` mark, drawn here and nobody's trademark: a globe of one circle, an upright ellipse and three
    horizontal lines, as fill path data in a 24-unit box (each ring an outer outline and an inner one wound the other
    way, so the nonzero fill leaves its inside open)."""
    def ring(rx_out: float, ry_out: float, rx_in: float, ry_in: float) -> str:
        def oval(rx: float, ry: float, sweep: int) -> str:
            return (f"M{12 - rx:g} 12a{rx:g} {ry:g} 0 1 {sweep} {2 * rx:g} 0"
                    f"a{rx:g} {ry:g} 0 1 {sweep} {-2 * rx:g} 0z")
        return oval(rx_out, ry_out, 1) + oval(rx_in, ry_in, 0)

    lines = ""
    for y in (6.5, 12.0, 17.5):
        half = math.sqrt(9.6 ** 2 - (y - 12) ** 2)
        lines += f"M{12 - half:.3f} {y - 0.9:g}H{12 + half:.3f}V{y + 0.9:g}H{12 - half:.3f}z"
    return ring(11, 11, 9.2, 9.2) + ring(4.6, 11, 2.8, 9.2) + lines


MARKS["site"] = ("0 0 24 24", _globe())
# The WordPress logo, the W in its ring: Wikimedia Commons, WordPress_blue_logo.svg, its five paths joined, each opening with an absolute M (and the third its first line written as a relative l) as it does on its own. The
# WordPress Foundation's trademark, drawn only into a launcher icon on the operator's own machine.
MARKS["wordpress"] = ("0 0 122.52 122.523", "M8.708 61.26c0 20.802 12.089 38.779 29.619 47.298l-25.069-68.686c-2.916 6.536-4.55 13.769-4.55 21.388z" "M96.74 58.608c0-6.495-2.333-10.993-4.334-14.494-2.664-4.329-5.161-7.995-5.161-12.324 0-4.831 3.664-9.328 8.825-9.328.233 0 .454.029.681.042-9.35-8.566-21.807-13.796-35.489-13.796-18.36 0-34.513 9.42-43.91 23.688 1.233.037 2.395.063 3.382.063 5.497 0 14.006-.667 14.006-.667 2.833-.167 3.167 3.994.337 4.329 0 0-2.847.335-6.015.501l19.138 56.925 11.501-34.493-8.188-22.434c-2.83-.166-5.511-.501-5.511-.501-2.832-.166-2.5-4.496.332-4.329 0 0 8.679.667 13.843.667 5.496 0 14.006-.667 14.006-.667 2.835-.167 3.168 3.994.337 4.329 0 0-2.853.335-6.015.501l18.992 56.494 5.242-17.517c2.272-7.269 4.001-12.49 4.001-16.989z" "M62.184 65.857l-15.768 45.819c4.708 1.384 9.687 2.141 14.846 2.141 6.12 0 11.989-1.058 17.452-2.979-.141-.225-.269-.464-.374-.724z" "M107.376 36.046c.226 1.674.354 3.471.354 5.404 0 5.333-.996 11.328-3.996 18.824l-16.053 46.413c15.624-9.111 26.133-26.038 26.133-45.426.001-9.137-2.333-17.729-6.438-25.215z" "M61.262 0c-33.779 0-61.262 27.481-61.262 61.26 0 33.783 27.483 61.263 61.262 61.263 33.778 0 61.265-27.48 61.265-61.263-.001-33.779-27.487-61.26-61.265-61.26zm0 119.715c-32.23 0-58.453-26.223-58.453-58.455 0-32.23 26.222-58.451 58.453-58.451 32.229 0 58.45 26.221 58.45 58.451 0 32.232-26.221 58.455-58.45 58.455z")
# An everyday link's mark: (the filled square's colour, the mark drawn in white). The agents keep their runner
# colours; WordPress is its own blue, and a site link the globe.
OVERLAYS = {**RUNNERS, "wordpress": ("#21759B", "wordpress"), "site": ("#2563EB", "site")}


# --- PNG ---------------------------------------------------------------------------------------------------------

def read_png(data: bytes) -> tuple[int, int, bytearray]:
    """(width, height, RGBA bytes) of an 8-bit PNG (greyscale, RGB, palette, with or without alpha)."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, palette, alpha_table = 8, bytearray(), None, None
    width = height = depth = colour = interlace = 0
    while pos < len(data):
        length, kind = struct.unpack_from(">I4s", data, pos)
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            alpha_table = body
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError("only 8-bit, non-interlaced PNG is read")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[colour]
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    rows, prev, pos = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = prev[i]
            corner = prev[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                line[i] = (line[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 255
        rows.append(line)
        prev = line
    out = bytearray(width * height * 4)
    for y, line in enumerate(rows):
        for x in range(width):
            o = (y * width + x) * 4
            if colour == 6:
                out[o:o + 4] = line[x * 4:x * 4 + 4]
            elif colour == 2:
                out[o:o + 4] = bytes(line[x * 3:x * 3 + 3]) + b"\xff"
            elif colour == 0:
                out[o:o + 4] = bytes((line[x],) * 3) + b"\xff"
            elif colour == 4:
                out[o:o + 4] = bytes((line[x * 2],) * 3) + bytes((line[x * 2 + 1],))
            else:
                if palette is None:
                    raise ValueError("a palette PNG without its palette")
                index = line[x]
                alpha = alpha_table[index] if alpha_table and index < len(alpha_table) else 255
                out[o:o + 4] = bytes(palette[index * 3:index * 3 + 3]) + bytes((alpha,))
    return width, height, out


def write_png(width: int, height: int, rgba: bytes | bytearray) -> bytes:
    raw = b"".join(b"\x00" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def resize(width: int, height: int, rgba: bytes | bytearray, size: int) -> bytearray:
    """Area-average `rgba` to `size` square, weighting colour by alpha so a transparent edge brings no dark fringe."""
    out = bytearray(size * size * 4)
    sx, sy = width / size, height / size
    for y in range(size):
        y0, y1 = y * sy, (y + 1) * sy
        for x in range(size):
            x0, x1 = x * sx, (x + 1) * sx
            r = g = b = a = weight = 0.0
            for yy in range(int(y0), min(height, math.ceil(y1))):
                wy = min(y1, yy + 1) - max(y0, yy)
                for xx in range(int(x0), min(width, math.ceil(x1))):
                    w = wy * (min(x1, xx + 1) - max(x0, xx))
                    o = (yy * width + xx) * 4
                    pa = rgba[o + 3] * w
                    r += rgba[o] * pa
                    g += rgba[o + 1] * pa
                    b += rgba[o + 2] * pa
                    a += pa
                    weight += w
            o = (y * size + x) * 4
            if a > 0:
                out[o:o + 4] = bytes((round(r / a), round(g / a), round(b / a), min(255, round(a / weight))))
    return out


def pack_ico(frames: list[tuple[int, bytes]]) -> bytes:
    """A Windows icon holding each (size, PNG) frame, smallest first."""
    frames = sorted(frames)
    head = struct.pack("<HHH", 0, 1, len(frames))
    offset = 6 + 16 * len(frames)
    entries, blobs = b"", b""
    for size, png in frames:
        side = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", side, side, 0, 0, 1, 32, len(png), offset)
        offset += len(png)
        blobs += png
    return head + entries + blobs


def read_ico(data: bytes) -> dict[int, tuple[int, int, bytearray]]:
    """The PNG frames of a Windows icon, by size."""
    count = struct.unpack_from("<HHH", data, 0)[2]
    frames = {}
    for i in range(count):
        w, _, _, _, _, _, length, offset = struct.unpack_from("<BBBBHHII", data, 6 + 16 * i)
        blob = data[offset:offset + length]
        if blob[:8] == b"\x89PNG\r\n\x1a\n":
            frames[w or 256] = read_png(blob)
    if not frames:
        raise ValueError("the icon holds no PNG frame")
    return frames


# --- shapes ------------------------------------------------------------------------------------------------------

def _tokens(path: str):
    for match in re.finditer(r"[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?", path):
        yield match.group(0)


def flatten_path(path: str, steps: int = 12) -> list[list[tuple[float, float]]]:
    """The subpaths of an SVG path as polygons: lines kept, curves and arcs cut into short segments."""
    tokens = list(_tokens(path))
    polygons, current = [], []
    x = y = sx = sy = 0.0
    last_control = None
    command = None
    i = 0

    def number():
        nonlocal i
        value = float(tokens[i])
        i += 1
        return value

    def flag():
        # Arc flags may be packed with what follows them ("001.999" is flags 0, 0 and the number 1.999).
        # An arc flag is one character, so the rest of a packed token is the next value.
        nonlocal i
        token = tokens[i]
        if len(token) > 1:
            tokens[i] = token[1:]
            return float(token[0])
        i += 1
        return float(token)

    while i < len(tokens):
        if re.fullmatch(r"[A-Za-z]", tokens[i]):
            command = tokens[i]
            i += 1
            if command in "Zz":
                if current:
                    polygons.append(current)
                current, x, y = [], sx, sy
                last_control = None
                continue
        if command is None:
            raise ValueError("a path must start with a command")
        rel = command.islower()
        kind = command.upper()
        if kind == "M":
            nx, ny = number(), number()
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            if current:
                polygons.append(current)
            current, sx, sy = [(x, y)], x, y
            command = "l" if rel else "L"
            last_control = None
        elif kind in "LHV":
            if kind == "L":
                nx, ny = number(), number()
                x, y = (x + nx, y + ny) if rel else (nx, ny)
            elif kind == "H":
                n = number()
                x = x + n if rel else n
            else:
                n = number()
                y = y + n if rel else n
            current.append((x, y))
            last_control = None
        elif kind in "CS":
            if kind == "C":
                c1 = (number(), number())
                c1 = (x + c1[0], y + c1[1]) if rel else c1
            else:
                c1 = (2 * x - last_control[0], 2 * y - last_control[1]) if last_control else (x, y)
            c2, end = (number(), number()), (number(), number())
            if rel:
                c2, end = (x + c2[0], y + c2[1]), (x + end[0], y + end[1])
            for step in range(1, steps + 1):
                t = step / steps
                u = 1 - t
                current.append((u ** 3 * x + 3 * u * u * t * c1[0] + 3 * u * t * t * c2[0] + t ** 3 * end[0],
                                u ** 3 * y + 3 * u * u * t * c1[1] + 3 * u * t * t * c2[1] + t ** 3 * end[1]))
            last_control, (x, y) = c2, end
        elif kind in "QT":
            if kind == "Q":
                c = (number(), number())
                c = (x + c[0], y + c[1]) if rel else c
            else:
                c = (2 * x - last_control[0], 2 * y - last_control[1]) if last_control else (x, y)
            end = (number(), number())
            end = (x + end[0], y + end[1]) if rel else end
            for step in range(1, steps + 1):
                t = step / steps
                u = 1 - t
                current.append((u * u * x + 2 * u * t * c[0] + t * t * end[0],
                                u * u * y + 2 * u * t * c[1] + t * t * end[1]))
            last_control, (x, y) = c, end
        elif kind == "A":
            rx, ry, rotation = abs(number()), abs(number()), number()
            large, sweep = flag(), flag()
            end = (number(), number())
            end = (x + end[0], y + end[1]) if rel else end
            current.extend(_arc(x, y, rx, ry, rotation, bool(large), bool(sweep), end[0], end[1], steps))
            (x, y), last_control = end, None
        else:
            raise ValueError(f"unsupported path command {command!r}")
    if current:
        polygons.append(current)
    return polygons


def _arc(x1, y1, rx, ry, rotation, large, sweep, x2, y2, steps):
    """An SVG elliptical arc as points (the endpoint-to-centre conversion of the SVG specification, F.6.5)."""
    if rx == 0 or ry == 0 or (x1, y1) == (x2, y2):
        return [(x2, y2)]
    phi = math.radians(rotation)
    cos, sin = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    xp, yp = cos * dx + sin * dy, -sin * dx + cos * dy
    scale = xp * xp / (rx * rx) + yp * yp / (ry * ry)
    if scale > 1:
        rx, ry = rx * math.sqrt(scale), ry * math.sqrt(scale)
    num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp
    den = rx * rx * yp * yp + ry * ry * xp * xp
    coef = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cxp, cyp = coef * rx * yp / ry, -coef * ry * xp / rx
    cx = cos * cxp - sin * cyp + (x1 + x2) / 2
    cy = sin * cxp + cos * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a
    start = angle(1, 0, (xp - cxp) / rx, (yp - cyp) / ry)
    delta = angle((xp - cxp) / rx, (yp - cyp) / ry, (-xp - cxp) / rx, (-yp - cyp) / ry)
    if not sweep and delta > 0:
        delta -= 2 * math.pi
    elif sweep and delta < 0:
        delta += 2 * math.pi
    points = []
    for step in range(1, steps + 1):
        t = start + delta * step / steps
        ex, ey = rx * math.cos(t), ry * math.sin(t)
        points.append((cos * ex - sin * ey + cx, sin * ex + cos * ey + cy))
    return points


def coverage(polygons, size: int, scale: float, dx: float, dy: float, samples: int = OVERSAMPLE) -> list[float]:
    """Nonzero-fill coverage (0 to 1) of `polygons`, scaled and moved, over a `size`-square grid."""
    edges = []
    for poly in polygons:
        pts = [(px * scale + dx, py * scale + dy) for px, py in poly]
        for (ax, ay), (bx, by) in zip(pts, pts[1:] + pts[:1]):
            if ay != by:
                edges.append((ax, ay, bx, by, 1 if by > ay else -1))
    cover = [0.0] * (size * size)
    sub = 1.0 / samples
    for py in range(size):
        for sy_ in range(samples):
            yy = py + (sy_ + 0.5) * sub
            crossings = []
            for ax, ay, bx, by, wind in edges:
                if (ay <= yy < by) or (by <= yy < ay):
                    crossings.append((ax + (yy - ay) * (bx - ax) / (by - ay), wind))
            crossings.sort()
            total = 0
            for (xa, wa), (xb, _) in zip(crossings, crossings[1:]):
                total += wa
                if total == 0:
                    continue
                for px in range(max(0, int(xa)), min(size, math.ceil(xb))):
                    for sx_ in range(samples):
                        xx = px + (sx_ + 0.5) * sub
                        if xa <= xx < xb:
                            cover[py * size + px] += 1.0 / (samples * samples)
    return cover


def circle(cx: float, cy: float, r: float, points: int = 96) -> list[tuple[float, float]]:
    return [(cx + r * math.cos(2 * math.pi * k / points), cy + r * math.sin(2 * math.pi * k / points))
            for k in range(points)]


def blend(rgba: bytearray, size: int, cover: list[float], colour: tuple[int, int, int]) -> None:
    for i, c in enumerate(cover):
        if c <= 0:
            continue
        c = min(1.0, c)
        o = i * 4
        a0 = rgba[o + 3] / 255
        a = c + a0 * (1 - c)
        for k in range(3):
            rgba[o + k] = round((colour[k] * c + rgba[o + k] * a0 * (1 - c)) / a) if a else 0
        rgba[o + 3] = round(a * 255)


def hex_colour(value: str) -> tuple[int, int, int]:
    if not re.fullmatch(r"#[0-9A-Fa-f]{6}", value):
        raise ValueError(f"{value!r} is not a #RRGGBB colour")
    return int(value[1:3], 16), int(value[3:5], 16), int(value[5:7], 16)


def badge_frame(size: int, rgba: bytearray, fill: str, mark: str | None) -> bytearray:
    """`rgba` (a `size`-square frame) with the runner's badge in the bottom right: Office's geometry, a white ring
    around the runner's colour, and the mark in white from MARK_FROM pixels up."""
    out = bytearray(rgba)
    radius = size * 0.24
    centre = size - radius - max(0.25, size * 0.01)
    ring = max(1.0, size * 0.025)
    blend(out, size, coverage([circle(centre, centre, radius)], size, 1, 0, 0), (255, 255, 255))
    blend(out, size, coverage([circle(centre, centre, radius - ring)], size, 1, 0, 0), hex_colour(fill))
    if mark and size >= MARK_FROM:
        box, path = MARKS[mark]
        bx, by, bw, bh = (float(v) for v in box.split())
        side = radius * 1.15
        scale = side / max(bw, bh)
        polys = flatten_path(path)
        left, top = centre - (bx + bw / 2) * scale, centre - (by + bh / 2) * scale
        blend(out, size, coverage(polys, size, scale, left, top), (255, 255, 255))
    return out


def base_frames(source: Path) -> dict[int, tuple[int, int, bytearray]]:
    data = source.read_bytes()
    if data[:4] == b"\x00\x00\x01\x00":
        return read_ico(data)
    return {0: read_png(data)}


def frame_at(frames: dict, size: int) -> bytearray:
    """The base frame of exactly `size` pixels, or the smallest larger one area-averaged down."""
    if size in frames and frames[size][0] == size:
        return bytearray(frames[size][2])
    larger = sorted(k for k in frames if (frames[k][0] >= size)) or sorted(frames, key=lambda k: -frames[k][0])
    width, height, rgba = frames[larger[0]]
    return resize(width, height, rgba, size)


def rounded_square(x: float, y: float, side: float, radius: float, steps: int = 16) -> list[tuple[float, float]]:
    """A square from (x, y), `side` wide, its corners rounded with `radius`, as one clockwise polygon."""
    points = []
    for cx, cy, start in ((x + side - radius, y + radius, -90), (x + side - radius, y + side - radius, 0),
                          (x + radius, y + side - radius, 90), (x + radius, y + radius, 180)):
        for k in range(steps + 1):
            a = math.radians(start + 90 * k / steps)
            points.append((cx + radius * math.cos(a), cy + radius * math.sin(a)))
    return points


def overlay_frame(size: int, rgba: bytearray, mark: str) -> bytearray:
    """`rgba` (a `size`-square frame) with an everyday link's mark in the bottom right: a rounded square, where a
    campaign link's badge is a circle. The geometry is the first project's, drawn at 512 pixels and scaled: a white
    outline square at 222 (286 wide, corners of 70), the mark's colour inside it at 236 (272 wide, corners of 60),
    and from MARK_FROM pixels up the mark in white, centred at 372 in a box 186 wide."""
    out = bytearray(rgba)
    fill, drawn = OVERLAYS[mark]
    k = size / 512
    blend(out, size, coverage([rounded_square(222 * k, 222 * k, 286 * k, 70 * k)], size, 1, 0, 0), (255, 255, 255))
    blend(out, size, coverage([rounded_square(236 * k, 236 * k, 272 * k, 60 * k)], size, 1, 0, 0), hex_colour(fill))
    if size >= MARK_FROM:
        box, path = MARKS[drawn]
        bx, by, bw, bh = (float(v) for v in box.split())
        scale = 186 * k / max(bw, bh)
        left, top = 372 * k - (bx + bw / 2) * scale, 372 * k - (by + bh / 2) * scale
        blend(out, size, coverage(flatten_path(path), size, scale, left, top), (255, 255, 255))
    return out


def write_atomic(target: Path, data: bytes) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(target.name + f".{os.getpid()}.tmp")
    try:
        temporary.write_bytes(data)
        os.replace(temporary, target)
    except BaseException:
        temporary.unlink(missing_ok=True)   # a failed write leaves the old icon and no half-written file
        raise


def badge(source: Path, runner: str, target: Path) -> None:
    fill, mark = BADGES[runner]
    frames = base_frames(source)
    out = [(size, write_png(size, size, badge_frame(size, frame_at(frames, size), fill, mark))) for size in SIZES]
    write_atomic(target, pack_ico(out))


def overlay(source: Path, mark: str, target: Path) -> None:
    frames = base_frames(source)
    out = [(size, write_png(size, size, overlay_frame(size, frame_at(frames, size), mark))) for size in SIZES]
    write_atomic(target, pack_ico(out))


def build_frames(large: Path, small: Path, target: Path) -> None:
    big, little = read_png(large.read_bytes()), read_png(small.read_bytes())
    out = []
    for size in SIZES:
        width, height, rgba = little if size <= SMALL_UP_TO else big
        out.append((size, write_png(size, size, resize(width, height, rgba, size))))
    write_atomic(target, pack_ico(out))


def main(argv: list[str]) -> int:
    try:
        if len(argv) == 4 and argv[0] == "badge" and argv[2] in BADGES:
            badge(Path(argv[1]), argv[2], Path(argv[3]))
            return 0
        if len(argv) == 4 and argv[0] == "overlay" and argv[2] in OVERLAYS and argv[3].lower().endswith(".ico"):
            overlay(Path(argv[1]), argv[2], Path(argv[3]))
            return 0
        if len(argv) == 4 and argv[0] == "frames":
            build_frames(Path(argv[1]), Path(argv[2]), Path(argv[3]))
            return 0
    except (OSError, ValueError, KeyError) as exc:
        print(f"campaign-icon: {exc}", file=sys.stderr)
        return 1
    print(USAGE, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
