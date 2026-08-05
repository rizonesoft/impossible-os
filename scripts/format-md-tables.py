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
import unicodedata
import sys
from pathlib import Path

_ROW_RE = re.compile(r"^\s*\|.*\|\s*$")
_SEP_CELL_RE = re.compile(r"^\s*:?-+:?\s*$")


def _dwidth(text: str) -> int:
    """DISPLAY width, not len(). A wide character occupies two terminal columns
    while len() reports one, so padding computed from len() renders one column
    too wide per wide char -- visible as `| star   |` where `| star  |` was
    meant. Reported by the operator 2026-08-05, immediately after the cap-40
    sweep re-padded 194 files with this bug in it.

    East Asian Width W/F are the two-column classes. AMBIGUOUS ('A') is
    deliberately treated as ONE: it covers section-sign and arrow glyphs this
    repo uses constantly, and they render single-width in the terminals and
    editors this corpus is read in. Combining marks (Mn/Me) add no width.
    """
    w = 0
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        # VARIATION SELECTOR-16 (U+FE0F) requests EMOJI presentation, which
        # renders double-width even when the base character is East-Asian
        # Narrow. Missing this scored the warning sign as 1 while it displays
        # as 2, so every row carrying it was padded one column too wide -- the
        # operator spotted it in an OS Comparison table where the warning rows
        # sat a column proud of the check-mark rows. U+FE0E is the opposite
        # request (text presentation) and stays narrow.
        if nxt == "\ufe0f":
            w += 2
            i += 2
            continue
        if nxt == "\ufe0e":
            w += 1
            i += 2
            continue
        i += 1
        if unicodedata.combining(ch) or unicodedata.category(ch) in ("Mn", "Me"):
            continue
        w += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return w


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


def _reescape(cell: str) -> str:
    """Re-escape pipes that `_split_row` decoded.

    `_split_row` turns a literal `\\|` inside a cell into a bare `|` so the cell
    text is what the reader sees. Rendering joined cells with a bare `|` and
    never put the escape back -- so a table containing `safeboot=minimal\\|network`
    was rewritten with that pipe as a COLUMN SEPARATOR, silently corrupting the
    row into one cell too many and making the whole table ragged. The formatter
    then refused to touch it ever again, which is how it hid.

    Found 2026-08-05 while repairing exactly such a row: the escape was applied,
    the formatter ran, and the escape was gone.
    """
    return cell.replace("|", "\\|")


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


def _render_table(lines: list[str], start: int, end: int, max_cell: int,
                  max_pad: int = 0) -> list[str] | None:
    rows = [_split_row(l) for l in lines[start:end]]
    ncols = len(rows[0])
    if any(len(r) != ncols for r in rows):
        return None  # ragged cell count (e.g. an escaped-pipe edge case) -- leave untouched
    if any(_dwidth(cell) > max_cell for row in rows for cell in row):
        return None  # prose table -- do not force-align
    aligns = [_alignment(c) for c in rows[1]]
    # PER-COLUMN CAP. Padding every cell to the column's WIDEST makes one long
    # entry inflate every other row in that column, and the cost is superlinear
    # in the outlier rather than in the average. Measured 2026-08-05 across the
    # 5,606 aligned rows in todo/: median 134 chars, 2,057 over 160 -- about a
    # third wrapping at any common terminal width, trading one raw-source
    # irritation for another.
    #
    # 40 chosen from the corpus, not intuition (operator decision, same day).
    # Simulated against the real tables -- median / rows>160 / cells left
    # unpadded: no cap 133/2134/2.0%, cap 50 126/1849/10.5%, CAP 40
    # 122/1635/17.1%, cap 30 117/1263/25.3%. Cap 30 reaches a nicer median but
    # leaves a QUARTER of cells ragged, undoing much of what the alignment was
    # introduced to buy.
    #
    # A cell WIDER than the cap is left unpadded rather than truncated -- content
    # is never touched, only inter-cell padding. And note the ceiling on what any
    # padding policy can achieve: the median NATURAL row is already 91 chars, so
    # ~49% of over-160 rows are wide because their CONTENT is wide.
    # NO CAP BY DEFAULT (max_pad=0). A per-column cap was applied on 2026-08-05
    # and REVERTED the same day on rendered evidence: a cell wider than the cap
    # is left UNPADDED, so its pipe juts out and the column stops lining up --
    # which is precisely the defect Check 17 exists to prevent. The measurement
    # that justified the cap scored LINE WIDTH (median 134 -> 124, rows over 200
    # halved) and reported the cost as "17% of cells left unpadded", which
    # sounded like a minor trade and is in fact broken alignment on one row in
    # six. Width was never the quantity that mattered.
    # Width is measured on the RE-ESCAPED text, because that is what lands in
    # the file and what a reader of the raw source sees. Measuring the decoded
    # cell instead left every escaped-pipe row one column short: `_reescape`
    # adds a backslash after the width was already fixed.
    widths = [
        max(3, max(_dwidth(_reescape(rows[r][c])) for r in range(len(rows)) if r != 1))
        for c in range(ncols)
    ]
    if max_pad:
        widths = [min(max_pad, w) for w in widths]

    def pad(cell: str, width: int, align: str) -> str:
        gap = width - _dwidth(cell)
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
            out.append("| " + " | ".join(_reescape(c) for c in cells) + " |")
        else:
            out.append("| " + " | ".join(
                pad(_reescape(row[c]), widths[c], aligns[c]) for c in range(ncols)
            ) + " |")
    return out


def process(path: Path, max_cell: int, check: bool, max_pad: int = 0) -> bool:
    """Return True if the file has (or would have) changes."""
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")
    trailing_newline = text.endswith("\n")
    changed = False
    for start, end in reversed(_find_tables(lines)):
        rendered = _render_table(lines, start, end, max_cell, max_pad)
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
    # Per-column padding ceiling. See _render_table for how 40 was chosen.
    ap.add_argument("--max-pad", type=int, default=0,
                    help="cap per-column padding (0 = no cap). See _render_table: "
                         "capping BREAKS alignment for over-cap cells, which is "
                         "worse than a wide table.")
    args = ap.parse_args()

    any_changed = False
    for f in _iter_md(args.paths):
        if process(f, args.max_cell, args.check, args.max_pad):
            any_changed = True
            verb = "would align" if args.check else "aligned"
            print(f"{verb}: {f}")

    if args.check:
        return 1 if any_changed else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
