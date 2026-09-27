#!/usr/bin/env python3
"""Generate the default "impossible silk" wallpapers (dark and light).

Original artwork, GPL-3.0-only. Translucent silk ribbons sweep up from the
lower left, twisting as they go: each ribbon's signed width passes through
zero, so its edges cross like real silk and its reverse face falls into
shadow. The back ribbons are softly out of focus, the front ones carry a lit
edge, and a glow sits behind the sweep. Every shape is computed here, so the
art is reproducible and reviewable as code:

    python3 scripts/site/gen_wallpapers.py            # write SVG sources + render JPEGs
    python3 scripts/site/gen_wallpapers.py --svg-only # sources only (no rsvg-convert needed)

Outputs: resources/backgrounds/src/silk-{dark,light}.svg,
resources/backgrounds/silk-{dark,light}.jpg (2560x1440) and
resources/backgrounds/background.jpg (the dark silk, which the OS ships).
Needs rsvg-convert (librsvg2-bin) and Pillow to render.
"""
from __future__ import annotations

import argparse
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "resources" / "backgrounds" / "src"
OUT = REPO / "resources" / "backgrounds"
W, H = 3840, 2160                 # source canvas (4K); rendered at 2560x1440
N = 240                           # samples per ribbon: enough that no segment shows

# y at the left, y at the right, wave amplitude, waves, wave phase, width,
# twists, twist phase, blur (0 = in focus), opacity. Back to front.
RIBBONS = [
    (2150, 700, 170, 0.8, 0.05, 620, 0.9, 0.10, 70, 0.60),
    (2000, 900, 210, 0.7, 0.30, 520, 1.1, 0.35, 30, 0.70),
    (1900, 820, 150, 0.9, 0.55, 460, 1.4, 0.05, 0, 0.88),
    (1750, 1000, 190, 0.6, 0.80, 380, 1.2, 0.60, 0, 0.82),
    (2050, 1150, 120, 1.0, 0.15, 280, 1.7, 0.25, 0, 0.72),
]

THEMES = {
    "dark": {
        "field": [("0", "#10307f"), ("0.45", "#071752"), ("1", "#020515")],
        "glow": ("#2f6ff0", 0.42),
        "vignette": 0.50,
        "reverse": ("#020a3a", 0.42),
        "edge": 0.85,
        "ribbons": [
            [(0, "#1b2fb0", 0), (0.35, "#2e55e6", 0.85), (0.7, "#6a46f2", 0.9), (1, "#2a2fb8", 0)],
            [(0, "#0f5ad8", 0), (0.4, "#1f7cf2", 0.9), (0.72, "#3fb6ff", 0.92), (1, "#0f5ad8", 0)],
            [(0, "#2a64f0", 0), (0.3, "#2f86f5", 0.92), (0.62, "#9fe3ff", 0.97), (0.88, "#4a7cff", 0.85), (1, "#2a64f0", 0)],
            [(0, "#3a3fe0", 0), (0.45, "#4f6af7", 0.9), (0.78, "#a6d2ff", 0.92), (1, "#3a3fe0", 0)],
            [(0, "#1470e8", 0), (0.55, "#66d6ff", 0.92), (1, "#1470e8", 0)],
        ],
    },
    "light": {
        "field": [("0", "#f7faff"), ("0.5", "#e2ecf9"), ("1", "#c6d7ef")],
        "glow": ("#8ab8ff", 0.40),
        "vignette": 0.10,
        "reverse": ("#0a2a8a", 0.16),
        "edge": 0.95,
        "ribbons": [
            [(0, "#5a7cf0", 0), (0.35, "#6e8ff5", 0.55), (0.7, "#9b86f7", 0.6), (1, "#5a7cf0", 0)],
            [(0, "#3f8cf5", 0), (0.4, "#4f9cf7", 0.6), (0.72, "#7ccaff", 0.65), (1, "#3f8cf5", 0)],
            [(0, "#3f7cf5", 0), (0.3, "#4a96f7", 0.72), (0.62, "#b4ebff", 0.85), (0.88, "#6a93ff", 0.7), (1, "#3f7cf5", 0)],
            [(0, "#5a62ee", 0), (0.45, "#6f86f8", 0.7), (0.78, "#bcdcff", 0.8), (1, "#5a62ee", 0)],
            [(0, "#3a8cf0", 0), (0.55, "#8adfff", 0.8), (1, "#3a8cf0", 0)],
        ],
    },
}


def ribbon_faces(y0, y1, amp, waves, phase, thick, twists, tphase):
    """The faces of one twisted ribbon: (front?, [(x, centre y, signed width)]).
    Where the signed width crosses zero a face ends and the next begins, at the
    exact crossing point, so the two edges meet cleanly."""
    pts = []
    for i in range(N + 1):
        t = i / N
        x = -400 + t * (W + 800)
        c = y0 + (y1 - y0) * t + amp * math.sin(2 * math.pi * (t * waves + phase))
        env = 0.30 + 0.70 * math.sin(math.pi * t) ** 0.6
        pts.append((x, c, thick * env * math.cos(2 * math.pi * (t * twists + tphase))))
    faces, cur, front = [], [], None
    for k, (x, c, w) in enumerate(pts):
        side = w >= 0
        if front is None:
            front = side
        if side != front and cur:
            px, pc, pw = pts[k - 1]
            f = pw / (pw - w)
            cross = (px + (x - px) * f, pc + (c - pc) * f, 0.0)
            faces.append((front, cur + [cross]))
            cur, front = [cross], side
        cur.append((x, c, w))
    faces.append((front, cur))
    return faces


def face_paths(face):
    top = [(x, c - w / 2) for x, c, w in face]
    bottom = [(x, c + w / 2) for x, c, w in face][::-1]
    fill = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in top + bottom) + "Z"
    edge = "M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in top)
    return fill, edge


def gradient(gid, stops):
    s = "".join(f'<stop offset="{o}" stop-color="{c}" stop-opacity="{a}"/>' for o, c, a in stops)
    return f'<linearGradient id="{gid}" gradientUnits="userSpaceOnUse" x1="0" y1="0" x2="{W}" y2="0">{s}</linearGradient>'


def svg(theme_name: str) -> str:
    t = THEMES[theme_name]
    field = "".join(f'<stop offset="{o}" stop-color="{c}"/>' for o, c in t["field"])
    grads = [gradient(f"silk-r{i}", s) for i, s in enumerate(t["ribbons"])]
    grads.append(gradient("silk-edge", [(0, "#ffffff", 0), (0.55, "#eaf8ff", t["edge"]), (1, "#ffffff", 0)]))
    rev_col, rev_op = t["reverse"]
    glow_col, glow_op = t["glow"]
    body = []
    for i, (y0, y1, amp, wv, ph, th, tw, tph, blur, op) in enumerate(RIBBONS):
        layers = []
        for front, face in ribbon_faces(y0, y1, amp, wv, ph, th, tw, tph):
            fill, edge = face_paths(face)
            layers.append(f'<path d="{fill}" fill="url(#silk-r{i})"/>')
            if not front and not blur:                      # the reverse face, in shadow (in focus only:
                                                            # shading a blurred ribbon reads as a smudge)
                layers.append(f'<path d="{fill}" fill="{rev_col}" fill-opacity="{rev_op}"/>')
            elif not blur:                                  # in-focus front face: a lit edge
                layers.append(f'<path d="{edge}" fill="none" stroke="url(#silk-edge)" stroke-width="2.4"/>')
        focus = f' filter="url(#silk-blur{blur})"' if blur else ""
        body.append(f'  <g opacity="{op}"{focus}>' + "".join(layers) + "</g>")
    # Blur regions in canvas units, larger than the canvas: a region that clips
    # a blurred ribbon shows as a hard vertical band where it ends.
    blurs = "".join(f'<filter id="silk-blur{b}" filterUnits="userSpaceOnUse" x="-800" y="-800" '
                    f'width="{W + 1600}" height="{H + 1600}"><feGaussianBlur stdDeviation="{b}"/></filter>'
                    for b in sorted({r[8] for r in RIBBONS if r[8]}))
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}">
  <!-- Impossible OS default wallpaper, {theme_name}: "impossible silk". Original artwork, GPL-3.0-only.
       Generated by scripts/site/gen_wallpapers.py; edit the generator, not this file. -->
  <defs>
    <radialGradient id="silk-field" cx="0.6" cy="0.52" r="0.85">{field}</radialGradient>
    <radialGradient id="silk-vignette" cx="0.5" cy="0.5" r="0.75"><stop offset="0.55" stop-color="#000" stop-opacity="0"/><stop offset="1" stop-color="#000" stop-opacity="{t["vignette"]}"/></radialGradient>
    {"".join(grads)}
    {blurs}
    <filter id="silk-glow" filterUnits="userSpaceOnUse" x="-800" y="-800" width="{W + 1600}" height="{H + 1600}"><feGaussianBlur stdDeviation="220"/></filter>
  </defs>
  <rect width="{W}" height="{H}" fill="url(#silk-field)"/>
  <ellipse cx="2350" cy="1150" rx="1300" ry="650" fill="{glow_col}" opacity="{glow_op}" filter="url(#silk-glow)"/>
{chr(10).join(body)}
  <rect width="{W}" height="{H}" fill="url(#silk-vignette)"/>
</svg>
'''


def render(svg_path: Path, jpg_path: Path, width: int = 2560, height: int = 1440) -> None:
    import numpy as np
    from PIL import Image
    with tempfile.TemporaryDirectory() as tmp:
        png = Path(tmp) / "w.png"
        subprocess.run(["rsvg-convert", "-w", str(width), "-h", str(height), str(svg_path), "-o", str(png)],
                       check=True)
        px = np.asarray(Image.open(png).convert("RGB"), dtype=np.float32)
    # DITHER: smooth dark gradients band into visible stripes at 8 bits per
    # channel. Fixed-seed noise of +/-1.5 levels breaks the bands up and keeps
    # the output byte-for-byte reproducible.
    noise = np.random.default_rng(8).uniform(-1.5, 1.5, size=px.shape[:2])[..., None]
    out = np.clip(np.rint(px + noise), 0, 255).astype(np.uint8)
    # BASELINE, not progressive: the kernel decodes background.jpg with stb_image,
    # and a progressive decode buffers coefficients for the whole image (tens of
    # MB of kernel heap at 2560x1440). 4:4:4 chroma keeps the gradients clean.
    Image.fromarray(out, "RGB").save(jpg_path, "JPEG", quality=94, optimize=True, progressive=False, subsampling=0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--svg-only", action="store_true")
    args = ap.parse_args()
    if not args.svg_only and not shutil.which("rsvg-convert"):
        print("gen_wallpapers: rsvg-convert not found (apt install librsvg2-bin)", file=sys.stderr)
        return 1
    for name in THEMES:
        path = SRC / f"silk-{name}.svg"
        path.write_text(svg(name), encoding="utf-8")
        if not args.svg_only:
            render(path, OUT / f"silk-{name}.jpg")
    if not args.svg_only:
        shutil.copyfile(OUT / "silk-dark.jpg", OUT / "background.jpg")
    print("gen_wallpapers: wrote silk-dark and silk-light" + ("" if args.svg_only else " (+ JPEGs, background.jpg)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
