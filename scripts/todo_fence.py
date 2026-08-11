#!/usr/bin/env python3
"""One fence tracker for every TODO GATE parser, wrapping the section-36 primitive.

WHY A SHIM AND NOT FOUR IMPORTS. `cache_schema.fence_scan` lives under
`scripts/todo-graph/`, whose directory name is not a legal Python identifier, so
every caller outside that directory has to load it by path. Four copies of an
`importlib.util.spec_from_file_location` dance is four places to get the path
wrong and four places a later move has to be repeated -- and the whole point of
section 36 was ONE tracker rather than one per parser. The import lives here.

WHAT THIS ADDS OVER RE-EXPORTING `fence_mask`. A mask alone is NOT enough for a
gate. `cache_schema.fence_mask` is fail-closed about emitting wrong structure --
an unclosed fence masks its opener through EOF -- but for a parser whose output
REFUSES a commit or a fixpoint that same rule is fail-OPEN: every later item,
heading, row and stamp disappears, so the gate goes quiet on exactly the
malformed document it should be loudest about. `build.py:1030` already consumes
`fence_scan` and REFUSES rather than publishing the erasure; a gate that took
only the mask would silently disagree with the producer about the same file.
So the terminal flags are part of this module's surface, `unclosed_reason()`
gives all four gates one wording, and each of them turns a truthy reason into a
visible refusal (Codex design review, section 38, [high]).

Measured 2026-08-11 across all 281 files under `todo/`: ZERO carry an unclosed
fence and ZERO carry an unclosed HTML comment, so the refusal costs nothing
today. That is the argument for adding it now rather than when it first fires.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

__all__ = [
    "fence_scan", "fence_mask", "scan_text", "unclosed_reason",
    "mask_text", "unmasked",
]

_CS = None


def _cache_schema():
    """The section-36 tracker, loaded by path and memoised.

    Loaded by path rather than by `sys.path` insertion on purpose: prepending
    `scripts/todo-graph/` to the global path makes every later plain-name import
    in the calling process resolve against that directory too, and these four
    callers are pre-commit gates running inside other tools' processes.

    AN ALREADY-IMPORTED COPY WINS. `todo-reachability.py:49` puts
    `scripts/todo-graph/` on `sys.path` and imports `cache_schema` for the
    shared cache validator, so a by-path load here would give that process a
    SECOND module object for the same source -- two sets of compiled regexes
    and two of every module-level constant, differing by identity even when
    they agree by value. Reusing the live one keeps `is` comparisons and
    `isinstance` checks against its types meaningful.
    """
    global _CS
    if _CS is None:
        _CS = sys.modules.get("cache_schema")
    if _CS is None:
        src = Path(__file__).resolve().parent / "todo-graph" / "cache_schema.py"
        spec = importlib.util.spec_from_file_location("_todo_cache_schema", src)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        _CS = mod
    return _CS


def fence_scan(lines):
    """`(mask, unclosed_fence, unclosed_comment)` -- see `cache_schema.fence_scan`."""
    return _cache_schema().fence_scan(list(lines))


def fence_mask(lines):
    """`mask` only. Use `fence_scan` in a GATE; see the module docstring."""
    return fence_scan(lines)[0]


def scan_text(text: str):
    """`(lines, mask, unclosed_fence, unclosed_comment)` for a whole document.

    Splits on `"\\n"` rather than `splitlines()` to match what every caller here
    already does, so a line NUMBER derived from this list indexes the same line
    the caller's own `read_text().split("\\n")` would give. `splitlines()` also
    breaks on lone CR, `\\x0b`, `\\x0c` and U+2028, which would shift every later
    index relative to the caller's coordinates. (That producer-side divergence
    is section 39's to reconcile; this module must not introduce a second one.)
    """
    lines = text.split("\n")
    mask, unclosed_fence, unclosed_comment = fence_scan(lines)
    return lines, mask, unclosed_fence, unclosed_comment


def unclosed_reason(unclosed_fence: bool, unclosed_comment: bool):
    """One wording for all four gates, or None when the document is well-formed.

    Named separately per construct because the message has to tell an author
    WHICH delimiter to close -- naming the wrong one sends them to the wrong
    line, which is the same reason `fence_scan` reports the two flags apart.
    """
    if unclosed_fence:
        return ("document ends inside an unclosed fenced code block, so every "
                "structural walk past the opener reads as empty; close the "
                "fence (a fence nested inside another must use a LONGER run "
                "than the block containing it)")
    if unclosed_comment:
        return ("document ends inside an unclosed `<!--` HTML comment, so "
                "every structural walk past the opener reads as empty; close "
                "it with `-->`")
    return None


def unmasked(lines, mask):
    """`(index, line)` for the lines that are ordinary Markdown structure.

    The index is into the ORIGINAL list, so a caller can still report a line
    number or slice the untouched document.
    """
    return ((i, l) for i, l in enumerate(lines) if not mask[i])


def mask_text(text: str) -> str:
    """`text` with every fenced/commented line blanked to same-length spaces.

    Same length so every character offset in the result is the offset of the
    same character in the original -- a whole-text regex caller keeps its
    coordinates and can still slice the untouched document.

    ONLY SAFE WITH LINE-LOCAL PATTERNS, and that is not a style preference.
    `\\s` and a negated class like `[^|]` both match a newline, so blanking the
    lines between two real ones lets a pattern spanning `\\s+` JOIN them.
    Reproduced 2026-08-11: a bare `##` line, a fenced block, then a `12. Title`
    line matches `^##\\s+12\\.\\s+(.+?)$` after masking though it matches nothing
    before, fabricating a section the file does not have; `| a | b` followed by
    a real row likewise satisfies an `[^|]*`-based Implementation Order pattern
    across the newline. Callers must use `[ \\t]` and `[^|\\n]`
    (Codex design review, section 38, [medium]). `scripts/tests/test_todo_fence.py`
    pins both the fabrication and its line-local suppression.
    """
    lines, mask, _, _ = scan_text(text)
    return "\n".join(" " * len(l) if mask[i] else l
                     for i, l in enumerate(lines))
