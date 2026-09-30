#!/usr/bin/env python3
"""Generate include/desktop/theme_tokens.h from docs/design/tokens.json.

The design tokens are canonical; the C header is a projection of them and is
never hand-edited. ``--check`` regenerates in memory and fails if the committed
header differs (wired into scripts/site/build.py --check, which lint Check 30
runs on every commit).

Naming: THEME_<THEME>_<TOKEN> for colours (0xAARRGGBB), THEME_MAT_<THEME>_<SURFACE>_<FIELD>
for acrylic/mica parameters, THEME_SIZE_*, THEME_RADIUS_*, THEME_SPACE_*,
THEME_TYPE_<STYLE>_{SIZE,LINE,WEIGHT}, THEME_ELEV_<NAME>_{Y,BLUR,ALPHA},
THEME_MOTION_* (ms).
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOKENS = ROOT / "docs" / "design" / "tokens.json"
HEADER = ROOT / "include" / "desktop" / "theme_tokens.h"


def argb(value: str) -> str:
    v = value.lstrip("#").upper()
    if len(v) == 6:
        v = "FF" + v
    if len(v) != 8 or any(c not in "0123456789ABCDEF" for c in v):
        raise ValueError(f"bad colour token: {value}")
    return f"0x{v}u"


def ident(*parts: str) -> str:
    return "_".join(p.upper() for p in parts)


def generate() -> str:
    t = json.loads(TOKENS.read_text(encoding="utf-8"))
    out: list[str] = [
        "/* GENERATED from docs/design/tokens.json by scripts/site/gen_theme_header.py.",
        " * Do not edit: change the tokens, then run",
        " *   python3 scripts/site/gen_theme_header.py",
        " * scripts/site/build.py --check (lint Check 30) fails if this file drifts.",
        " * Colours are 0xAARRGGBB; sizes are pixels at 100% scale. */",
        "#ifndef DESKTOP_THEME_TOKENS_H",
        "#define DESKTOP_THEME_TOKENS_H",
        "",
    ]
    themes = t["color"]["themes"]
    for name in sorted(themes):
        out.append(f"/* ---- colour: {name} ---- */")
        for key in sorted(themes[name]):
            out.append(f"#define {ident('THEME', name, key):<44} {argb(themes[name][key])}")
        out.append("")
    for theme in sorted(k for k in t["material"] if not k.startswith("_")):
        out.append(f"/* ---- material: {theme} ---- */")
        for surface in sorted(t["material"][theme]):
            for field, val in sorted(t["material"][theme][surface].items()):
                v = argb(val) if isinstance(val, str) else f"{int(val)}"
                out.append(f"#define {ident('THEME_MAT', theme, surface, field):<44} {v}")
        out.append("")
    for group, prefix in (("size", "THEME_SIZE"), ("radius", "THEME_RADIUS"), ("spacing", "THEME_SPACE")):
        out.append(f"/* ---- {group} ---- */")
        for key, val in sorted(t[group].items()):
            if key.startswith("_"):
                continue
            out.append(f"#define {ident(prefix, key):<44} {int(val)}")
        out.append("")
    out.append("/* ---- type ramp ---- */")
    for style, spec in sorted(t["type"]["ramp"].items()):
        for field in ("size", "line", "weight"):
            out.append(f"#define {ident('THEME_TYPE', style, field):<44} {int(spec[field])}")
    out.append("")
    out.append("/* ---- elevation: [offset_y, blur, alpha] for gfx_drop_shadow ---- */")
    for name, (y, blur, alpha) in sorted((k, v) for k, v in t["elevation"].items() if not k.startswith("_")):
        out.append(f"#define {ident('THEME_ELEV', name, 'y'):<44} {int(y)}")
        out.append(f"#define {ident('THEME_ELEV', name, 'blur'):<44} {int(blur)}")
        out.append(f"#define {ident('THEME_ELEV', name, 'alpha'):<44} {int(alpha)}")
    out.append("")
    out.append("/* ---- motion (ms) ---- */")
    for key, val in sorted(t["motion"].items()):
        if key.startswith("_") or isinstance(val, list):
            continue
        out.append(f"#define {ident('THEME_MOTION', key, 'ms'):<44} {int(val)}")
    out.append("")
    out.append("#endif /* DESKTOP_THEME_TOKENS_H */")
    return "\n".join(out) + "\n"


def check() -> list[str]:
    want = generate()
    have = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""   # fail-direction: a missing header differs and is reported
    if want != have:
        return [f"{HEADER.relative_to(ROOT)} is out of date with docs/design/tokens.json "
                f"(run: python3 scripts/site/gen_theme_header.py)"]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if args.check:
        errs = check()
        for e in errs:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1 if errs else 0
    HEADER.write_text(generate(), encoding="utf-8")
    print(f"wrote {HEADER.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
