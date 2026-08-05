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


# A declarator modifier that may sit between a split return type and the
# symbol: a GNU attribute, or a calling-convention macro. MEASURED on the live
# tree 2026-08-05: ZERO definitions currently use this shape (EFIAPI appears
# only inside typedefs such as src/boot/uefi/efi.h:173, which begin with
# `typedef` and are correctly refused). It is supported because section 10
# names `__attribute__((...))`-prefixed definitions explicitly, so the value is
# forward-looking robustness rather than a coverage win today.
_DECL_MODIFIER_RE = re.compile(
    r"^\s*(?:__attribute__\s*\(\([^()]*(?:\([^()]*\)[^()]*)*\)\)"
    r"|EFIAPI|UEFI_EFIAPI|WINAPI|NTAPI|[A-Z][A-Z0-9_]*CALL)\s+"
)

# A physical line that carries ONLY return-type tokens -- the leading line(s)
# of a split function head. Deliberately the same character class as the
# single-line head regex, so pass 2 cannot accept a token shape pass 1 would
# have rejected.
_TYPE_ONLY_LINE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_*\s]*$")

_WINDOW_LINES = 3  # terminal line + up to 2 preceding type-only lines

def _split_head_candidates(head_window: tuple, symbol: str):
    """Yield line indices in `head_window` that could be a split head's terminal
    line for `symbol`, using only cheap substring tests.

    COST DISCIPLINE. This runs inside the pre-commit lint, once per stamped
    symbol reference, on every commit in the repo. Pass 2 fires precisely when
    pass 1 FAILED, and 136 of this corpus's 193 references are unresolved, so it
    is the COMMON path. A first cut masked and regex-matched all 512 head lines
    per reference: 52,693 masking calls over 2.26M characters, taking the Check
    7 walk from 0.078s to 0.494s (7.1x) for +2 resolved references. A per-file
    candidate index was tried next and profiled at 0.210s of a 0.36s walk -- it
    was built for 56 files to produce those same 2 resolutions.

    A substring test is the right filter: `symbol in line` is a C-level scan, and
    a line that does not contain the symbol at all can never be its head. The
    expensive masking and regex work then runs on the handful of survivors, and
    the head regex still makes every accept/reject decision -- so this narrows
    only the SEARCH, never the semantics.
    """
    for i in range(1, len(head_window)):  # a split head needs a preceding line
        if symbol in head_window[i]:
            yield i


def _mask_line_comments(line: str) -> str:
    """Blank out `//` and `/* ... */` spans with SPACES, preserving length.

    Offset-preserving on purpose. The stripping variant (`_strip_line_comments`)
    shortens the line, so a column measured on the stripped text does not index
    the raw text -- and pass 2 needs a RAW column to hand `_confirm_definition`.
    Computing that column by searching the raw line for the symbol instead is
    what broke: a comment or attribute containing the same `symbol(` earlier on
    the line captured the search, the walk started at the wrong parenthesis, and
    the real definition was silently dropped. Reproduced on both
    `/* foo(legacy) */ foo(void)` and `__attribute__((foo())) foo(void)`.
    Masking keeps match offsets valid, which removes the search entirely.
    """
    # FAST PATH: a line with no comment introducer and no quote has nothing to
    # mask, and that is the overwhelming majority. Building a character list for
    # every line of every scanned file is what made the index expensive enough
    # to matter (this runs inside the pre-commit lint, on every commit).
    if "/" not in line and '"' not in line and "'" not in line:
        return line
    out = list(line)
    i = 0
    n = len(line)
    while i < n:
        if line[i:i + 2] == "//":
            for k in range(i, n):
                out[k] = " "
            break
        if line[i:i + 2] == "/*":
            end = line.find("*/", i + 2)
            stop = n if end < 0 else end + 2
            for k in range(i, stop):
                out[k] = " "
            i = stop
            continue
        i += 1
    return "".join(out)


def _mask_decl_modifiers(text: str) -> str:
    """Blank a bounded run of leading declarator modifiers with SPACES so the
    symbol can lead the line for matching while offsets stay raw-accurate."""
    for _ in range(4):  # bounded: no unbounded rescan on adversarial input
        m = _DECL_MODIFIER_RE.match(text)
        if not m:
            break
        text = " " * m.end() + text[m.end():]
    return text


def _iter_code_chars(lines: list, start_idx: int, start_col: int, end_idx: int):
    """Yield (line_idx, col, ch) for CODE characters only -- skipping `//` and
    `/* */` comments and the contents of string and character literals.

    ONE scanner for every structural walk in this module. Three separate
    ad-hoc passes previously did their own per-line comment stripping, and each
    got it wrong in its own way:
      * the body brace count lost block-comment state across lines, so a
        multi-line comment containing `{` made a real definition unresolvable;
      * `is_stub_body`'s opener scan had the same defect independently;
      * none of them knew about string literals, so an attribute string
        containing `(`, `)` or `//` -- 9 files in this tree carry that shape --
        could unbalance the paren depth or truncate a line.
    A shared scanner is also the only way the resolver and the classifier can
    be guaranteed to agree about where a body starts, which is what Check 7's
    line numbers and its INTENTIONAL-STUB allowlist both depend on.

    String state deliberately resets at end of line: a C string literal does not
    span lines without a backslash continuation, and resetting is the safer
    failure direction (a stray quote cannot swallow the rest of the file).
    """
    in_block = False
    for i in range(start_idx, min(len(lines), end_idx)):
        line = lines[i]
        j = start_col if i == start_idx else 0
        in_str = None
        n = len(line)
        while j < n:
            ch = line[j]
            if in_block:
                if ch == "*" and j + 1 < n and line[j + 1] == "/":
                    in_block = False
                    j += 2
                    continue
                j += 1
                continue
            if in_str is not None:
                if ch == "\\":
                    j += 2
                    continue
                if ch == in_str:
                    in_str = None
                j += 1
                continue
            if ch == "/" and j + 1 < n:
                nxt = line[j + 1]
                if nxt == "/":
                    break  # rest of the physical line is a comment
                if nxt == "*":
                    in_block = True
                    j += 2
                    continue
            if ch == '"' or ch == "'":
                in_str = ch
                j += 1
                continue
            yield (i, j, ch)
            j += 1


def _confirm_definition(lines: list, head_idx: int, after_paren_col: int):
    """Walk forward from just past a candidate head's opening `(` and decide
    what the candidate really is. Returns (kind, body_open_idx) where kind is
    "def" (body_open_idx set), "decl", or "reject".

    PAREN-DEPTH AWARE, unlike the original walk this replaces. That walk took
    the first `{` appearing before the first `;` anywhere in a 16-line window,
    which mis-reads two real shapes:
      * `int\\nfoo(struct Local { int v; } *arg)` -- the PARAMETER struct's
        brace was taken as the function body, yielding a wrong line range.
      * `TYPE\\nSYMBOL(args) = { ... };` -- an object initializer produced by a
        function-like macro was accepted as a definition, because `=` was
        ignored entirely.
    Both are latent in the single-line path too; the fix applies to both passes
    and the corpus snapshot (corpus_resolution_snapshot.py) is what proves no
    existing mapping moved as a result.

    K-and-R definitions are explicitly DECLINED, not supported: a parameter
    declaration`s `;` at depth 0 classifies as "decl". This tree is modern C
    and has none; supporting them would widen the accept surface for no gain.
    """
    depth = 1  # we start immediately inside the parameter list
    for line_i, col, ch in _iter_code_chars(
            lines, head_idx, after_paren_col, head_idx + 16):
        if depth > 0:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            continue
        # depth == 0: the parameter list has closed.
        if ch.isspace():
            continue
        if ch == "(":
            # A trailing attribute group, e.g. `__attribute__((noreturn))`.
            depth += 1
            continue
        if ch == "{":
            # Return the EXACT column too. The caller brace-counts the body
            # from here; starting at column 0 of this line instead re-counted
            # any parameter-list braces that share it, so
            # `foo(struct Local { int v; } *arg) {` ended the "body" at the
            # parameter struct's closing brace and returned a truncated range
            # that Check 7 still counted as resolved.
            return ("def", line_i, col)
        if ch == ";":
            return ("decl", None, None)
        if ch.isalnum() or ch == "_":
            # Attribute / calling-convention identifier between the parameter
            # list and the body. Keep walking.
            continue
        # Anything else at depth 0 -- notably `=` (an object initializer
        # produced by a function-like macro) or `,` (a declarator list) --
        # means this is not a function definition. ONE reject path, so a
        # mutation of it is individually provable: an earlier draft also
        # special-cased `,=` above, and the two branches shadowed each other so
        # neither could be shown to matter on its own.
        return ("reject", None, None)
    return ("reject", None, None)


def resolve_symbol(file_path: str, symbol: str) -> Optional[Tuple[str, int, int]]:
    """Return (file_path, line_start, line_end) for the function `symbol`
    defined in `file_path`, or None if not found / not a real function
    definition / file unreadable.

    line_start is the 1-based line number where the function-definition HEAD
    BEGINS, and line_end is the 1-based line number of the closing `}`.

    For a single-line head that is the line carrying the symbol + `(`. For a
    SPLIT head it is the first return-type line, which is earlier than the
    symbol's line -- e.g. `static inline const char *` on line 30 with
    `uki_find_disk_override_token(...)` on line 31 yields line_start 30. The
    contract is "where the head begins" precisely so the returned range always
    CONTAINS the body opener, which is what `is_stub_body` scans for and what
    the INTENTIONAL-STUB allowlist is read from.
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
    head_window = lines[:_DEF_HEAD_LIMIT]

    # ---- PASS 1: single-line head. UNCHANGED CANDIDATE SET. -------------
    # Collect EVERY candidate head; keep walking past prototypes that turn
    # out to be declarations (Codex F2: `static int foo(void);` followed
    # by the real definition is a common C pattern). The head_idx that
    # carries the body opener wins; if none do, fall through to pass 2.
    head_idx = -1
    body_open_idx = -1
    body_open_col = -1
    for cand, ln in enumerate(head_window):
        m = head_re.match(ln)
        if not m:
            continue
        kind, local_body_idx, local_body_col = _confirm_definition(
            lines, cand, m.end())
        if kind == "def":
            head_idx, body_open_idx, body_open_col = (
                cand, local_body_idx, local_body_col)
            break
        # "decl": forward declaration -- keep walking to the real definition.
        # "reject": not a function definition -- skip it too.

    # ---- PASS 2: split head, FALLBACK ONLY. -----------------------------
    # Runs only when pass 1 found nothing, so a resolution pass 1 can make is
    # never displaced and the resolved COUNT cannot fall by construction. That
    # ordering is the whole reason this section's first attempt was reverted
    # (5cff59cc): it REPLACED the single-line matcher and took live coverage
    # from 52/186 to 1/186 while its own synthetic fixtures passed.
    if head_idx < 0:
        # `^\s*(?:\*\s*)?`, NOT `^\s*\*?\s*`. The latter puts two whitespace
        # quantifiers either side of an optional atom, so on a long run of
        # whitespace that ultimately FAILS to match, the engine explores
        # quadratically many partitions: measured 0.38s at 20k characters and
        # 2.3s at 50k, versus ~1ms for this form. That input is reachable -- the
        # substring filter admits a line whose only occurrence of the symbol is
        # inside a block comment, and masking then turns the line into pure
        # whitespace -- so a single long generated comment could hang the
        # commit-time lint. The parent resolver rejected such a line outright.
        lead_re = re.compile(r"^\s*(?:\*\s*)?" + re.escape(symbol) + r"\s*\(")
        for term in _split_head_candidates(head_window, symbol):
            # MASKED, not stripped: masking preserves length, so `lead.end()`
            # below is a RAW-line offset that can go straight to
            # _confirm_definition with no search back into the raw text.
            body = _mask_decl_modifiers(_mask_line_comments(head_window[term]))
            # The raw substring filter matched, but masking may have removed the
            # only occurrence (it was inside a comment or a string). Bail before
            # the regex rather than handing it a long masked line to fail on.
            if symbol not in body:
                continue
            # The symbol must OPEN the terminal line (after any modifiers and an
            # optional pointer star). That anchor is what keeps a call statement
            # such as `total = foo(x);` from becoming a candidate head.
            lead = lead_re.match(body)
            if not lead:
                continue
            lead_end = lead.end()
            # Walk back over up to _WINDOW_LINES-1 preceding type-only lines.
            prefix_parts = []
            for back in range(1, _WINDOW_LINES):
                pi = term - back
                if pi < 0:
                    break
                prev = _mask_line_comments(head_window[pi]).strip()
                if not prev:
                    break
                if not _TYPE_ONLY_LINE_RE.match(prev):
                    break
                prefix_parts.insert(0, prev)
                # A single type-only line is enough to make a valid head; keep
                # collecting only while the joined text still fails to match.
                joined = " ".join(prefix_parts) + " " + body.strip()
                if not head_re.match(joined):
                    continue
                # Confirm through the SAME depth-aware walk pass 1 uses.
                # `lead_end` is a RAW-line column (the index masks to
                # equal-length spaces) sitting just past the opening paren,
                # which is exactly where _confirm_definition expects to start.
                kind, local_body_idx, local_body_col = _confirm_definition(
                    lines, term, lead_end)
                if kind == "def":
                    head_idx, body_open_idx, body_open_col = (
                        pi, local_body_idx, local_body_col)
                    break
            if head_idx >= 0:
                break

    if head_idx < 0 or body_open_idx < 0:
        return None

    # Brace-count from the body opener to find the body end.
    #
    # Starts at the opener's exact COLUMN, not column 0 of its line. Starting at
    # the line start re-counted any braces the parameter list left on that same
    # line, so `foo(struct Local { int v; } *arg) {` closed the "body" at the
    # parameter struct's `}` and returned a truncated range -- which Check 7
    # still counted as resolved while never examining the real body.
    #
    # Uses the shared lexical scanner, so a `{` or `}` inside a multi-line
    # comment or a string literal no longer moves the depth. Previously a block
    # comment containing an unbalanced brace made a real definition unresolvable.
    depth = 0
    end_idx = -1
    for line_i, _col, ch in _iter_code_chars(
            lines, body_open_idx, body_open_col,
            body_open_idx + _BODY_SCAN_LIMIT):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end_idx = line_i
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
    # PAREN-DEPTH AWARE, for the same reason _confirm_definition is. Taking the
    # FIRST `{` in the range picks the wrong line for
    # `foo(struct Local { int v; } *arg)`: it names the PARAMETER list's brace
    # as the body opener. That line number is not cosmetic -- it is what Check 7
    # prints as the finding location AND the only line the INTENTIONAL-STUB
    # allowlist is read from, so a marker on the real opener is never seen and a
    # deliberate stub becomes a blocking false positive. Latent for single-line
    # heads too; split-head support made it reachable more often.
    # Uses the SHARED lexical scanner, so this agrees with resolve_symbol by
    # construction rather than by two hand-written walks happening to match.
    # The per-line strip it replaces lost block-comment state across lines and
    # knew nothing about string literals.
    body_open_line_idx = None  # 0-based within body_text_lines
    body_open_col = -1
    paren_depth = 0
    for i, col, ch in _iter_code_chars(
            body_text_lines, 0, 0, len(body_text_lines)):
        if ch == "(":
            paren_depth += 1
        elif ch == ")":
            if paren_depth > 0:
                paren_depth -= 1
        elif ch == "{" and paren_depth == 0:
            body_open_line_idx, body_open_col = i, col
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
    # For a one-line body, slice from the OPENER'S COLUMN, not the line's first
    # `{`. Splitting on the first brace picked the PARAMETER LIST's for a one-line
    # `static int foo(struct Local { int v; } *arg) { return 0; }`, so the
    # extracted text never matched return-constant and a real stub-behind-stamp
    # went unreported. The multi-line end-to-end fixture did not reach this path.
    if not inner:
        opener = body_text_lines[body_open_line_idx]
        close = opener.rfind("}")
        if close > body_open_col:
            inner = [opener[body_open_col + 1:close]]
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
