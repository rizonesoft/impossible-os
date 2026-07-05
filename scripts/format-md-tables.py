#!/usr/bin/env python3
"""Align markdown pipe-table columns for readability in the raw source.

GitHub/most renderers ignore source whitespace in tables entirely -- this is
a SOURCE-readability tool only (skimming todo/*.md tables in a terminal or
editor), not a rendering fix. Two things it deliberately does NOT do:

- Touch PROSE tables (any cell wider than --max-cell, default 80 chars).
  Reference/History-log tables carry multi-sentence Summary cells; forcing
  column alignment on those would bloat the file for zero rendering benefit
  and turn every future single-cell edit into a whole-table reformat diff.
  Short, genuinely tabular tables (Implementation Order, OS Comparison,
  reference tables) are the actual target.
- Reflow or wrap cell CONTENT. Only inter-cell padding changes.

Usage:
  format-md-tables.py [--check] [--max-cell N] PATH [PATH ...]
    PATH may be a file or a directory (recursed for *.md).
    --check: report files that would change, write nothing, exit 1 if any.
    default: rewrite files in place, exit 0. Idempotent -- a second run over
    already-aligned output changes nothing.

Stdlib only.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

_ROW_RE = re.compile(r"^\s*\|.*\|\s*$")
_SEP_CELL_RE = re.compile(r"^\s*:?-+:?\s*$")


def _split_row(line: str) -> list[str]:
    """Split a pipe-table row into cell strings (stripped, no outer pipes)."""
    s = line.strip()
    s = s[1:] if s.startswith("|") else s
    s = s[:-1] if s.endswith("|") else s
    # cells may contain escaped pipes "\|"; split on unescaped pipes only.
    cells: list[str] = []
    buf = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s) and s[i + 1] == "|":
            buf.append("|")
            i += 2
            continue
        if s[i] == "|":
            cells.append("".join(buf))
            buf = []
        else:
            buf.append(s[i])
        i += 1
    cells.append("".join(buf))
    return [c.strip() for c in cells]


def _alignment(sep_cell: str) -> str:
    """left/right/center/default -- 'left' (explicit ':---') and 'default'
    (bare '---') render identically but are kept distinct so a table with no
    alignment markers at all does not gain them as a side effect of aligning.
    """
    left = sep_cell.startswith(":")
    right = sep_cell.endswith(":")
    if left and right:
        return "center"
    if right:
        return "right"
    if left:
        return "left"
    return "default"


def _find_tables(lines: list[str]) -> list[tuple[int, int]]:
    """Return (start, end_exclusive) line-index ranges of table blocks."""
    tables = []
    i = 0
    n = len(lines)
    while i < n - 1:
        if _ROW_RE.match(lines[i]):
            sep_cells = _split_row(lines[i + 1]) if _ROW_RE.match(lines[i + 1]) else None
            if sep_cells and all(_SEP_CELL_RE.match(c) for c in sep_cells) and sep_cells:
                header_cells = _split_row(lines[i])
                if len(header_cells) == len(sep_cells):
                    j = i + 2
                    while j < n and _ROW_RE.match(lines[j]):
                        j += 1
                    tables.append((i, j))
                    i = j
                    continue
        i += 1
    return tables


def _render_table(lines: list[str], start: int, end: int, max_cell: int) -> list[str] | None:
    rows = [_split_row(l) for l in lines[start:end]]
    ncols = len(rows[0])
    if any(len(r) != ncols for r in rows):
        return None  # ragged cell count (e.g. an escaped-pipe edge case) -- leave untouched
    if any(len(cell) > max_cell for row in rows for cell in row):
        return None  # prose table -- do not force-align
    aligns = [_alignment(c) for c in rows[1]]
    widths = [
        max(3, max(len(rows[r][c]) for r in range(len(rows)) if r != 1))
        for c in range(ncols)
    ]

    def pad(cell: str, width: int, align: str) -> str:
        gap = width - len(cell)
        if gap <= 0:
            return cell
        if align == "right":
            return " " * gap + cell
        if align == "center":
            left = gap // 2
            return " " * left + cell + " " * (gap - left)
        return cell + " " * gap

    out = []
    for r, row in enumerate(rows):
        if r == 1:
            cells = []
            for c in range(ncols):
                w = widths[c]
                a = aligns[c]
                if a == "center":
                    cell = ":" + "-" * (w - 2) + ":"
                elif a == "right":
                    cell = "-" * (w - 1) + ":"
                elif a == "left":
                    cell = ":" + "-" * (w - 1)
                else:
                    cell = "-" * w
                cells.append(cell)
            out.append("| " + " | ".join(cells) + " |")
        else:
            out.append("| " + " | ".join(
                pad(row[c], widths[c], aligns[c]) for c in range(ncols)
            ) + " |")
    return out


def process(path: Path, max_cell: int, check: bool) -> bool:
    """Return True if the file has (or would have) changes."""
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")
    trailing_newline = text.endswith("\n")
    changed = False
    for start, end in reversed(_find_tables(lines)):
        rendered = _render_table(lines, start, end, max_cell)
        if rendered is None:
            continue
        if rendered != lines[start:end]:
            changed = True
            if not check:
                lines[start:end] = rendered
    if changed and not check:
        new_text = "\n".join(lines)
        if trailing_newline and not new_text.endswith("\n"):
            new_text += "\n"
        path.write_text(new_text, encoding="utf-8")
    return changed


def _iter_md(paths: list[str]) -> list[Path]:
    out = []
    for p in paths:
        pp = Path(p)
        if pp.is_dir():
            out.extend(sorted(pp.rglob("*.md")))
        elif pp.suffix == ".md":
            out.append(pp)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--max-cell", type=int, default=80)
    args = ap.parse_args()

    any_changed = False
    for f in _iter_md(args.paths):
        if process(f, args.max_cell, args.check):
            any_changed = True
            verb = "would align" if args.check else "aligned"
            print(f"{verb}: {f}")

    if args.check:
        return 1 if any_changed else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
