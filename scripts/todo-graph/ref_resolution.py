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
# mtime against `todo/**/*.md` ONLY (`cache_schema.check_freshness`, shared by
# both cache readers since section 17).
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
from contextlib import contextmanager
from functools import wraps
from pathlib import Path
from typing import Optional

import resolve_symbol as _rs
import snapshot_protocol as _protocol

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
#               caller must count it in. A `Bucket` member drawn from
#               EMITTED_BUCKETS -- and `Bucket` members ARE `str`, so the names
#               match the existing cov keys exactly and the reporting contract
#               is unchanged.
class Verdict(namedtuple("Verdict", "abs_path rel_path provenance bucket")):
    """A verdict whose bucket field CANNOT hold a name outside the contract.

    This is the RUNTIME half of the section 20 emission contract; see
    EMITTED_BUCKETS below for the static half and for why one alone is not
    enough.

    OVERRIDING `__new__` ALONE IS NOT ENOUGH, and the first cut did exactly
    that. A namedtuple carries three more construction routes that do not go
    through it, and a probe drove a RETIRED bucket through every one (Codex
    adversarial, section 20): `_replace` (which calls `_make`), `_make` itself
    (a classmethod over `tuple.__new__`), and the un-subclassed base. So
    `_make` is overridden here -- which covers `_replace` too, since `_replace`
    is defined in terms of it -- and the base namedtuple is declared INLINE so
    the module never binds a name for an unchecked constructor.

    `tuple.__new__(Verdict, ...)` remains reachable and is NOT closed by any of
    this; nothing in Python can close it. That is why the public return
    boundary re-validates (`resolve_ref`, `classify_ref`) and why the AST
    contract bans these shapes in the emitter: the guarantee is layered, not
    absolute at any single point.
    """

    __slots__ = ()

    def __new__(cls, abs_path, rel_path, provenance, bucket):
        _check_emitted(bucket)
        return super().__new__(cls, abs_path, rel_path, provenance, bucket)

    @classmethod
    def _make(cls, iterable):
        return cls(*iterable)

class _Lru(dict):
    """A dict with a capacity, evicting least-recently-USED on insert.

    Only legitimate for a CONTENT-BOUND cache -- one whose every entry carries
    the `_load_file_lines` tuple it was derived from and is revalidated by
    identity on read. For those, an eviction costs a recompute and returns the
    same answer, so the bound is purely a memory bound. It is NOT legitimate
    for a topology-derived cache; see the lifecycle note below."""

    __slots__ = ("cap",)

    def __init__(self, cap: int):
        super().__init__()
        self.cap = cap

    def get(self, key, default=None):
        if key in self:
            val = super().pop(key)
            super().__setitem__(key, val)   # renew recency
            return val
        return default

    def __setitem__(self, key, value):
        if key in self:
            super().pop(key)
        elif len(self) >= self.cap:
            super().pop(next(iter(self)))   # oldest insertion == LRU
        super().__setitem__(key, value)


# ---------------------------------------------------------------------------
# TWO CACHE LIFECYCLES, and they are not interchangeable (Codex design review,
# section 15).
#
# CONTENT-BOUND caches below store `(lines object, value)` and revalidate by
# `is` on every read, so the file's own generation decides whether an entry is
# still true. Dropping one can only cost a recompute -- the recompute reads the
# same pinned content and produces the same answer -- so these are LRU-BOUNDED.
#
# TOPOLOGY-DERIVED caches (`_BASENAME_INDEX`, `_EFFECTIVE_PATH_MEMO`) answer
# from the directory tree instead: which files EXIST, and whether a basename is
# unique among them. Nothing pins that. Under LRU those two would recompute
# against a tree that may have gained or lost a file since the walk began, so a
# late ref could get a different effective path or bucket than an early one --
# a silent RESULT change, which is exactly what this section's own eviction
# invariant forbids. They are therefore WALK-SCOPED: unbounded for the duration
# of one walk, so every ref in that walk sees ONE topology snapshot, and
# released at the walk boundary (`clear_caches`, or the `walk_scope` context
# manager) rather than trimmed inside it. Memory stays bounded by the walk, not
# by a capacity that would have to trade consistency for it.
# ---------------------------------------------------------------------------
_BASENAME_INDEX = {}  # WALK-SCOPED: repo_root(str) -> {basename: (rel, ...)}
_EFFECTIVE_PATH_MEMO = {}  # WALK-SCOPED: (rel, root) -> (abs|None, prov|bucket)
# WALK-SCOPED for the SAME reason, though it looks content-bound: a MISS or a
# followed-header answer also depends on `follow_declaration`'s live
# `src/**/<basename>.c` glob, which no lines object pins. Creating a sibling can
# turn zero matches into one, or one into ambiguity, without invalidating any
# entry here. That is tolerable only while the whole walk shares one topology
# snapshot -- the argument `_resolve_end_to_end`'s docstring already makes about
# `_basename_index`. Under eviction it stops holding: a dropped entry recomputes
# against the CURRENT tree while a retained one answers from the old, so two
# refs in one walk can disagree (Codex adversarial, section 15).
_END_TO_END = {}      # WALK-SCOPED: (abs, symbol, root) -> (lines, answer, dep)

# Capacities are sized against the live corpus working set (measured 2026-08-06
# over one Check 7 walk: ~924 resolve pairs, ~998 effective paths, ~260
# identifier sets) with headroom, so an ordinary walk never evicts and a
# runaway importer still cannot grow without limit.
_RESOLVE_MEMO = _Lru(8192)   # (abs_path, symbol) -> (lines object, result)
_MENTION_INDEX = _Lru(512)   # abs_path -> (lines object, frozenset of idents)
_RAW_TEXT = _Lru(512)        # abs_path -> (lines object, joined raw text)
_CODE_TEXT = _Lru(512)       # abs_path -> (lines object, blanked text)

# How many times the literal/comment strip has actually RUN. The per-file cache
# it guards replaced a per-symbol scan that took Check 7 from 147ms to 829ms, so
# the regression is invisible to a cache-size check (a dict keyed on the path is
# bounded by the file count however often it is rebuilt) and flaky as a
# wall-clock assertion on a loaded host. Counting the expensive operation itself
# is the invariant that actually matters, and it also makes the content-binding
# observable: a file rewritten mid-run must REBUILD rather than answer stale.
_CODE_TEXT_BUILDS = 0

# The coverage buckets, split by WHERE the verdict is reached. Both callers
# report against these names, so they are a published contract:
# `corpus_resolution_snapshot` validates a snapshot's bucket strings against
# them, which makes a typo'd or retired bucket a hard error rather than an
# unrecognised value that silently compares unequal forever.
#
# RE-EXPORTED, no longer DEFINED here (section 18). They moved to the
# inert `snapshot_protocol.json`, loaded by `snapshot_protocol.py`, because a
# bucket migration otherwise had to edit THIS file -- the one the identity gate
# needs byte-identical to prove no resolver change rode along with it. That made
# the gate's separation rule unsatisfiable and left it failing closed on every
# bucket migration (section 16). Every access site is unchanged: `_rr.ALL_BUCKETS`
# and `_rr.POST_RESOLUTION_BUCKETS` still resolve here.
PRE_RESOLUTION_BUCKETS = _protocol.PRE_RESOLUTION_BUCKETS
POST_RESOLUTION_BUCKETS = _protocol.POST_RESOLUTION_BUCKETS
ALL_BUCKETS = _protocol.ALL_BUCKETS
Bucket = _protocol.Bucket

# THE DECLARED EMITTED-MEMBER SET (section 20). This is the whole contract: the
# set of buckets this resolver can produce, written down in ONE place, in a
# shape a checker can read without executing anything and without inferring it
# from source text.
#
# WHY IT IS DECLARED AND NOT DISCOVERED. The first design inferred the set by
# collecting every `Bucket.<MEMBER>` attribute access in this file. That is
# bypassable and was rejected in design review: `next(iter(Bucket))`, or an
# alias `B = Bucket` followed by `B.PATH_ESCAPE`, emits a bucket while the file
# contains no bucket literal, no subscript, no dynamic construction and no
# `Bucket.<MEMBER>` expression at all -- so a retirement gate reading the
# inferred set would call the member absent while this module could still emit
# it, recreating exactly the latent undeclared-verdict failure the section
# exists to close.
#
# WHY BOTH HALVES ARE NEEDED.
#   STATIC  -- `scripts/lint/check_bucket_emission.py` pins the literal AST
#              shape of this assignment (a module-level `frozenset({...})` of
#              `Bucket.<MEMBER>` elements, assigned exactly once and never
#              rebound), so the set is readable as a static property. That is
#              what `identity-gate.sh` consults to prove a retired bucket can
#              no longer be produced.
#   RUNTIME -- `_check_emitted` refuses any other value at construction, so a
#              computed name cannot escape the declaration even though no AST
#              rule can enumerate every way to compute one.
# The static half makes the set READABLE; the runtime half makes it TRUE.
#
# RETIRING A BUCKET is therefore the documented two-step sequence, with step
# one now provable: remove its member from this set (an executable change, put
# through the resolver differential like any other), then remove the name from
# `snapshot_protocol.json` (a data-only change the identity gate can approve).
EMITTED_BUCKETS = frozenset({
    Bucket.UNPAIRED_REF,
    Bucket.PATH_ESCAPE,
    Bucket.UNSUPPORTED_LANG,
    Bucket.MISSING_FILE,
    Bucket.UNRESOLVED_CALLLIKE,
    Bucket.NO_CALLLIKE_TOKEN,
})


class BucketContractError(RuntimeError):
    """A verdict tried to carry a bucket outside the declared emitted set."""


def _check_emitted(bucket) -> None:
    """Refuse any bucket value the declaration does not cover.

    `None` is the resolved case and always legal. Everything else must be BOTH
    a `Bucket` instance AND a member of EMITTED_BUCKETS. Both halves are load-
    bearing and neither is redundant:

      MEMBERSHIP alone is too weak in the other direction -- an instance check
      would accept every DECLARED bucket, including ones this resolver does not
      emit, and the retirement proof reads the EMITTED set, not the vocabulary.

      INSTANCE alone is too weak in this one -- `Bucket` is str-valued, so a
      bare `"path_escape"` string hashes and compares equal to the member and
      sails through a membership test on its own. Measured while building this
      contract: the first cut checked membership only and accepted both a raw
      literal and a runtime concatenation. That would leave the enum typing
      decorative and the AST literal ban unbacked at runtime.
    """
    if bucket is None or (isinstance(bucket, Bucket)
                          and bucket in EMITTED_BUCKETS):
        return
    raise BucketContractError(
        f"bucket {bucket!r} ({type(bucket).__name__}) is not a `Bucket` member "
        f"of the declared emitted set {sorted(str(b) for b in EMITTED_BUCKETS)}"
        f". A verdict may only carry a bucket this module declares it can emit "
        f"(ref_resolution.EMITTED_BUCKETS), named as `Bucket.<MEMBER>`; if this "
        f"is a new bucket, declare it there and in snapshot_protocol.json in "
        f"the same change.")


def _bucket_bounded(expected):
    """Re-validate the bucket at the resolver's PUBLIC return boundary.

    The constructors bound ORDINARY construction, but `tuple.__new__(Verdict,
    ...)` is reachable in Python and cannot be taken away -- so a value built
    that way inside this module would carry any string at all. Every bucket a
    caller can observe leaves through `classify_ref` or `resolve_ref`, so
    checking here bounds the OBSERVABLE emission set regardless of how the
    value was constructed. Cheap: one type + membership test per ref (~1,600
    per walk).

    TYPED, because `getattr(result, "bucket", None)` FAILED OPEN. A missing
    attribute read as `None`, which is the legal RESOLVED value, so a function
    returning a raw tuple carrying an undeclared bucket passed the boundary
    untouched (Codex adversarial, section 20 round 2). Requiring the declared
    outcome type and reading `.bucket` directly means an unexpected return
    shape is refused here rather than crashing a consumer later.
    """

    def _decorate(fn):
        @wraps(fn)
        def _bounded(*args, **kwargs):
            result = fn(*args, **kwargs)
            # EXACT TYPE, not `isinstance`. A SUBCLASS can override `bucket`
            # with a property, and a probe returned a declared member on the
            # boundary's read and a RETIRED one on the consumer's next read --
            # so the caller observed a value the boundary never approved (Codex
            # adversarial, section 20 post-commit round). Nothing legitimate
            # returns a subclass: `Verdict` and `RefResult` subclass anonymous
            # namedtuples, but every value the resolver constructs has exactly
            # one of those two types.
            if type(result) is not expected:
                raise BucketContractError(
                    f"{fn.__name__} returned {type(result).__name__}, not "
                    f"exactly {expected.__name__}; the bucket contract can "
                    f"only be enforced on the declared outcome types, and a "
                    f"subclass can change `bucket` after it is validated")
            _check_emitted(result.bucket)
            return result

        return _bounded

    return _decorate


# A DECLARED MEMBER THAT IS NOT IN THE VOCABULARY IS A TYPO OR A HALF-DONE
# RETIREMENT, and either way every snapshot it reached would compare unequal
# forever. Checked at import so it cannot wait for a ref to land in it.
_undeclared = sorted(str(b) for b in EMITTED_BUCKETS if b not in ALL_BUCKETS)
if _undeclared:
    raise _protocol.ProtocolError(
        f"ref_resolution.EMITTED_BUCKETS names {_undeclared}, which the "
        f"published vocabulary does not declare")
del _undeclared


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


_WALK_DEPTH = 0


@contextmanager
def walk_scope():
    """One walk's cache lifetime, released on exit.

    The boundary the topology-derived caches need in order to be BOTH
    consistent and bounded: inside it they never evict, so every ref in the
    walk answers from one directory snapshot -- the basename index, the
    effective-path memo, the end-to-end memo, and (since the same review) the
    declaration-following sibling index. Leaving it drops them, so a
    long-lived importer serving several worktrees does not accumulate a
    tree-sized index per root forever. The CLI callers get this for free
    either way (a lint invocation is a fresh interpreter that exits), which is
    why this is housekeeping rather than a correctness gate -- but a gate is
    exactly what an in-process caller would otherwise lack.

    RE-ENTRANT, and that is load-bearing rather than politeness. Only the
    OUTERMOST scope clears. A nested scope that cleared on exit would drop
    `resolve_symbol`'s generation PINS while the outer walk is still running,
    and those pins are the only thing that turns a mid-walk file rewrite into
    the documented rc 9 / rc 3 infrastructure failure -- so the mutation would
    instead be accepted as an innocent first read, which is the one outcome
    indistinguishable from a clean pass (Codex consistency, section 15).

    SINGLE-THREADED BY CONTRACT, and the depth counter does not change that.
    Every cache this module and `resolve_symbol` hold -- the seven dicts above,
    `_content_cache`, `_pinned`, and the `lru_cache`-backed file index -- is
    unsynchronized process-global state, and always has been. Both consumers
    are single-shot CLI processes. A lock around THIS boundary alone would
    serialize walks while leaving all of that unguarded, which advertises a
    thread-safety property the module does not have; stating the contract is
    the honest form."""
    global _WALK_DEPTH
    # `prior` is captured, and restored in a `finally` whose protected region
    # begins BEFORE the clear and the increment. Incrementing outside the
    # `try` left a real window: an exception delivered between the increment
    # and the block (a KeyboardInterrupt is enough) stranded the depth above
    # zero permanently, and every later walk then skipped clearing -- so
    # `_SIBLING_INDEX` entries, including cached ZERO-match tuples, and the
    # generation pins survived boundaries they exist to be dropped at (Codex
    # re-adversarial, section 15).
    prior = _WALK_DEPTH
    try:
        if prior == 0:
            clear_caches()
        _WALK_DEPTH = prior + 1
        yield
    finally:
        _WALK_DEPTH = prior
        if prior == 0:
            clear_caches()


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
        return (None, Bucket.PATH_ESCAPE)
    abs_p = _contained(rel, repo_root)
    if abs_p is None:
        return (None, Bucket.PATH_ESCAPE)
    if abs_p.suffix not in _C_SUFFIXES:
        return (None, Bucket.UNSUPPORTED_LANG)
    if abs_p.is_file():
        return (abs_p, "authored")
    if "/" in rel or "\\" in rel:
        return (None, Bucket.MISSING_FILE)
    cands = _basename_index(repo_root).get(rel, ())
    if len(cands) != 1:
        return (None, Bucket.MISSING_FILE)
    abs_p = _contained(cands[0], repo_root)
    if abs_p is None or not abs_p.is_file():
        return (None, Bucket.MISSING_FILE)
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


class SectionScope:
    """One section's pairing evidence: its authored files and its unpaired
    symbols, computed once per section instead of once per ref.

    NO BATCHING HAPPENS HERE, and the class must not imply otherwise. An
    earlier revision carried a `_defs` per-file batch map plus a docstring
    promising "one batch per candidate file per section"; the batch was
    measured slower than the C-speed per-symbol scan and removed, but the
    member and the promise survived the revert -- a dead attribute advertising
    a performance contract the code does not honour, on the very module whose
    purpose is that both gates share ONE truthful rule (Codex consistency,
    section 15). `defines()` performs one memoized resolve per (file, symbol);
    the cost shape that follows from that is stated on `_defining_candidates`.

    BOTH LISTS ARE LAZY. 357 of the corpus's 830 sections hold an unpaired
    symbol; the rest never consult `files` at all, and canonicalizing paths
    for them was measured waste. Nothing is computed until a ref asks.
    """

    __slots__ = ("_items", "_root", "_files", "_symbols")

    def __init__(self, items: list, repo_root: Path):
        self._items = items or []
        self._root = repo_root
        self._files = None
        self._symbols = None

    @property
    def symbols(self) -> frozenset:
        """Every symbol this section names with NO file of its own -- exactly
        the population the section-scope pairing has to answer for, and so
        exactly the set worth resolving per candidate file in one pass."""
        if self._symbols is None:
            self._symbols = frozenset(
                r.get("symbol") for it in self._items
                for r in (it.get("refs") or [])
                if r.get("kind") == "symbol" and not r.get("file")
                and r.get("symbol"))
        return self._symbols

    @property
    def files(self) -> tuple:
        if self._files is None:
            self._files = (section_candidate_files(self._items, self._root)
                           if self.symbols else ())
        return self._files

    def defines(self, abs_path: str, symbol: str):
        """Does `abs_path` define `symbol`, per the resolver's own rule?

        A thin pass-through to the shared per-symbol memo, deliberately. This
        is where a BATCHED per-file definition lookup was implemented and then
        REMOVED, because measurement refuted the premise it was built on --
        see the section 15 note in the module header. It stays a named method
        on the scope so the pairing pass has one place to ask the question,
        and so a future batching attempt has a seam that does not require
        touching `classify_ref` again."""
        return _resolve_memoized(abs_path, symbol)


def section_scope(items: list, repo_root: Path) -> SectionScope:
    """THE shared constructor for a section's pairing evidence.

    Both callers build their per-section scope with this and nothing else. It
    replaces the identical `_needs` + `section_candidate_files` preamble each
    had copied, which is the same duplication-of-a-shared-rule this module was
    created to end -- two copies of "which sections need candidates" can drift
    apart exactly like two copies of the resolution sequence did."""
    return SectionScope(items, repo_root)


EMPTY_SCOPE = SectionScope([], Path("."))


def _defining_candidates(scope: "SectionScope", symbol: str,
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
    for abs_str in scope.files:
        # A ResolverInputError from either call propagates on purpose: a
        # refused input is an infrastructure failure the CALLER surfaces as
        # rc 9, never evidence about this symbol, and must not read as
        # "not defined here".
        if not _mentions(abs_str, symbol):
            continue
        if scope.defines(abs_str, symbol) is not None:
            hits.append(abs_str)
    return hits


@_bucket_bounded(Verdict)
def classify_ref(ref: dict, scope: "SectionScope", repo_root: Path) -> Verdict:
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
        return Verdict(None, None, provenance, Bucket.UNPAIRED_REF)
    if not rel:
        if not scope.files:
            return Verdict(None, None, provenance, Bucket.UNPAIRED_REF)
        hits = _defining_candidates(scope, symbol, repo_root)
        if len(hits) != 1:
            return Verdict(None, None, provenance, Bucket.UNPAIRED_REF)
        abs_p = Path(hits[0])
        try:
            eff = str(abs_p.relative_to(repo_root)).replace("\\", "/")
        except ValueError:
            return Verdict(None, None, "section", Bucket.PATH_ESCAPE)
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
        return Verdict(None, None, provenance, Bucket.PATH_ESCAPE)
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
class RefResult(namedtuple(
        "RefResult",
        "rel_path provenance bucket def_abs def_rel line_start line_end")):
    """The end-to-end answer, bucket-contract-checked exactly as `Verdict` is.

    Both outcome types are checked because both are constructed directly on
    emission paths: `resolve_ref` builds a RefResult from a Verdict's bucket,
    and `_resolve_end_to_end` builds one from the post-resolution branch, so
    checking only Verdict would leave the post-resolution half unbounded. The
    `_make`/`_replace`/inline-base reasoning is identical; see `Verdict`.
    """

    __slots__ = ()

    def __new__(cls, rel_path, provenance, bucket, def_abs, def_rel,
                line_start, line_end):
        _check_emitted(bucket)
        return super().__new__(cls, rel_path, provenance, bucket, def_abs,
                               def_rel, line_start, line_end)

    @classmethod
    def _make(cls, iterable):
        return cls(*iterable)


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
        answer = (Bucket.UNRESOLVED_CALLLIKE if has_calllike_token(abs_path, symbol)
                  else Bucket.NO_CALLLIKE_TOKEN)
    else:
        answer = (resolved[0], resolved[1], resolved[2])
        def_abs = str(Path(resolved[0]).resolve())
        if def_abs != abs_path:
            dep = (def_abs, _rs._load_file_lines(def_abs))
    _END_TO_END[key] = (lines, answer, dep)
    return answer


@_bucket_bounded(RefResult)
def resolve_ref(ref: dict, scope: "SectionScope", repo_root: Path) -> RefResult:
    """THE shared verdict for one `kind=symbol` ref: classify, resolve, bucket.

    Both consumers of the cache's `stamped_items[].refs` call exactly this and
    nothing else from the resolver. A caller that reached past it to add its own
    fallback would put the identity snapshot back out of step with lint Check 7,
    which is the failure this module exists to prevent.

    `scope` is the section's `SectionScope` (see `section_scope`); pass
    `EMPTY_SCOPE` for a ref with no section context, which classifies a
    file-less symbol as `unpaired_ref` exactly as an empty file tuple did."""
    verdict = classify_ref(ref, scope, repo_root)
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
