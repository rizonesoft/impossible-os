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
#   - Scan CODE CHARACTERS, not physical lines. `_code_chars()` drops
#     comments and the contents of string/character literals and emits a
#     newline as a space, so a declarator head reads the same whether the
#     author wrote it on one line or four. This is what lets a
#     `static inline\nint *\nfoo(void)` definition resolve at all; the
#     original line-anchored regex required the return type and the symbol
#     to share a physical line, and every split head was silently invisible.
#   - Track PARENTHESIS DEPTH while scanning. `{` and `;` are read as body
#     opener / declaration terminator only at depth 0, so a prototype-scope
#     structure -- `int foo(struct Local { int value; } *arg)`, which is
#     legal C -- no longer terminates the head inside its own parameter
#     list. That shape used to return a one-line range whose body was the
#     structure rather than the function, and a real stub behind it read as
#     clean.
#   - A candidate head must still begin at COLUMN 0 with a letter or
#     underscore. That is the false-positive guard: an indented call site
#     can never start a candidate, which is what keeps a lexical scan from
#     being looser than the regex it replaces.
#   - Walk past prototypes: a declarator whose depth-0 terminator is `;` is
#     a declaration, and the next candidate is tried (`static int foo(void);`
#     ahead of the real definition is a common C shape).
#   - Bounded: `_DEF_HEAD_LIMIT` lines of search window, `_HEAD_SCAN_LINES`
#     per declarator, `_BODY_SCAN_LIMIT` for the body brace-count.
#
# NOT a C parser, and deliberately so. It declines rather than guesses on a
# preprocessor directive inside a head (a conditional can change what the
# following text means) and on a declarator whose return type carries its
# own parentheses, e.g. `int (*foo(void))(int)`. Both resolve to None, which
# today is SILENT -- TODO-06 section 11 owns turning an unresolved symbol
# into a counted number instead of an absence, and that is the half of the
# problem this module cannot fix on its own.
#
# Pure stdlib. Zero subprocess fork. Runs in-process during cache build
# AND inside scripts/lint.sh Check 7.
# ============================================================================

import re
from functools import lru_cache
from pathlib import Path
from typing import Iterator, Optional, Tuple

_DEF_HEAD_LIMIT = 512    # lines to scan before giving up the search
_BODY_SCAN_LIMIT = 1024  # additional lines to scan once the head is found
_HEAD_SCAN_LINES = 8     # physical lines one declarator head may span

# A GNU attribute prefix carries its own parentheses, and the FIRST `(` of a
# declarator is otherwise taken to open the parameter list -- so
# `__attribute__((noinline)) static int foo(void)` would end the head at the
# attribute and never see the symbol. When the accumulated prefix ends in
# `__attribute__`, its parentheses are consumed and the token dropped, and
# prefix accumulation continues. Matching on the TAIL of the prefix (rather
# than substituting a full attribute pattern afterwards) is what makes this
# work for the same-line form, where the closing parens have not been read
# yet at the moment the decision is needed.
_ATTR_TAIL_RE = re.compile(r"__attribute__\s*$")

_START_CHAR_RE = re.compile(r"[A-Za-z_]")


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
    are NOT handled here. Used only by is_stub_body()'s body-SHAPE check,
    where the text under examination is a handful of lines already known to
    sit inside one brace pair; all STRUCTURAL scanning goes through
    _code_chars(), which is comment- and literal-aware."""
    # Strip `//` style.
    idx = line.find("//")
    if idx >= 0:
        line = line[:idx]
    # Strip same-line `/* ... */` blocks.
    line = re.sub(r"/\*.*?\*/", "", line)
    return line


def _build_def_head_re(symbol: str) -> "re.Pattern[str]":
    # Per-symbol pattern: function-style return type tokens, then the
    # symbol, then `(`. The first character must be a letter or underscore
    # so #defines and labels do not match, and at least one character must
    # precede the symbol, so a bare `foo(` -- a call site, or an
    # implicit-int K&R head this repo does not use -- is not read as a head.
    return re.compile(
        r"^[A-Za-z_][A-Za-z0-9_*\s]*\b"
        + re.escape(symbol)
        + r"\s*\("
    )


def _code_chars(
    lines, start: int, stop: int, directives: str = "stop"
) -> Iterator[Tuple[int, str]]:
    """Yield (physical_line_index, char) for the CODE characters of
    lines[start:stop].

    Comments are dropped. So are the CONTENTS of string and character
    literals -- each literal collapses to one space -- so a `}` or `;`
    inside one cannot be read as punctuation. Every end-of-line emits a
    space, which is what makes a head spanning physical lines readable as a
    single declarator.

    `directives` decides what a preprocessor line does. "stop" ENDS the
    stream: inside a declarator head a conditional can change what the
    following text means, and declining beats guessing. "skip" ignores the
    line and continues, which is what a function BODY needs -- `#ifdef`
    inside a body is ordinary, and stopping there would truncate the brace
    count and make the whole function unresolvable.
    """
    in_block = False
    for idx in range(start, min(stop, len(lines))):
        line = lines[idx]
        if not in_block and line.lstrip()[:1] == "#":
            if directives == "stop":
                return
            continue
        i, n = 0, len(line)
        while i < n:
            ch = line[i]
            if in_block:
                if ch == "*" and i + 1 < n and line[i + 1] == "/":
                    in_block = False
                    i += 2
                    continue
                i += 1
                continue
            if ch == "/" and i + 1 < n:
                nxt = line[i + 1]
                if nxt == "/":
                    break
                if nxt == "*":
                    in_block = True
                    i += 2
                    continue
            if ch in ('"', "'"):
                quote = ch
                i += 1
                while i < n:
                    if line[i] == "\\":
                        i += 2
                        continue
                    if line[i] == quote:
                        i += 1
                        break
                    i += 1
                yield (idx, " ")
                continue
            yield (idx, ch)
            i += 1
        yield (idx, " ")


def _scan_declarator(lines, start: int, head_re) -> Tuple[str, Optional[int]]:
    """Read ONE declarator beginning at physical line `start`.

    Returns ("def", body_open_line_index) when it is a function definition
    whose head matches `head_re`, ("decl", None) when it is a declaration or
    prototype (so the caller keeps walking toward the real definition), and
    ("no", None) otherwise.
    """
    prefix = []
    depth = 0
    attr_depth = 0
    seen_open = False
    for idx, ch in _code_chars(lines, start, start + _HEAD_SCAN_LINES):
        if attr_depth:
            # Inside an attribute's parentheses: consume and discard.
            if ch == "(":
                attr_depth += 1
            elif ch == ")":
                attr_depth -= 1
                if attr_depth == 0:
                    prefix.append(" ")
            continue
        if not seen_open:
            if ch == "(":
                text = "".join(prefix)
                if _ATTR_TAIL_RE.search(text):
                    prefix = list(_ATTR_TAIL_RE.sub("", text))
                    attr_depth = 1
                    continue
                seen_open = True
                depth = 1
                if not head_re.match(text.lstrip() + "("):
                    return ("no", None)
                continue
            if ch in ";={}":
                # A depth-0 terminator before any parameter list: an object
                # declaration, an initializer, or the tail of the previous
                # construct. Not a function head.
                return ("no", None)
            prefix.append(ch)
            continue
        if ch == "(":
            depth += 1
            continue
        if ch == ")":
            depth = max(0, depth - 1)
            continue
        if depth > 0:
            # Inside the parameter list. A `{` or `;` here belongs to a
            # prototype-scope structure, not to the function.
            continue
        if ch == "{":
            return ("def", idx)
        if ch == ";":
            return ("decl", None)
        if ch in "=}":
            return ("no", None)
    return ("no", None)


def _find_body_end(lines, body_open_idx: int) -> Optional[int]:
    """Brace-count from the body opener to the matching close. Returns the
    physical line index of the closing `}`, or None when the body does not
    close within _BODY_SCAN_LIMIT lines."""
    depth = 0
    for idx, ch in _code_chars(
        lines, body_open_idx, body_open_idx + _BODY_SCAN_LIMIT, directives="skip"
    ):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    return None


def _find_body_open(lines, start_idx: int, stop_idx: int) -> Optional[int]:
    """Physical line index of the first `{` at parenthesis depth 0 within
    lines[start_idx:stop_idx], or None. Depth-awareness is what keeps a
    prototype-scope structure's brace, which sits inside the parameter
    list, from being mistaken for the body opener."""
    depth = 0
    for idx, ch in _code_chars(lines, start_idx, stop_idx, directives="skip"):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif ch == "{" and depth == 0:
            return idx
    return None


def resolve_symbol(file_path: str, symbol: str) -> Optional[Tuple[str, int, int]]:
    """Return (file_path, line_start, line_end) for the function `symbol`
    defined in `file_path`, or None if not found / not a real function
    definition / file unreadable.

    line_start is the 1-based line number of the FIRST line of the
    function-definition head -- the return-type line, when the head is
    split across lines. For a single-line head that is the line carrying
    the symbol, which is what this returned before split heads resolved at
    all. line_end is the 1-based line number of the closing `}`.
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

    # Candidate starts. A head that defines `symbol` must MENTION it within
    # _HEAD_SCAN_LINES of its first line, so only lines that can reach an
    # occurrence are scanned at all. Without this pre-filter, lexically
    # scanning every column-0 line would be markedly more expensive than
    # the per-line regex sweep it replaces; with it, the handful of
    # occurrences per file bound the work.
    starts = set()
    window = min(len(lines), _DEF_HEAD_LIMIT + _HEAD_SCAN_LINES)
    for occ in range(window):
        if symbol not in lines[occ]:
            continue
        for cand in range(max(0, occ - _HEAD_SCAN_LINES + 1), occ + 1):
            if cand >= _DEF_HEAD_LIMIT:
                continue
            if _START_CHAR_RE.match(lines[cand][:1]):
                starts.add(cand)

    for cand in sorted(starts):
        kind, body_open_idx = _scan_declarator(lines, cand, head_re)
        if kind != "def":
            # "decl" -> forward declaration, keep walking (Codex F2).
            # "no"   -> not this symbol's head.
            continue
        end_idx = _find_body_end(lines, body_open_idx)
        if end_idx is None:
            # The head resolved but the body does not close inside the scan
            # bound. Fail closed rather than resolving some LATER candidate:
            # returning a different function's range under this symbol's
            # name would be worse than returning nothing.
            return None
        return (str(p), cand + 1, end_idx + 1)
    return None


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

    # Find the line carrying the opening `{`. The head may straddle several
    # lines, and its parameter list may itself contain a brace, so this is
    # parenthesis-depth-aware rather than "first line containing a `{`".
    open_abs = _find_body_open(list(all_lines_tuple), line_start - 1, line_end)
    if open_abs is None:
        return None
    body_open_line_idx = open_abs - (line_start - 1)  # 0-based within body
    # Allowlist scan: same line as the body opener. Read from the RAW line,
    # because _code_chars drops comments and the marker IS a comment.
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
