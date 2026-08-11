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
import os
import subprocess
import sys
from pathlib import Path

__all__ = [
    "fence_step", "fence_scan", "fence_mask", "scan_text", "unclosed_reason",
    "terminal_category", "mask_text", "unmasked", "replace_atomically",
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


def fence_step(state, line: str):
    """One line of fenced-block state -- see `cache_schema.fence_step`.

    THE LEAF RULE, and it expects a line whose enclosing container prefixes are
    already stripped. `validate.py` is the one caller that steps line by line
    rather than taking a whole-document mask, and it drives the container phase
    itself for exactly that reason.

    Its other caller was `todo-reflow.py`, which ran it over indent-stripped
    lines to cover the container-indented fences the mask could not see. Section
    39 made `fence_scan` container-aware and that fallback was retired: an
    indent-stripped step is a heuristic superset that cannot tell a container
    fence from a root indented code block, so it is the wrong tool now that the
    shared scan answers the question properly. A caller reaching for this to
    hand-roll container handling is re-opening the closed limit.
    """
    return _cache_schema().fence_step(state, line)


def fence_scan(lines):
    """A `ScanResult` -- see `cache_schema.fence_scan`.

    NOT A TUPLE since section 42. A gate takes `.mask` and calls
    `.require_closed()` (or reads `.terminal`); the old positional unpack now
    raises `TypeError` rather than silently reading the terminal object as a
    boolean flag.
    """
    return _cache_schema().fence_scan(list(lines))


def fence_mask(lines):
    """`mask` only. Use `fence_scan` in a GATE; see the module docstring."""
    return fence_scan(lines).mask


def scan_text(text: str):
    """A `ScanResult` for a whole document, `lines` included.

    THE SHARED NORMALISER, same as the producer and the validator. This split
    on `"\\n"` alone so a line number indexed the caller's own
    `read_text().split("\\n")`, and once `cache_schema.scan_text` began folding
    every `splitlines()` break the two disagreed about the same document: a
    valid CRLF file with a CLOSED fence reported `unclosed_fence=True` through
    this shim while the producer reported False, so the gates would refuse a
    file the builder accepts (Codex adversarial, section 39 round 3, [medium]).
    That is exactly the producer/gate split this module exists to end, so the
    shim adopts the contract rather than keeping its own.

    The coordinate promise still holds where it matters: normalisation only
    REPLACES a separator with `\\n`, never inserts or removes one, so the line
    COUNT and every line INDEX are unchanged.
    """
    return _cache_schema().scan_text(text)


def unclosed_reason(terminal):
    """One wording for all four gates -- see `cache_schema.unclosed_reason`.

    Takes the `Terminal` value (or None) since section 42, not a pair of
    booleans: with seven HTML block types plus fences there is no fixed set of
    flags to pass, and the message names the construct AND its opening line.

    The sentence MOVED to `cache_schema` in section 39 rather than being copied
    there: `validate.py` sits beside that module and cannot import this shim
    without a `sys.path` insertion, and a second copy of the wording is the same
    drift this module exists to end. This stays because the gates reach the
    tracker through here and should not each learn where it really lives.
    """
    return _cache_schema().unclosed_reason(terminal)


def terminal_category(terminal) -> str:
    """The producer error category for a `Terminal` -- see `cache_schema`.

    Shared so a gate reporting an unreadable document labels it exactly as
    `build.py` does; a local two-flag ternary would call an unclosed `<script>`
    a comment.
    """
    return _cache_schema().terminal_category(terminal)


def unmasked(lines, mask):
    """`(index, line)` for the lines that are ordinary Markdown structure.

    The index is into the ORIGINAL list, so a caller can still report a line
    number or slice the untouched document.
    """
    return ((i, l) for i, l in enumerate(lines) if not mask[i])


def replace_atomically(path, text: str, expect: str) -> None:
    """Rewrite `path` with `text`, or raise having changed nothing.

    THE OBVIOUS SPELLING DESTROYS DATA. `open(path, "w")` TRUNCATES before the
    new bytes are committed, so a full disk, an interruption, or a killed
    process leaves a TODO empty or half-written -- and both repair tools that
    call this run unattended, where a truncated roadmap is not noticed until
    something else refuses to parse it (Codex adversarial, section 41
    post-commit, [high]). Writing a sibling temp file, flushing it to disk and
    then `os.replace`-ing means the file is either the old bytes or the new
    ones, never neither.

    `expect` closes the read-modify-write race the same way: these tools read a
    document, compute a rewrite, and only then write. An edit landing in that
    window would be silently overwritten by a rewrite of the version we read,
    so the bytes on disk are re-checked immediately before the swap. It is not
    a lock -- a writer between this check and the replace still wins -- but it
    turns the common case (an editor save, an operator repair, a concurrent
    hook) from silent loss into a refusal the caller reports.

    The directory fsync is what makes the RENAME durable rather than merely the
    file contents; without it a crash can leave the directory entry pointing at
    the old inode with the new data already on disk.
    """
    path = Path(path)
    current = path.read_text(encoding="utf-8")
    if current != expect:
        raise OSError(f"{path}: changed on disk since it was read; not rewritten")
    tmp = path.with_name(f".{path.name}.todo-tmp-{os.getpid()}")
    try:
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise
    dir_fd = os.open(path.parent or ".", os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)


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

    Same length LINE FOR LINE, so a whole-text regex caller keeps its
    coordinates within the returned string and can still slice it.

    THE COORDINATE BASE IS THE RETURNED TEXT, not the argument, and saying so
    is the honest version of the older promise. `scan_text` now applies the
    shared newline normaliser, which strips a BOM and folds CRLF -- both change
    the byte length -- so an offset here indexes the NORMALISED document. Every
    caller works entirely within this result (`lint.sh:991` regexes the
    returned string and never mixes it with the raw bytes), which is the usage
    the guarantee is written for; a caller needing raw-byte offsets must
    normalise first and use that as its own base (Codex adversarial, section 39
    round 3, [medium]).

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
    r = scan_text(text)
    return "\n".join(" " * len(l) if r.mask[i] else l
                     for i, l in enumerate(r.lines))
