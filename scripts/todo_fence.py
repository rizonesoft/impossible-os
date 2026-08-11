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
import subprocess
import sys
from pathlib import Path

__all__ = [
    "fence_scan", "fence_mask", "scan_text", "unclosed_reason",
    "mask_text", "unmasked",
    "StagedSnapshotError", "index_tree", "staged_docs",
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


class StagedSnapshotError(RuntimeError):
    """The staged index could not be read as one coherent generation.

    An EXCEPTION rather than a None return, because every caller here is a GATE:
    silently degrading to the working tree is the exact failure this helper
    exists to remove, and a return value invites exactly that.
    """


def _git(root, *args, **kw):
    return subprocess.run(["git", "-C", str(root)] + list(args),
                          capture_output=True, check=False, **kw)


def index_tree(root) -> str:
    """The tree id of the CURRENT index -- a generation fingerprint.

    `git write-tree` also REFUSES an index with unmerged paths, so a conflicted
    tree is rejected here rather than each caller inventing its own check.
    """
    r = _git(root, "write-tree")
    if r.returncode != 0:
        raise StagedSnapshotError(
            "cannot pin the index (unmerged paths, or git refused write-tree): "
            + r.stderr.decode("utf-8", "replace").strip()[:200])
    return r.stdout.decode("ascii").strip()


# Git object modes that are a readable text document. A symlink (120000) is
# ALSO reported as a blob by `cat-file`, but its content is the link TARGET
# PATHNAME -- scanning that as Markdown finds no headings, no rows and no
# stamps, so a symlinked TODO would sail through a gate that a worktree
# `read_text()` (which follows the link) would have judged on the real file.
# A gitlink (160000) refuses only incidentally, because its object type is
# `commit` rather than `blob`. Both are named here instead (Codex adversarial,
# section 38 review, [high]).
_TEXT_MODES = (b"100644", b"100755")


def staged_docs(root):
    """`(docs, tree_id)` for every `todo/**.md` IN THE INDEX.

    `docs` maps a repo-relative path to its staged text. Raises
    `StagedSnapshotError` on anything that would make the answer partial or
    ambiguous -- an unmerged index, a non-regular entry, malformed `cat-file`
    framing, an undecodable blob, or an index that MOVED while being read.

    ONE SNAPSHOT, SHARED BY BOTH STAGED GATES. `lint.sh` Checks 10/11 and
    `todo-staged-check.py` were each enumerating the index their own way and
    disagreed about three real cases: git C-QUOTES a non-ASCII or newline path
    in `--name-only` output so one gate never saw such a file at all, an
    unmerged entry fell back to worktree bytes in one and refused in the other,
    and an undecodable blob raised in one and was skipped in the other. Two
    mechanisms for one invariant is the drift this module exists to end
    (Codex consistency, section 38 review, [medium]).

    GENERATION-BOUND. The tree id is taken before AND after the read and must
    match, so a `git add` landing mid-read cannot hand a caller a mask from one
    generation and line numbers from another. Callers that derive anything
    ELSE from the index (a diff, say) re-check `index_tree()` against the
    returned id afterwards.
    """
    tree_before = index_tree(root)
    ls = _git(root, "ls-files", "-z", "-s", "--", "todo")
    if ls.returncode != 0:
        raise StagedSnapshotError("git ls-files failed on todo/")
    entries = []
    for rec in ls.stdout.split(b"\0"):
        if not rec:
            continue
        meta, sep, path = rec.partition(b"\t")
        parts = meta.split()
        if not sep or len(parts) != 3:
            raise StagedSnapshotError("unparseable ls-files record")
        mode, sha, stage = parts
        if stage != b"0":
            raise StagedSnapshotError(
                "unmerged index entry: " + path.decode("utf-8", "replace"))
        if not path.endswith(b".md"):
            continue
        if mode not in _TEXT_MODES:
            raise StagedSnapshotError(
                "non-regular index entry (mode %s): %s -- a symlink or gitlink "
                "is not a document this gate can read"
                % (mode.decode(), path.decode("utf-8", "replace")))
        entries.append((sha.decode("ascii"), path))
    docs = {}
    if entries:
        batch = _git(root, "cat-file", "--batch",
                     input=b"\n".join(s.encode() for s, _ in entries) + b"\n")
        if batch.returncode != 0:
            raise StagedSnapshotError("git cat-file --batch failed")
        out, pos = batch.stdout, 0
        # STRICT FRAMING. Every field is checked because none of them is free:
        # an unchecked size desynchronises the walk so one file's content is
        # attributed to another path, and an unchecked response id means a
        # `missing`/`ambiguous` reply is silently consumed as the next blob
        # (Codex adversarial, section 38 review, [medium]).
        for want_sha, path in entries:
            nl = out.find(b"\n", pos)
            if nl < 0:
                raise StagedSnapshotError("truncated cat-file response")
            hdr = out[pos:nl].split()
            if len(hdr) != 3 or hdr[1] != b"blob":
                raise StagedSnapshotError(
                    "unexpected cat-file response: "
                    + out[pos:nl].decode("utf-8", "replace")[:120])
            if hdr[0].decode("ascii") != want_sha:
                raise StagedSnapshotError("cat-file returned a different object")
            try:
                size = int(hdr[2])
            except ValueError:
                raise StagedSnapshotError("non-numeric cat-file size")
            body_start = nl + 1
            if size < 0 or body_start + size + 1 > len(out):
                raise StagedSnapshotError("cat-file size out of range")
            body = out[body_start:body_start + size]
            if out[body_start + size:body_start + size + 1] != b"\n":
                raise StagedSnapshotError("missing cat-file record separator")
            pos = body_start + size + 1
            try:
                docs[path.decode("utf-8")] = body.decode("utf-8")
            except UnicodeDecodeError:
                raise StagedSnapshotError(
                    "undecodable staged blob: "
                    + path.decode("utf-8", "replace"))
        if pos != len(out):
            raise StagedSnapshotError("trailing bytes after the last cat-file record")
    tree_after = index_tree(root)
    if tree_after != tree_before:
        raise StagedSnapshotError(
            "the index changed while it was being read (a concurrent `git add`) "
            "-- refusing rather than mixing two generations")
    return docs, tree_before


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
