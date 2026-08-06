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
#   - Read the whole file once (cached, one pinned generation per process) and
#     build a per-file lexical index: line-start offsets for C-level candidate
#     search, plus the spans that are comment or literal rather than code. The
#     head search is POSITIONALLY UNBOUNDED; the bound is _MAX_FILE_BYTES on
#     the INPUT, because a positional cap is silent tail blindness (it hid
#     kmalloc at heap.c:666 and schedule at task.c:1721 for as long as it
#     existed) whereas an oversized file raises.
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

import bisect
import os
import re
from functools import lru_cache
from itertools import accumulate
from operator import add
from pathlib import Path
from typing import Optional, Tuple

_BODY_SCAN_LIMIT = 1024  # lines to scan once the head is found

# Per-file hard ceiling. A file above this is refused LOUDLY (see
# ResolverInputError) rather than read: the head search below is positionally
# unbounded, so the file's SIZE is the only remaining bound on work and
# resident memory, and a silent empty return would demote a pathological input
# to an ordinary "unresolved" ref -- fail-open on exactly the input the limit
# exists to notice. The largest file in this tree is well under 1 MiB, so
# 16 MiB is orders of magnitude of headroom and cannot fire on real source.
_MAX_FILE_BYTES = 16 * 1024 * 1024


class ResolverInputError(RuntimeError):
    """An input the resolver refuses to answer about.

    Distinct from "not found": None means the symbol is not defined in a file
    that was read successfully, whereas this means no trustworthy read could be
    established at all. Consumers must SURFACE it, never bucket it as an
    ordinary unresolved reference -- that would be fail-open on the one input
    class the limits exist to notice.
    """


class _AmbiguousEscapeSplice(Exception):
    """A string/char literal's escaping backslash lands exactly on a spliced
    line boundary, so its target character (whatever it turns out to be) sits
    on the NEXT physical line -- ambiguous without carrying escape state
    across the splice, which this module's per-line scanner does not do
    (TODO-06 section 12: a known, corpus-verified-near-absent residual --
    fires once in the whole tree, inside a comment, never in live string
    content). Module-PRIVATE and never crosses a public function boundary:
    every internal caller of `_iter_code_chars` catches it and treats it the
    SAME as "the walk did not find what it was looking for" -- the safe
    direction is to fail toward unresolved, never toward a silently wrong
    range. Distinct from `ResolverInputError`: this is about one symbol's
    resolution being unsafe, not about the FILE being untrustworthy."""


# ---------------------------------------------------------------------------
# File loading. ONE GENERATION PER PROCESS, and mutation is detected.
#
# The cache is keyed on PATH ALONE, deliberately. Keying on
# (path, size, mtime_ns) was considered and REJECTED: resolve_symbol() returns
# line coordinates from the generation it read, and is_stub_body() then loads
# the same path independently to classify the body. If a rewrite between those
# two calls selected a NEWER generation, old coordinates would be applied to
# new contents -- a false clean or a false finding, silently. A path-only key
# is internally consistent by construction.
#
# What a path-only key cannot do on its own is NOTICE the rewrite, which is the
# real gap (the module docstring's "safe today only because each lint
# invocation is a fresh interpreter"). So the generation is PINNED at first
# read and every later load re-stats and refuses on drift: consistency AND
# detection, instead of trading one for the other.
_pinned = {}  # path_str -> (st_dev, st_ino, st_size, st_mtime_ns)


# path_str -> (meta, lines) for the LAST generation actually read. Separate
# from `_pinned`: this is a pure PERFORMANCE cache (skip re-reading unchanged
# bytes), never a correctness promise -- `_read_file_lines` still re-fstats on
# EVERY call regardless of what is in here.
_content_cache = {}


def _read_file_lines(path_str: str) -> tuple:
    """Open-fstat, and read-fstat ONLY IF the metadata changed since the last
    read of THIS path, ONE generation of `path_str`. Returns (meta, lines).
    Raises OSError on an unreadable path (caller decides what that means) and
    ResolverInputError on an oversized file or a file that changed DURING the
    read.

    NOT `@lru_cache`d by path alone, deliberately: that was tried first and is
    a real bug, not a style choice -- `functools.lru_cache` serves the SAME
    cached (meta, lines) forever for a given path_str, so it never re-opens
    the file on a second call and the mutation-detection contract this module
    exists to provide becomes DEAD CODE: `_load_file_lines`'s pin-vs-meta
    comparison can never see a changed `meta`, because a cached call never
    produces one. Caught by this section's OWN regression fixture: a
    same-process rewrite between two resolves of the same path silently
    served the FIRST generation's cached lines forever. The fix re-fstats on
    EVERY call (cheap: one syscall) and reuses the expensive parsed-lines
    tuple only when that fresh fstat matches what was cached.

    DESCRIPTOR-based, not path-based: `os.fstat(fd)` reports the metadata of
    the OPEN FILE, immune to whatever the pathname is later renamed over --
    `os.stat(path)` before `open(path)` cannot make that guarantee, since a
    replacement landing in the stat-to-open window is invisible to a
    path-based check and the bytes actually read could belong to a different
    generation than the one the ceiling and the pin recorded. Both fstats use
    the SAME descriptor, so a size/mtime change between them means the
    underlying file was rewritten in place while being read (not merely
    renamed-over), and the read result is refused as incoherent rather than
    silently returned.

    `tuple(ln.rstrip("\\n") for ln in fh)` measured 34ms of a 307ms corpus walk
    purely in the generator expression. `read().split("\\n")` does the same work
    inside CPython. A trailing newline yields a final empty element that file
    iteration would not produce, so it is dropped -- line COUNT is load-bearing
    here (every returned coordinate is an index into this tuple)."""
    fd = os.open(path_str, os.O_RDONLY)
    try:
        st1 = os.fstat(fd)
        if st1.st_size > _MAX_FILE_BYTES:
            raise ResolverInputError(
                f"{path_str}: {st1.st_size} bytes exceeds the "
                f"{_MAX_FILE_BYTES}-byte per-file resolver limit")
        meta = (st1.st_dev, st1.st_ino, st1.st_size, st1.st_mtime_ns)
        cached = _content_cache.get(path_str)
        if cached is not None and cached[0] == meta:
            return cached  # fstat-confirmed unchanged; skip the read+split
        with os.fdopen(fd, "r", encoding="utf-8", errors="replace") as fh:
            fd = -1  # fdopen() took ownership of the descriptor
            # BOUND THE READ ITSELF, not just the pre-read fstat. `fh.read()`
            # with no argument reads to EOF regardless of what st1 measured --
            # a file that GROWS after this fstat (concurrent writer) would be
            # read in full, unbounded, before the post-read fstat below ever
            # gets a chance to notice the drift (Codex adversarial: the
            # ceiling "raises eventually but does not provide the memory/work
            # bound it claims"). Reading one char past the limit is the
            # cheapest way to detect "this generation is too big" without
            # ever materializing more than limit+1 characters.
            text = fh.read(_MAX_FILE_BYTES + 1)
            if len(text) > _MAX_FILE_BYTES:
                raise ResolverInputError(
                    f"{path_str}: grew past the {_MAX_FILE_BYTES}-byte "
                    f"resolver limit during the read (was {st1.st_size} "
                    f"bytes at open)")
            st2 = os.fstat(fh.fileno())
    finally:
        if fd >= 0:
            os.close(fd)
    meta2 = (st2.st_dev, st2.st_ino, st2.st_size, st2.st_mtime_ns)
    if meta != meta2:
        raise ResolverInputError(
            f"{path_str}: changed on disk WHILE being read (was {meta}, now "
            f"{meta2}) -- the bytes just read are not one coherent "
            f"generation")
    if not text:
        result = (meta, tuple())
    else:
        parts = text.split("\n")
        if parts and parts[-1] == "":
            parts.pop()
        result = (meta, tuple(parts))
    _content_cache[path_str] = result
    return result


def _load_file_lines(path_str: str) -> tuple:
    """Cached physical lines for `path_str`, or () when there is no readable
    file. () is reserved for missing / unreadable / empty ON THE FIRST READ,
    which every caller already treats as "not resolvable here"; once a path
    has been PINNED (a prior read succeeded), any LATER failure -- open,
    fstat, read, or a metadata mismatch against the pin -- raises
    ResolverInputError instead of quietly degrading to empty, because
    coordinates already returned for that generation would otherwise be
    silently orphaned. The pin is written only after a read succeeds (see
    `_read_file_lines`), and records the fstat metadata of the descriptor
    actually read, not a separate path-based stat that could describe a
    different generation than the bytes returned.
    """
    pinned_before = path_str in _pinned
    try:
        meta, lines = _read_file_lines(path_str)
    except ResolverInputError:
        raise
    except OSError as exc:
        if pinned_before:
            raise ResolverInputError(
                f"{path_str}: became unreadable during this run (previously "
                f"pinned as {_pinned[path_str]}): {exc}") from exc
        return tuple()
    prev = _pinned.get(path_str)
    if prev is not None and prev != meta:
        raise ResolverInputError(
            f"{path_str}: changed on disk during this run (was {prev}, now "
            f"{meta}) -- line coordinates already returned for this file no "
            f"longer describe its contents")
    _pinned[path_str] = meta
    return lines


def cache_clear() -> None:
    """Clear the per-process file caches AND the generation pins. Tests that
    mutate fixtures on disk AND want a CLEAN read of the new generation (not a
    ResolverInputError) must call this between mutations -- that is the
    difference between "picked up" and "detected as drift", and both are
    legitimate depending on what the test is proving. Production callers do
    not need it: each lint invocation spawns a fresh interpreter."""
    _content_cache.clear()
    _file_index.cache_clear()
    _masked_text_for_index.cache_clear()
    _pinned.clear()


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


@lru_cache(maxsize=1024)
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

# The DOCUMENTED marker shape is `/* INTENTIONAL-STUB: <reason> */`, not the
# bare substring "INTENTIONAL-STUB" -- a plain `in` check (Codex adversarial,
# TODO-06 section 12) also matched negated prose (`/* not an
# INTENTIONAL-STUB */`), a bare token with no colon/reason, and even the
# substring appearing inside an unrelated string literal, ALL of which
# silently suppressed a genuine stub-behind-stamp finding. Requires a real
# (non-empty) reason starting right after the colon -- `(?!\*/)` rejects an
# immediately-closing comment (`INTENTIONAL-STUB: */`, no actual reason).
# Deliberately does NOT require the closing `*/` on this same line: the
# reason is free-form prose and commonly wraps onto following lines (both
# real markers in this tree do, e.g. `src/kernel/drivers/hpet.c`) -- only
# the marker's OPENING must be on the body-opener line, matching the
# documented contract exactly.
_INTENTIONAL_STUB_RE = re.compile(r"/\*\s*INTENTIONAL-STUB:\s*(?!\*/)\S")

_WINDOW_LINES = 3  # terminal line + up to 2 preceding type-only lines

# Generous, pathological-input-only bound for follow_declaration's linkage
# lookback (walking back through preceding declaration-specifier lines
# looking for `static`). Unrelated to `_WINDOW_LINES` -- that constant is
# sized for the split-head resolver's OWN 2-preceding-line contract, not for
# how many lines a valid C declaration-specifier sequence can span before a
# self-sufficient single-line head (Codex adversarial: no real declaration
# comes anywhere close to this many lines).
_LINKAGE_LOOKBACK_LIMIT = 32

# CONDITIONAL preprocessor directives only -- #if/#ifdef/#ifndef/#else/#elif/
# #endif may straddle a `static` on a branch this lexical resolver cannot
# evaluate (no preprocessing happens anywhere in this module). #include,
# #define, #pragma, and other non-conditional directives are NOT ambiguous:
# they are an ordinary line ending the preamble, ANY of which is a ordinary
# declaration boundary like any other line of real code.
_COND_DIRECTIVE_RE = re.compile(r"#\s*(if|ifdef|ifndef|else|elif|endif)\b")

# A standalone GCC/Clang `__attribute__((...))` or C23/C++11 `[[...]]` line
# sitting alone between `static` and an otherwise self-sufficient head --
# e.g. `static\n__attribute__((noinline))\nvoid foo(void)`, real, valid GCC
# style. Bounded-depth paren/bracket balance (never unbounded backtracking):
# up to 4 levels of nesting, comfortably past any attribute seen in this
# tree. Part of follow_declaration's linkage lookback "continue" set,
# alongside `_TYPE_ONLY_LINE_RE`.
_STANDALONE_ATTR_RE = re.compile(
    r"^__attribute__\s*\(\(([^()]*|\([^()]*\)){0,4}\)\)\s*$"
    r"|^\[\[([^\[\]]*|\[[^\[\]]*\]){0,4}\]\]\s*$"
)


# ---------------------------------------------------------------------------
# ONE lexical pass per file, shared by candidate discovery and every
# structural walk.
#
# C translation phase 2 deletes each backslash immediately followed by a
# newline, BEFORE phase 3 splits the result into tokens and comments. This
# module cannot splice the text and lex the result, because every consumer
# (Check 7's finding location, the INTENTIONAL-STUB allowlist line) reports
# PHYSICAL line numbers. So splicing is modelled instead: the scanner carries
# lexer state across a spliced newline while still reporting physical
# coordinates.
#
# There is no parity rule. An earlier draft only spliced an "unescaped"
# trailing backslash, reasoning by analogy with string escapes; C has no such
# notion at phase 2, where EVERY backslash-newline is deleted regardless of
# what precedes it. `\\` at end of line splices exactly as `\` does.
#
# The three states carry differently, and the difference is load-bearing:
#   * block comment  -- carries across EVERY physical newline (a /* */ spans
#     lines by definition, splice or not);
#   * string / char  -- carries ONLY across a genuine splice (a literal cannot
#     otherwise span a line, and resetting is the safe direction: a stray quote
#     must not swallow the rest of the file);
#   * line comment   -- carries ONLY across a genuine splice, and it MUST carry
#     there, which is the shape that makes this more than a lexing nicety:
#     `// disabled \` followed by what looks like a definition means that
#     "definition" is commented out, and candidate discovery has to know.
def _scan_line(line: str, in_block: bool, in_str, in_lc: bool,
               start_col: int, emit: bool, pending=None, line_idx: int = -1):
    """Lex ONE physical line from `start_col`. Returns
    (out_block, out_str, out_lc, chars, out_pending, carried).

    `chars` is the list of (col, ch) CODE characters on THIS line when `emit`.
    `pending` / `out_pending` model a half-open two-character comment
    delimiter ('/' awaiting '*' or '/' to open a comment; '*' awaiting '/' to
    close a block comment, while `in_block`) that sat at the very end of a
    PRIOR spliced line, where this scanner could not look past the deleted
    backslash-newline to see the other half. It is a (kind, orig_line_idx,
    orig_col) tuple, ARBITRARY-HOP: it carries forward, unresolved, across
    any run of spliced lines that themselves contribute zero scannable
    characters (a line that is nothing but a lone backslash is itself
    spliced again), and resolves against the first scannable character
    reached -- which may be several lines later. `carried` is an optional
    (orig_line_idx, orig_col, ch) for a PRIOR line's deferred '/' that turned
    out to be ordinary code once this line's first character settled it; the
    caller must yield it BEFORE this line's own `chars` (it belongs to an
    earlier physical position).

    A trailing splice backslash is never emitted: phase 2 deletes it, so it is
    not a code character and must not reach a brace counter or a paren walk.
    """
    n = len(line)
    spliced = line.endswith("\\")
    limit = n - 1 if spliced else n
    chars = []
    carried = None
    j = start_col

    if pending is not None:
        if limit == 0:
            # This line contributes nothing. Carry the pending state forward
            # unresolved only if there is a further splice to look past;
            # otherwise the chain ends here -- a real (unspliced) newline
            # follows, so a deferred '/' was always ordinary code, and a
            # deferred '*' just stays inside the (still-open) block comment.
            if spliced:
                return in_block, in_str, in_lc, chars, pending, None
            kind, oi, oc = pending
            if kind == "slash" and emit:
                carried = (oi, oc, "/")
            return in_block, in_str, in_lc, chars, None, carried
        kind, oi, oc = pending
        first = line[0]
        if kind == "slash":
            if first == "*":
                in_block = True
                j = 1
            elif first == "/":
                in_lc = True
                j = 1
            else:
                if emit:
                    carried = (oi, oc, "/")
                j = 0
        else:  # "star" -- in_block was already True when this was deferred
            if first == "/":
                in_block = False
                j = 1
            else:
                j = 0  # still in_block; this line's own content scans below

    out_pending = None
    while j < limit:
        ch = line[j]
        if in_lc:
            break
        if in_block:
            if ch == "*" and j + 1 < limit and line[j + 1] == "/":
                in_block = False
                j += 2
                continue
            if ch == "*" and j + 1 == limit and spliced:
                out_pending = ("star", line_idx, j)
                j += 1
                break
            j += 1
            continue
        if in_str is not None:
            if ch == "\\":
                if j + 1 >= limit and spliced:
                    # This backslash's escape target is off the end of this
                    # line's scannable content -- it is the SURVIVING
                    # backslash of an even-length run that phase 2's splice
                    # deletion left dangling, and it escapes whatever
                    # character starts the NEXT physical line. Every EARLIER
                    # escape pair on this line was already resolved correctly
                    # by this same per-character loop; only this boundary
                    # case is ambiguous, and only ever exactly here. Fail
                    # closed rather than silently guess.
                    raise _AmbiguousEscapeSplice()
                j += 2
                continue
            if ch == in_str:
                in_str = None
            j += 1
            continue
        if ch == "/" and j + 1 < limit:
            nxt = line[j + 1]
            if nxt == "/":
                in_lc = True
                break
            if nxt == "*":
                in_block = True
                j += 2
                continue
        if ch == "/" and j + 1 == limit and spliced:
            out_pending = ("slash", line_idx, j)
            j += 1
            break
        if ch == '"' or ch == "'":
            in_str = ch
            j += 1
            continue
        if emit:
            chars.append((j, ch))
        j += 1
    if not spliced:
        # Not a continuation: a literal and a line comment both end here.
        in_str = None
        in_lc = False
    return in_block, in_str, in_lc, chars, out_pending, carried


# The non-code constructs, as ONE regex over the whole file. Ordering is the
# correctness argument, exactly as in check_stub_behind_stamp._code_text: a
# string or character literal is tried FIRST at each position, so it consumes
# through its own terminator and a `//` or `/*` sitting inside a literal can
# never start a comment.
#
# Each literal and line-comment alternative is SPLICE-TOLERANT (`\\\n` is
# accepted mid-construct), which is what makes `// disabled \` swallow the
# following physical line. Every alternative is anchored to real terminators,
# so an UNTERMINATED construct falls through to the trailing catch-alls: an
# unterminated block comment runs to EOF (it does in C too), while an
# unterminated literal simply fails to match and the text reads as code --
# the same conservative direction `_scan_line` takes by resetting literal
# state at an unspliced end of line.
#
# THE DELIMITER ITSELF can also straddle a splice (`/\` + newline + `* ... */`
# is a real, if rare, opener) -- `_SPLICE` is spliced ZERO OR MORE times
# between the two characters of `//`, `/*`, and `*/`, matching `_scan_line`'s
# arbitrary-hop pending-delimiter carry exactly (same grammar, two engines).
# It is anchored to the LITERAL two-byte sequence `\` + newline, never a bare
# unspliced newline -- C deletes only that exact sequence at phase 2, so a
# plain `/` at end of line followed by a real line break is NOT an opener.
#
# WHY A REGEX AND NOT THE CHARACTER SCANNER. `_scan_line` is exact and is still
# what every bounded structural walk uses, but running it over every line of
# every file to build this index measured 284ms against a 39ms baseline (7.2x)
# on the live corpus -- a Python character loop per file, which is precisely
# the shape that cost Check 7 a 5.62x regression in section 11 and pass 2 a
# 7.1x one in section 10. `re` runs the same tokenisation in C.
_SPLICE = r"(?:\\\n)*"  # zero or more literal backslash-newline splices

_LEX_SPAN_RE = re.compile(
    r'"(?:\\.|[^"\\\n])*"'                                    # string literal
    r"|'(?:\\.|[^'\\\n])*'"                                   # char literal
    rf"|/{_SPLICE}/(?:\\\n|[^\n])*"                           # line comment
    rf"|/{_SPLICE}\*(?:[^*]|\*(?!{_SPLICE}/))*\*{_SPLICE}/"   # block comment
    rf"|/{_SPLICE}\*.*",                          # unterminated block -> EOF
    re.S)


class _FileIndex:
    """Per-file lexical snapshot: the joined text plus line-start offsets, used
    to find candidate head lines with one C-level scan, and the spans of the
    file that are NOT code.

    Both halves exist for measured reasons. The offsets replace a Python loop
    over every line of every file (`symbol in line`), which is what made an
    unbounded head search expensive: removing the 512-line head cap cost 2.63x
    naively (38ms -> 101ms) and 1.39x with this index. The spans are what make
    candidate discovery agree with C -- without them the head regex matches raw
    physical lines and happily accepts a "definition" that phase-2 splicing
    puts inside a `//` comment.
    """

    __slots__ = ("lines", "text", "offs", "_span_start", "_span_end")

    def __init__(self, lines: tuple):
        self.lines = lines
        self.text = text = "\n".join(lines)
        # Line-start offsets, built with C-level primitives. The obvious
        # `for ln in lines: offs.append(pos); pos += len(ln) + 1` measured 69ms
        # of tottime across 77 files -- a Python loop over every line of every
        # file, the same shape the regex above replaced. offs[i] is the sum of
        # the preceding line LENGTHS plus one separator per preceding line,
        # which `accumulate(map(len, ...))` and `map(add, ..., count())` compute
        # without ever entering the interpreter loop.
        self.offs = list(map(add, accumulate(map(len, lines), initial=0),
                             range(len(lines) + 1)))
        starts, ends = [], []
        for mo in _LEX_SPAN_RE.finditer(text):
            starts.append(mo.start())
            ends.append(mo.end())
        self._span_start = starts
        self._span_end = ends

    def _in_span(self, off: int) -> bool:
        """True when `off` sits strictly INSIDE a non-code span. Spans do not
        overlap (finditer resumes at each match end), so the candidate span is
        the last one starting at or before `off`."""
        i = bisect.bisect_right(self._span_start, off) - 1
        return i >= 0 and off < self._span_end[i]

    def candidate_lines(self, symbol: str):
        """Physical line indices whose CODE could open a head for `symbol`.

        Narrows the SEARCH only -- the head regex still makes every accept /
        reject decision, exactly as the pre-existing pass-2 substring filter
        did. A line is PERMANENTLY excluded (all its occurrences) when the
        PREVIOUS line ends in a backslash: a spliced continuation cannot BEGIN
        a logical line, and the head regex is `^`-anchored -- post-splice that
        position is mid-line, a property of the line itself.

        The comment/literal check is PER-OCCURRENCE, not per-line: checking
        only the line's START offset (an earlier version of this method) wrongly
        excluded the whole line whenever it happened to OPEN inside a comment,
        even when a later occurrence on that same physical line sat in real code
        after the comment closed (`/* foo(legacy) */ foo(void)` -- regression
        caught by the fixture guarding exactly this shape). Each occurrence is
        checked at its OWN position; a comment-only occurrence is skipped
        without deciding the line, so a later real-code occurrence still gets
        a chance.
        """
        text, offs, lines = self.text, self.offs, self.lines
        out = []
        decided = set()
        pos = 0
        find = text.find
        while True:
            k = find(symbol, pos)
            if k < 0:
                break
            pos = k + 1
            i = bisect.bisect_right(offs, k) - 1
            if i in decided:
                continue
            if i > 0 and lines[i - 1].endswith("\\"):
                decided.add(i)  # this line can never begin a logical line
                continue
            if self._in_span(k):
                continue  # this occurrence is comment/literal; keep looking
            decided.add(i)
            out.append(i)
        return out


@lru_cache(maxsize=512)
def _file_index_cached(path_str: str, lines: tuple) -> _FileIndex:
    return _FileIndex(lines)


def _file_index(path_str: str) -> _FileIndex:
    return _file_index_cached(path_str, _load_file_lines(path_str))


# `cache_clear` reaches for `_file_index.cache_clear`; route it to the real one.
_file_index.cache_clear = _file_index_cached.cache_clear


@lru_cache(maxsize=128)
def _masked_text_for_index(index: "_FileIndex") -> str:
    """The WHOLE file's text with every non-code span (comment or string
    literal) blanked to spaces, newlines preserved, length-preserving.

    Built from `index`'s already-computed multi-line-aware span data (the
    same `_LEX_SPAN_RE` walk `candidate_lines` uses), unlike
    `_mask_line_comments` which only sees ONE physical line and therefore
    cannot tell that a bare `*/ #endif` line is the TAIL of a block comment
    opened several lines earlier -- follow_declaration's linkage lookback
    hit exactly that gap (Codex adversarial: `/* explanation\\n */ #endif`
    read as raw text, matched neither the type-only nor the directive
    pattern, and was wrongly treated as a confident non-static boundary).
    Cached per `_FileIndex` (itself cached per file generation), so this
    costs one O(file length) pass, not one per lookback call.
    """
    chars = list(index.text)
    for s, e in zip(index._span_start, index._span_end):
        for k in range(s, e):
            if chars[k] != "\n":
                chars[k] = " "
    return "".join(chars)


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

    SPLICE-AWARE (C translation phase 2). String and line-comment state carry
    across a backslash-newline and reset otherwise; block-comment state carries
    across every newline. The trailing backslash is not yielded, because phase
    2 deletes it -- yielding it would feed a non-character to the paren walk
    and the brace counter. Delegates every rule to `_scan_line` so this walk
    and the per-file state index cannot drift apart, including its
    ARBITRARY-HOP carry of a comment delimiter half ('/' or '*') left
    unresolved at the very end of a spliced line -- `pending` threads that
    state across physical lines here, and a `carried` char (a deferred '/'
    that turned out to be ordinary code) is yielded at its OWN earlier
    physical position, before this line's own characters.
    """
    in_block, in_str, in_lc = False, None, False
    pending = None
    for i in range(start_idx, min(len(lines), end_idx)):
        col = start_col if i == start_idx else 0
        in_block, in_str, in_lc, chars, pending, carried = _scan_line(
            lines[i], in_block, in_str, in_lc, col, True, pending, i)
        if carried is not None:
            yield carried
        for c, ch in chars:
            yield (i, c, ch)
    if pending is not None and pending[0] == "slash":
        # The scan window ended (EOF or end_idx) with an unresolved deferred
        # '/' -- there is no further line to combine with, so it was always
        # ordinary code.
        yield (pending[1], pending[2], "/")


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
    try:
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
    except _AmbiguousEscapeSplice:
        # Fail toward "not a confirmed definition" -- the SAME safe direction
        # every other reject path here takes. Never claim a range built on an
        # ambiguous scan.
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
    full_tuple = _load_file_lines(str(p))
    if not full_tuple:
        return None
    lines = list(full_tuple)

    head_re = _build_def_head_re(symbol)
    # POSITIONALLY UNBOUNDED. The head search used to stop after 512 lines
    # (_DEF_HEAD_LIMIT, removed). `_load_file_lines` already caches the WHOLE
    # file, so the cap bought no memory -- it only bought blindness: `kmalloc`
    # at src/kernel/mm/heap.c:666 and `schedule` at src/kernel/sched/task.c:1721
    # are ordinary single-line heads that it could not see. MEASURED on the live
    # corpus of 186 unique refs: removing it resolves +35, with 0 prior mappings
    # lost and 0 moved (proved by corpus_resolution_snapshot.py, not by the
    # count). The bound that remains is _MAX_FILE_BYTES, which is a bound on the
    # INPUT rather than a positional cap -- a positional cap is silent tail
    # blindness by construction, whereas an oversized file raises.
    index = _file_index(str(p))
    candidates = index.candidate_lines(symbol)

    # ---- PASS 1: single-line head. ---------------------------------------
    # Collect EVERY candidate head; keep walking past prototypes that turn
    # out to be declarations (Codex F2: `static int foo(void);` followed
    # by the real definition is a common C pattern). The head_idx that
    # carries the body opener wins; if none do, fall through to pass 2.
    head_idx = -1
    body_open_idx = -1
    body_open_col = -1
    for cand in candidates:
        m = head_re.match(lines[cand])
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
        for term in candidates:
            if term < 1:
                continue  # a split head needs a preceding type-only line
            # MASKED, not stripped: masking preserves length, so `lead.end()`
            # below is a RAW-line offset that can go straight to
            # _confirm_definition with no search back into the raw text.
            body = _mask_decl_modifiers(_mask_line_comments(lines[term]))
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
                prev = _mask_line_comments(lines[pi]).strip()
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
    try:
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
    except _AmbiguousEscapeSplice:
        pass  # end_idx stays -1 -- same as "no closing brace found"
    if end_idx < 0:
        return None
    return (str(p), head_idx + 1, end_idx + 1)


def follow_declaration(file_path: str, symbol: str,
                        repo_root: str) -> Optional[Tuple[str, int, int]]:
    """When `resolve_symbol(file_path, symbol)` returns None because the only
    head candidate in `file_path` is a forward DECLARATION -- resolve_symbol's
    own "decl" verdict from `_confirm_definition`, e.g. a function prototype
    in a public header -- follow to the real definition, but ONLY through
    this repository's own documented convention: `include/` mirrors `src/`
    (CLAUDE.md "Repository Layout"), so a header's public API is declared FOR
    exactly the implementation file that shares its basename.

    Deliberately NOT a general cross-file symbol search. TODO-06 section 12's
    design review flagged that "a unique name anywhere under repo_root" can
    still bind an UNRELATED same-named definition in a different link product
    (kernel vs. user-program vs. test vs. host-tool) with no way for a bare
    per-file resolver to prove linkage. Restricting to the header's OWN
    basename-unique `.c` sibling sidesteps that entirely: it is not a name
    search, it is the repo's documented public-header-to-implementation
    contract, and the file that shares a header's basename IS the file that
    header exists to declare for -- verified against the live corpus, all 18
    header-only decl-only refs resolve this way, none `static`. `file_path`
    must itself be a `.h`: a `.c` file's own `extern` declaration pointing at
    an unrelated `.c` file (e.g. a test runner's
    `extern void test_register_x(void);`) is exactly the general-search case
    this function does NOT attempt -- measured 2/20 of the corpus's decl-only
    population, left unresolved rather than guessed at.

    Returns None (never guesses) when: `file_path` is not a header; no local
    "decl" candidate exists for `symbol` (nothing to follow -- resolve_symbol
    would already have found a "def" candidate directly); the basename search
    under `repo_root` finds zero or more than one `<basename>.c`; the matched
    definition's head line is `static` (internal linkage cannot satisfy an
    external declaration -- a same-named `static` function in an unrelated
    file is a DIFFERENT function, not this one); or the resolved path would
    escape `repo_root`.
    """
    p = Path(file_path)
    if p.suffix != ".h":
        return None
    full_tuple = _load_file_lines(str(p))
    if not full_tuple:
        return None
    lines = list(full_tuple)
    head_re = _build_def_head_re(symbol)
    index = _file_index(str(p))
    has_decl = False
    for cand in index.candidate_lines(symbol):
        m = head_re.match(lines[cand])
        if not m:
            continue
        kind, _, _ = _confirm_definition(lines, cand, m.end())
        if kind == "def":
            return None  # directly resolvable; caller should not be here
        if kind == "decl":
            has_decl = True
    if not has_decl:
        return None

    root = Path(repo_root).resolve()
    basename = p.stem + ".c"
    matches = [m for m in root.glob(f"src/**/{basename}") if m.is_file()]
    if len(matches) != 1:
        return None  # zero or ambiguous -- never guess
    cand_path = matches[0].resolve()
    try:
        cand_path.relative_to(root)
    except ValueError:
        return None  # defense in depth; a glob under root/"src" cannot
                     # actually escape root, but the guard is cheap and this
                     # function's whole point is to never trust a path blind

    res = resolve_symbol(str(cand_path), symbol)
    if res is None:
        return None
    cand_lines = _load_file_lines(str(cand_path))
    cand_index = _file_index(str(cand_path))  # NOT `index` -- that is `p`'s
    # `static` ANYWHERE before the symbol, not just on `res[1]`'s own line:
    # C's declaration-specifiers have no fixed order (`inline static int
    # foo(void)` is exactly as internal-linkage as `static inline int
    # foo(void)`), AND a real repo idiom puts a bare `static` alone on the
    # line before an otherwise self-sufficient single-line head -- e.g.
    # `static` / `mz_bool foo(...)` sits at src/libs/miniz/miniz.c:4482-4483.
    # `resolve_symbol` resolves that shape via PASS 1 (the symbol's own line
    # is already a complete head), so `res[1]` points at the SYMBOL line, not
    # the `static` line, and a check of only `cand_lines[res[1]-1]` misses it
    # entirely (Codex adversarial). So walk back through preceding
    # type-only-or-blank lines and check all of them, not just the one
    # `res[1]` happens to report.
    #
    # UNBOUNDED by `_WINDOW_LINES` (Codex adversarial, 4th round): that
    # constant is the split-head resolver's OWN limit ("terminal line + up to
    # 2 preceding type-only lines"), sized for how resolve_symbol's pass 2
    # walks a split head -- it has no relationship to how many lines a valid
    # C declaration-specifier sequence can occupy before pass 1's
    # self-sufficient-single-line match, which is unrelated and unbounded
    # (`static\ninline\nconst\nint foo(void)` is valid C). BLANK lines also
    # count as "part of the same declaration" now, not just type-only ones --
    # `_TYPE_ONLY_LINE_RE` requires >= 1 character, so a genuinely empty line
    # between `static` and the symbol previously terminated the walk even
    # though it is not a declaration boundary in C. Capped at
    # `_LINKAGE_LOOKBACK_LIMIT` purely as a pathological-input bound (no real
    # declaration is anywhere near this long), not a correctness limit.
    # CHECK BEFORE APPEND, not after: an earlier draft appended the line
    # unconditionally and only THEN tested whether it was type-only, so the
    # first disqualifying line still got scanned for `static` before the loop
    # broke. Confirmed live false rejection: src/libs/monocypher/monocypher.c
    # line 158 is an unrelated one-line `static u64 x64(...)  { ... }`
    # immediately before line 159's real, external-linkage `crypto_verify16`
    # -- the disqualifying line 158 was appended before its own type-only
    # check failed and broke the loop, so its `static` wrongly rejected line
    # 159 (Codex adversarial). The symbol's OWN line always qualifies; every
    # earlier line must PASS the type-only-or-blank test before being added.
    # FAIL CLOSED on a preprocessor directive during the lookback (Codex
    # adversarial, 5th round on this guard): a REAL, live shape in this
    # corpus is `#if defined(X)` / `static` / `#endif` immediately before a
    # function head (src/libs/mbedtls/library/sha512.c:558-566) -- the
    # `static` is CONDITIONAL on a macro this lexical resolver cannot
    # evaluate (no preprocessing happens anywhere in this module), so "the
    # walk didn't find a literal `static` before hitting `#endif`" is not
    # evidence of external linkage; it is undecided linkage. Treating
    # "undecided" as "external" was the bug -- absence of proof is not proof
    # of absence, and a wrongly-followed internal definition can misclassify
    # a real function's stub-behind-stamp status either direction.
    #
    # MULTI-LINE-COMMENT-AWARE (Codex adversarial, 6th round): `_mask_line_
    # comments` only recognizes a comment opened on the SAME physical line,
    # so `/* explanation\n */ #endif` -- a block comment whose CLOSE shares
    # a line with the directive -- read as raw `*/ #endif` text, which
    # matches neither the type-only pattern nor the directive pattern, and
    # was wrongly treated as a confident boundary (accept). Uses `index`'s
    # already-computed, multi-line-aware span data (the SAME mechanism
    # candidate_lines already relies on) instead of re-deriving comment
    # state per line: `_masked_text_for_index` blanks every span across the
    # WHOLE file once, so a line ending mid-comment reads correctly no
    # matter how many physical lines the comment spans.
    masked_text = _masked_text_for_index(cand_index)
    # DEFAULT FLIPPED (Codex adversarial, 7th round): earlier drafts treated
    # any line NOT matching a known-safe "continue" pattern as a confident
    # boundary (stop, accept) -- correct for the shapes tested, but each new
    # review round found ANOTHER valid C/GCC declaration-specifier shape the
    # "safe" set didn't cover (a standalone `__attribute__((...))` line
    # between `static` and the head is the latest; `static\n__attribute__
    # ((noinline))\nvoid foo(void)` is valid and real GCC style). Enumerating
    # every such shape is whack-a-mole. So the walk now recognizes a SMALL
    # set of CONFIDENT BOUNDARIES (a masked line ending in `;` or `}` -- the
    # unambiguous end of a prior, unrelated statement/block; or a
    # non-conditional preprocessor line, which cannot hide a `static`) and
    # fails closed on anything that matches NEITHER a known-safe "continue"
    # pattern NOR a confident boundary, rather than defaulting to accept.
    check_lines = []
    for i in range(res[1] - 1,
                   max(-1, res[1] - 1 - _LINKAGE_LOOKBACK_LIMIT), -1):
        if i < 0 or i >= len(cand_lines):
            break
        line = cand_lines[i]
        line_start = cand_index.offs[i]
        stripped = masked_text[line_start:line_start + len(line)].strip()
        if i != res[1] - 1 and stripped and not (
                _TYPE_ONLY_LINE_RE.match(stripped)
                or _STANDALONE_ATTR_RE.match(stripped)):
            if _COND_DIRECTIVE_RE.match(stripped):
                # A CONDITIONAL directive (#if/#ifdef/#ifndef/#else/#elif/
                # #endif) may be hiding a `static` on an untaken branch --
                # undecidable without a preprocessor.
                return None  # preprocessor-conditional linkage: undecidable
            if stripped.startswith("#") or stripped.endswith((";", "}")):
                # An ordinary #include/#define/#pragma cannot hide a
                # `static`, and a line ending `;`/`}` is the unambiguous end
                # of a PRIOR, unrelated statement or block (the monocypher
                # false-rejection fixture: an unrelated one-line `static`
                # function ending in `}` immediately before the real symbol
                # -- this is exactly the confident-boundary case, not a
                # "continue" case). Both are genuine boundaries.
                break  # this line is not part of the same declaration
            # UNRECOGNIZED shape: neither a known-safe continuation nor a
            # confident boundary. Fail closed rather than guess -- this is
            # the fix for the NEXT declaration-specifier shape nobody has
            # found yet, not just the ones already reported.
            return None
        check_lines.append(i)
    # MASKED first (whole-file, multi-line-aware): a trailing
    # `// static in the old design`-style comment, or one spanning multiple
    # lines, must not trigger a false rejection of a real external-linkage
    # function.
    for i in check_lines:
        line_start = cand_index.offs[i]
        masked_line = masked_text[line_start:line_start + len(cand_lines[i])]
        if re.search(r"\bstatic\b", masked_line):
            return None  # internal linkage cannot satisfy an external decl
    return res


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
    try:
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
    except _AmbiguousEscapeSplice:
        pass  # body_open_line_idx stays None -- same as "opener not found"
    if body_open_line_idx is None:
        return None
    # Allowlist scan: same line as the body opener. Honors only the
    # DOCUMENTED marker shape, not a bare substring match -- and the marker
    # must be a REAL, lexically-independent comment span, not marker-shaped
    # TEXT absorbed inside a `//` line comment or a string/attribute literal
    # (Codex adversarial, TODO-06 section 12: `// example: /* INTENTIONAL-
    # STUB: not an allowlist */` matched the raw-text regex and silently
    # suppressed a real finding). `_FileIndex`'s span scan (`_LEX_SPAN_RE`)
    # already resolves this correctly by construction: a `//` comment or a
    # string literal is matched as ONE span starting at its own opener, so
    # an embedded `/*...*/`-shaped substring is absorbed inside that larger
    # span and never itself starts an independent span -- checking that the
    # marker's `/*` coincides EXACTLY with a recorded span start proves it
    # opens its own real comment, not text living inside something else's.
    # Checks EVERY match, not just the first (Codex adversarial): a
    # marker-shaped FAKE occurrence earlier on the line (e.g. inside a
    # string literal) that fails the lexical-independence check must not
    # short-circuit past a genuinely real, independent marker later on the
    # SAME line -- `search()` alone stops at the first match regardless of
    # its validity.
    findex = _file_index(str(file_path))
    abs_open_idx = (line_start - 1) + body_open_line_idx
    opener_line_off = findex.offs[abs_open_idx]
    for marker_match in _INTENTIONAL_STUB_RE.finditer(
            body_text_lines[body_open_line_idx]):
        marker_start_abs = opener_line_off + marker_match.start()
        span_idx = bisect.bisect_left(findex._span_start, marker_start_abs)
        if (span_idx < len(findex._span_start)
                and findex._span_start[span_idx] == marker_start_abs):
            return None

    # Find the matching CLOSE brace's exact (line, col) via the same
    # depth-aware scanner the rest of this module uses (mirrors
    # resolve_symbol's own body-close walk), rather than assuming its
    # position from slice emptiness. An earlier version treated
    # `body_text_lines[body_open_line_idx+1:-1]` being empty as "opener and
    # closer share ONE physical line" and searched for `}` only on the
    # OPENER's line -- true for a real one-liner, but ALSO true for a
    # genuine two-line body (`int f(void) {` / `    return 0; }`), where the
    # closer is on a DIFFERENT line the code never looked at. That body
    # returned None (missed stub) instead of being classified (Codex
    # adversarial). Depth-walking from the opener finds the true closer
    # regardless of how many physical lines it spans.
    depth = 0
    close_line_idx = None
    close_col = None
    try:
        for i, col, ch in _iter_code_chars(
                body_text_lines, body_open_line_idx, body_open_col,
                len(body_text_lines)):
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    close_line_idx, close_col = i, col
                    break
    except _AmbiguousEscapeSplice:
        pass
    if close_line_idx is None:
        return None  # could not confidently find the matching close brace

    # Extract the masked text strictly between opener (exclusive) and closer
    # (exclusive), across however many physical lines that spans -- content
    # sharing EITHER boundary line is included, not discarded.
    # MULTI-LINE-COMMENT-AWARE (Codex adversarial, TODO-06 section 12):
    # `_strip_line_comments` only sees ONE physical line, so a body
    # containing `/* explanation\ncontinues */\nreturn 0;` left both comment
    # fragments in `real_lines` and the joined text never matched the
    # return-constant pattern. Reuses the whole-file, multi-line-aware span
    # index `follow_declaration`'s linkage lookback already relies on
    # (`_masked_text_for_index`) instead of a second, narrower stripper, so
    # both consumers see the SAME comment boundaries.
    masked_whole = _masked_text_for_index(findex)
    abs_close_idx = (line_start - 1) + close_line_idx
    inner_start = findex.offs[abs_open_idx] + body_open_col + 1
    inner_end = findex.offs[abs_close_idx] + close_col
    inner_masked = masked_whole[inner_start:inner_end]
    real_lines = [ln.strip() for ln in inner_masked.split("\n")]
    real_lines = [ln for ln in real_lines if ln and ln not in ("{", "}")]
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
