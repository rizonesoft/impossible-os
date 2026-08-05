#!/usr/bin/env python3
# ============================================================================
# resolve_symbol.py -- C function definition resolver for the TODO cache.
#
# Owner: per-item stamped_items cache extension. Consumed by lint Check 7
# (stub-behind-stamp) to map a stamped `[x]` item's `{file, symbol}` ref
# pair to the function body's line range, then classify whether the body
# is a stub-behind-stamp (<=3-line return-constant body without an
# /* INTENTIONAL-STUB: <reason> */ marker on the body-opening line).
#
# Strategy:
#   - Read up to ~512 lines of the file (early-exit on first match) so a
#     pathological 50k-line generated header cannot stall the lint pass.
#   - Locate the function definition with a line-anchored regex:
#         ^[a-zA-Z_][a-zA-Z0-9_*\s]*\s+SYMBOL\s*\(
#   - Confirm a real function definition by walking forward past the
#     parameter list close-paren to the next non-comment, non-blank line:
#     a body-opening `{` makes it a definition; a `;` makes it a
#     declaration / typedef / function-pointer field. (Codex design
#     review F2/Q3: declarations and typedefs must NOT reach Check 7.)
#   - Brace-count from the opening `{` to find the body close. Bound the
#     scan to 1024 additional lines so a deeply-nested helper still
#     terminates.
#   - Return (file_path, line_start, line_end) on match; None otherwise.
#
# Pure stdlib. Zero subprocess fork. Runs in-process during cache build
# AND inside scripts/lint.sh Check 7.
# ============================================================================

import re
from functools import lru_cache
from pathlib import Path
from typing import Optional, Tuple

_DEF_HEAD_LIMIT = 512    # lines to scan before giving up the search
_BODY_SCAN_LIMIT = 1024  # additional lines to scan once the head is found


# Module-level file-line cache: a single per-process cache shared by
# resolve_symbol() and is_stub_body(). Both callers use the same key
# (path_str only) so a head-limited resolve_symbol() lookup populates
# the same entry that is_stub_body() later reads. lru_cache keeps the
# working set bounded for long-running consumers (lint Check 7 scans
# ~140 unique source files for ~330 unique (file, symbol) refs in the
# live tree; 512 entries is comfortably above either ceiling).
#
# We read the WHOLE file once instead of capping resolve_symbol at
# _DEF_HEAD_LIMIT + _BODY_SCAN_LIMIT. The original cap was a defensive
# bound against pathological multi-MB generated headers; even at 50k
# lines that is ~5MB per cached entry, well within the maxsize budget.
# resolve_symbol still applies its own slicing bounds to the in-memory
# tuple so the head/body scan logic remains O(_DEF_HEAD_LIMIT).
@lru_cache(maxsize=512)
def _load_file_lines(path_str: str) -> tuple:
    try:
        with open(path_str, "r", encoding="utf-8", errors="replace") as fh:
            return tuple(ln.rstrip("\n") for ln in fh)
    except OSError:
        return tuple()


def cache_clear() -> None:
    """Clear the per-process file cache. Tests that mutate fixtures on
    disk should call this between mutations so they do not see a stale
    cached read. Production callers do not need it: each lint invocation
    spawns a fresh interpreter."""
    _load_file_lines.cache_clear()


def _strip_line_comments(line: str) -> str:
    """Strip `//` and `/* ... */` from a line. Multi-line block comments
    are NOT handled here -- the caller drops blank-only lines and the
    open-brace search tolerates one hop past a `/*` on its own line.
    Adequate for the stub-behind-stamp body-shape check; not a full C
    preprocessor."""
    # Strip `//` style.
    idx = line.find("//")
    if idx >= 0:
        line = line[:idx]
    # Strip same-line `/* ... */` blocks.
    line = re.sub(r"/\*.*?\*/", "", line)
    return line


def _build_def_head_re(symbol: str) -> "re.Pattern[str]":
    # Per-symbol pattern: function-style return type tokens, then the
    # symbol, then `(`. The first non-space character must be a letter or
    # underscore so #defines and labels do not match.
    return re.compile(
        r"^[A-Za-z_][A-Za-z0-9_*\s]*\b"
        + re.escape(symbol)
        + r"\s*\("
    )


def resolve_symbol(file_path: str, symbol: str) -> Optional[Tuple[str, int, int]]:
    """Return (file_path, line_start, line_end) for the function `symbol`
    defined in `file_path`, or None if not found / not a real function
    definition / file unreadable.

    line_start is the 1-based line number of the function-definition head
    (the line containing the symbol + `(`). line_end is the 1-based line
    number of the closing `}`.
    """
    p = Path(file_path)
    # Pull from shared module cache (single key per file). The head/body
    # scan logic below applies _DEF_HEAD_LIMIT / _BODY_SCAN_LIMIT slicing
    # to the in-memory tuple so the original O(scan-window) cost is
    # preserved even when the cached tuple is large.
    full_tuple = _load_file_lines(str(p))
    if not full_tuple:
        return None
    lines = list(full_tuple[: _DEF_HEAD_LIMIT + _BODY_SCAN_LIMIT])

    head_re = _build_def_head_re(symbol)
    # Collect EVERY candidate head; keep walking past prototypes that turn
    # out to be declarations (Codex F2: `static int foo(void);` followed
    # by the real definition is a common C pattern). The head_idx that
    # carries the body opener wins; if none do, return None.
    head_candidates = [
        i for i, ln in enumerate(lines[:_DEF_HEAD_LIMIT]) if head_re.match(ln)
    ]
    if not head_candidates:
        return None

    head_idx = -1
    body_open_idx = -1
    for cand in head_candidates:
        in_block_comment = False
        local_body_idx = -1
        is_decl = False
        for j in range(cand, min(len(lines), cand + 16)):
            raw = lines[j]
            if in_block_comment:
                if "*/" in raw:
                    raw = raw.split("*/", 1)[1]
                    in_block_comment = False
                else:
                    continue
            if j == cand:
                head_match = head_re.match(raw)
                if head_match:
                    raw = raw[head_match.end():]
            raw = _strip_line_comments(raw)
            if "/*" in raw and "*/" not in raw:
                in_block_comment = True
                raw = raw.split("/*", 1)[0]
            stripped = raw.strip()
            if not stripped:
                continue
            brace_pos = stripped.find("{")
            semi_pos = stripped.find(";")
            if brace_pos >= 0 and (semi_pos < 0 or brace_pos < semi_pos):
                local_body_idx = j
                break
            if semi_pos >= 0:
                is_decl = True
                break
        if local_body_idx >= 0:
            head_idx = cand
            body_open_idx = local_body_idx
            break
        # is_decl=True: candidate was a forward declaration; keep walking.
        # local_body_idx<0 and not is_decl: ambiguous head (no terminator
        # in 16-line window); skip it too.
    if head_idx < 0 or body_open_idx < 0:
        return None
    if body_open_idx < 0:
        return None

    # Brace-count from the body opener to find the body end.
    depth = 0
    end_idx = -1
    for k in range(body_open_idx, min(len(lines), body_open_idx + _BODY_SCAN_LIMIT)):
        raw = lines[k]
        clean = _strip_line_comments(raw)
        # Naive brace count -- ignores braces inside strings. Adequate for
        # well-formed kernel C; the lint check is advisory in any case.
        for ch in clean:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    end_idx = k
                    break
        if end_idx >= 0:
            break
    if end_idx < 0:
        return None
    return (str(p), head_idx + 1, end_idx + 1)


def is_stub_body(
    file_path: str, line_start: int, line_end: int
) -> Optional[Tuple[str, int]]:
    """Return (matched_return_text, body_open_line) when the function body
    spanning [line_start, line_end] (1-based) is a stub-behind-stamp:
    <=3 non-blank, non-brace, non-comment lines whose only statement is a
    `return CONSTANT;`. Returns None when the body is non-stub OR an
    INTENTIONAL-STUB allowlist marker is present on the body-opening line.

    Allowlist marker: /* INTENTIONAL-STUB: <reason> */ on the same line as
    the body-opening `{` (mirrors lint Check 6's TEST-TAUTOLOGY-OK pattern).
    """
    # Shared cache: same key as resolve_symbol() so the file is read at
    # most once per process across both callers.
    all_lines_tuple = _load_file_lines(str(file_path))
    if not all_lines_tuple:
        return None
    if line_start < 1 or line_end > len(all_lines_tuple) or line_start > line_end:
        return None

    body_text_lines = list(all_lines_tuple[line_start - 1: line_end])

    # Find the line carrying the opening `{` (body open). When the head
    # straddles, body open may be on a later line. The marker scan is
    # explicit: any line in the head-through-body-open range carrying
    # /* INTENTIONAL-STUB: */ disables the check.
    body_open_line_idx = None  # 0-based within body_text_lines
    for i, ln in enumerate(body_text_lines):
        if "{" in _strip_line_comments(ln):
            body_open_line_idx = i
            break
    if body_open_line_idx is None:
        return None
    # Allowlist scan: same line as the body opener.
    if "INTENTIONAL-STUB" in body_text_lines[body_open_line_idx]:
        return None

    # Body content: everything AFTER body_open_line_idx until close.
    inner = body_text_lines[body_open_line_idx + 1: -1]  # exclude `}` line
    # If body opener and closer are on same line (e.g. `int f(void) { return 0; }`),
    # extract content between `{` and `}` on that line.
    if not inner:
        opener = body_text_lines[body_open_line_idx]
        if "{" in opener and "}" in opener:
            inner_text = opener.split("{", 1)[1].rsplit("}", 1)[0]
            inner = [inner_text]
    # Filter to "real" content lines.
    real_lines = []
    for ln in inner:
        clean = _strip_line_comments(ln).strip()
        if not clean:
            continue
        if clean in ("{", "}"):
            continue
        real_lines.append(clean)
    if len(real_lines) > 3:
        return None
    # All non-blank lines must collectively be a single `return CONSTANT;`
    # statement. Concatenate (handles `return\n    STATUS_NOT_IMPL;`) and
    # match the canonical shape.
    joined = " ".join(real_lines)
    # Must end with `;`. Allow optional trailing whitespace.
    m = re.match(
        r"^return\s+([A-Za-z_][A-Za-z0-9_]*|0|-?\d+|0x[0-9A-Fa-f]+|NULL|true|false)\s*;\s*$",
        joined,
    )
    if not m:
        return None
    return (m.group(1), line_start + body_open_line_idx)


# Allow `python3 scripts/todo-graph/resolve_symbol.py <file> <symbol>`
# for ad-hoc debugging.
if __name__ == "__main__":  # pragma: no cover
    import sys
    if len(sys.argv) != 3:
        sys.stderr.write("usage: resolve_symbol.py <file> <symbol>\n")
        sys.exit(2)
    res = resolve_symbol(sys.argv[1], sys.argv[2])
    if res is None:
        sys.stderr.write("not found\n")
        sys.exit(1)
    print(f"{res[0]}:{res[1]}:{res[2]}")
    stub = is_stub_body(res[0], res[1], res[2])
    if stub:
        print(f"STUB: returns {stub[0]} (body opens at line {stub[1]})")
