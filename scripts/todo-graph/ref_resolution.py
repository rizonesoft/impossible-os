#!/usr/bin/env python3
# ============================================================================
# ref_resolution.py -- the SINGLE effective-path rule for a stamped-item
# symbol ref.
#
# Owner: todo-metadata-layer roadmap, stored-ref-repair section. Imported by
# BOTH consumers of the cache's `stamped_items[].refs`:
#
#   scripts/lint/check_stub_behind_stamp.py  (_walk -- lint Check 7)
#   scripts/todo-graph/corpus_resolution_snapshot.py (collect -- identity gate)
#
# Why one module rather than two matching walks. The snapshot exists to PROVE
# that a resolver change added mappings without losing or moving any, so a rule
# the snapshot does not share is a rule the snapshot cannot gate. That defect
# already happened once in this roadmap: the first draft of decl-following
# taught only the lint walk, and its 0-lost/0-moved proof silently covered 36
# of 54 added mappings. Codex design review raised the same shape again for the
# repairs below (a separately-stored resolved path would have been invisible to
# `collect()`), so the rule now lives in exactly one place and both callers
# consume its verdict rather than re-deriving it.
#
# WHY RESOLUTION TIME AND NOT EXTRACTION. Both repairs below are functions of
# the SOURCE TREE (which basenames are unique, which of a section's files
# defines a symbol), while `build.py`'s extraction is a pure function of
# (TODO body, TODO path) and the cache's freshness check compares the cache
# mtime against `todo/**/*.md` ONLY (check_stub_behind_stamp._check_staleness).
# Storing a tree-derived path in the cache would therefore create a claim that
# goes stale on a source edit with nothing able to notice: add a second file
# with the same basename, move the selected target, or move a definition out of
# a section's file, and the stored path keeps looking valid while pointing at
# the wrong body. Resolving here re-reads the tree on every walk, so the
# verdict cannot outlive its evidence, and the cache keeps saying exactly what
# the TODO author wrote.
#
# WHAT IS DELIBERATELY *NOT* DONE HERE: a general tree-wide unique-name search
# for an unpaired symbol. 673 of the corpus's 736 unique unpaired symbols do
# have exactly one C definition tree-wide, and binding them would resolve 1,028
# occurrences -- but this roadmap already rejected that mechanism once, with
# reason: a bare unique-name search can bind an unrelated same-named definition
# in a different link product with no way to prove linkage, and the existing
# narrow use of tree-wide basename uniqueness is an explicitly accepted risk
# that was paid for by hand-verifying all 18 of its mappings. 673 is 37x that,
# unverifiable at this section's scale. Section scope below is the authored
# evidence a tree-wide search lacks.
#
# Pure stdlib. No subprocess. Runs inside the lint's single Python process.
# ============================================================================

import os
import re
from collections import namedtuple
from pathlib import Path
from typing import Optional

import resolve_symbol as _rs

# Suffixes the resolver can answer about at all. Mirrors the caller-side check
# it replaces; a ref naming a .md/.py/.sh file is not a resolver failure.
_C_SUFFIXES = (".c", ".h")

# Directories never searched when indexing basenames. `build/` in particular
# holds generated copies whose basenames would manufacture ambiguity against
# their own sources.
_SKIP_DIRS = {".git", "build", "__pycache__", "node_modules", ".venv"}

# A verdict about ONE symbol ref.
#   abs_path    absolute path to check against, or None
#   rel_path    repo-relative form of abs_path, or None
#   provenance  how abs_path was arrived at: "authored" | "basename" | "section"
#   bucket      None when resolved to a path; otherwise the coverage bucket the
#               caller must count it in. Names match the existing cov keys
#               exactly so the reporting contract is unchanged.
Verdict = namedtuple("Verdict", "abs_path rel_path provenance bucket")

_BASENAME_INDEX = {}  # repo_root(str) -> {basename: (rel, ...)} sorted
_RESOLVE_MEMO = {}    # (abs_path, symbol) -> resolve_symbol() result
_EFFECTIVE_PATH_MEMO = {}  # (rel, repo_root) -> (abs_path|None, provenance|bucket)
_MENTION_INDEX = {}   # file line-tuple -> frozenset of identifiers in it


def clear_caches() -> None:
    """Drop the per-process basename index and resolution memo. Tests that
    mutate a fixture tree between walks must call this; a long-lived process
    would otherwise answer from state built before the mutation."""
    _BASENAME_INDEX.clear()
    _RESOLVE_MEMO.clear()
    _EFFECTIVE_PATH_MEMO.clear()
    _MENTION_INDEX.clear()
    # ONE reset, not two. This module memoizes resolver ANSWERS while
    # resolve_symbol memoizes the file content, lexical index and generation
    # pins they were derived from, so clearing either alone leaves the other
    # able to serve a pre-mutation verdict -- and `resolve_symbol.cache_clear`
    # cannot reach in here (Codex adversarial, round 2).
    _rs.cache_clear()


def _clear_local_only() -> None:
    """The half of `clear_caches` that does NOT call back into resolve_symbol.

    Registered as a resolve_symbol reset dependent so the documented
    `resolve_symbol.cache_clear()` entry point invalidates this module's
    derived answers too. Without it the reset was one-way: clearing the lower
    level left `_RESOLVE_MEMO` able to serve a pre-mutation verdict without
    ever re-entering the freshly reset resolver (Codex adversarial, round 3).
    Calling `clear_caches` here instead would recurse."""
    _BASENAME_INDEX.clear()
    _RESOLVE_MEMO.clear()
    _EFFECTIVE_PATH_MEMO.clear()
    _MENTION_INDEX.clear()


_rs.register_reset_dependent(_clear_local_only)


def _basename_index(repo_root: Path) -> dict:
    """{basename: (repo-relative path, ...)} over every .c/.h file in the tree.

    Sorted, so an ambiguous basename reports the same candidates in the same
    order on every run -- the ambiguity is REPORTED, never broken by picking
    the first hit."""
    key = str(repo_root)
    hit = _BASENAME_INDEX.get(key)
    if hit is not None:
        return hit
    acc = {}
    for dirpath, dirnames, filenames in os.walk(str(repo_root)):
        dirnames[:] = [d for d in dirnames if d not in _SKIP_DIRS]
        for name in filenames:
            if not name.endswith(_C_SUFFIXES):
                continue
            rel = os.path.relpath(os.path.join(dirpath, name), str(repo_root))
            acc.setdefault(name, []).append(rel.replace(os.sep, "/"))
    idx = {k: tuple(sorted(v)) for k, v in acc.items()}
    _BASENAME_INDEX[key] = idx
    return idx


def _contained(rel: str, repo_root: Path) -> Optional[Path]:
    """Absolute path for a repo-relative ref, or None when it escapes the repo
    (symlink included -- `.resolve()` runs before the containment test)."""
    try:
        abs_p = (repo_root / rel).resolve()
        abs_p.relative_to(repo_root)
    except (ValueError, OSError):
        return None
    return abs_p


def _effective_path(rel: str, repo_root: Path) -> tuple:
    """Memoized `_effective_path_uncached`.

    The corpus asks the same question about the same stored path many times
    (a section's candidate list is rebuilt per section, and popular files are
    named by dozens of items). The answer is a pure function of the path and
    the tree, and the underlying `Path.resolve()`/`relative_to()` pair is the
    single most-called thing in the walk -- 5,146 calls and ~1.1s of a 3.7s
    profile before this memo."""
    key = (rel, str(repo_root))
    hit = _EFFECTIVE_PATH_MEMO.get(key)
    if hit is None:
        hit = _effective_path_uncached(rel, repo_root)
        _EFFECTIVE_PATH_MEMO[key] = hit
    return hit


def _effective_path_uncached(rel: str, repo_root: Path) -> tuple:
    """(abs_path, provenance) for a stored repo-relative path, or
    (None, bucket) when it cannot be resolved to a real file.

    THE ONE PLACE the basename repair lives, so a `kind=file` ref and a
    `kind=symbol` ref's stored path are treated identically. They were not at
    first: only symbol refs were repaired, which silently starved the
    section-scope pairing below of candidates -- MEASURED 2026-08-06, 673 of
    the corpus's 1,167 stamped .c/.h FILE refs do not exist at their authored
    path and 629 of those are bare and tree-unique, so a section whose file
    refs are all bare filenames contributed no candidates at all (Codex
    adversarial)."""
    if rel.startswith("/"):
        return (None, "path_escape")
    abs_p = _contained(rel, repo_root)
    if abs_p is None:
        return (None, "path_escape")
    if abs_p.suffix not in _C_SUFFIXES:
        return (None, "unsupported_lang")
    if abs_p.is_file():
        return (abs_p, "authored")
    if "/" in rel or "\\" in rel:
        return (None, "missing_file")
    cands = _basename_index(repo_root).get(rel, ())
    if len(cands) != 1:
        return (None, "missing_file")
    abs_p = _contained(cands[0], repo_root)
    if abs_p is None or not abs_p.is_file():
        return (None, "missing_file")
    return (abs_p, "basename")


def section_candidate_files(items: list, repo_root: Path) -> tuple:
    """Every .c/.h file a SECTION's `[x]` items name, as EFFECTIVE paths.

    This is the authored evidence behind the section-scope pairing below: the
    files are ones a human wrote into this section's own stamped items, not
    names matched out of the tree. Each goes through `_effective_path`, so a
    bare filename counts exactly as its full path does. Deduped by the
    CANONICAL path -- two items naming `foo.c` and `src/foo.c` are one
    candidate, and a section that names the same file twice must not look
    ambiguous."""
    out = []
    seen = set()
    for it in items or []:
        for ref in it.get("refs") or []:
            if ref.get("kind") != "file":
                continue
            rel = ref.get("file")
            if not rel:
                continue
            abs_p, _prov = _effective_path(rel, repo_root)
            if abs_p is None or str(abs_p) in seen:
                continue
            seen.add(str(abs_p))
            try:
                out.append(str(abs_p.relative_to(repo_root)).replace("\\", "/"))
            except ValueError:
                continue
    return tuple(out)


_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _mentions(abs_path: str, symbol: str) -> bool:
    """Does `symbol` appear as an identifier anywhere in this file's RAW text?

    Deliberately raw, not masked. Raw text is a strict SUPERSET of code text,
    so a raw miss is a definitive "not defined here" while a raw hit only
    means "ask the resolver" -- the pre-filter can never change a verdict, and
    a masked pre-filter would pay a whole-file lexical index plus a masking
    pass (measured 0.27s over 281 files) just to reject files the raw test
    rejects for free.

    INDEXED PER FILE, not scanned per (file, symbol). A per-symbol scan keeps
    the pairing's cost proportional to file SIZE -- the U x F x file-size shape
    Codex flagged in round 2, unbounded in both U (a section's unpaired
    symbols) and F (its authored files). Tokenizing each candidate file ONCE
    into an identifier set makes every later question O(1), and the set is
    reused across every section that names the file. Reads through
    resolve_symbol's own cached line tuple, so no file is read twice."""
    # `_load_file_lines` is called on EVERY probe on purpose: it is where the
    # resolver re-stats the file and raises ResolverInputError on a same-run
    # rewrite. Caching the identifier set against `abs_path` alone skipped that
    # check, so a file that changed mid-run and GAINED the symbol returned a
    # stale miss and swallowed the rc 9 drift failure the resolver promises
    # (Codex adversarial, round 3). Keying on the returned line tuple keeps the
    # index content-bound: new content is a new key, and the old set simply
    # goes unused.
    lines = _rs._load_file_lines(abs_path)
    idents = _MENTION_INDEX.get(lines)
    if idents is None:
        idents = frozenset(_IDENT_RE.findall("\n".join(lines)))
        _MENTION_INDEX[lines] = idents
    return symbol in idents


def _resolve_memoized(abs_path: str, symbol: str):
    """`resolve_symbol` behind a process-wide memo.

    The pairing pass asks the same (file, symbol) question from several places
    -- once per candidate here, then once more in the caller for the file that
    won -- and `resolve_symbol` itself is not memoized (only its file reads and
    lexical index are)."""
    key = (abs_path, symbol)
    if key not in _RESOLVE_MEMO:
        _RESOLVE_MEMO[key] = _rs.resolve_symbol(abs_path, symbol)
    return _RESOLVE_MEMO[key]


def _defining_candidates(section_files: tuple, symbol: str,
                         repo_root: Path) -> list:
    """Which of a section's authored files actually DEFINE `symbol`.

    Authored scope alone is not enough -- a section routinely names several
    files and the symbol lives in one of them -- so the pairing is confirmed by
    the resolver's own definition rule rather than by proximity. Exactly one
    hit is a pairing; zero or several leave the ref unpaired and counted.

    PRE-FILTERED on the symbol's mere presence in the file's masked text
    before the resolver runs. A section's candidate list is mostly files that
    do not mention the symbol at all, and the full resolve pays a regex search
    over the whole file to learn that. Measured cost of not pre-filtering
    (Codex perf): Check 7 0.42-0.45s -> 1.12-1.21s and the identity snapshot
    0.28-0.31s -> 0.85-0.90s against the live corpus. The pre-filter reuses
    resolve_symbol's OWN already-cached lexical index rather than a second
    scanner, so it can only skip a file that provably does not name the symbol
    in code -- never change a verdict."""
    hits = []
    for rel in section_files:
        abs_p = _contained(rel, repo_root)
        if abs_p is None:
            continue
        # A ResolverInputError from either call propagates on purpose: a
        # refused input is an infrastructure failure the CALLER surfaces as
        # rc 9, never evidence about this symbol, and must not read as
        # "not defined here".
        if not _mentions(str(abs_p), symbol):
            continue
        if _resolve_memoized(str(abs_p), symbol) is not None:
            hits.append(rel)
    return hits


def classify_ref(ref: dict, section_files: tuple, repo_root: Path) -> Verdict:
    """Decide which file a `kind=symbol` ref is checked against.

    The order of tests is the caller's historical order, preserved exactly so
    an unchanged corpus classifies unchanged:

      1. no symbol, or no file AND no section-scope pairing -> unpaired_ref
      2. absolute path, or a path escaping the repo         -> path_escape
      3. suffix outside .c/.h                               -> unsupported_lang
      4. file absent, and no unique basename repair         -> missing_file

    Two repairs sit inside that order:

      * SECTION SCOPE (step 1): a symbol with no file in its own item, but
        exactly one file among the section's authored files that defines it.
        The extractor pairs a symbol to the first file ref in the SAME ITEM
        (build.py), so an item whose sibling item named the file loses a
        pairing the section really does carry.
      * BASENAME (step 4): a stored path with NO directory component that does
        not exist, whose basename is unique among the tree's .c/.h files.
        `boot_desktop.c` was authored; `src/kernel/main/boot_desktop.c` is that
        same file's real path, not a different claim. Ambiguous (several files
        share the basename) and absent basenames are NEVER guessed -- they stay
        in missing_file and stay counted.
    """
    symbol = ref.get("symbol")
    rel = ref.get("file")
    provenance = "authored"
    if not symbol:
        return Verdict(None, None, provenance, "unpaired_ref")
    if not rel:
        if not section_files:
            return Verdict(None, None, provenance, "unpaired_ref")
        hits = _defining_candidates(section_files, symbol, repo_root)
        if len(hits) != 1:
            return Verdict(None, None, provenance, "unpaired_ref")
        rel, provenance = hits[0], "section"
    abs_p, path_prov = _effective_path(rel, repo_root)
    if abs_p is None:
        return Verdict(None, None, provenance, path_prov)
    # A section-scope hit is already an effective path (the candidate list is
    # built from them), so its own provenance stays "section"; otherwise the
    # path's provenance -- "authored" or "basename" -- is the ref's.
    if provenance != "section":
        provenance = path_prov
    try:
        eff_rel = str(abs_p.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        return Verdict(None, None, provenance, "path_escape")
    return Verdict(str(abs_p), eff_rel, provenance, None)
