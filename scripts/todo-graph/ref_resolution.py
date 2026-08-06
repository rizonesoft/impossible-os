#!/usr/bin/env python3
# ============================================================================
# ref_resolution.py -- the SINGLE END-TO-END rule for a stamped-item symbol
# ref: which file it is checked against, whether a definition was found there,
# and which coverage bucket it lands in when one was not.
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
_RESOLVE_MEMO = {}    # (abs_path, symbol) -> (lines object, result)
_EFFECTIVE_PATH_MEMO = {}  # (rel, repo_root) -> (abs_path|None, provenance|bucket)
_MENTION_INDEX = {}   # abs_path -> (lines object, frozenset of identifiers)
_RAW_TEXT = {}        # abs_path -> (lines object, joined raw text)
_CODE_TEXT = {}       # abs_path -> (lines object, literal/comment-blanked text)
_END_TO_END = {}      # (abs_path, symbol, repo_root) -> (lines object, answer)

# How many times the literal/comment strip has actually RUN. The per-file cache
# it guards replaced a per-symbol scan that took Check 7 from 147ms to 829ms, so
# the regression is invisible to a cache-size check (a dict keyed on the path is
# bounded by the file count however often it is rebuilt) and flaky as a
# wall-clock assertion on a loaded host. Counting the expensive operation itself
# is the invariant that actually matters, and it also makes the content-binding
# observable: a file rewritten mid-run must REBUILD rather than answer stale.
_CODE_TEXT_BUILDS = 0

# The coverage buckets, split by WHERE the verdict is reached. Both callers
# report against these names, so they are this module's published contract:
# `corpus_resolution_snapshot` validates a baseline's bucket strings against
# ALL_BUCKETS, which makes a typo'd or retired bucket a hard baseline error
# rather than an unrecognised value that silently compares unequal forever.
PRE_RESOLUTION_BUCKETS = ("unpaired_ref", "path_escape", "unsupported_lang",
                          "missing_file")
POST_RESOLUTION_BUCKETS = ("unresolved_calllike", "no_calllike_token")
ALL_BUCKETS = PRE_RESOLUTION_BUCKETS + POST_RESOLUTION_BUCKETS


def clear_caches() -> None:
    """Drop the per-process basename index and resolution memo. Tests that
    mutate a fixture tree between walks must call this; a long-lived process
    would otherwise answer from state built before the mutation."""
    _BASENAME_INDEX.clear()
    _RESOLVE_MEMO.clear()
    _EFFECTIVE_PATH_MEMO.clear()
    _MENTION_INDEX.clear()
    _RAW_TEXT.clear()
    _CODE_TEXT.clear()
    _END_TO_END.clear()
    global _CODE_TEXT_BUILDS
    _CODE_TEXT_BUILDS = 0
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
    _RAW_TEXT.clear()
    _CODE_TEXT.clear()
    _END_TO_END.clear()
    global _CODE_TEXT_BUILDS
    _CODE_TEXT_BUILDS = 0


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
            # ABSOLUTE and already canonical. Returning repo-relative strings
            # forced `_defining_candidates` to re-run `_contained` -- i.e. a
            # `Path.resolve()` + `relative_to()` pair -- once per
            # (candidate, symbol) probe, 2,709 calls and ~0.5s of a 1.5s walk
            # (Codex perf, post-commit).
            out.append(str(abs_p))
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
    hit = _MENTION_INDEX.get(abs_path)
    if hit is None or hit[0] is not lines:
        idents = frozenset(_IDENT_RE.findall("\n".join(lines)))
        _MENTION_INDEX[abs_path] = (lines, idents)
    return symbol in _MENTION_INDEX[abs_path][1]


def _resolve_memoized(abs_path: str, symbol: str):
    """`resolve_symbol` behind a process-wide memo.

    The pairing pass asks the same (file, symbol) question from several places
    -- once per candidate here, then once more in the caller for the file that
    won -- and `resolve_symbol` itself is not memoized (only its file reads and
    lexical index are)."""
    # `_load_file_lines` FIRST, on every call: it is the only layer that
    # re-stats a pinned file and raises ResolverInputError on a same-run
    # rewrite, and a memo that answers without it hands out pre-mutation
    # coordinates instead of the promised infrastructure failure (Codex
    # adversarial, post-commit). Binding the entry to the returned lines
    # OBJECT keeps the check O(1): a path key hashes in constant time, while
    # keying on the tuple itself re-hashes every line on every lookup
    # (measured: 0.680s vs 0.0023s over 20,000 probes against bootx64.c).
    lines = _rs._load_file_lines(abs_path)
    key = (abs_path, symbol)
    hit = _RESOLVE_MEMO.get(key)
    if hit is None or hit[0] is not lines:
        hit = (lines, _rs.resolve_symbol(abs_path, symbol))
        _RESOLVE_MEMO[key] = hit
    return hit[1]


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
    for abs_str in section_files:
        # A ResolverInputError from either call propagates on purpose: a
        # refused input is an infrastructure failure the CALLER surfaces as
        # rc 9, never evidence about this symbol, and must not read as
        # "not defined here".
        if not _mentions(abs_str, symbol):
            continue
        if _resolve_memoized(abs_str, symbol) is not None:
            hits.append(abs_str)
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
        abs_p = Path(hits[0])
        try:
            eff = str(abs_p.relative_to(repo_root)).replace("\\", "/")
        except ValueError:
            return Verdict(None, None, "section", "path_escape")
        return Verdict(str(abs_p), eff, "section", None)
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


# ---------------------------------------------------------------------------
# POST-RESOLUTION classification. Moved here from check_stub_behind_stamp.py so
# the identity snapshot and lint Check 7 share the END-TO-END verdict, not just
# its first half.
#
# Sharing only `classify_ref` was not enough, and the reason is the same defect
# this module was created to close. Each caller ran its OWN direct-resolve ->
# `follow_declaration` -> bucket sequence, so a fallback added to one and not
# the other silently re-creates the incident in the header above: the identity
# proof covering 36 of 54 added mappings. `resolve_ref` below is now the only
# place that sequence exists, and both callers consume its answer rather than
# orchestrating any resolver step themselves.
# ---------------------------------------------------------------------------

# String/char literal FIRST, then line comment, then block comment -- see the
# ordering argument in `_code_text`. Unterminated constructs simply fail to
# match and are left as code, which is the conservative direction here: it can
# only make a ref look call-like, never hide one.
_STRIP_RE = re.compile(
    r'"(?:\\.|[^"\\\n])*"'      # string literal (post-splice, so no newline)
    r"|'(?:\\.|[^'\\\n])*'"     # char literal
    r"|//[^\n]*"                # line comment
    r"|/\*[^*]*\*+(?:[^/*][^*]*\*+)*/",   # block comment (C comments do not nest)
    re.S)


def _raw_text(file_abs: str) -> str:
    """The file's lines joined once, cached, for the pre-filter's substring test.

    CONTENT-BOUND, not `lru_cache(file_abs)`. Keyed on the path alone, a cache
    HIT never runs the body, so `_load_file_lines` -- the only layer that
    re-stats a pinned file and raises ResolverInputError on a same-run rewrite
    -- was never reached after the first call. A file rewritten mid-walk then
    answered from pre-mutation text and produced a stale BUCKET instead of the
    infrastructure failure the resolver promises. Same defect and same repair as
    `_mentions` above (Codex design review, section 14)."""
    lines = _rs._load_file_lines(file_abs)
    hit = _RAW_TEXT.get(file_abs)
    if hit is None or hit[0] is not lines:
        _RAW_TEXT[file_abs] = (lines, "\n".join(lines))
    return _RAW_TEXT[file_abs][1]


def _code_text(file_abs: str) -> str:
    """The file with comments and string/char literals blanked, as ONE string.

    PER FILE, NOT PER SYMBOL. The first version of this did the stateful scan
    inside the per-`(file, symbol)` helper, so a file was re-lexed once for
    every unresolved symbol naming it. MEASURED on the live corpus: Check 7
    went from 147.4ms to 828.6ms, a 5.62x regression on a check that runs in
    the PRE-COMMIT HOOK for every contributor on every commit. The stripped
    text does not depend on the symbol, so it is computed once and cached and
    the per-symbol step becomes one regex over it.

    `resolve_symbol._strip_line_comments` is NOT reusable here: it documents
    that it does not track multi-line block comments, so a symbol named inside
    a `/* ... */` prose block would read as code and land in the wrong bucket.

    LINE SPLICING IS APPLIED FIRST (C translation phase 2), which is safe HERE
    and not in `resolve_symbol`: this helper answers a yes/no question and
    never reports coordinates, whereas every resolver consumer prints real line
    numbers. Splicing fixes two shapes that were classified wrong -- a string
    continued across a backslash-newline no longer hides its terminator, and an
    identifier joined to its `(` across a continuation is seen.

    Content-bound for the same reason as `_raw_text` above."""
    global _CODE_TEXT_BUILDS
    lines = _rs._load_file_lines(file_abs)
    hit = _CODE_TEXT.get(file_abs)
    if hit is not None and hit[0] is lines:
        return hit[1]
    _CODE_TEXT_BUILDS += 1

    spliced, buf = [], ""
    for line in lines:
        if line.endswith("\\"):
            buf += line[:-1]
            continue
        spliced.append(buf + line)
        buf = ""
    if buf:
        spliced.append(buf)

    # ONE C-level regex pass, not a Python character loop. The char loop was
    # correct but cost ~5ms per file over 55 files; `re` runs the same
    # tokenization in C. Alternation ORDER is the correctness argument: a
    # string/char literal is tried FIRST at each position, so it consumes
    # through its own terminator and a `//` or `/*` sitting inside a literal
    # can never start a comment. C block comments do not nest, so non-greedy
    # `/\*.*?\*/` is exact rather than approximate.
    #
    # Each match is replaced by a space plus its own newlines, which keeps
    # tokens from merging across a removed literal AND preserves line
    # structure, so `\s*` in the caller's pattern still spans a definition
    # written `symbol\n(args)`. Scanning line-by-line could not see that shape.
    text = _STRIP_RE.sub(lambda mo: " " + "\n" * mo.group(0).count("\n"),
                         "\n".join(spliced))
    _CODE_TEXT[file_abs] = (lines, text)
    return text


def has_calllike_token(file_abs: str, symbol: str) -> bool:
    """True when `symbol(` occurs in CODE -- outside comments and strings.

    Proves a call-like TOKEN is present and nothing more. A call site, a
    prototype, a macro invocation and a struct function-pointer field all
    satisfy it -- which is exactly why the bucket it feeds is named for the
    token rather than for a cause.
    """
    # RAW PRE-FILTER. Blanking literals and comments only ever REMOVES text, so
    # a symbol absent from the raw file cannot appear in the stripped text --
    # this skips building it entirely. Sound in one direction only, which is
    # the direction used: absent-in-raw is conclusive, present-in-raw still has
    # to be confirmed against stripped text.
    #
    # Against the CACHED JOINED text, not `any(sym in line for line in ...)`.
    # The generator form ran a Python-level loop over every line on all 136
    # calls (~272k iterations) and cost more than the scan it was added to
    # avoid; one `in` over one cached string is a single C-level scan.
    if symbol not in _raw_text(file_abs):
        return False
    return re.search(r"\b" + re.escape(symbol) + r"\s*\(",
                     _code_text(file_abs)) is not None


# The END-TO-END answer for one symbol ref.
#   rel_path    repo-relative EFFECTIVE path the ref was checked against, or
#               None when a pre-resolution bucket was reached
#   provenance  "authored" | "basename" | "section"
#   bucket      None when a definition was found; otherwise the coverage bucket
#   def_abs     absolute path of the file holding the DEFINITION, or None. Not
#               always `rel_path`: a followed declaration resolves in the
#               sibling .c, and findings must be reported where the body is.
#   def_rel     repo-relative form of def_abs, or None
#   line_start  first line of the definition, or None
#   line_end    last line of the definition, or None
RefResult = namedtuple(
    "RefResult", "rel_path provenance bucket def_abs def_rel line_start line_end")


def _resolve_end_to_end(abs_path: str, symbol: str, repo_root: Path):
    """(def_path, start, end) for a resolvable ref, else a POST_RESOLUTION bucket.

    The direct resolve, the declaration-following fallback and the final
    call-like bucketing in ONE place. Memoized exactly as `_resolve_memoized`
    is -- bound to the lines OBJECT so new content is a new entry, and reached
    only after `_load_file_lines` has re-stat'd the file, so a mid-run rewrite
    still raises ResolverInputError instead of being served a stale verdict.

    BOUND TO THE DEFINITION FILE TOO, not just the declaring one. A followed
    declaration resolves in a SIBLING .c, so an entry validated only against
    `abs_path` handed back coordinates for a file it never re-read: rewrite the
    .c mid-run and the header still looked unchanged, so the walk answered with
    stale line numbers instead of raising ResolverInputError -- defeating the
    generation pinning in exactly the cross-file case where it matters most.
    Found independently by BOTH review legs (Codex adversarial + test-coverage,
    section 14). Every file the answer was derived from is re-entered on a hit.

    NOT covered, and pre-existing rather than introduced here: a sibling CREATED
    mid-run can make a previously-unique basename ambiguous without any file the
    answer depends on changing. `_basename_index` above is already cached per
    repo_root for the whole walk, so the existing basename repair has the same
    property -- a walk does not re-enumerate the tree.
    """
    lines = _rs._load_file_lines(abs_path)
    key = (abs_path, symbol, str(repo_root))
    hit = _END_TO_END.get(key)
    if hit is not None and hit[0] is lines:
        dep_path, dep_lines = hit[2]
        # `_load_file_lines` is the ONLY layer that re-stats and raises on
        # drift, so a dependency must be RE-ENTERED on a hit, never assumed.
        if dep_path is None or _rs._load_file_lines(dep_path) is dep_lines:
            return hit[1]

    resolved = _resolve_memoized(abs_path, symbol)
    if resolved is None:
        # A ref naming a HEADER often resolves via its own "decl" verdict (a
        # prototype, no local body) -- follow to the repo-convention sibling
        # .c before giving up.
        resolved = _rs.follow_declaration(abs_path, symbol, str(repo_root))
    dep = (None, None)
    if resolved is None:
        answer = ("unresolved_calllike" if has_calllike_token(abs_path, symbol)
                  else "no_calllike_token")
    else:
        answer = (resolved[0], resolved[1], resolved[2])
        def_abs = str(Path(resolved[0]).resolve())
        if def_abs != abs_path:
            dep = (def_abs, _rs._load_file_lines(def_abs))
    _END_TO_END[key] = (lines, answer, dep)
    return answer


def resolve_ref(ref: dict, section_files: tuple, repo_root: Path) -> RefResult:
    """THE shared verdict for one `kind=symbol` ref: classify, resolve, bucket.

    Both consumers of the cache's `stamped_items[].refs` call exactly this and
    nothing else from the resolver. A caller that reached past it to add its own
    fallback would put the identity snapshot back out of step with lint Check 7,
    which is the failure this module exists to prevent."""
    verdict = classify_ref(ref, section_files, repo_root)
    if verdict.bucket is not None:
        return RefResult(verdict.rel_path, verdict.provenance, verdict.bucket,
                         None, None, None, None)
    answer = _resolve_end_to_end(verdict.abs_path, ref.get("symbol"), repo_root)
    if isinstance(answer, str):
        return RefResult(verdict.rel_path, verdict.provenance, answer,
                         None, None, None, None)
    def_path, start, end = answer
    def_abs = Path(def_path).resolve()
    try:
        def_rel = str(def_abs.relative_to(repo_root)).replace("\\", "/")
    except ValueError:
        # ALWAYS repo-relative for the reported/recorded form. An absolute path
        # differs between checkouts and would read as a false MOVE in the
        # identity gate, so a definition outside the root is a hard error
        # rather than a quietly-different value.
        raise _rs.ResolverInputError(
            f"definition resolved outside repo root: {def_abs} "
            f"(root {repo_root})")
    return RefResult(verdict.rel_path, verdict.provenance, None,
                     str(def_abs), def_rel, start, end)
