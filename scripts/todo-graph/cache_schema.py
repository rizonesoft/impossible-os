#!/usr/bin/env python3
# ============================================================================
# ONE cache-schema validator for BOTH cache readers (TODO-06 section 17).
#
# WHAT THIS MODULE IS -- and, just as load-bearing, what it is NOT.
#
# Two readers consume build/todo-cache.json and, before this module, disagreed
# about which caches are valid at all:
#
#   corpus_resolution_snapshot._load_nodes  fail-closed, every check traceable
#                                           to a Codex finding, one code (rc 3)
#   check_stub_behind_stamp.main            validated containers only, leaned on
#                                           `except Exception -> rc 6`, and
#                                           SILENTLY walked zero refs on a
#                                           wrong-typed-but-falsey shape
#
# So the same corrupt cache produced an infrastructure refusal on one side and
# a clean-looking lint on the other. That is the divergence this module ends.
#
# SCOPE -- the two kinds of check here are DELIBERATELY separate, because
# conflating them is what let a producer regression read as "old cache":
#
#   SCHEMA checks    enforce schema/cache.schema.json for THE FIELDS BOTH
#                    READERS CONSUME: the `stamped_items` subtree, plus the
#                    node-level `file_path` that both walks build occurrence
#                    keys from. "Consumed by a reader" is the scope line, not
#                    "inside stamped_items" -- an unvalidated consumed field
#                    becomes identity data and turns a corrupt cache into a
#                    VERDICT rather than an infrastructure refusal. Until
#                    this module, NOTHING in the repo validated against that
#                    file: it was checked-in documentation with no enforcement
#                    path (Codex design review, section 17). The rules here are
#                    derived from it, and test_build.sh asserts that agreement
#                    against `jsonschema` as an INDEPENDENT oracle -- not by the
#                    two readers agreeing with each other, which would only
#                    prove they share a defect.
#
#   READINESS checks are operational, not schema: does the cache exist, does it
#                    parse, does it hold any nodes, is it stale against
#                    todo/**/*.md, does it carry a stamped population at all.
#                    A JSON Schema cannot express any of them, and TWO of them
#                    deliberately DIVERGE from the schema -- the schema
#                    describes what the producer may legally EMIT, while a
#                    reader additionally needs something to walk. Both are
#                    refused as READINESS reasons, never as schema violations,
#                    and both are listed rather than hidden:
#                      * a root `[]` is schema-valid, refused as REASON_EMPTY.
#                      * a cache where EVERY node omits `stamped_items` is also
#                        schema-valid (the field is emitted only when a body has
#                        a [x] item), and is refused as
#                        REASON_LEGACY_NO_STAMPED_ITEMS -- it cannot be
#                        distinguished from a pre-extension cache, which is why
#                        the lint routes it to a WARN rather than an error.
#                    An earlier revision of this header claimed the root `[]`
#                    was the ONLY divergence. It was not, and the fixture oracle
#                    had been asserting the second one as `accept` all along
#                    without the prose admitting it (Codex consistency, section
#                    17 review).
#
# NOT IN SCOPE: the node-level frontmatter fields neither reader consumes
# (id/domain/status/sections and friends). Validating them here would advertise
# a contract this module does not test. `build.py` owns them.
#
# THE POPULATION QUESTION IS THE CALLER'S, NOT THIS MODULE'S. An empty stamped
# population is not a shape error, and the two readers legitimately want
# different outcomes for it (a vacuous baseline is useless to the snapshot; a
# population COLLAPSE must reach the lint's own rc 7 regression gate rather
# than be pre-empted as a warning). So this module REPORTS the population as a
# fact and lets each caller apply policy -- see `CacheInfo` below.
#
# SHARING THE RULE MUST NOT COLLAPSE THE CODES. The snapshot documents rc 3 for
# "could not run"; the lint documents 2/3/4/5 and section 16's wiring depends
# on both contracts. This module therefore RAISES with a `reason` tag and never
# calls sys.exit; each caller maps reason -> its own documented code.
# ============================================================================

import json
import os
from pathlib import Path

# Reason tags. Callers map these to their OWN documented exit codes; a caller
# that grows a new code maps it here rather than re-deriving the rule.
REASON_MISSING = "MISSING"                  # cache file absent
REASON_UNREADABLE = "UNREADABLE"            # unreadable or not parseable JSON
REASON_SHAPE = "SHAPE"                      # parsed, but violates the schema
REASON_EMPTY = "EMPTY"                      # readiness: no nodes to walk at all
REASON_LEGACY_NO_STAMPED_ITEMS = "LEGACY"   # predates the section 9 extension
REASON_STALE = "STALE"                      # older than the newest TODO

# Path to the schema these rules implement. Quoted in errors so a failure names
# the contract it violated instead of just the offending value.
SCHEMA_REL = "scripts/todo-graph/schema/cache.schema.json"

# Input budget, enforced BEFORE the file is materialised. The live cache is
# ~3.3 MiB across 232 nodes, so 64 MiB is ~19x headroom while still bounding a
# hostile input. Same role as `resolve_symbol._MAX_FILE_BYTES` (16 MiB), larger
# because this is the whole corpus in one document rather than one source file.
_MAX_CACHE_BYTES = 64 * 1024 * 1024


# ---------------------------------------------------------------------------
# CALLER PROFILES (TODO-06 section 19).
#
# Section 17 shipped ONE contract because it had exactly two callers and both
# consumed the same subtree. Section 19 routes readers that consume DIFFERENT
# subtrees and none of them touch `stamped_items` at all, which broke the fixed
# contract in two directions at once:
#
#   * `validate_nodes` RAISED `REASON_LEGACY_NO_STAMPED_ITEMS` whenever no node
#     carried `stamped_items`. For `todo-reachability`, which reads only
#     `sections[].n/.status`, that refuses a schema-valid and entirely usable
#     cache. The refusal is a READINESS policy belonging to the two stamped-item
#     readers, and this module's own header already says the population question
#     is the CALLER's -- `CacheInfo.key_present` exists precisely to report it.
#
#   * Conversely, those readers got NO validation of the fields they actually
#     consume, so "routed through the shared validator" would have advertised
#     protection that was never delivered (Codex design review, section 19,
#     [high]).
#
# A `require_stamped_items` boolean was considered and REJECTED as the wrong
# axis: it still validates an irrelevant subtree while leaving the caller's real
# inputs unchecked. The two questions are INDEPENDENT and are declared
# separately here:
#
#   subtrees                    WHAT I READ -- validated for me.
#   require_stamped_population  WHETHER AN EMPTY POPULATION IS FATAL TO ME.
#
# SCOPE IS STILL "FIELDS A READER CONSUMES", unchanged from the header above. A
# subtree earns a validator when a routed reader reads it, never speculatively:
# `inputs_xrefs` is deliberately absent because no reader routed in section 19
# consumes it, and `sections[].depends_on` keeps its bare-array shape for the
# same reason (its consumer, `validate.py`, is section 23). Adding either would
# re-commit the exact overreach the header forbids.
# ---------------------------------------------------------------------------

SUBTREE_STAMPED_ITEMS = "stamped_items"
SUBTREE_SECTIONS = "sections"
SUBTREE_STAMPS_XREFS = "stamps_xrefs"

_KNOWN_SUBTREES = frozenset((
    SUBTREE_STAMPED_ITEMS, SUBTREE_SECTIONS, SUBTREE_STAMPS_XREFS,
))


class Profile:
    """What a caller CONSUMES, declared separately from what it REQUIRES.

    `subtrees` is the set of node subtrees to validate for this caller.
    `require_stamped_population` is a READINESS policy: raise
    `REASON_LEGACY_NO_STAMPED_ITEMS` when no node carries `stamped_items` at
    all. Only a reader that walks stamped items wants that.

    An unknown subtree name is rejected at CONSTRUCTION rather than ignored at
    walk time. A typo'd profile that silently validates nothing is the vacuous
    pass this whole module exists to refuse, and it would be invisible: the
    caller would look routed and be unprotected.
    """

    __slots__ = ("name", "subtrees", "require_stamped_population")

    def __init__(self, name: str, subtrees=(), require_stamped_population=False):
        unknown = sorted(set(subtrees) - _KNOWN_SUBTREES)
        if unknown:
            raise ValueError(
                f"profile {name!r} declares unknown subtree(s) {unknown}; "
                f"known: {sorted(_KNOWN_SUBTREES)}")
        self.name = name
        self.subtrees = frozenset(subtrees)
        self.require_stamped_population = bool(require_stamped_population)


# The section 17 contract, unchanged and still the DEFAULT, so both readers
# routed there keep their shipped reason-to-code mappings byte-for-byte. Making
# the new behavior opt-in is the whole reason Candidate B (change both existing
# readers) was rejected.
PROFILE_STAMPED_ITEMS = Profile(
    "stamped-items", (SUBTREE_STAMPED_ITEMS,), require_stamped_population=True)

# `todo-reachability.py`: reads `sections[].n` and `sections[].status`. A cache
# with no shipped items anywhere is perfectly usable to it.
PROFILE_SECTIONS = Profile("sections", (SUBTREE_SECTIONS,))

# `decision-registry.py`: reads the Accepted/Deferred stamp XREFs.
PROFILE_STAMP_XREFS = Profile("stamp-xrefs", (SUBTREE_STAMPS_XREFS,))


class CacheSchemaError(RuntimeError):
    """The cache cannot support a trustworthy walk.

    Carries a `reason` tag rather than an exit code: the two readers document
    DIFFERENT codes for the same condition and both contracts are load-bearing.
    """

    def __init__(self, reason: str, message: str):
        super().__init__(message)
        self.reason = reason


class CacheInfo:
    """Facts a caller needs for POLICY, which this module deliberately does not
    make. `population` is the total number of stamped items across all nodes;
    `key_present` is whether ANY node carries the `stamped_items` key at all.

    The pair separates two conditions that a single "no stamped items" check
    conflated (Codex design review, section 17, [high]): a LEGACY cache that
    predates the extension (key absent everywhere -- a genuine "run build",
    warn-worthy) from a CURRENT cache whose producer regressed to emitting an
    empty population (key present, zero items -- a regression that must not be
    downgraded to a warning).

    `corpus` is the TODO-corpus fingerprint taken during validation. It is
    handed back so a caller can re-verify it AFTER its own walk via
    `check_corpus_unchanged`. Without that, the generation binding covered only
    the few milliseconds inside `check_freshness`, while the window that
    actually matters is the caller's ~1s symbol-resolution walk (Codex
    adversarial, section 17 review).
    """

    __slots__ = ("population", "key_present", "corpus")

    def __init__(self, population: int, key_present: bool, corpus=None):
        self.population = population
        self.key_present = key_present
        self.corpus = corpus


def _err(reason: str, message: str):
    raise CacheSchemaError(reason, message)


def _require_int(value, lo: int, where: str, field: str, path):
    """Schema says integer with a minimum. `bool` is an `int` in Python and is
    NOT an integer here -- rejected explicitly rather than silently accepted as
    0/1, which would let `section_n: true` become a dict key downstream.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        _err(REASON_SHAPE,
             f"{where} `{field}` is {type(value).__name__}, expected integer "
             f"(per {SCHEMA_REL}): {path}")
    if value < lo:
        _err(REASON_SHAPE,
             f"{where} `{field}` is {value}, expected >= {lo} "
             f"(per {SCHEMA_REL}): {path}")


def _require_str(value, where: str, field: str, path, min_len: int = 0):
    if not isinstance(value, str):
        _err(REASON_SHAPE,
             f"{where} `{field}` is {type(value).__name__}, expected string "
             f"(per {SCHEMA_REL}): {path}")
    if len(value) < min_len:
        _err(REASON_SHAPE,
             f"{where} `{field}` is empty, expected minLength {min_len} "
             f"(per {SCHEMA_REL}): {path}")


_ITEM_KEYS = ("section_n", "item_idx", "item_text", "refs")
_REF_KEYS_FILE = ("kind", "file", "line")
_REF_KEYS_SYMBOL = ("kind", "symbol", "file")
# Section 19 subtrees. Mirrors cache.schema.json; test_build.sh asserts the two
# agree using `jsonschema` as an INDEPENDENT oracle, so a divergence is a test
# failure rather than a silent drift.
_SECTION_KEYS = ("n", "deliverable", "depends_on", "status")
_STAMP_XREF_REQUIRED = ("kind", "severity", "target_path", "target_section")
_STAMP_XREF_KEYS = _STAMP_XREF_REQUIRED + ("item_name",)


def _validate_sections(node, i: int, path):
    """`sections[]` -- the Implementation Order rows. Consumed by
    `todo-reachability.py`, which keys a dict on `n` and compares `status`.

    The schema already constrains this subtree (required n/deliverable/
    depends_on/status, additionalProperties false); until section 19 NOTHING
    enforced it, so the schema was documentation. `n` is nullable per the schema
    -- a row whose Section column is unparseable -- and the reader must cope, so
    a null `n` is VALID here and is the reader's problem, not a shape error.

    `depends_on` is checked only for being an array. Its ITEM shape is consumed
    by `validate.py` (section 23), and validating it here would advertise a
    contract this module does not test.
    """
    if SUBTREE_SECTIONS not in node:
        _err(REASON_SHAPE,
             f"cache node {i} is missing required `sections` "
             f"(per {SCHEMA_REL}): {path}")
    sections = node[SUBTREE_SECTIONS]
    # Same falsey trap as `stamped_items`: `sections: {}` is not a list and must
    # not read as "no sections".
    if not isinstance(sections, list):
        _err(REASON_SHAPE,
             f"cache node {i} `sections` is {type(sections).__name__}, "
             f"expected array (per {SCHEMA_REL}): {path}")
    for j, s in enumerate(sections):
        where = f"cache node {i} section {j}"
        if not isinstance(s, dict):
            _err(REASON_SHAPE,
                 f"{where} is not an object ({type(s).__name__}): {path}")
        for field in _SECTION_KEYS:
            if field not in s:
                _err(REASON_SHAPE,
                     f"{where} is missing required `{field}` "
                     f"(per {SCHEMA_REL}): {path}")
        extra = [k for k in s if k not in _SECTION_KEYS]
        if extra:
            _err(REASON_SHAPE,
                 f"{where} has unknown key(s) {sorted(extra)} "
                 f"(additionalProperties false per {SCHEMA_REL}): {path}")
        # `n` is the DICT KEY the reader builds, so a non-scalar is unhashable
        # and raises TypeError from inside the walk -- the same escape class
        # `section_n` closed on the stamped-items side.
        if s["n"] is not None:
            _require_int(s["n"], 0, where, "n", path)
        _require_str(s["deliverable"], where, "deliverable", path)
        # `.strip()` is called on this by the reader, so a non-string raises
        # AttributeError mid-walk instead of refusing here.
        _require_str(s["status"], where, "status", path)
        if not isinstance(s["depends_on"], list):
            _err(REASON_SHAPE,
                 f"{where} `depends_on` is {type(s['depends_on']).__name__}, "
                 f"expected array (per {SCHEMA_REL}): {path}")


def _validate_stamps_xrefs(node, i: int, path):
    """`stamps_xrefs[]` -- Accepted/Deferred stamp XREFs. Consumed by
    `decision-registry.py`, which reads `kind`, `target_path`, `target_section`
    and the optional `item_name`.

    The schema carried this as a bare `{"type": "array"}` with no item shape, so
    a malformed entry passed both the schema and every reader. Constrained here
    and in the schema together (section 19).
    """
    if SUBTREE_STAMPS_XREFS not in node:
        _err(REASON_SHAPE,
             f"cache node {i} is missing required `stamps_xrefs` "
             f"(per {SCHEMA_REL}): {path}")
    xrefs = node[SUBTREE_STAMPS_XREFS]
    if not isinstance(xrefs, list):
        _err(REASON_SHAPE,
             f"cache node {i} `stamps_xrefs` is {type(xrefs).__name__}, "
             f"expected array (per {SCHEMA_REL}): {path}")
    for j, x in enumerate(xrefs):
        where = f"cache node {i} stamp xref {j}"
        if not isinstance(x, dict):
            # The reader ALREADY skips a non-dict here (`if not isinstance(x,
            # dict): continue`). That defensive skip is exactly the silent
            # narrowing this module refuses: the answer looks complete while
            # entries were dropped.
            _err(REASON_SHAPE,
                 f"{where} is not an object ({type(x).__name__}): {path}")
        for field in _STAMP_XREF_REQUIRED:
            if field not in x:
                _err(REASON_SHAPE,
                     f"{where} is missing required `{field}` "
                     f"(per {SCHEMA_REL}): {path}")
        extra = [k for k in x if k not in _STAMP_XREF_KEYS]
        if extra:
            _err(REASON_SHAPE,
                 f"{where} has unknown key(s) {sorted(extra)} "
                 f"(additionalProperties false per {SCHEMA_REL}): {path}")
        # `.lower()` is called on `kind` by the reader; the other three are
        # compared and joined as strings.
        for field in _STAMP_XREF_REQUIRED:
            _require_str(x[field], where, field, path)
        # `item_name` is OPTIONAL -- 73 of 994 live entries omit it (a stamp
        # clause naming a section rather than an item) -- but when present it
        # must be a usable string.
        if "item_name" in x:
            _require_str(x["item_name"], where, "item_name", path)


def _validate_stamped_items(node, i: int, path):
    """`stamped_items[]` -- the section 17 contract, unchanged.

    Returns `(population_delta, key_present)`. Extracted from `validate_nodes`
    so the per-node walk can dispatch on a profile without this block's early
    `continue` swallowing the other subtrees.
    """
    if SUBTREE_STAMPED_ITEMS not in node:
        # Legal and COMMON: the schema emits `stamped_items` only when a
        # body has at least one [x] item. Measured on the live cache:
        # 156 of 232 nodes have no shipped items and correctly omit it.
        return 0, False
    population = 0
    items = node[SUBTREE_STAMPED_ITEMS]
    # NOT a truthiness test. `stamped_items: {}` is wrong-typed but falsey,
    # so `node.get("stamped_items") or []` treated it as absent and walked
    # zero items while reporting a clean run -- the exact vacuous-pass class
    # section 14 closed on the snapshot side and the lint still carried.
    if not isinstance(items, list):
        _err(REASON_SHAPE,
             f"cache node {i} `stamped_items` is "
             f"{type(items).__name__}, expected array "
             f"(per {SCHEMA_REL}): {path}")

    for j, it in enumerate(items):
        where = f"cache node {i} item {j}"
        if not isinstance(it, dict):
            _err(REASON_SHAPE,
                 f"{where} is not an object "
                 f"({type(it).__name__}): {path}")
        for field in _ITEM_KEYS:
            if field not in it:
                _err(REASON_SHAPE,
                     f"{where} is missing required `{field}` "
                     f"(per {SCHEMA_REL}): {path}")
        extra = [k for k in it if k not in _ITEM_KEYS]
        if extra:
            # additionalProperties: false. An unknown key is producer drift;
            # failing closed here is what forces the schema and the readers
            # to be updated together instead of silently diverging.
            _err(REASON_SHAPE,
                 f"{where} has unknown key(s) {sorted(extra)} "
                 f"(additionalProperties false per {SCHEMA_REL}): {path}")
        # `section_n` is used as a DICT KEY by both readers, so a non-scalar
        # is unhashable and raises TypeError from inside the walk -- which
        # escaped as the tool's documented "a prior verdict changed" code
        # (Codex consistency, section 16).
        _require_int(it["section_n"], 1, where, "section_n", path)
        _require_int(it["item_idx"], 0, where, "item_idx", path)
        _require_str(it["item_text"], where, "item_text", path)

        refs = it["refs"]
        # Same falsey trap as `stamped_items`: `refs: {}` is not a list and
        # must not read as "no refs".
        if not isinstance(refs, list):
            _err(REASON_SHAPE,
                 f"{where} `refs` is {type(refs).__name__}, expected array "
                 f"(per {SCHEMA_REL}): {path}")
        population += 1

        for k, r in enumerate(refs):
            rwhere = f"{where} ref {k}"
            if not isinstance(r, dict):
                _err(REASON_SHAPE,
                     f"{rwhere} is not an object "
                     f"({type(r).__name__}): {path}")
            kind = r.get("kind")
            # `kind` is a schema `const`, not free text: the readers branch
            # on it, so an unknown kind silently drops the ref from every
            # bucket rather than being counted as anything.
            if kind == "file":
                allowed = _REF_KEYS_FILE
                _require_str(r.get("file"), rwhere, "file", path, 1)
                if "line" in r:
                    _require_int(r["line"], 1, rwhere, "line", path)
            elif kind == "symbol":
                allowed = _REF_KEYS_SYMBOL
                _require_str(r.get("symbol"), rwhere, "symbol", path, 1)
                # `file` is OPTIONAL on a symbol ref (absent when the item
                # names a symbol with no nearby file reference), but when
                # present it must be a usable non-empty string: the readers
                # call `.startswith` on it.
                if "file" in r:
                    _require_str(r["file"], rwhere, "file", path, 1)
            else:
                _err(REASON_SHAPE,
                     f"{rwhere} `kind` is {kind!r}, expected 'file' or "
                     f"'symbol' (per {SCHEMA_REL}): {path}")
            extra = [x for x in r if x not in allowed]
            if extra:
                _err(REASON_SHAPE,
                     f"{rwhere} has unknown key(s) {sorted(extra)} "
                     f"(additionalProperties false per {SCHEMA_REL}): "
                     f"{path}")
    return population, True


def validate_nodes(nodes, path, profile: Profile = PROFILE_STAMPED_ITEMS) -> CacheInfo:
    """Validate the subtrees `profile` declares, of an already-parsed cache.

    Split out of `load_and_validate` so a caller holding nodes in memory (and
    the tests) can apply the identical rule without touching the filesystem.

    `profile` defaults to the section 17 contract, so both readers routed there
    are byte-for-byte unaffected by section 19.
    """
    if not isinstance(nodes, list):
        _err(REASON_SHAPE, f"cache is not a JSON array: {path}")
    if not nodes:
        # READINESS, not schema (see the header): an empty array parses cleanly
        # and then walks nothing -- the vacuous pass this module exists to
        # refuse. Distinct from LEGACY: a cache that exists and contains no
        # nodes at all did not come from a working producer, so it is not the
        # "contributor has not built it yet" case either.
        _err(REASON_EMPTY, f"cache is an empty node array: {path}")

    population = 0
    key_present = False

    for i, node in enumerate(nodes):
        if not isinstance(node, dict):
            _err(REASON_SHAPE,
                 f"cache node {i} is not an object "
                 f"({type(node).__name__}): {path}")
        # NODE-LEVEL FIELDS THE WALKS CONSUME ARE IN SCOPE. `file_path` is a
        # required string in the schema and BOTH readers build occurrence keys
        # from it (`node.get("file_path") or "?"`). Left unchecked, a
        # list-valued one is truthy and gets STRINGIFIED into the key, so the
        # snapshot reports a bogus DROPPED/ADDED at rc 1 -- a verdict -- or
        # writes a poisoned baseline, instead of refusing with rc 3 (Codex
        # adversarial, section 17). That is why the scope line is "fields the
        # readers consume", not "the stamped_items subtree".
        if "file_path" not in node:
            _err(REASON_SHAPE,
                 f"cache node {i} is missing required `file_path` "
                 f"(per {SCHEMA_REL}): {path}")
        _require_str(node["file_path"], f"cache node {i}", "file_path", path, 1)

        # PER-SUBTREE DISPATCH. A caller is validated for exactly what it
        # declared it consumes -- no more (that would advertise an untested
        # contract) and no less (that would be routing in name only).
        if SUBTREE_STAMPED_ITEMS in profile.subtrees:
            delta, present = _validate_stamped_items(node, i, path)
            population += delta
            key_present = key_present or present
        if SUBTREE_SECTIONS in profile.subtrees:
            _validate_sections(node, i, path)
        if SUBTREE_STAMPS_XREFS in profile.subtrees:
            _validate_stamps_xrefs(node, i, path)

    if profile.require_stamped_population and not key_present:
        # LEGACY, and reported distinctly from an empty population: this cache
        # predates the section 9 extension or came from a different generator,
        # which is a "rebuild it" condition rather than a regression.
        #
        # GATED ON THE PROFILE (section 19). This is a READINESS policy owned by
        # the callers that walk stamped items; a reader of `sections[]` is
        # perfectly served by a cache where nothing has shipped yet, and raising
        # here would refuse it. `key_present` is still REPORTED to every caller
        # via CacheInfo, which is what the module header always said it was for.
        _err(REASON_LEGACY_NO_STAMPED_ITEMS,
             f"cache carries no stamped_items -- it predates the section 9 "
             f"extension, or came from a different generator: {path}")

    return CacheInfo(population=population, key_present=key_present)


def _scan_corpus(todo_root: Path, on_err):
    """One generation of the TODO corpus: {path: (dev, ino, size, mtime_ns)}."""
    seen = {}
    for dirpath, _, files in os.walk(todo_root, onerror=on_err):
        for f in files:
            if f.startswith("TODO-") and f.endswith(".md"):
                fp = os.path.join(dirpath, f)
                st = os.stat(fp)
                seen[fp] = (st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns)
    return seen


def check_freshness(cache_path: Path, todo_root: Path,
                    cache_mtime: float = None):
    """Refuse a cache older than the newest TODO. FAIL-CLOSED in all three ways
    the lint copy was fail-open (Codex design review, section 17):

      * `os.walk` SWALLOWS traversal errors unless given an `onerror` callback,
        so an unreadable subtree left `newest` low and an old cache looked
        fresh. The snapshot copy passes `onerror`; the lint copy did not.
      * a `stat` failure returned "not stale" instead of raising.
      * zero TODO files found read as fresh, so a cache was called fresh
        against an empty corpus.
    """
    def _walk_err(exc):
        _err(REASON_STALE, f"cannot traverse todo tree: {exc}")

    todo_root = Path(todo_root)
    if not todo_root.is_dir():
        # The lint guarded this with `if todo_root.is_dir()` and SKIPPED the
        # whole staleness check when it was absent -- the fail-open the check
        # exists to close. `todo/` is tracked, so its absence is infrastructure.
        _err(REASON_STALE,
             f"todo root is not a readable directory: {todo_root}")
    def _scan():
        return _scan_corpus(todo_root, _walk_err)

    try:
        # `cache_mtime` is the mtime of the descriptor the caller actually READ.
        # Falling back to a fresh stat is only for a direct caller that has no
        # descriptor; `load_and_validate` always passes the bound value, which
        # is what makes the freshness verdict describe the parsed bytes.
        cache_m = (Path(cache_path).stat().st_mtime if cache_mtime is None
                   else cache_mtime)
        # THE CORPUS IS GENERATION-BOUND TOO, not just the cache. Each TODO used
        # to be statted ONCE into a running `max()`, so a file edited AFTER the
        # walk had already visited it was never observed: its new mtime could
        # not raise `newest`, the cache itself need not change, the descriptor
        # comparison in the caller still passed, and both readers walked stale
        # nodes and produced a VERDICT instead of the documented STALE
        # infrastructure code (Codex adversarial round 2, section 17). Two
        # scans, compared as whole sets, also catch a TODO added or removed
        # mid-walk, which a running maximum structurally cannot see.
        before = _scan()
        newest = max((v[3] for v in before.values()), default=0) / 1e9
    except OSError as exc:
        _err(REASON_STALE, f"cannot determine cache freshness: {exc}")
    if not before:
        _err(REASON_STALE,
             f"no TODO files found under {todo_root} -- refusing to call a "
             f"cache fresh against an empty corpus")
    if newest > cache_m:
        _err(REASON_STALE,
             f"cache is STALE (a TODO is newer than {cache_path}); rebuild via "
             f"scripts/todo-graph/build-and-validate.sh --keep-cache")
    try:
        after = _scan()
    except OSError as exc:
        _err(REASON_STALE, f"cannot determine cache freshness: {exc}")
    _diff_or_ok(before, after, "while freshness was being checked")
    return after


def _diff_or_ok(before, after, when: str) -> None:
    if after == before:
        return
    changed = sorted(set(before) ^ set(after)) or sorted(
        p for p in before if p in after and before[p] != after[p])
    _err(REASON_STALE,
         f"the TODO corpus changed {when} "
         f"({len(changed)} file(s), e.g. {changed[0] if changed else '?'}); "
         f"re-run rather than certify a cache against a moving corpus")


def check_corpus_unchanged(todo_root: Path, corpus) -> None:
    """Re-verify the corpus fingerprint AFTER the caller's own walk.

    THE WINDOW THAT MATTERS IS THE CALLER'S WALK, NOT THIS MODULE'S CHECK. The
    two scans inside `check_freshness` are adjacent, so on their own they bound
    only a few milliseconds -- while both readers then spend ~1s resolving
    symbols, during which a TODO edit would leave the in-memory nodes stale and
    still let a VERDICT be returned (Codex adversarial, section 17 review).
    Callers therefore re-verify here once their walk completes, which is the
    point at which the verdict is actually about to be published.

    A no-op when `corpus` is None (a caller that loaded with `check_stale=False`
    never took a fingerprint and has nothing to compare).
    """
    if corpus is None:
        return

    def _walk_err(exc):
        _err(REASON_STALE, f"cannot traverse todo tree: {exc}")

    try:
        after = _scan_corpus(Path(todo_root), _walk_err)
    except OSError as exc:
        _err(REASON_STALE, f"cannot re-verify cache freshness: {exc}")
    _diff_or_ok(corpus, after, "during the walk")


def load_and_validate(cache_path: Path, todo_root: Path,
                      check_stale: bool = True,
                      profile: Profile = PROFILE_STAMPED_ITEMS):
    """Read, parse and validate the cache. Returns `(nodes, CacheInfo)`.

    MemoryError IS NORMALIZED ACROSS THE WHOLE OPERATION, which is why this is
    a thin wrapper. The bounded read, the UTF-8 decode and `validate_nodes` all
    allocate OUTSIDE the `json.loads` handler, and both readers catch only
    `CacheSchemaError` -- so an allocation failure in any other stage escaped as
    a bare rc 1, which `corpus_resolution_snapshot` DOCUMENTS as "a prior
    verdict changed". An OOM would have been reported to the section 16 gate as
    a resolver REGRESSION (Codex adversarial round 2, section 17). Compact JSON
    also expands several-fold into Python objects, so this is reachable under a
    constrained CI memory budget well before the byte ceiling binds.
    """
    try:
        return _load_and_validate(cache_path, todo_root, check_stale, profile)
    except MemoryError:
        # Deliberately no f-string interpolation of the exception: formatting a
        # message is itself an allocation, and this handler runs precisely when
        # allocation is failing.
        _err(REASON_UNREADABLE,
             "cache could not be loaded: out of memory while reading, "
             "decoding or validating it")


def _load_and_validate(cache_path: Path, todo_root: Path,
                       check_stale: bool = True,
                       profile: Profile = PROFILE_STAMPED_ITEMS):
    """The real body. See `load_and_validate` for the MemoryError contract.

    Raises `CacheSchemaError` with a `reason` tag; the caller maps it to its own
    documented exit code and NEVER lets it escape as a traceback -- an uncaught
    one exits 1, which both readers document as a real verdict rather than an
    infrastructure failure.

    THE READ IS GENERATION-BOUND. The cache is opened ONCE and every later
    decision -- the size budget, the freshness comparison, the rewrite check --
    comes from `fstat` on THAT descriptor rather than a fresh `stat` of the
    pathname. Reading bytes and then separately stat-ing the path is a real
    TOCTOU here: `build.py` rewrites this exact file, so a reader could parse
    the OLD cache, the producer could finish writing the NEW one, and the
    freshness check would certify stale in-memory nodes with the new file's
    mtime (Codex adversarial, section 17). `resolve_symbol` guards source files
    the same way.
    """
    cache_path = Path(cache_path)
    body_ok = False
    try:
        fh = open(cache_path, "rb")
    except FileNotFoundError:
        _err(REASON_MISSING,
             f"cache not found: {cache_path} (run build-and-validate.sh)")
    except OSError as exc:
        _err(REASON_UNREADABLE, f"cache unreadable: {cache_path}: {exc}")
    try:
        try:
            st_before = os.fstat(fh.fileno())
            # BUDGET BEFORE ALLOCATION. `read_text` + `json.loads` materialise
            # the whole file and then its entire object graph with no ceiling,
            # so a hostile or corrupt cache could exhaust memory and kill the
            # process OUTSIDE the documented reason codes -- the one failure
            # mode neither reader can report (Codex adversarial, section 17).
            # Mirrors `resolve_symbol._MAX_FILE_BYTES`.
            if st_before.st_size > _MAX_CACHE_BYTES:
                _err(REASON_UNREADABLE,
                     f"cache is {st_before.st_size} bytes, past the "
                     f"{_MAX_CACHE_BYTES}-byte ceiling: {cache_path}")
            # limit+1 so a file that GREW between fstat and read is caught by
            # the length test below rather than silently truncated into what
            # would look like an ordinary parse error.
            blob = fh.read(_MAX_CACHE_BYTES + 1)
        except OSError as exc:
            _err(REASON_UNREADABLE, f"cache unreadable: {cache_path}: {exc}")
        if len(blob) > _MAX_CACHE_BYTES:
            _err(REASON_UNREADABLE,
                 f"cache grew past the {_MAX_CACHE_BYTES}-byte ceiling while "
                 f"being read: {cache_path}")
        try:
            raw = blob.decode("utf-8")
        except ValueError as exc:
            # UnicodeDecodeError IS a ValueError but neither an OSError nor a
            # JSONDecodeError, so a narrower tuple let it escape as a bare
            # exit 1 on the lint side.
            _err(REASON_UNREADABLE, f"cache unreadable: {cache_path}: {exc}")
        try:
            nodes = json.loads(raw)
        except (ValueError, RecursionError, MemoryError) as exc:
            # RecursionError is NEITHER a ValueError nor an OSError: deeply
            # nested JSON blew the parser stack and escaped the lint's handler
            # entirely. MemoryError is caught for the same reason -- an
            # allocation failure inside the parser must still report as an
            # infrastructure reason rather than a traceback.
            _err(REASON_UNREADABLE, f"cache unreadable: {cache_path}: {exc}")

        info = validate_nodes(nodes, cache_path, profile)
        if check_stale:
            # The fingerprint returned here is handed to the caller so it can
            # re-verify AFTER its own walk (`check_corpus_unchanged`).
            # Compare against the mtime of the descriptor actually READ, never
            # a fresh stat of the name.
            info.corpus = check_freshness(cache_path, todo_root,
                                          cache_mtime=st_before.st_mtime)
            # ...and prove the file did not change underneath the operation. A
            # rewrite means the parsed nodes may already describe a tree that
            # no longer exists, so the honest answer is an infrastructure
            # refusal rather than a verdict computed from mixed generations.
            try:
                st_after = os.fstat(fh.fileno())
                st_path = os.stat(cache_path)
            except OSError as exc:
                _err(REASON_STALE,
                     f"cache became unreadable during validation: "
                     f"{cache_path}: {exc}")
            if (st_after.st_mtime_ns != st_before.st_mtime_ns
                    or st_after.st_size != st_before.st_size
                    or (st_path.st_dev, st_path.st_ino)
                    != (st_before.st_dev, st_before.st_ino)):
                _err(REASON_STALE,
                     f"cache was rewritten while it was being validated (a "
                     f"concurrent build-and-validate.sh?); re-run rather than "
                     f"trust a verdict from mixed generations: {cache_path}")
        body_ok = True
    finally:
        # A CLOSE FAILURE IS ALSO INFRASTRUCTURE. A bare `fh.close()` here sat
        # outside every OSError normalization path, so a raising close escaped
        # as a raw OSError -- the lint exits an undocumented rc 1 and the
        # snapshot exits its DOCUMENTED "a prior verdict changed" code. Worse,
        # an exception from `finally` REPLACES an in-flight CacheSchemaError,
        # turning a precise diagnosis into an opaque one (Codex adversarial,
        # section 17 review), so while the body is already failing the close
        # error is swallowed and the original reason survives.
        #
        # The discriminator is a FUNCTION-LOCAL flag, deliberately NOT
        # `sys.exc_info()` (which the first version of this used). That state
        # is AMBIENT: it also reports an exception being handled by an OUTER
        # caller, so a load invoked from inside someone else's `except` block
        # would look like it was already failing and its close error would be
        # swallowed -- silently certifying a load whose descriptor failed to
        # close (Codex re-adversarial, section 17). `body_ok` is true only when
        # THIS body completed, which is exactly when a close failure is the
        # only thing left to report.
        try:
            fh.close()
        except OSError as exc:
            if body_ok:
                _err(REASON_UNREADABLE,
                     f"cache descriptor failed to close: {cache_path}: {exc}")
    return nodes, info
