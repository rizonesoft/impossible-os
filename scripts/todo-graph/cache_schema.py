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

import hashlib
import json
import os
import re
import stat
import subprocess
from pathlib import Path, PurePosixPath

# Reason tags. Callers map these to their OWN documented exit codes; a caller
# that grows a new code maps it here rather than re-deriving the rule.
REASON_MISSING = "MISSING"                  # cache file absent
REASON_UNREADABLE = "UNREADABLE"            # unreadable or not parseable JSON
REASON_SHAPE = "SHAPE"                      # parsed, but violates the schema
REASON_EMPTY = "EMPTY"                      # readiness: no nodes to walk at all
REASON_LEGACY_NO_STAMPED_ITEMS = "LEGACY"   # predates the section 9 extension
REASON_STALE = "STALE"                      # older than the newest TODO
REASON_LEGACY_FORMAT = "LEGACY_FORMAT"      # another producer contract

# Path to the schema these rules implement. Quoted in errors so a failure names
# the contract it violated instead of just the offending value.
SCHEMA_REL = "scripts/todo-graph/schema/cache.schema.json"

# Input budget, enforced BEFORE the file is materialised. The live cache is
# ~3.3 MiB across 232 nodes, so 64 MiB is ~19x headroom while still bounding a
# hostile input. Same role as `resolve_symbol._MAX_FILE_BYTES` (16 MiB), larger
# because this is the whole corpus in one document rather than one source file.
_MAX_CACHE_BYTES = 64 * 1024 * 1024

# Section-number ceiling. THIS IS A CPU BUDGET, not tidiness, and it is the
# companion the byte ceiling above was missing: the byte ceiling bounds memory
# while leaving CPU unbounded. Consumers put these integers into a `set` (the
# uniqueness rule below) and into `dict` keys, and CPython hashes an int as
# `value % (2**61 - 1)` -- so values separated by that modulus COLLIDE, and a
# schema-valid cache full of colliding numbers degrades those O(1) operations
# to O(n). Measured by Codex perf review, section 19: doubling from 4,000 to
# 8,000 colliding sections took validation from 0.143s to 0.588s from a 611 KB
# input, which scales into the every-commit lint's 600s timeout well inside the
# 64 MiB budget. Bounding the VALUE closes it outright -- every number below the
# modulus hashes to itself, so collisions cannot be engineered -- and it bounds
# the COUNT as a side effect, because duplicates are refused on first repeat.
#
# 65535 against a live maximum of 62 (TODO-04) and a documented 60-section hard
# cap in CLAUDE.md: ~1000x headroom, so no real corpus can reach it.
_MAX_SECTION_N = 65535


# ---------------------------------------------------------------------------
# CACHE-FORMAT IDENTITY (TODO-06 section 25).
#
# `schema_version` on a NODE is the TODO's own frontmatter generation, copied
# straight out of the file (build.py:1124). Nothing identified the ARTIFACT's
# own contract, so no reader could tell a cache written by today's producer
# from one written months ago -- which is why `validate.py --diff` could only
# ASK the caller to regenerate its baseline and then validate it on SHAPE
# alone. A field that kept its type and changed its meaning passed that, and
# manufactured or suppressed graph deltas a human then actioned.
#
# TWO VALUES, BECAUSE ONE CANNOT DO THE JOB. Neither half is decoration:
#
#   CACHE_FORMAT_VERSION      hand-bumped for a SEMANTIC change -- a field that
#                             keeps its name and type and changes its meaning.
#                             No digest can see that.
#   PRODUCER_CONTRACT_DIGEST  derived from the declaration below, so a
#                             STRUCTURAL change (a field added, removed or
#                             renamed) invalidates old artifacts even when the
#                             human forgets to bump the integer.
#
# The digest is taken over the DECLARATION, never over an observed cache's
# keys: hashing output would make the identity a function of whatever the
# producer happened to emit on that corpus, so a node-shape regression would
# quietly re-certify itself. Same "declare, do not infer" rule section 20
# established for `ref_resolution.EMITTED_BUCKETS`, and for the same reason.
#
# IT RIDES IN THE CORPUS BINDING SIDECAR, not in the cache root. A root envelope
# was designed first and REJECTED on measurement: it makes the cache
# self-describing, which is the nicer property, but the root is a bare array
# that tools outside this package parse directly -- `decision-registry.py:67`
# and two of its tests -- and JSON offers no shape that is both an object and a
# list, so there is no compatibility path for them. The sidecar already carries
# every other artifact-level fact (`SIDECAR_SCHEMA`, `cache_sha256`,
# `history_id`), and its FILENAME is the cache digest, so an identity recorded
# there is bound to exactly the bytes it describes -- the same binding strength
# a root key would have had.
#
# THE CHECK IS NOT A PROFILE AXIS. `Profile` declares what a caller CONSUMES and
# what it REQUIRES of the corpus; this is neither. It is a fact about the
# artifact in front of this read, exactly like the generation binding at the
# foot of `_load_and_validate`. A reader that could opt out of identity is a
# reader that can be handed an unidentifiable cache.
# v1 -> v2 (section 27): `history_id` KEEPS ITS NAME AND TYPE AND CHANGED ITS
# MEANING, which is the exact case the paragraph above reserves this integer
# for. A v1 producer computed the tip and the count under the EFFECTIVE history
# -- replacements and grafts applied; a v2 producer computes them with both
# neutralised. The two strings compare EQUAL in precisely the case that matters (a
# replacement preserving tip and count), so without the bump a v2 reader would
# accept a v1 cache whose timestamps came from a replaced history, and a v1
# reader would accept a v2 cache after a rollback -- both silently, because no
# digest can see a meaning change (Codex adversarial, section 27, [high]).
CACHE_FORMAT_VERSION = 2

# Every key the producer may place on a node, WITH THE MODE IT IS EMITTED IN.
# `build.py` imports this and refuses to emit a node carrying anything outside
# it, so the declaration and the emission cannot drift apart silently;
# `cache.schema.json` lists the same names with their types. Adding a field here
# is a STRUCTURAL change and moves the digest, which is the intended migration
# path.
#
# THE MODE IS PART OF THE DECLARATION, not a build.py-local detail, and that is
# a section-25 review correction. A bare name set FLATTENS requiredness, so a
# coordinated edit could move `sections` from always-emitted to conditional --
# in the producer's tuples AND in `cache.schema.json`'s `required` list -- and
# every check still passed: the name set was unchanged, so the union assert
# held and `PRODUCER_CONTRACT_DIGEST` was identical. Nodes missing a formerly
# required field then published under the SAME identity as the old contract, so
# a reader could not tell the two apart and fell through to a shape refusal or a
# silent default (Codex re-adversarial, section 25, rounds 4-5, [medium]).
#
# Reclassification is exactly the "keeps its name and type, changes its meaning"
# case the hand-bumped `CACHE_FORMAT_VERSION` is meant to cover -- but the whole
# reason a digest sits beside that integer is that humans forget it. Putting the
# mode in the digest moves this class out of the remembers-to-bump bucket and
# into the machine-catches bucket, which is the split the two values exist for.
FIELD_ALWAYS = "always"            # on every node; mirrors cache.schema.json `required`
FIELD_CONDITIONAL = "conditional"  # emitted only when non-empty, by contract
FIELD_OPTIONAL = "optional"        # copied from frontmatter when authored

EMITTED_NODE_FIELD_MODES = {
    "id": FIELD_ALWAYS,
    "schema_version": FIELD_ALWAYS,
    "domain": FIELD_ALWAYS,
    "status": FIELD_ALWAYS,
    "title": FIELD_ALWAYS,
    "file_path": FIELD_ALWAYS,
    "created_at": FIELD_ALWAYS,
    "last_active_at": FIELD_ALWAYS,
    "sections": FIELD_ALWAYS,
    "section_headings": FIELD_ALWAYS,
    "inputs_xrefs": FIELD_ALWAYS,
    "stamps_xrefs": FIELD_ALWAYS,
    "stamped_items": FIELD_CONDITIONAL,
    "effort": FIELD_OPTIONAL,
    "owners": FIELD_OPTIONAL,
    "depends_on": FIELD_OPTIONAL,
    "satisfies": FIELD_OPTIONAL,
    "superseded_by": FIELD_OPTIONAL,
    "file_patterns": FIELD_OPTIONAL,
}
EMITTED_NODE_FIELDS = frozenset(EMITTED_NODE_FIELD_MODES)

# THE MODE DOMAIN IS CLOSED, and this assert is what makes the derived sets an
# exhaustive PARTITION rather than three filters that happen to cover the map.
# Every producer-side guard selects by exact mode equality, so a value outside
# the three -- a plain typo, `"conditionl"` -- puts a field in none of the
# always/conditional/optional sets while leaving it in `EMITTED_NODE_FIELDS`.
# The name-set check still passes, the schema-required and optional-frontmatter
# comparisons still pass because neither set changed, and the digest happily
# hashes the unrecognized mode: the contract is certified and the field is
# guarded by nothing, so a later lost emission is invisible again (Codex
# re-adversarial, section 25, round 6, [medium]).
_FIELD_MODES = frozenset((FIELD_ALWAYS, FIELD_CONDITIONAL, FIELD_OPTIONAL))
_BAD_MODES = sorted((f, m) for f, m in EMITTED_NODE_FIELD_MODES.items()
                    if m not in _FIELD_MODES)
if _BAD_MODES:
    raise AssertionError(
        f"EMITTED_NODE_FIELD_MODES declares unknown mode(s) {_BAD_MODES}; "
        f"every value must be one of {sorted(_FIELD_MODES)}, or the field is "
        f"certified by the contract digest while no producer guard selects it")
_PARTITION = frozenset(
    f for f, m in EMITTED_NODE_FIELD_MODES.items() if m in _FIELD_MODES)
if _PARTITION != EMITTED_NODE_FIELDS:
    raise AssertionError(
        f"the always/conditional/optional sets do not cover "
        f"EMITTED_NODE_FIELDS; unguarded={sorted(EMITTED_NODE_FIELDS - _PARTITION)}")

# Sidecar keys carrying the identity. Named constants because the producer
# writes them and this module reads them; a retyped literal on one side is how
# a contract check comes to validate a field nobody emits.
CACHE_FORMAT_VERSION_KEY = "cache_format_version"
PRODUCER_CONTRACT_DIGEST_KEY = "producer_contract_digest"


def _producer_contract_digest() -> str:
    """sha256 over the canonical declaration -- version AND field set together.

    Both inputs are in the same digest deliberately: a bumped version with an
    unchanged field set must still invalidate old artifacts, and a changed
    field set must invalidate them even at the same version. `sort_keys` plus
    the sorted field list make the value independent of set iteration order,
    which is randomized per interpreter run.
    """
    payload = json.dumps({
        "cache_format_version": CACHE_FORMAT_VERSION,
        "node_fields": sorted(EMITTED_NODE_FIELDS),
        # MODES TOO, so a required-to-conditional reclassification moves the
        # digest even though the name set is untouched. `node_fields` is kept
        # alongside rather than derived from this, so the payload still states
        # the name set explicitly and a mode map that lost a key changes both.
        "node_field_modes": sorted(EMITTED_NODE_FIELD_MODES.items()),
    }, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(payload.encode("utf-8")).hexdigest()


PRODUCER_CONTRACT_DIGEST = _producer_contract_digest()

# Gantt row duration (TODO-06 section 25). THE GRAMMAR LIVES HERE, in the module
# the producer and every routed reader already import, because three copies of
# it is how `build.py` comes to accept a value `render.py` cannot draw. Narrower
# than Mermaid's full duration syntax on purpose: a roadmap needs whole days and
# weeks, and every unit this rejects is one that cannot reach the emitted line.
# Anchored, and a leading zero is refused -- `0d` is a zero-length task.
# `\Z`, NOT `$`, AND THIS IS THE WHOLE POINT OF THE GUARD. Python's `$` matches
# at a trailing newline as well as at end-of-string, so `^[1-9][0-9]*[dw]$`
# ACCEPTS "2w\n" -- measured, not reasoned about. That is exactly the value this
# grammar exists to refuse: the renderer interpolates it into
# `  id :status, id, start, <effort>`, and an embedded newline ends the row
# early and silently reshapes the chart. `\Z` is end-of-string with no
# exception. The JSON Schema mirror in cache.schema.json is unaffected -- ECMA
# regex `$` has no such newline behaviour without the `m` flag -- so the two
# spellings differ on purpose rather than by drift.
EFFORT_REGEX = re.compile(r"\A[1-9][0-9]*[dw]\Z")
EFFORT_DEFAULT = "1w"


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
# subtree earns a validator when a routed reader reads it, never speculatively.
#
# THE TWO EXAMPLES THIS BLOCK USED TO GIVE HAVE BOTH EXPIRED, and leaving them
# would have turned a TIMING rule into a prohibition. Section 19 wrote that
# `inputs_xrefs` was "deliberately absent because no reader routed in section 19
# consumes it" and that `sections[].depends_on` "keeps its bare-array shape for
# the same reason (its consumer, `validate.py`, is section 23)". Section 22
# routed `query.py`, which consumes both: the group item shape became
# `SUBTREE_SECTION_DEPS`, and the `inputs_xrefs` entry shape moved inside
# `_validate_node_fields` (:713-749). Section 23 is the section that block
# named, and it arrived to find the comment still describing the pre-22 world.
#
# The live example is now `section_headings`, and it moved the OTHER way in this
# very section: it was unvalidated because no routed reader dereferenced it, and
# routing `validate.py` -- which does, at validate.py:644 and :678 -- is what
# earned it `SUBTREE_SECTION_HEADINGS` below. That is the rule working: a
# subtree is validated in the section its consumer routes, not before and not
# after.
#
# ONE PROFILE IS DEFINED WITHOUT A ROUTED CONSUMER, and saying so is the point.
# `PROFILE_STAMP_XREFS` is NOT wired to anything: its intended consumer,
# `scripts/overnight/decision-registry.py`, lives under `scripts/overnight/**`,
# which an unattended run may not edit, so section 19 filed the routing instead
# of doing it. An earlier revision of this block named that file as the
# consumer in the present tense, which was false in two ways at once -- the
# reader is not routed here, and it dereferences `target`/`target_file`/`text`/
# `raw` (decision-registry.py:76-77) while the producer emits `target_path`/
# `target_section`/`item_name`, which is a filed defect in its own right
# (overnight-runner-improvements-v10). The SHAPE below is still derived from
# the producer's live output rather than guessed -- all 994 entries in the live
# cache satisfy it, and the fixtures pin it -- but until the control-plane
# routing lands, the profile is available and unused. Codex consistency,
# section 19 review, [high].
# ---------------------------------------------------------------------------

SUBTREE_STAMPED_ITEMS = "stamped_items"
SUBTREE_SECTIONS = "sections"
SUBTREE_STAMPS_XREFS = "stamps_xrefs"
# NOT a subtree, and deliberately not spelled like one any more. The entry
# shape is validated INSIDE `_validate_node_fields`, so a `SUBTREE_`-prefixed
# name for it sat in the public vocabulary while appearing in no registry, no
# profile and no dispatch -- a member no caller could ever select (Codex
# consistency, section 23 review, [medium]).
_INPUTS_XREFS_KEY = "inputs_xrefs"
# Section 22. Two subtrees that are NOT node keys but named walks, which is why
# they carry a qualified spelling: `sections.depends_on` is the GROUP shape
# inside the `sections` subtree, and `node_fields` is the set of node-level
# scalars and edge collections the readers consume.
SUBTREE_SECTION_DEPS = "sections.depends_on"
SUBTREE_NODE_FIELDS = "node_fields"
# Section 23. The `## N.` headings parsed out of the body, as distinct from the
# Implementation Order ROWS in `sections[]` -- the two disagree exactly when a
# file has a section body with no IO row or an IO row with no body, which is
# what `validate.py`'s dangling-section and orphan-IO-row checks exist to find.
SUBTREE_SECTION_HEADINGS = "section_headings"
# The CROSS-NODE uniqueness rules -- `id` and the derived resolver key -- split
# out of `node_fields`, where they had been bundled with the per-node field
# shapes. They are not a shape at all: they are a question about the SET, and
# more importantly they are a question SOME READERS ANSWER THEMSELVES.
#
# A reader that keys a dict on `id` cannot survive a duplicate (the second node
# silently replaces the first, so an edge naming either resolves to whichever
# won), and for those readers a refusal is the only safe answer. But
# `validate.py` REPORTS duplicate ids -- that is its duplicate-id check -- and
# it refuses to resolve an ambiguous resolver key rather than guessing
# (`build_path_index` records `dn_collisions` / `stem_collisions` for exactly
# that). Holding it to these rules deleted its own check and re-reported the
# finding as an infrastructure refusal at the wrong exit code: a corpus problem
# a human must fix, announced as a broken tool. Caught by the existing
# duplicate-id fixture the moment `validate.py` was routed.
SUBTREE_NODE_IDENTITY = "node_identity"

_KNOWN_SUBTREES = frozenset((
    SUBTREE_STAMPED_ITEMS, SUBTREE_SECTIONS, SUBTREE_STAMPS_XREFS,
    SUBTREE_SECTION_DEPS, SUBTREE_NODE_FIELDS, SUBTREE_SECTION_HEADINGS,
    SUBTREE_NODE_IDENTITY,
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

    `requires_history` is the third, independent axis: whether this caller
    consumes the fields `build.py` derives from `git log` (`created_at` /
    `last_active_at`). Only a caller that reads them cares whether the corpus
    HISTORY moved, and establishing that costs FOUR git subprocesses -- 47.9ms
    measured 2026-08-08, growing with TOTAL REPO commits (Codex perf, section
    21, [medium]: two routed readers were each paying it per lint for data
    neither consumes).

    THE SECTION 21 FIGURES THIS COMMENT USED TO CARRY ARE SUPERSEDED, and both
    were wrong in a way that mattered: it said three subprocesses (it is four)
    and that the cost grows with CORPUS-TOUCHING commits (it grows with total
    repo commits, so the crossover arrives ~1.4x sooner). Section 24 re-measured
    rather than reusing them, which is the only reason the model was corrected;
    the full measurement and its method live on `corpus_history_id` below.
    A history-consuming caller pays ONE probe per invocation, not two -- see
    `check_freshness`'s `history_out`.

    The producer records the history id unconditionally, so turning this on for
    a future profile needs no migration -- the evidence is already in every
    binding.
    """

    __slots__ = ("name", "subtrees", "require_stamped_population",
                 "requires_history")

    def __init__(self, name: str, subtrees=(), require_stamped_population=False,
                 requires_history=False):
        unknown = sorted(set(subtrees) - _KNOWN_SUBTREES)
        if unknown:
            raise ValueError(
                f"profile {name!r} declares unknown subtree(s) {unknown}; "
                f"known: {sorted(_KNOWN_SUBTREES)}")
        self.name = name
        self.subtrees = frozenset(subtrees)
        self.require_stamped_population = bool(require_stamped_population)
        self.requires_history = bool(requires_history)


# The section 17 contract, unchanged and still the DEFAULT, so both readers
# routed there keep their shipped reason-to-code mappings byte-for-byte. Making
# the new behavior opt-in is the whole reason Candidate B (change both existing
# readers) was rejected.
PROFILE_STAMPED_ITEMS = Profile(
    "stamped-items", (SUBTREE_STAMPED_ITEMS,), require_stamped_population=True)

# `todo-reachability.py`: reads `sections[].n` and `sections[].status`. A cache
# with no shipped items anywhere is perfectly usable to it.
PROFILE_SECTIONS = Profile("sections", (SUBTREE_SECTIONS,))

# The Accepted/Deferred stamp XREFs, on their own. The PROFILE is still unwired
# -- `decision-registry.py` is control plane and its routing is filed, not done
# -- but the SUBTREE is no longer unconsumed: `PROFILE_QUERY` below declares it,
# because `query.py` walks `stamps_xrefs` for `deferred` / `deferred-by` /
# `backlinks` (query.py:347, 591, 628, 752). Section 19 said "no reader is
# routed through this yet"; section 22 is when that stopped being true.
PROFILE_STAMP_XREFS = Profile("stamp-xrefs", (SUBTREE_STAMPS_XREFS,))

# `query.py` (and `render.py` behind `cmd_render`). THE WIDEST PROFILE, because
# the query CLI is the widest reader: it walks the section rows, the dependency
# groups inside them, the stamp XREFs, and the node-level scalars and edge
# collections. Each of those was a `_safe_list()` or an `isinstance(...):
# continue` in the reader -- a silent narrowing that produced a complete-looking
# answer from a partial cache.
#
# `requires_history` is FALSE, and section 24 PINNED it there rather than
# leaving it unset -- but the axis moved from the READER to the VERB, because
# the reader-wide answer was wrong in both directions. Section 22 recorded that
# "`query.py` sorts `stale` / `deferred` by `last_active_at`"; that is WRONG
# about `deferred`, which never dereferences a timestamp (`cmd_deferred` at
# query.py:1765 and its body read status and XREFs only). The actual consumers
# are three of thirteen commands, and they are named on PROFILE_QUERY_HISTORY
# below. So this profile stays history-blind and costs nothing extra, and the
# ten verbs routed through it -- ready, deferred, blocked, blocking, backlinks,
# code, code-by, by-domain, orphans, deferred-by -- pay no history probe at all.
#
# ONE TUPLE, TWO PROFILES. `PROFILE_QUERY_HISTORY` below differs from this
# profile on the history axis ALONE, so the subtrees are named once here rather
# than repeated. Repeating them meant a later widening of one could silently
# leave the other narrower, and the same reader would then accept under `stale`
# a cache it rejects under `ready` (Codex consistency, section 24, [medium]).
_QUERY_SUBTREES = (SUBTREE_SECTIONS, SUBTREE_SECTION_DEPS,
                   SUBTREE_STAMPS_XREFS, SUBTREE_NODE_FIELDS,
                   SUBTREE_NODE_IDENTITY)

PROFILE_QUERY = Profile("query", _QUERY_SUBTREES)

# THE SAME READER, ONE AXIS STRICTER, SELECTED PER VERB (section 24). Identical
# subtrees to PROFILE_QUERY -- the walk is the same, so narrowing or widening it
# here would be a second contract to keep in sync for no reason -- with
# `requires_history` ON.
#
# THE COST IS WHY THIS IS NOT JUST A FLAG ON PROFILE_QUERY. Measured 2026-08-08
# on the live corpus: `corpus_history_id` is 47.9ms median, of which
# `git rev-list --count HEAD -- .` alone is 44.2ms, against a 169ms `query.py`
# invocation. Turning it on reader-wide would put ~28% on EVERY query to protect
# fields that ten of the thirteen commands never read. Selected per verb, the
# three that do read them pay it and the rest do not.
#
# THE CONSUMERS, each confirmed by dereference and not by name:
#   `stale`  -- cmd_stale (query.py:893) sorts and filters on `last_active_at`.
#   `stats`  -- cmd_stats (query.py:1009, :1028, :1034) computes the
#               longest-deferred table from `last_active_at`.
#   `render --render-format gantt` -- render.py:375 uses `created_at` as each
#               bar's start date, reached through cmd_render (query.py:1143).
# That third one is the reason this comment lists dereferences rather than verb
# names: an inventory built by grepping `query.py` alone MISSED it, because the
# renderer lives in a different module behind a CLI-only command (Codex design
# review, section 24, [medium]).
PROFILE_QUERY_HISTORY = Profile("query-history", _QUERY_SUBTREES,
                                requires_history=True)

# `validate.py`'s CURRENT cache (section 23). Everything `PROFILE_QUERY` walks,
# plus `section_headings`, which no earlier reader dereferenced and this one
# does: `validate.py:644` and `:678` both build `{h["n"] for h in
# node["section_headings"]}`. Left out, a non-dict entry raises TypeError and a
# dict missing `n` raises KeyError, and BOTH escape the walk as an uncaught
# traceback -- exit 1, which this validator documents as GRAPH FINDINGS. That
# collision is the whole subject of section 23, so a profile that reproduced it
# one field over would be routing in name only.
#
# `requires_history` is FALSE: `validate.py` reads neither `created_at` nor
# `last_active_at` (grep-confirmed across all nine checks and the diff walk),
# so it must not pay section 21's three git subprocesses per lint.
#
# `SUBTREE_NODE_IDENTITY` IS DELIBERATELY ABSENT, and it is the only profile
# here that omits it. `validate.py` REPORTS duplicate ids as a graph finding and
# refuses ambiguous resolver keys inside `build_path_index`, so taking the
# module's refusals would delete its own check and re-report a corpus problem as
# a broken tool. Declaring what a caller consumes cuts both ways: this one
# consumes the field SHAPES and owns the SET question itself.
PROFILE_VALIDATE = Profile(
    "validate",
    (SUBTREE_SECTIONS, SUBTREE_SECTION_DEPS, SUBTREE_STAMPS_XREFS,
     SUBTREE_NODE_FIELDS, SUBTREE_SECTION_HEADINGS),
)

# `validate.py --diff BASELINE` (section 23). The SAME file format read under
# different readiness rules, which is the case the caller-declared profiles API
# was built for -- and the second cache section 19's inventory missed entirely.
#
# NARROWER than PROFILE_VALIDATE by two subtrees, because `diff_caches`
# (validate.py:1151-1245) consumes strictly less: it reads `id`, `status`,
# `file_path`, `depends_on`, `satisfies`, `superseded_by`, `inputs_xrefs`,
# `stamps_xrefs[].target_path` and `sections[].depends_on[].target`, and never
# `sections[].n` / `.status` / `.deliverable` nor `section_headings`. Declaring
# either would hold a historical artifact to a contract this walk does not read.
#
# THE FRESHNESS AXIS IS THE POINT, and it is the CALLER's to set: the baseline
# is DELIBERATELY old, and it is not even built from this checkout's corpus --
# `.github/workflows/todo-graph.yml:89` builds it from a worktree of the PR base
# with its own `--root`. A current-corpus staleness verdict against it is not a
# strict check, it is a meaningless one, so the caller passes `check_stale=False`
# (the generation binding on the bytes read is unconditional and is retained).
#
# NO CROSS-VERSION COMPATIBILITY IS CLAIMED, and that is deliberate rather than
# unfinished. There is no cache-FORMAT version to gate on: `schema_version`
# (build.py:1020) is the per-node TODO frontmatter version, not an identity for
# the artifact as a whole. Shape alone cannot detect a field that kept its type
# and changed its MEANING, so the honest contract is the narrow one -- THE
# BASELINE MUST COME FROM THE CURRENT `build.py`, which is exactly what CI does.
# That is a REQUIREMENT ON THE CALLER, not a check this profile performs, and
# the distinction is the whole point: an older artifact that still satisfies
# every consumed field's SHAPE is accepted here and can manufacture deltas. An
# earlier revision of this comment said such an artifact "is REFUSED", which
# contradicted the sentence above it and promised a guarantee no code delivers
# (Codex consistency, section 23 review, [medium]). Making it enforceable needs
# a cache-format identity from the producer, filed against the section that
# owns fields the readers consume and the producer never emits.
# It DOES declare `node_identity`, where `PROFILE_VALIDATE` does not, and the
# asymmetry is the point: `diff_caches` builds `build_id_index(baseline_nodes)`
# and keys a dict on the baseline's ids, so a duplicate there silently rebinds
# a backlink and manufactures a delta -- and a duplicate id in the HISTORICAL
# corpus is not a finding this run reports, because the run's job is the
# CURRENT tree. The current cache reports; the baseline refuses.
PROFILE_BASELINE = Profile(
    "baseline",
    (SUBTREE_SECTION_DEPS, SUBTREE_STAMPS_XREFS, SUBTREE_NODE_FIELDS,
     SUBTREE_NODE_IDENTITY),
)


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

    `history` is the same idea one axis over, and it was MISSING (Codex design
    review, section 24, [medium]). `check_freshness` validates the history id for
    a `requires_history` profile at one instant and then discarded it, so the
    post-walk re-verification covered TODO BYTES ONLY: a rebase or a deepening
    landing during a query left the content identical, the derived timestamps
    stale, and the verdict publishable. It is None for a profile that does not
    consume the derived timestamps -- those callers have nothing to re-check and
    must not pay for the probe.

    IT HOLDS THE RECORDED ID, AND CALLING `check_history_unchanged` WITH IT IS
    NOT OPTIONAL. Since the load defers the history comparison to that one
    post-walk probe (Codex perf, section 24, [medium]), `load_and_validate`
    alone no longer refuses a moved history -- the pair does. A caller that
    declares `requires_history` and then never re-checks gets NO history
    protection, silently. There is exactly one such caller (`query.py`), the
    pairing is pinned by a fixture, and any new one must follow it.
    """

    __slots__ = ("population", "key_present", "corpus", "history")

    def __init__(self, population: int, key_present: bool, corpus=None,
                 history=None):
        self.population = population
        self.key_present = key_present
        self.corpus = corpus
        self.history = history


def _err(reason: str, message: str):
    raise CacheSchemaError(reason, message)


def _require_int(value, lo: int, where: str, field: str, path, hi: int = None):
    """Schema says integer with a minimum. `bool` is an `int` in Python and is
    NOT an integer here -- rejected explicitly rather than silently accepted as
    0/1, which would let `section_n: true` become a dict key downstream.

    `hi` is the CPU budget described at `_MAX_SECTION_N`, applied to every field
    a consumer uses as a set member or dict key.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        _err(REASON_SHAPE,
             f"{where} `{field}` is {type(value).__name__}, expected integer "
             f"(per {SCHEMA_REL}): {path}")
    if value < lo:
        _err(REASON_SHAPE,
             f"{where} `{field}` is {value}, expected >= {lo} "
             f"(per {SCHEMA_REL}): {path}")
    if hi is not None and value > hi:
        _err(REASON_SHAPE,
             f"{where} `{field}` is {value}, past the {hi} ceiling -- consumers "
             f"hash it as a set member or dict key, and unbounded values can be "
             f"chosen to collide (per {SCHEMA_REL}): {path}")


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
# Closed by CONSTRUCTION in the producer: `build.py:821` matches
# `\[([ x/])\]` and falls back to "". Four values, no others reachable.
_SECTION_STATUSES = frozenset(("", " ", "x", "/"))
_STAMP_XREF_REQUIRED = ("kind", "severity", "target_path", "target_section")
_STAMP_XREF_KEYS = _STAMP_XREF_REQUIRED + ("item_name",)
# Section 22 subtrees -- the `sections[].depends_on` GROUP shape and the
# node-level fields, both consumed by `query.py`. Shapes were MEASURED against
# the 232 live nodes before being written, not guessed: all 2,529 dep groups
# carry exactly `{sections, target}` with a string target and integer section
# numbers, and all 945 `inputs_xrefs` entries carry exactly
# `{target_path, target_section}`.
_SECTION_DEP_KEYS = ("target", "sections")
_INPUTS_XREF_KEYS = ("target_path", "target_section")
# Node fields the readers consume that the schema marks REQUIRED. All are
# nullable per the schema except `domain`; `query.py` compares and joins them as
# strings, so a non-string is a shape error rather than the reader's problem.
_NODE_REQUIRED_STR_FIELDS = ("id", "status", "domain", "title",
                             "created_at", "last_active_at")
# Node fields the schema marks OPTIONAL. VALIDATED ONLY WHEN PRESENT -- absence
# is the norm rather than a defect, and this is the measurement that matters
# most here: `depends_on`, `satisfies`, `superseded_by` and `owners` are absent
# from ALL 232 live nodes and `file_patterns` from 231 of them, because
# `build.py:1028` copies each one only when the authored frontmatter carries it.
# Requiring any of them would refuse every cache a working producer emits.
_NODE_OPTIONAL_STR_LISTS = ("depends_on", "satisfies", "file_patterns")


def _validate_sections(node, i: int, path):
    """`sections[]` -- the Implementation Order rows. Consumed by
    `todo-reachability.py`, which keys a dict on `n` and compares `status`.

    The schema already constrains this subtree (required n/deliverable/
    depends_on/status, additionalProperties false); until section 19 NOTHING
    enforced it, so the schema was documentation. `n` is nullable per the schema
    -- a row whose Section column is unparseable -- and the reader must cope, so
    a null `n` is VALID here and is the reader's problem, not a shape error.

    `depends_on` is checked only for being an array HERE. Its ITEM shape has its
    own subtree, `SUBTREE_SECTION_DEPS`, so a caller that opens the groups
    declares that separately -- see `_validate_section_deps`. Section 19 wrote
    that the item shape was "consumed by validate.py (section 23)"; that was
    incomplete, because `query.py:357-366` opens the same groups, and section 22
    found it while routing that reader. `PROFILE_SECTIONS` is deliberately
    unchanged: `todo-reachability.py` reads `n`/`status` and never a group, so
    holding it to the item shape would be the unconsumed contract this module
    forbids.
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
    seen_n = set()
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
        # UNIQUENESS IS LOAD-BEARING, not tidiness. `todo-reachability` builds
        # `{sec["n"]: status}`, so two rows sharing an `n` SILENTLY OVERWRITE --
        # a shape-valid but hostile cache with a duplicate `n: 1` can replace a
        # section's real status and suppress an `open-in-done` finding while the
        # audit reports success (Codex adversarial, section 19, [medium]).
        if s["n"] is not None:
            _require_int(s["n"], 0, where, "n", path, hi=_MAX_SECTION_N)
            if s["n"] in seen_n:
                _err(REASON_SHAPE,
                     f"{where} repeats section number {s['n']} already seen in "
                     f"this node; consumers key a dict on it, so a duplicate "
                     f"silently overwrites: {path}")
            seen_n.add(s["n"])
        _require_str(s["deliverable"], where, "deliverable", path)
        # STATUS IS A CLOSED DOMAIN, not free text. `build.py:821` extracts it
        # with `re.search(r"\[([ x/])\]", ...)` and emits "" when no marker
        # matched, so the producer can emit EXACTLY these four values --
        # confirmed across all 2,394 sections in the live cache. The consumer
        # tests `status not in ("x", "/")`, so ANY unrecognised value reads as
        # "not done": an untrusted cache saying `"[x]"` or `"wat"` is silently
        # treated as unfinished rather than refused. Accepting a value no
        # producer emits is how a shape check stops being a contract.
        _require_str(s["status"], where, "status", path)
        if s["status"] not in _SECTION_STATUSES:
            _err(REASON_SHAPE,
                 f"{where} `status` is {s['status']!r}, expected one of "
                 f"{sorted(_SECTION_STATUSES)} (per {SCHEMA_REL}): {path}")
        if not isinstance(s["depends_on"], list):
            _err(REASON_SHAPE,
                 f"{where} `depends_on` is {type(s['depends_on']).__name__}, "
                 f"expected array (per {SCHEMA_REL}): {path}")


def _validate_section_deps(node, i: int, path):
    """`sections[].depends_on` ITEM shape -- the dependency GROUPS.

    A SEPARATE SUBTREE from `sections`, deliberately. `_validate_sections` checks
    that `depends_on` is an array and stops there, because its only routed
    consumer at the time (`todo-reachability.py`, PROFILE_SECTIONS) reads
    `n`/`status` and never opens a group. `query.py` DOES open them --
    `query.py:357-366` dereferences `grp.get("target")` and `grp.get("sections")`
    to build the inbound edge index -- so it needs the item shape while the
    reachability audit still must not be held to a contract it does not consume.
    Folding this into `_validate_sections` would impose exactly that unconsumed
    contract (Codex design review, section 22, [high]).

    THE SILENT SKIP IS THE DEFECT. `query.py:358` does `if not isinstance(grp,
    dict): continue`, so a malformed group DROPS A DEPENDENCY EDGE and
    `blocked` / `blocking` / `backlinks` answer from a subset of the graph while
    reporting success -- the partial-cache false completeness this section
    exists to end.
    """
    rows = node.get(SUBTREE_SECTIONS)
    # Self-contained: a profile may declare this subtree without `sections`, and
    # a caller must not get a TypeError instead of a reason-tagged refusal.
    # LOCAL NAMES ARE DELIBERATELY DISTINCT from `_validate_sections`
    # (`rows`/`row`, not `sections`/`s`): the per-rule mutation harness in
    # test_build.sh identifies each rule by a UNIQUE source line and fails when
    # a needle matches twice, so two validators sharing a guard line would make
    # both rules unmutatable and silently un-verified.
    if not isinstance(rows, list):
        _err(REASON_SHAPE,
             f"cache node {i} `sections` is "
             f"{type(rows).__name__ if SUBTREE_SECTIONS in node else 'absent'}, "
             f"expected array (per {SCHEMA_REL}): {path}")
    for j, row in enumerate(rows):
        if not isinstance(row, dict):
            _err(REASON_SHAPE,
                 f"cache node {i} section {j} is not an object "
                 f"({type(row).__name__}): {path}")
        groups = row.get("depends_on")
        if not isinstance(groups, list):
            _err(REASON_SHAPE,
                 f"cache node {i} section {j} `depends_on` is "
                 f"{type(groups).__name__ if 'depends_on' in row else 'absent'}, "
                 f"expected array (per {SCHEMA_REL}): {path}")
        for k, grp in enumerate(groups):
            where = f"cache node {i} section {j} depends_on group {k}"
            if not isinstance(grp, dict):
                _err(REASON_SHAPE,
                     f"{where} is not an object ({type(grp).__name__}): {path}")
            for field in _SECTION_DEP_KEYS:
                if field not in grp:
                    _err(REASON_SHAPE,
                         f"{where} is missing required `{field}` "
                         f"(per {SCHEMA_REL}): {path}")
            extra = [x for x in grp if x not in _SECTION_DEP_KEYS]
            if extra:
                _err(REASON_SHAPE,
                     f"{where} has unknown key(s) {sorted(extra)} "
                     f"(additionalProperties false per {SCHEMA_REL}): {path}")
            # `target` is resolved through the id/path index and compared
            # against the literal "self"; a non-string cannot be either.
            _require_str(grp["target"], where, "target", path, 1)
            secs = grp["sections"]
            if not isinstance(secs, list):
                _err(REASON_SHAPE,
                     f"{where} `sections` is {type(secs).__name__}, "
                     f"expected array (per {SCHEMA_REL}): {path}")
            for m, num in enumerate(secs):
                # The reader renders these into an edge label; a bool is an int
                # in Python and would render as a section named "True".
                _require_int(num, 0, f"{where} sections[{m}]", "section number",
                             path, hi=_MAX_SECTION_N)


_SECTION_HEADING_KEYS = ("n", "title")


def _validate_section_headings(node, i: int, path):
    """`section_headings[]` -- the `## N.` bodies, consumed by `validate.py`.

    A SEPARATE SUBTREE from `sections`, and the distinction is the reason both
    exist: `sections[]` is the Implementation Order TABLE and `section_headings`
    is the BODIES, and `validate.py` compares the two sets against each other
    (`:644` dangling-section, `:678` orphan-IO-row). A validator that conflated
    them would be unable to express the very disagreement those checks look for.

    THE DEREFERENCE IS UNGUARDED AT BOTH SITES, which is why this subtree earns
    a validator now and did not before. Both do `{h["n"] for h in
    n.get("section_headings", [])}` with no isinstance test and no `.get`: a
    string entry raises TypeError, a dict without `n` raises KeyError, and
    neither is a `CacheSchemaError`, so both escape `main()` as a traceback and
    exit 1 -- the code this validator documents as GRAPH FINDINGS. An
    infrastructure failure would read as "the graph has problems".

    `n` IS NOT NULLABLE HERE, unlike `sections[].n`. That asymmetry is in the
    published schema (`cache.schema.json` requires `n`/`title` and types `n` as
    a bare integer, against `["integer", "null"]` on the section row) and it is
    correct: a row's Section column is free text that may not parse, whereas a
    heading is MATCHED by `build.extract_section_headings` (build.py:518) and so
    cannot exist without a number.

    Bounds and uniqueness mirror `_validate_sections` for the same two reasons:
    `n` lands in a SET here rather than a dict key, so a duplicate does not
    overwrite a value -- but it does silently shrink the set, and these sets are
    compared for MEMBERSHIP against the IO rows, so a duplicated heading number
    is a heading that vanishes from the comparison. The upper bound is the same
    CPython hash-collision budget `_MAX_SECTION_N` carries elsewhere.

    LOCAL NAMES ARE DELIBERATELY DISTINCT from every other validator
    (`heads`/`head`/`seen_head_n`, not `rows`/`row` or `sections`/`s`): the
    per-rule mutation harness in test_build.sh identifies each rule by a UNIQUE
    source line and fails when a needle matches twice.
    """
    if SUBTREE_SECTION_HEADINGS not in node:
        _err(REASON_SHAPE,
             f"cache node {i} is missing required `section_headings` "
             f"(per {SCHEMA_REL}): {path}")
    heads = node[SUBTREE_SECTION_HEADINGS]
    if not isinstance(heads, list):
        _err(REASON_SHAPE,
             f"cache node {i} `section_headings` is {type(heads).__name__}, "
             f"expected array (per {SCHEMA_REL}): {path}")
    seen_head_n = set()
    for j, head in enumerate(heads):
        where = f"cache node {i} section heading {j}"
        if not isinstance(head, dict):
            _err(REASON_SHAPE,
                 f"{where} is not an object ({type(head).__name__}): {path}")
        for field in _SECTION_HEADING_KEYS:
            if field not in head:
                _err(REASON_SHAPE,
                     f"{where} is missing required `{field}` "
                     f"(per {SCHEMA_REL}): {path}")
        extra = [k for k in head if k not in _SECTION_HEADING_KEYS]
        if extra:
            _err(REASON_SHAPE,
                 f"{where} has unknown key(s) {sorted(extra)} "
                 f"(additionalProperties false per {SCHEMA_REL}): {path}")
        _require_int(head["n"], 0, where, "n", path, hi=_MAX_SECTION_N)
        if head["n"] in seen_head_n:
            _err(REASON_SHAPE,
                 f"{where} repeats heading number {head['n']} already seen in "
                 f"this node; the consumer compares these as a SET against the "
                 f"Implementation Order rows, so a duplicate silently removes a "
                 f"heading from that comparison: {path}")
        seen_head_n.add(head["n"])
        _require_str(head["title"], where, "title", path)


def _validate_node_ids_unique(nodes, path):
    """Non-null `id` must be unique across the cache.

    `file_path` uniqueness is already enforced for every caller, because every
    routed reader keys a dict on it. `id` is the OTHER key readers build a dict
    on and it was unchecked: `build_id_index` is a dict comprehension, so two
    nodes sharing an id leave only the LAST one reachable, and a third node's
    `depends_on: ["dup"]` binds to that survivor while the first node silently
    gets no inbound edge. `ready` / `blocked` / `blocking` / `backlinks` /
    `stats` then return rc 0 with misattributed edges -- a wrong graph that
    looks complete, which is the exact class this profile exists to refuse
    (Codex adversarial, section 22 round 2, [high]).

    `validate.py` has carried a `check_duplicate_id` graph check for this all
    along, but that is a FINDING about the corpus reported at its own exit
    code; it does not stop a reader trusting the cache, and it is not run by
    anything on the query path.

    NULL IS NOT A DUPLICATE. The schema declares `id` nullable and a
    pre-migration corpus has many null ids at once; `build_id_index` skips
    them, so they bind nothing and collide with nothing.
    """
    seen = {}
    for i, node in enumerate(nodes):
        # `node` shape and `id` type are already established by the per-node
        # walk; this pass only answers the cross-node question.
        ident = node.get("id")
        if ident is None:
            continue
        if ident in seen:
            _err(REASON_SHAPE,
                 f"cache node {i} repeats id {ident!r} already used by node "
                 f"{seen[ident]}; readers key a dict on it, so the duplicate "
                 f"silently rebinds every edge naming it: {path}")
        seen[ident] = i


# MIRRORS `validate.build_path_index`. The resolver reduces a compact `T01` /
# `D01T01` reference AND a full `01-a/TODO-01-first.md` path to the same
# `(domain-code, TODO-number)` key, so that derived pair -- not `file_path` --
# is what an edge actually resolves through. Kept as a local derivation rather
# than importing `validate`, which is a CLI module and the wrong layer to pull
# into the schema validator; `test_build.sh` asserts the two agree over the
# live corpus, so a drift is a test failure rather than a silent divergence.
_DOMAIN_DIR_RE = re.compile(r"^(\d\d)-")
_TODO_NUM_RE = re.compile(r"^TODO-(\d{1,2})-")


def resolver_key(file_path: str):
    """The `(domain, number)` pair `build_path_index` files this node under, or
    None when the path is not in the numbered-domain shape it indexes.

    PUBLIC, and `validate.build_path_index` CALLS IT -- one derivation, not two
    that must be kept in agreement. The first version of this rule duplicated
    the logic here and pinned the pair with a parity test over the live corpus,
    which proved only today's domain set: `build_path_index` hardcoded the
    prefixes `00-` through `18-` while this matched any two digits, so a
    legitimate domain-19 TODO would have been keyed here and NOT there --
    making the validator refuse a collision the resolver never creates, and
    breaking the parity test the moment such a file was added (Codex
    adversarial, section 22 round 4, [medium]). The hardcoded list was also a
    maintenance trap in its own right: it silently stops indexing a new domain.
    """
    parts = PurePosixPath(file_path).parts
    if len(parts) < 2:
        return None
    dom = _DOMAIN_DIR_RE.match(parts[-2])
    num = _TODO_NUM_RE.match(parts[-1])
    if not dom or not num:
        return None
    return (dom.group(1), int(num.group(1)))


def _validate_resolver_keys_unique(nodes, path):
    """The DERIVED resolver identity must be unique, not merely `file_path`.

    Distinct ids and distinct file paths are not enough. `build_path_index`
    files nodes under `(domain-code, TODO-number)`, so
    `todo/01-a/TODO-01-first.md` and `todo/01-a/TODO-01-second.md` occupy ONE
    slot and the later node overwrites the earlier. An edge naming either the
    compact `T01` form or the explicit full path `01-a/TODO-01-first.md` then
    resolves to the survivor, because the resolver reduces both spellings to
    that same overwritten key.

    MEASURED with such a pair present: `backlinks first` returned 0 rows and
    `backlinks second` returned 1 -- for an Inputs XREF naming `first`
    explicitly -- at rc 0. Same false-completeness class as the duplicate-id
    rule, one index over (Codex adversarial, section 22 round 3, [high]).

    Zero collisions exist across the 232 live nodes, so this refuses nothing a
    correctly-numbered corpus produces; a collision is a real authoring error
    (two TODOs sharing a number inside one domain).
    """
    seen = {}
    for i, node in enumerate(nodes):
        key = resolver_key(node["file_path"])
        if key is None:
            # Not in the indexed shape, so it occupies no resolver slot and
            # cannot collide with anything.
            continue
        if key in seen:
            _err(REASON_SHAPE,
                 f"cache node {i} ({node['file_path']}) derives resolver key "
                 f"{key} already used by node {seen[key]}; compact and "
                 f"full-path references both reduce to it, so the duplicate "
                 f"silently rebinds every edge naming either file: {path}")
        seen[key] = i


def _validate_node_fields(node, i: int, path):
    """The NODE-LEVEL fields `query.py` and `render.py` consume.

    SCOPE IS STILL "FIELDS A READER CONSUMES". `owners` and `schema_version` are
    deliberately ABSENT from this walk: no routed reader dereferences them, and
    validating them would advertise a contract this module does not test -- the
    overreach the header at the top of this file forbids. `section_headings` was
    named here too until section 23 routed `validate.py`, which DOES dereference
    it; it now has its own subtree (`_validate_section_headings`) rather than a
    place in this one, because its consumer compares it AGAINST `sections[]` and
    a profile must be able to declare one without the other. The inventory below
    was taken from the readers, not the schema:
    `query.py` reads id / status / domain / title / last_active_at /
    depends_on / satisfies / superseded_by / inputs_xrefs / file_patterns, and
    `render.py` adds created_at.

    EVERY ONE OF THESE IS WRAPPED IN `_safe_list` OR A DEFENSIVE SKIP BY THE
    READER, which is the whole problem. `query.py:464` does
    `deps = _safe_list(n.get("depends_on"))`, so a SCALAR `depends_on` becomes
    an empty list, `all_done` stays True, and a TODO with unmet dependencies is
    reported READY -- a wrong answer that looks like a complete one (Codex
    design review, section 22, [high]).
    """
    for field in _NODE_REQUIRED_STR_FIELDS:
        if field not in node:
            _err(REASON_SHAPE,
                 f"cache node {i} is missing required `{field}` "
                 f"(per {SCHEMA_REL}): {path}")
        value = node[field]
        # Nullable per the schema for everything except `domain`; the readers
        # cope with None (they format it or fall back), so null is VALID and is
        # the reader's problem, exactly as a null `sections[].n` is.
        if value is None and field != "domain":
            continue
        _require_str(value, f"cache node {i}", field, path)

    for opt in _NODE_OPTIONAL_STR_LISTS:
        # Loop variable named `opt`, not `field`, so the per-rule mutation
        # harness can address the REQUIRED-field guard above by a unique line.
        if opt not in node:
            continue  # absence is the norm -- see _NODE_OPTIONAL_STR_LISTS
        value = node[opt]
        if not isinstance(value, list):
            _err(REASON_SHAPE,
                 f"cache node {i} `{opt}` is {type(value).__name__}, "
                 f"expected array (per {SCHEMA_REL}): {path}")
        seen_members = set()
        for j, item in enumerate(value):
            _require_str(item, f"cache node {i} {opt}[{j}]", opt, path, 1)
            # UNIQUENESS IS PART OF THE PUBLISHED CONTRACT, not tidiness: the
            # schema declares `uniqueItems` on all three of these, and the
            # consumer COUNTS them. `cmd_blocking` does
            # `counts[tgt] = counts.get(tgt, 0) + 1` per ref, so
            # `depends_on: ["a", "a"]` reports an inbound count of 2 for ONE
            # dependency and moves that node up `stats.top_blocking` -- a
            # ranking a hostile or regressed producer can manipulate while the
            # cache stays schema-shaped. Validating member TYPE while ignoring
            # a constraint the schema states is the "routed but unprotected"
            # shape this module exists to refuse (Codex adversarial, section
            # 22, [medium]).
            if item in seen_members:
                _err(REASON_SHAPE,
                     f"cache node {i} `{opt}` repeats {item!r} "
                     f"(uniqueItems per {SCHEMA_REL}); consumers COUNT these, "
                     f"so a duplicate inflates a dependency ranking: {path}")
            seen_members.add(item)

    if "superseded_by" in node and node["superseded_by"] is not None:
        _require_str(node["superseded_by"], f"cache node {i}",
                     "superseded_by", path, 1)

    # `effort` earns a place in this walk under the same rule as everything else
    # above -- a routed reader dereferences it. `render.py`'s Gantt emitter
    # interpolates it straight into a mermaid duration field, so a value the
    # grammar does not admit is not a cosmetic problem: a newline or a comma
    # ends the row early and silently reshapes the chart, and `4mo` draws a
    # length nobody authored. GRAMMAR, NOT JUST TYPE, for that reason -- the
    # `_require_str` this used to be would have passed all three.
    # NO `is not None` ESCAPE, unlike `superseded_by` above. The schema declares
    # `effort` as `"type": "string"` with no null member and `validate_frontmatter`
    # refuses a null, so accepting one here would be the schema-accepts /
    # Python-refuses gap section 23 closed for `sections[].n`, running the other
    # way: the module would admit a value the published contract forbids.
    if "effort" in node:
        _require_str(node["effort"], f"cache node {i}", "effort", path, 1)
        if not EFFORT_REGEX.match(node["effort"]):
            _err(REASON_SHAPE,
                 f"cache node {i} `effort` is {node['effort']!r}, which does "
                 f"not match {EFFORT_REGEX.pattern} (per {SCHEMA_REL}); the "
                 f"Gantt emitter interpolates this into a mermaid duration, "
                 f"so an unadmitted value reshapes the chart rather than "
                 f"failing: {path}")

    # `inputs_xrefs` is REQUIRED by the schema and `query.py:340-344` skips a
    # non-dict entry, dropping an Inputs edge silently.
    if _INPUTS_XREFS_KEY not in node:
        _err(REASON_SHAPE,
             f"cache node {i} is missing required `inputs_xrefs` "
             f"(per {SCHEMA_REL}): {path}")
    # Local names distinct from `_validate_stamps_xrefs` for the mutation-harness
    # uniqueness reason documented in `_validate_section_deps`.
    inputs = node[_INPUTS_XREFS_KEY]
    if not isinstance(inputs, list):
        _err(REASON_SHAPE,
             f"cache node {i} `inputs_xrefs` is {type(inputs).__name__}, "
             f"expected array (per {SCHEMA_REL}): {path}")
    for j, entry in enumerate(inputs):
        where = f"cache node {i} inputs xref {j}"
        if not isinstance(entry, dict):
            _err(REASON_SHAPE,
                 f"{where} is not an object ({type(entry).__name__}): {path}")
        for key in _INPUTS_XREF_KEYS:
            if key not in entry:
                _err(REASON_SHAPE,
                     f"{where} is missing required `{key}` "
                     f"(per {SCHEMA_REL}): {path}")
        # Named `extra` like every other additionalProperties rule: the mutation
        # harness addresses these by their ASSIGNMENT line (the `if extra:`
        # below is ambiguous across four validators), and `if unknown:` collided
        # with the Profile constructor's own unknown-subtree guard.
        extra = [k for k in entry if k not in _INPUTS_XREF_KEYS]
        if extra:
            _err(REASON_SHAPE,
                 f"{where} has unknown key(s) {sorted(extra)} "
                 f"(additionalProperties false per {SCHEMA_REL}): {path}")
        # `target_section` is nullable -- an Inputs XREF may name a file with no
        # section. `target_path` is resolved through the path index.
        _require_str(entry["target_path"], where, "target_path", path, 1)
        if entry["target_section"] is not None:
            _require_str(entry["target_section"], where, "target_section", path)


def _validate_stamps_xrefs(node, i: int, path):
    """`stamps_xrefs[]` -- Accepted/Deferred stamp XREFs, as the PRODUCER emits
    them: `kind`, `severity`, `target_path`, `target_section` and the optional
    `item_name`.

    NO READER IS ROUTED THROUGH THIS YET (see the profiles block above). The
    schema carried this as a bare `{"type": "array"}` with no item shape, so a
    malformed entry passed both the schema and every reader; the shape is
    constrained here and in the schema together (section 19), derived from the
    994 live entries, and waits for the control-plane routing that will use it.
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
        _require_int(it["section_n"], 1, where, "section_n", path,
                     hi=_MAX_SECTION_N)
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
    seen_file_path = {}

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

        # THE PATH IS UNTRUSTED INPUT, and every routed reader joins it onto a
        # repo root. Until now the only rule was "a non-empty string", so
        # `../../outside.md` and `/etc/passwd` both validated -- confirmed by
        # probe against the live cache. `repo_root / rel` then discards the root
        # entirely for an absolute path and walks out of it for a relative one.
        #
        # That was survivable while every consumer only READ. It is not now:
        # `validate.py --fix-line-numbers --write` REPLACES the files these
        # paths name, so a caller-supplied cache could direct a rewrite at any
        # file the user can write. And this module is precisely what makes that
        # cache look trustworthy -- bounded, shape-checked, generation-bound --
        # so the guarantee has to cover the paths too (Codex re-adversarial
        # round 5, section 23 review, [high]).
        #
        # All 232 nodes of the live corpus already satisfy this, so it refuses
        # nothing a real producer emits.
        rel_path = node["file_path"]
        if rel_path.startswith("/") or (len(rel_path) > 1 and rel_path[1] == ":"):
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` is absolute ({rel_path!r}); "
                 f"consumers join it onto a repo root, so an absolute path "
                 f"escapes that root entirely: {path}")
        if "\\" in rel_path:
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` uses backslashes ({rel_path!r}); "
                 f"cache paths are POSIX-relative: {path}")
        if "\x00" in rel_path:
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` contains a NUL byte; that is not "
                 f"a filename any producer can emit: {path}")
        parts = rel_path.split("/")
        # CANONICAL SPELLING, because `file_path` is the node's IDENTITY and
        # the uniqueness check above compares it as a STRING. `todo//x.md`,
        # `todo/./x.md` and `todo/x.md/` all name the same inode as
        # `todo/x.md` while comparing unequal, so a poisoned cache could carry
        # one physical file under several identities -- duplicating it in every
        # index, or rebinding another node's edges to it. Rejecting the
        # non-canonical spellings is what makes the string comparison a real
        # identity test (Codex re-adversarial round 6, section 23 review,
        # [medium]).
        if len(parts) < 2 or not parts[-1]:
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` names no file ({rel_path!r}); "
                 f"every node is a file under {_CORPUS_DIR}/, not the "
                 f"directory itself and not a trailing separator: {path}")
        if "" in parts or "." in parts:
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` is not canonical ({rel_path!r}); "
                 f"an empty or `.` component names the same file under a "
                 f"different spelling, and this path IS the node's identity: "
                 f"{path}")
        if ".." in parts:
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` traverses upward ({rel_path!r}); "
                 f"consumers join it onto a repo root and a reader -- or the "
                 f"line-number repair, which WRITES -- would leave the "
                 f"corpus: {path}")
        if parts[0] != _CORPUS_DIR:
            _err(REASON_SHAPE,
                 f"cache node {i} `file_path` is outside the corpus "
                 f"({rel_path!r}); every node names a file under "
                 f"{_CORPUS_DIR}/: {path}")

        # `file_path` IS THE NODE'S IDENTITY, and identity must be unique for
        # the same reason `sections[].n` must be: every routed reader keys a
        # dict on it (`_CACHE[node["file_path"]]` in todo-reachability, the
        # occurrence key in both stamped-items walks), so a second node naming
        # the same file SILENTLY REPLACES the first. A schema-valid cache can
        # then hide a file's real section statuses behind a decoy node and
        # suppress a finding while the audit reports success -- the same
        # overwrite class the per-node `n` rule already refuses, one level up
        # (Codex adversarial, section 19 review, [high]). Zero duplicates exist
        # across the 232 live nodes, so this refuses nothing a real producer
        # emits.
        if node["file_path"] in seen_file_path:
            _err(REASON_SHAPE,
                 f"cache node {i} repeats file_path {node['file_path']!r} "
                 f"already used by node {seen_file_path[node['file_path']]}; "
                 f"consumers key a dict on it, so a duplicate silently "
                 f"overwrites: {path}")
        seen_file_path[node["file_path"]] = i

        # PER-SUBTREE DISPATCH. A caller is validated for exactly what it
        # declared it consumes -- no more (that would advertise an untested
        # contract) and no less (that would be routing in name only).
        if SUBTREE_STAMPED_ITEMS in profile.subtrees:
            delta, present = _validate_stamped_items(node, i, path)
            population += delta
            key_present = key_present or present
        if SUBTREE_SECTIONS in profile.subtrees:
            _validate_sections(node, i, path)
        if SUBTREE_SECTION_DEPS in profile.subtrees:
            _validate_section_deps(node, i, path)
        if SUBTREE_STAMPS_XREFS in profile.subtrees:
            _validate_stamps_xrefs(node, i, path)
        if SUBTREE_NODE_FIELDS in profile.subtrees:
            _validate_node_fields(node, i, path)
        if SUBTREE_SECTION_HEADINGS in profile.subtrees:
            _validate_section_headings(node, i, path)

    # CROSS-NODE rules run after the per-node walk, because they are questions
    # about the SET rather than about any one node. They are gated on their OWN
    # subtree, not on `node_fields`: see SUBTREE_NODE_IDENTITY for why a reader
    # that reports duplicates itself must be able to take the field shapes
    # without taking the refusals.
    if SUBTREE_NODE_IDENTITY in profile.subtrees:
        _validate_node_ids_unique(nodes, path)
        _validate_resolver_keys_unique(nodes, path)

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


# The corpus generator excludes this basename (`build.walk_todo_files`), so it
# contributes no node and cannot make a cache stale. Excluding it HERE too is
# what lets the producer record the very map a reader recomputes: with the two
# file sets identical, "the corpus the cache was built from" is one rule with
# one implementation instead of a producer subset compared against a reader
# superset (section 21).
_CORPUS_EXCLUDE = frozenset({"TODO-00-INDEX.md"})

# The one directory every cache node lives under. Node paths are validated
# against it so an untrusted cache cannot name a file outside the corpus.
_CORPUS_DIR = "todo"

# Sidecar wire format. Bumping this string invalidates every existing sidecar,
# which is the intended migration path: a reader that does not understand the
# recorded shape must refuse rather than guess.
SIDECAR_SCHEMA = "todo-cache-corpus-v1"


# Per-TODO input budget. The largest TODO in the live corpus is well under
# 1 MiB; 16 MiB is the same ceiling `resolve_symbol` puts on a source file, and
# it exists for the same reason `_MAX_CACHE_BYTES` does -- a bound enforced
# BEFORE the bytes are materialised.
_MAX_TODO_BYTES = 16 * 1024 * 1024


def read_corpus_file(path) -> bytes:
    """Read one corpus entry, or refuse it. THE SHARED RULE (section 21).

    The producer and every reader must agree on which directory entries are
    corpus files, and agreeing on the NAME is not enough: `build.py` skipping a
    FIFO that `_scan_corpus` refuses meant a successful build published a cache
    every reader then rejected -- a producer failing OPEN against a reader
    failing closed (Codex re-adversarial, section 21, [medium]). Both sides call
    this, so there is one answer.
    """
    return _read_regular(path, _MAX_TODO_BYTES, "corpus entry")


def file_digest(path) -> str:
    """sha256 of a regular file's bytes, hex. The corpus fingerprint unit.

    TYPE-CHECKED AND BOUNDED, because this runs on EVERY freshness check and
    therefore on every reader invocation (Codex adversarial, section 21,
    [medium]). `os.walk` reports a symlink to a FIFO as an ordinary file, so a
    `TODO-*.md` pointing at one would block the reader forever, and a symlink to
    a huge file outside the corpus would pull it through unbounded. Neither
    needs an overflow or an overread to deny service. The descriptor is opened
    FIRST and inspected with `fstat`, so the file that is measured is the file
    that is read -- checking the pathname and then opening it is the TOCTOU this
    module exists to avoid.
    """
    return hashlib.sha256(read_corpus_file(path)).hexdigest()


# The binding is a map of the corpus plus three scalars: ~29 KB for 232 files,
# so 8 MiB is ~280x headroom. It is bounded for the same reason the cache is --
# it is read on EVERY reader invocation, and an unbounded read of a corrupt or
# hostile file is the one failure mode no reason code can report (Codex
# adversarial, section 21, [medium]).
_MAX_SIDECAR_BYTES = 8 * 1024 * 1024


def _read_regular(path, ceiling: int, what: str) -> bytes:
    """Bounded read that REFUSES anything that is not a regular file.

    The descriptor is opened FIRST and inspected with `fstat`, so the file that
    is measured is the file that is read -- checking the pathname and then
    opening it is the TOCTOU this module exists to avoid. `O_NONBLOCK` means
    opening a FIFO cannot block before the type check runs; it has no effect on
    a regular file. Without this, `os.walk` reporting a symlink-to-FIFO as an
    ordinary file would hang every reader forever -- denial of service needing
    no overflow or overread.
    """
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    try:
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode):
            _err(REASON_STALE,
                 f"{what} is not a regular file (mode {st.st_mode:#o}): {path}")
        if st.st_size > ceiling:
            _err(REASON_STALE,
                 f"{what} is {st.st_size} bytes, past the {ceiling}-byte "
                 f"ceiling: {path}")
        with os.fdopen(fd, "rb") as fh:
            fd = -1  # fdopen owns it now
            # BOUNDED BY THE FILE, NOT BY THE CEILING (Codex perf, section 24,
            # [medium]). This asked for `ceiling + 1` on every read, and
            # `read(n)` pre-allocates an n-byte buffer -- so each of 232 corpus
            # files paid a 16 MiB allocation to hand back a ~47 KB TODO, twice
            # per process. Measured: 13.7ms for this wrapper against 4.3ms for
            # a plain read of the same corpus, which is why the fstat-first
            # read and not the sha256 was the larger term inside `_scan_corpus`.
            # The fstat above already knows the size, so ask for exactly one
            # byte more than that. Every protection is retained: the descriptor
            # is still opened first, still type-checked, still ceiling-checked.
            blob = fh.read(min(st.st_size, ceiling) + 1)
    except OSError as exc:
        _err(REASON_STALE, f"{what} unreadable: {path}: {exc}")
    finally:
        if fd >= 0:
            os.close(fd)
    if len(blob) > ceiling:
        _err(REASON_STALE,
             f"{what} grew past the {ceiling}-byte ceiling while being read: "
             f"{path}")
    if len(blob) > st.st_size:
        # STRICTER than the ceiling test above, not weaker: the file gained
        # bytes between the fstat and the read, so hashing what came back would
        # fingerprint a generation that never existed on disk. That is the
        # moving-corpus case this module refuses everywhere else.
        _err(REASON_STALE,
             f"{what} grew from {st.st_size} bytes while being read: {path}")
    return blob


def _read_bounded(path: Path, ceiling: int, what: str) -> bytes:
    """Read a file with the ceiling enforced on the DESCRIPTOR before the
    allocation, then again against what was actually read (so a file that grows
    between the fstat and the read is caught rather than silently truncated).
    Mirrors the cache's own bounded read.

    `O_NONBLOCK` + `S_ISREG`, the same descriptor-first pattern `_read_regular`
    above uses, and for the same reason: a plain `open()` on a FIFO with no
    writer BLOCKS before `fstat`, so the size ceiling, the JSON handling and
    every reason-code normalisation below are unreachable -- a hang needing no
    overflow and no overread. That was survivable while this helper only ever
    read a binding beside the repo's own generated cache; the cache-format
    identity work pointed it at freshness-disabled foreign caches and `--diff`
    baselines, where the CALLER owns the directory and can name the sidecar
    (Codex adversarial, [medium]).
    """
    # FileNotFoundError still propagates: callers distinguish it.
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    # OWNERSHIP SENTINEL, the shape `_read_regular` uses, and not the narrower
    # `except OSError` this had first: `os.fdopen` can fail with MemoryError,
    # which is NOT an OSError, and that path left the raw descriptor open --
    # confirmed by probe as an extra entry in /proc/self/fd. `load_and_validate`
    # normalises MemoryError into a reason code and the MCP server survives it,
    # so repeated degraded calls leak until exhaustion (Codex re-adversarial,
    # [medium]).
    fh = None
    try:
        fh = os.fdopen(fd, "rb")
    finally:
        if fh is not None:
            fd = -1            # fdopen owns it now
        elif fd >= 0:
            os.close(fd)
    try:
        st = os.fstat(fh.fileno())
        if not stat.S_ISREG(st.st_mode):
            _err(REASON_STALE,
                 f"{what} is not a regular file (mode {st.st_mode:#o}): {path}")
        if st.st_size > ceiling:
            _err(REASON_STALE,
                 f"{what} is {st.st_size} bytes, past the {ceiling}-byte "
                 f"ceiling: {path}")
        blob = fh.read(ceiling + 1)
    except OSError as exc:
        _err(REASON_STALE, f"{what} unreadable: {path}: {exc}")
    finally:
        try:
            fh.close()
        except OSError:
            pass
    if len(blob) > ceiling:
        _err(REASON_STALE,
             f"{what} grew past the {ceiling}-byte ceiling while being read: "
             f"{path}")
    return blob


def _scan_corpus(todo_root: Path, on_err):
    """One generation of the TODO corpus: {rel_posix_path: sha256_hex}.

    CONTENT, NOT CLOCK (section 21). This used to return
    `{abs_path: (dev, ino, size, mtime_ns)}`, and the freshness rule built on it
    compared `max(mtime) > cache_mtime` -- an ORDERING of two wall-clock stamps.
    That is not a fact about the data: this host's clock demonstrably steps
    backward (`test_build.sh` records a separate "-139ms, clock stepped
    mid-measurement" failure), and a backward step between a TODO write and the
    cache write inverts the comparison. identity-gate fixture 22b failed that
    way about 1 run in 4 on an unchanged tree. A content digest cannot be
    inverted by a clock, and it is also STRICTLY more accurate in both
    directions: a bare `touch` no longer reads as an edit, and an edit whose
    size happens to match is no longer invisible.

    Keys are RELATIVE to `todo_root` so a producer and a reader that reach the
    same corpus by different absolute paths still compare equal.

    RUN TWICE PER PROCESS, AND IT STAYS THAT WAY (section 24, resolving a
    section 21 `[L]` acceptance). `check_freshness` scans once and
    `check_corpus_unchanged` scans again after the caller's walk. The second pass
    is NOT a redundant hash of the same bytes -- it is a second OBSERVATION at a
    different time, and it is the entire mechanism by which a corpus edit landing
    mid-walk is caught. Removing it, or cheapening it with a stat/mtime
    pre-filter, would reintroduce the clock proxy the paragraph above exists to
    have removed. So the cost is owned, not eliminated.

    Measured 2026-08-08 on the live corpus (232 files, 10.83 MB): 27.8ms per
    pass warm (p50 of 15 runs), so ~56ms for the pair. The previously recorded
    "9.5ms per pass warm" was 3x optimistic on an essentially unchanged corpus
    and is corrected here. Attribution within one pass: ~7ms walk, 13.7ms
    `read_corpus_file` (the bounded fstat-first read, against 4.3ms for a plain
    read -- the safety wrapper is the larger term), 6.7ms sha256.

    SECTION 21'S 5%-CROSSOVER PROJECTION WAS ANCHORED TO THE WRONG BASELINE. It
    compared the pair against "the ~1s both readers spend resolving symbols",
    which is `validate.py`'s profile; there the pair is ~5.6% and the projection
    holds. `query.py` does not have that baseline -- it is a 169ms call -- so the
    pair is ALREADY ~33% of it, and the "5% at a 30-70 MB corpus" crossover was
    passed on the query path before it was ever written down. That is a
    measurement to act on if the query path ever needs optimizing; it is not a
    reason to weaken the second observation.
    """
    todo_root = Path(todo_root)
    seen = {}
    for dirpath, _, files in os.walk(todo_root, onerror=on_err):
        for f in files:
            if not (f.startswith("TODO-") and f.endswith(".md")):
                continue
            if f in _CORPUS_EXCLUDE:
                continue
            fp = Path(dirpath) / f
            seen[fp.relative_to(todo_root).as_posix()] = file_digest(fp)
    return seen


# ---------------------------------------------------------------------------
# THE TWO MECHANISMS THAT REWRITE ANCESTRY WITHOUT MOVING THE TIP OR THE COUNT
# (section 27). `corpus_history_id` projects the effective history onto a tip
# and a count, and both `git replace` and the deprecated graft file can change
# what `git log` reports for an OLDER commit while leaving that pair equal --
# so the cache gets certified against a history it did not come from.
#
# THEY ARE DISABLED BY CONSTRUCTION, NOT DETECTED BY A PROBE, and that choice
# is the whole design. A probe -- "does a graft file exist?", "digest
# refs/replace" -- is a SECOND read that is not causally bound to the traversal
# it is supposed to describe, so a graft can be present for the producer's walk
# and absent by the time the reader probes (Codex design review, section 27,
# [high]). Disabling removes the race by removing the probe: producer and
# reader both read replacement- and graft-neutralised ancestry, so they agree by
# construction. NOT "raw" ancestry -- a shallow boundary is still effective for
# both, which is section 31's open axis, not this one's.
#
# MEASURED 2026-08-09 on git 2.43.0, in a throwaway repo, each with a control:
#   - replace ref on the oldest corpus commit (identical tree and parents, new
#     committer date): tip and count both UNCHANGED, walk %ct 1577836800 ->
#     1622959566. With `--no-replace-objects`: back to 1577836800.
#   - `GIT_REPLACE_REF_BASE=refs/myreplace/`: `git for-each-ref refs/replace`
#     returns EMPTY while the walk is still replaced -- which is why a digest of
#     refs/replace was rejected as the mechanism. `--no-replace-objects`
#     defends anyway, because it does not care which namespace was used.
#   - ancestry-changing graft: count 3 -> 2 and a commit vanished from the
#     walk. With `GIT_GRAFT_FILE` pointed at an empty file: both restored.
#
# The cost of both is zero: no extra process, no extra read.
HISTORY_GIT_GLOBALS = ("--no-replace-objects",)


def history_git_env(base_env=None) -> dict:
    """Environment for a git call whose answer feeds a derived timestamp.

    Points `GIT_GRAFT_FILE` at the null device so the deprecated graft file is
    inert for this invocation. An EMPTY graft file is a no-op to git (verified
    2026-08-09: count stayed 3 with a zero-byte file), so this is the graft
    equivalent of `--no-replace-objects` -- for which git offers no flag.

    `os.devnull` rather than a literal `/dev/null` so the mechanism does not
    become the reason this stops working off Linux; the rest of the environment
    is inherited, because an inherited `GIT_REPLACE_REF_BASE` is already
    defeated by `--no-replace-objects` and stripping variables we do not
    understand would be its own hazard.
    """
    env = dict(os.environ if base_env is None else base_env)
    env["GIT_GRAFT_FILE"] = os.devnull
    return env


def corpus_history_id(todo_root: Path):
    """The id of the git history the corpus's DERIVED fields came from, or None.

    THE CACHE IS NOT A PURE FUNCTION OF THE TODO BYTES (Codex design review,
    section 21, [high]). `build.collect_git_timestamps` runs one path-limited
    `git log` and writes `created_at` / `last_active_at` into every node, and
    `query.py` sorts `stale` results by `last_active_at`, `stats` builds its
    longest-deferred table from it, and `render --render-format gantt` uses
    `created_at` as each bar's start date. (`deferred`, which an earlier version
    of this sentence named, dereferences NEITHER field -- corrected in section
    24 along with its twin above.) So a
    corpus whose CONTENT is unchanged while its HISTORY moved -- an amend, a
    rebase, or an edit reverted to identical bytes after being committed --
    produces a cache that a content-only check would happily certify.

    Binding to the tip of the corpus-limited log is sound rather than a
    heuristic: commit ids hash their ancestry, so rewriting ANY commit that
    touches the corpus changes every descendant id and therefore this tip. A
    commit that does not touch the corpus leaves both the tip and the timestamp
    projection alone, so this does not invalidate the cache on unrelated work.

    THE TIP ALONE IS NOT ENOUGH, so the COUNT rides with it (Codex adversarial,
    section 21, [high]). `collect_git_timestamps` walks `--reverse` and takes the
    FIRST commit it sees for each path as `created_at`, so DEEPENING a shallow
    clone changes those values while adding only ancestors -- the tip does not
    move and a tip-only binding would accept the stale cache. The count of
    corpus-touching commits catches the boundary moves that change CARDINALITY:
    deepening raises it, truncation lowers it, and a rewrite changes the tip.
    It does NOT catch a boundary move that holds the cardinality fixed -- an
    earlier wording here claimed it "moves in every direction that matters",
    which is false and is corrected below (section 27).

    BOTH HALVES FOLLOW THE EFFECTIVE GIT ENVIRONMENT, which is the property that
    killed the cheap replacement section 24 proposed. (An earlier wording said
    the COUNT was the only half that does. It is not -- the tip comes from
    `git log -1` under that same environment, and a shallow boundary can become
    the newest apparent corpus touch: verified 2026-08-09 on git 2.43.0 with C0
    touching the corpus and C1 not, where a boundary at C1 moved the tip from C0
    to C1 with the count unchanged at 1. Codex adversarial, section 27,
    [medium].)
    A digest of the shallow-boundary FILE looks equivalent and is not: with
    `GIT_SHALLOW_FILE` pointing elsewhere, `git log` and `git rev-list` both walk
    the overridden boundary while `<git-common-dir>/shallow` stays byte-identical
    and `rev-parse --git-path shallow` still resolves the default file.
    Reproduced 2026-08-08 on git 2.43: the count went 2 -> 1 across the override
    with the on-disk file unchanged. The count is computed by git under the same
    environment as the walk, so it cannot be fooled that way; a file digest can.
    (`--git-common-dir` also returns a RELATIVE path, so concatenating it is a
    second trap.) Section 24 rejected the substitution on those grounds.

    WHAT THIS PAIR DOES NOT COVER, stated because a projection onto two values
    necessarily discards information (Codex design review, section 24, [medium]).
    It covers ancestry rewrites and depth/cardinality changes on the
    REPLACEMENT- AND GRAFT-NEUTRALISED history. That phrase is deliberately
    clumsy and deliberately not "raw": shallow ancestry is still EFFECTIVE here,
    so "raw" would name a stronger contract than the code delivers (Codex
    adversarial, section 27, [medium]).

    IT DOES NOT COVER AN EQUAL-COUNT SHALLOW BOUNDARY, and the older wording
    here ("boundary changes however they are reached") was simply wrong
    (Codex adversarial, section 27, [high]). The count follows the effective git
    environment, which is why section 24 believed it caught every
    `GIT_SHALLOW_FILE` case -- but it catches only the ones that CHANGE the
    count. Reproduced 2026-08-09 on git 2.43.0: with C0 adding a TODO, C1/C2
    touching nothing under the corpus, and C3 touching the TODO, the full
    history walks {C0, C3} while a boundary at C2 walks {C2, C3}. Same tip, same
    count of 2, and `created_at` moves 1577836800 -> 1583020800. Closing that
    axis needs a decision this section did not own -- refuse shallow corpora
    outright, or bind the boundary causally to the walk -- and it is section 31.

    THE PAIR STILL CANNOT SEE A REPLACEMENT, AND NO LONGER HAS TO (section 27).
    Replace an older corpus-touching commit with one carrying an identical tree
    and identical parents but a different committer timestamp, and the `%ct`
    that `build.collect_git_timestamps` consumes changes while the latest
    touching hash and the commit count both stay equal; a graft can do the same.
    Section 24 named that as this pair's open axis. It is closed NOT by widening
    the projection -- which would have meant digesting the whole walk, and
    section 24 measured that at 312ms against 47.9ms for the projection -- but by
    removing what the projection could not see: every git call on both sides now
    carries `HISTORY_GIT_GLOBALS` and `history_git_env()`, so replacements and
    grafts are inert for producer and reader alike. The guarantee is therefore
    narrower than it looks and deliberately so: this pair identifies the
    replacement- and graft-neutralised history, which is the history everything
    here reads -- and NOT the raw one, because the shallow boundary above is
    still effective on both sides.

    Measured 2026-08-08 on the live corpus (3,807 corpus-touching commits of
    5,387 total): 47.9ms median, of which `rev-list --count` is 44.2ms and the
    three `rev-parse`/`log` probes are ~1.5ms each. AND THE COST GROWS WITH
    TOTAL REPO COMMITS, NOT CORPUS-TOUCHING ONES -- section 21 recorded the
    latter and it is the wrong variable. Measured by walk depth: 8.2ms at 100,
    14.7ms at 1,000, 22.4ms at 2,000, 36.1ms at 4,000, 44.5ms at 5,387 -- about
    6.9us per commit walked plus ~7.5ms of process spawn, and a pathspec matching
    only 44 commits still costs 25.5ms because the expense is the tree-diff
    across the whole walk, not the matches. So the ~100ms figure section 21
    projected at "10,000 commits" arrives at 10,000 TOTAL commits, roughly 1.4x
    sooner than its own model implied. `git commit-graph write --changed-paths`
    does NOT rescue it (45.3ms -> 38.8ms, 1.2x): `--count` cannot early-exit.
    Against 312ms to recompute the full timestamp map, the projection is still
    the right thing to record.

    "NOT A REPOSITORY" AND "COULD NOT ASK" ARE DIFFERENT ANSWERS. Returning None
    for both let a transient git failure on the producer compare EQUAL to a
    transient failure on the reader, certifying a cache neither had evidence for.
    A determinate not-a-repo (test fixtures under /tmp) returns a stable
    sentinel; anything indeterminate raises, so the caller fails closed.
    """
    todo_root = str(todo_root)

    def _git(*args):
        # The globals ride on EVERY probe, not only the two that walk history.
        # `rev-parse --git-dir` and the unborn-HEAD check do not traverse
        # replacements today, so this is drift protection rather than a fix:
        # the next probe added here inherits the contract instead of having to
        # remember it (Codex design review, section 27, [high]).
        try:
            return subprocess.run(["git", "-C", todo_root,
                                   *HISTORY_GIT_GLOBALS, *args],
                                  env=history_git_env(),
                                  capture_output=True, text=True, check=True)
        except subprocess.CalledProcessError as exc:
            return exc
        except (FileNotFoundError, OSError) as exc:
            _err(REASON_STALE,
                 f"cannot determine the corpus git history: {exc}")

    probe = _git("rev-parse", "--git-dir")
    if isinstance(probe, subprocess.CalledProcessError):
        # git ran and answered: this path is not in a repository. That is a
        # FACT about the corpus, and it is stable across producer and reader.
        if "not a git repository" in (probe.stderr or "").lower():
            return "no-repo"
        _err(REASON_STALE,
             f"cannot determine the corpus git history "
             f"(git rev-parse exited {probe.returncode}): "
             f"{(probe.stderr or '').strip()[:200]}")

    # AN UNBORN HEAD IS DETERMINATE. `git init` plus files but no commit leaves
    # rev-parse --git-dir succeeding while `log`/`rev-list HEAD` both fail, which
    # the generic handler below would report as "could not ask" -- refusing a
    # perfectly legitimate tree that `collect_git_timestamps` already handles by
    # emitting None timestamps (Codex adversarial, section 21, [medium]).
    # ONLY the documented unborn result. `--quiet` makes rev-parse exit exactly
    # 1 with empty output when HEAD names no commit; a corrupt HEAD, a broken
    # ref or a repository failure exits differently and is INDETERMINATE -- and
    # collapsing those into the same stable sentinel would let a producer and a
    # reader compare equal during the same failure (Codex re-adversarial,
    # section 21, [medium]).
    unborn = _git("rev-parse", "--verify", "--quiet", "HEAD")
    if isinstance(unborn, subprocess.CalledProcessError):
        if unborn.returncode == 1 and not (unborn.stdout or "").strip():
            return "unborn-head"
        _err(REASON_STALE,
             f"cannot verify the corpus HEAD (git exited {unborn.returncode}): "
             f"{(unborn.stderr or '').strip()[:200]}")

    tip = _git("log", "-1", "--format=%H", "--", ".")
    count = _git("rev-list", "--count", "HEAD", "--", ".")
    for r in (tip, count):
        if isinstance(r, subprocess.CalledProcessError):
            _err(REASON_STALE,
                 f"cannot determine the corpus git history "
                 f"(git exited {r.returncode}): "
                 f"{(r.stderr or '').strip()[:200]}")
    # An empty tip is determinate too: a repo whose corpus has no history yet.
    return f"{tip.stdout.strip() or 'no-history'}:{count.stdout.strip() or '0'}"


def sidecar_path(cache_path, cache_sha: str) -> Path:
    """Where the binding for a cache with these exact bytes lives.

    KEYED BY THE CACHE DIGEST, AND THEREFORE IMMUTABLE (Codex design review,
    section 21, [medium]). A single fixed sidecar pathname cannot be committed
    atomically with the cache: replace the cache and die before replacing the
    sidecar and the durable state is new-cache + old-sidecar, which every reader
    then refuses until somebody rebuilds. With the digest in the NAME the
    producer writes the new sidecar FIRST and replaces the cache SECOND, so
    whichever cache generation survives a crash, its own binding is already on
    disk beside it.
    """
    cache_path = Path(cache_path)
    return cache_path.with_name(f"{cache_path.name}.corpus-{cache_sha[:16]}.json")


_REBUILD = ("rebuild via scripts/todo-graph/build-and-validate.sh --keep-cache")


def check_freshness(cache_path: Path, todo_root: Path, cache_bytes: bytes,
                    profile: 'Profile' = None, history_out=None, rec=None):
    """Refuse a cache that was not built from the corpus now on disk.

    THE QUESTION CHANGED, AND THAT IS THE POINT (section 21). This used to ask
    "is any TODO newer than the cache file?", which is a proxy -- and a proxy
    resolved by comparing two wall-clock stamps, so a backward clock step made
    a correct cache look stale (see `_scan_corpus`). It now asks the question
    directly: the producer RECORDS the fingerprint of the corpus it consumed,
    and this compares that record against the corpus right now. No clock is
    consulted at any point, and no ordering is assumed.

    The record is bound to the cache by DIGEST, not by pathname, so the bytes
    the caller actually parsed are the bytes whose binding is read. `cache_bytes`
    is required for exactly that reason.

    FAIL-CLOSED throughout, including the three ways the pre-section-17 lint
    copy was fail-open: `os.walk` traversal errors are raised via `onerror`
    rather than silently lowering the corpus, a stat/read failure raises instead
    of returning "fresh", and an empty corpus is refused rather than certified.
    A MISSING sidecar joins that list: it means the cache predates this binding
    or was copied without it, and both are rebuild conditions, not evidence of
    freshness.

    Returns the live corpus fingerprint so the caller can re-verify it after its
    own walk (`check_corpus_unchanged`).
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

    try:
        live = _scan_corpus(todo_root, _walk_err)
    except OSError as exc:
        _err(REASON_STALE, f"cannot determine cache freshness: {exc}")
    if not live:
        _err(REASON_STALE,
             f"no TODO files found under {todo_root} -- refusing to call a "
             f"cache fresh against an empty corpus")

    cache_sha = hashlib.sha256(cache_bytes).hexdigest()
    side = sidecar_path(cache_path, cache_sha)
    # THE CALLER MAY HAND US THE RECORD IT ALREADY VALIDATED, and
    # `_load_and_validate` does. Re-opening the binding here would certify the
    # CORPUS from a generation the format check never saw, because the producer
    # can replace the pair between the two opens -- both checks would pass, each
    # against a different sidecar (Codex adversarial, [medium]). The standalone
    # path (fixtures calling this directly) still reads it itself.
    if rec is None:
        try:
            raw = _read_bounded(side, _MAX_SIDECAR_BYTES, "corpus binding")
        except FileNotFoundError:
            # STANDALONE PATH ONLY. Every read that arrives through
            # `_load_and_validate` has already been refused by
            # `check_cache_format`, which sees the same absence earlier and
            # reports it as LEGACY_FORMAT; this branch survives for fixtures
            # and callers that invoke freshness directly.
            _err(REASON_STALE,
                 f"cache carries no corpus binding ({side.name} is absent) -- it "
                 f"predates the section 21 producer contract, was copied without "
                 f"its binding, or was written by a producer that refused to "
                 f"certify it; {_REBUILD}")
        except OSError as exc:
            _err(REASON_STALE, f"corpus binding unreadable: {side}: {exc}")
        try:
            rec = json.loads(raw.decode("utf-8"))
        except (ValueError, RecursionError) as exc:
            _err(REASON_STALE,
                 f"corpus binding is not readable JSON: {side}: {exc}")
    if not isinstance(rec, dict) or rec.get("schema") != SIDECAR_SCHEMA:
        _err(REASON_STALE,
             f"corpus binding is not {SIDECAR_SCHEMA}: {side}; {_REBUILD}")
    # The digest is in the FILENAME, so a mismatch here means someone renamed a
    # binding onto a cache it does not describe. Checking it costs nothing and
    # turns that into a refusal instead of a silent wrong answer.
    if rec.get("cache_sha256") != cache_sha:
        _err(REASON_STALE,
             f"corpus binding names cache {rec.get('cache_sha256')} but the "
             f"cache read is {cache_sha}: {side}; {_REBUILD}")

    recorded = rec.get("corpus")
    if not isinstance(recorded, dict) or not recorded:
        _err(REASON_STALE,
             f"corpus binding records no corpus: {side}; {_REBUILD}")
    _diff_or_ok(recorded, live, "since the cache was built")

    # HISTORY, NOT JUST CONTENT -- FOR THE CALLERS THAT CONSUME IT. `created_at`
    # / `last_active_at` are derived from the corpus-limited git log, so
    # identical bytes over a rewritten history still means stale nodes (Codex
    # design review, section 21, [high]). But establishing that forks three git
    # processes and walks the corpus history (40ms today, growing with COMMIT
    # count), and neither routed reader consumes either field -- so it is a
    # profile declaration rather than an unconditional toll (Codex perf, section
    # 21, [medium]). The producer records the id either way, so a profile can
    # turn this on later with no migration.
    if profile is not None and not profile.requires_history:
        return live
    # ONE PROBE, NOT TWO (Codex perf, section 24, [medium]). A caller that will
    # re-verify after its own walk passes `history_out`; it then gets the
    # RECORDED id handed back and this function does not walk git at all.
    # Comparing that recorded id against a single post-walk probe proves exactly
    # what two probes proved -- that the history the cache was built from is
    # still the history at publication -- for half the cost, because the
    # load-time comparison was only ever a weaker prefix of the post-walk one.
    # (An A-to-B-to-A change evades both designs equally; neither claims to
    # catch it.) Omit `history_out` -- every standalone fixture call -- and the
    # probe-and-compare below runs exactly as before.
    if history_out is not None:
        recorded = rec.get("history_id")
        if not isinstance(recorded, str) or not recorded:
            # FAIL CLOSED. Deferring the comparison must never become skipping
            # it: a binding with no history id cannot be re-verified later, so
            # it is a rebuild condition here and now.
            _err(REASON_STALE,
                 f"cache binding records no history id, so a history-consuming "
                 f"reader cannot verify it: {side}; {_REBUILD}")
        history_out["recorded"] = recorded
        return live
    live_hist = corpus_history_id(todo_root)
    if rec.get("history_id") != live_hist:
        _err(REASON_STALE,
             f"the corpus git history moved since the cache was built "
             f"(recorded {rec.get('history_id')}, now {live_hist}), so its "
             f"created_at/last_active_at fields are stale; {_REBUILD}")
    return live


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

    THE WINDOW THAT MATTERS IS THE CALLER'S WALK, NOT THIS MODULE'S CHECK.
    `check_freshness` proves the cache matches the corpus at ONE instant, while
    both readers then spend ~1s resolving symbols, during which a TODO edit
    would leave the in-memory nodes stale and still let a VERDICT be returned
    (Codex adversarial, section 17 review). Callers therefore re-verify here
    once their walk completes, which is the point at which the verdict is
    actually about to be published.

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


def check_history_unchanged(todo_root: Path, history) -> None:
    """Verify, after the caller's walk, that the RECORDED history is still live.

    THE CONTENT AXIS HAD THIS AND THE HISTORY AXIS DID NOT (Codex design review,
    section 24, [medium]). `check_corpus_unchanged` above closes the caller's
    walk window for TODO BYTES; a rebase, an amend, a deepening or a truncation
    landing in that same window changes `created_at` / `last_active_at` while
    leaving every byte identical, so `stale`, `stats` and the Gantt renderer
    could publish timestamps from a history that no longer exists -- past a
    freshness check that had just certified them.

    `history` is the id RECORDED IN THE BINDING, not one probed at load time, so
    this single comparison proves the whole property end to end: the history the
    cache was built from is the history live at publication. That is why
    `check_freshness` no longer probes for a deferring caller.

    A no-op when `history` is None, which is every caller whose profile does not
    declare `requires_history`. Those callers dereference no derived timestamp,
    so they have nothing to protect and must not pay the probe.

    THE RESIDUAL WINDOW IS REAL AND IS NOT CLOSED BY ORDERING (Codex
    adversarial, section 24, [medium]). Two sequential checks cannot both be
    last: with history first, a history change during the ~28ms content scan is
    missed; with content first, a corpus edit during the ~48ms history probe is
    missed. Callers run history FIRST because that leaves the SMALLER window and
    keeps the content fingerprint -- the cheaper and more frequently-violated
    axis -- as the final act before publication. Adding a third check would move
    the window again at another 48ms, not remove it; closing it properly needs
    one coordinated snapshot, which this module does not have.
    """
    if history is None:
        return
    live = corpus_history_id(todo_root)
    if live != history:
        _err(REASON_STALE,
             f"the corpus git history moved since the cache was built (was "
             f"{history}, now {live}), so the created_at/last_active_at fields "
             f"this command consumed no longer describe it; re-run rather than "
             f"publish a result from a history that changed underneath it")


def check_cache_format(cache_path: Path, cache_bytes: bytes,
                       require_binding: bool) -> None:
    """Refuse a cache written against a producer contract this reader does not
    speak (section 25).

    Raises `CacheSchemaError` rather than returning a verdict, so every routed
    reader maps it through the reason table it already implements.

    IT GETS ITS OWN REASON RATHER THAN JOINING `REASON_SHAPE`. Grouping them
    would be cheaper and is wrong twice over: the operator is told the artifact
    "violates the schema" when it was perfectly valid under the contract it was
    written against, and `validate.py`'s recovery could not tell a corrupt cache
    from an old one. `REASON_LEGACY_FORMAT` is rebuildable for the ONE cache
    this repo generates and a plain refusal for anything else, which is the
    ownership rule section 23 already established -- a historical `--diff`
    baseline in this state is refused with its bytes intact, never overwritten.

    A PRESENT BINDING IS ALWAYS CHECKED; `require_binding` decides only the
    REASON an absent one carries, and it is DERIVED from `check_stale` rather
    than declared per caller. Either way the refusal happens HERE, so no read
    path can reach `validate_nodes` from an artifact that declares no producer
    contract:

    - Freshness OFF (`validate.py --diff`, `query.py` with an explicit
      non-canonical `--cache`, `check_consumer_delegation`): nothing else in
      the read will ever look at the binding.
    - Freshness ON: `check_freshness` would ALSO reject it later, but later is
      after `validate_nodes`.

    THE ORDERING IS THE FIX; THE REASON IS UNIFORM. An earlier draft returned
    quietly under freshness-ON and left the refusal to `check_freshness`, which
    runs AFTER the node walk -- so an unbound cache that ALSO violated a subtree
    was reported as SHAPE, sending the operator after a corpus fault when the
    artifact in fact declared no contract at all, and every node was walked
    before the read was refused regardless (Codex adversarial, section 25,
    [medium]).

    Refusing HERE as `REASON_STALE` was tried next and was WRONG, which the
    recovery fixtures caught: STALE is deliberately absent from
    `validate.py:_REBUILDABLE_REASONS` so a missed rebuild is reported rather
    than silently regenerated, and routing an unbound cache there converted a
    RECOVERABLE legacy artifact into a hard refusal -- the auto-rebuild of the
    one cache this repo owns stopped happening, and the MCP transport's
    structured-error envelope changed shape. `REASON_LEGACY_FORMAT` is both the
    honest description (no declared contract) and the rebuildable route.

    A PER-CALLER FLAG WAS TRIED FIRST AND WAS WRONG, which is worth recording
    because it is the same conflation the generation binding at the foot of
    `_load_and_validate` was moved out of `check_stale` to escape. The argument
    for it was that section 22 deliberately shape-checks a foreign `--cache`
    without certifying its corpus -- true, but that is a decision about the
    CORPUS. Whether this artifact was written by a producer this reader
    understands is a fact about the READ, and waiving one does not waive the
    other. The flag let an unidentified legacy cache reach every renderer and
    produce normal-looking output, which is the exact ambiguity this section
    exists to remove (Codex adversarial, section 25, [medium]). The 16 fixtures
    that made the narrower rule tempting hand the tools an unbound cache, which
    is not a supported artifact; they bind it now.

    ONE READ, HANDED ON. The parsed record is RETURNED so `check_freshness`
    validates the same generation this call certified. Each function used to
    open the digest-named binding independently, so a reader could certify the
    FORMAT from one sidecar and the CORPUS from its replacement -- the producer
    swaps the pair between the two opens and each check passes against a record
    the other never saw (Codex adversarial, [medium]).
    """
    cache_sha = hashlib.sha256(cache_bytes).hexdigest()
    side = sidecar_path(cache_path, cache_sha)
    try:
        raw = _read_bounded(side, _MAX_SIDECAR_BYTES, "corpus binding")
    except FileNotFoundError:
        # ONE REASON FOR ONE CONDITION, and `require_binding` selects only the
        # explanatory clause. An absent binding means the artifact declares no
        # producer contract, which is a fact about the ARTIFACT and does not
        # change because this particular read also intends to check the corpus.
        # LEGACY_FORMAT is also the reason that keeps the recovery honest: it
        # is rebuildable, so `validate.py` regenerates the ONE cache it owns
        # (`_REBUILDABLE_REASONS`) instead of refusing, while STALE is
        # deliberately non-rebuildable and would have converted a recoverable
        # legacy cache into a hard stop.
        why = (f"freshness is disabled for this read, so the binding is the "
               f"only evidence of which contract wrote it"
               if require_binding else
               f"it predates the section 21 producer contract, was copied "
               f"without its binding, or was written by a producer that "
               f"refused to certify it")
        _err(REASON_LEGACY_FORMAT,
             f"cache carries no corpus binding ({side.name} is absent), so it "
             f"declares no producer contract: {cache_path}; {why}; {_REBUILD}")
    except OSError as exc:
        _err(REASON_LEGACY_FORMAT,
             f"corpus binding unreadable: {side}: {exc}")
    try:
        rec = json.loads(raw.decode("utf-8"))
    except (ValueError, RecursionError, UnicodeDecodeError) as exc:
        _err(REASON_LEGACY_FORMAT,
             f"corpus binding is not readable JSON: {side}: {exc}")
    if not isinstance(rec, dict):
        _err(REASON_LEGACY_FORMAT,
             f"corpus binding is {type(rec).__name__}, expected an object: "
             f"{side}; {_REBUILD}")
    # THE BINDING'S OWN WIRE FORMAT, CHECKED HERE TOO. `check_freshness` makes
    # the same comparison, but `validate.py --diff` sets `check_stale=False` and
    # never reaches it -- so for the one caller that REQUIRES a binding, the
    # documented rule that bumping `SIDECAR_SCHEMA` invalidates every existing
    # binding did not hold, and a sidecar declaring any other schema was trusted
    # as long as it carried the four fields read below (Codex adversarial,
    # section 25, [medium]). Checked BEFORE any other field is interpreted,
    # because their meanings are what the schema string identifies.
    if rec.get("schema") != SIDECAR_SCHEMA:
        _err(REASON_LEGACY_FORMAT,
             f"corpus binding declares schema {rec.get('schema')!r}, not "
             f"{SIDECAR_SCHEMA}: {side}; the fields below mean whatever that "
             f"format says they mean, so they are not read; {_REBUILD}")
    # The digest is in the FILENAME, so it binds by name already -- but only by
    # its first 16 hex characters. Comparing the recorded value costs nothing
    # and refuses a binding renamed onto a cache it does not describe, which is
    # the same reasoning `check_freshness` applies to the same field. Kept here
    # rather than left to that function because the caller that most needs this
    # (`--diff`) has freshness disabled and never reaches it.
    if rec.get("cache_sha256") != cache_sha:
        _err(REASON_LEGACY_FORMAT,
             f"corpus binding names cache {rec.get('cache_sha256')!r} but the "
             f"cache read is {cache_sha}: {side}; {_REBUILD}")
    if CACHE_FORMAT_VERSION_KEY not in rec:
        _err(REASON_LEGACY_FORMAT,
             f"corpus binding declares no {CACHE_FORMAT_VERSION_KEY}: {side}; "
             f"it was written before the producer contract was identified at "
             f"all, so which fields the cache beside it carries is unknowable; "
             f"{_REBUILD}")
    got_v = rec[CACHE_FORMAT_VERSION_KEY]
    # `bool` IS an `int` in Python and `True == 1`, so a binding carrying `true`
    # would compare equal to version 1 and certify itself.
    if isinstance(got_v, bool) or not isinstance(got_v, int):
        _err(REASON_LEGACY_FORMAT,
             f"{CACHE_FORMAT_VERSION_KEY} is {got_v!r}, expected an integer: "
             f"{side}; {_REBUILD}")
    if got_v != CACHE_FORMAT_VERSION:
        _err(REASON_LEGACY_FORMAT,
             f"cache was written by producer contract v{got_v}; this reader "
             f"speaks v{CACHE_FORMAT_VERSION}: {cache_path}; a field that kept "
             f"its type and changed its MEANING is exactly what shape "
             f"validation cannot see, which is why the version is compared "
             f"rather than inferred; {_REBUILD}")
    got_d = rec.get(PRODUCER_CONTRACT_DIGEST_KEY)
    if got_d != PRODUCER_CONTRACT_DIGEST:
        # SAME VERSION, DIFFERENT FIELD SET is producer/schema drift rather than
        # an ordinary old artifact -- somebody moved the emitted set without
        # bumping the version -- so the message says that instead of the
        # regenerate advice above, which would send them after the wrong thing.
        _err(REASON_LEGACY_FORMAT,
             f"producer contract digest {got_d!r} does not match this reader's "
             f"{PRODUCER_CONTRACT_DIGEST!r} at the same "
             f"{CACHE_FORMAT_VERSION_KEY} {CACHE_FORMAT_VERSION}: "
             f"{cache_path}; the emitted node-field set moved without a version "
             f"bump (cache_schema.EMITTED_NODE_FIELDS); {_REBUILD}")
    return rec


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

        # IDENTITY BEFORE SHAPE (section 25). An artifact whose contract this
        # reader does not speak must not be walked at all: validating it under
        # the current subtree rules reports a v2 field as a v1 schema
        # violation, which names the wrong defect and sends the operator after
        # a corpus problem that does not exist. Bound to `blob` -- the bytes
        # this descriptor actually yielded -- for the same reason
        # `check_freshness` takes them rather than re-reading the pathname.
        binding = check_cache_format(cache_path, blob,
                                     require_binding=not check_stale)
        info = validate_nodes(nodes, cache_path, profile)
        if check_stale:
            # The fingerprint returned here is handed to the caller so it can
            # re-verify AFTER its own walk (`check_corpus_unchanged`).
            # BOUND TO THE BYTES ACTUALLY PARSED: `blob` is what came off the
            # single descriptor opened above, and the corpus binding is located
            # by ITS digest -- never by a fresh read of the pathname, which
            # `build.py` rewrites.
            # NOT PROBED HERE AT ALL. `corpus_history_id` costs ~48ms (section
            # 24 measurement), and the caller is going to probe once after its
            # walk anyway -- so this takes the RECORDED id off the binding and
            # lets that single post-walk probe do the whole comparison.
            history_out = ({} if profile is not None
                           and profile.requires_history else None)
            info.corpus = check_freshness(cache_path, todo_root,
                                          cache_bytes=blob, profile=profile,
                                          history_out=history_out,
                                          rec=binding)
            if history_out is not None:
                info.history = history_out["recorded"]
        # THE GENERATION BINDING IS NOT PART OF THE FRESHNESS POLICY, and it
        # used to sit inside the `if check_stale` block above. That coupled two
        # independent protections behind one flag: `check_stale=False` means "I
        # do not care whether a TODO is newer than the cache" (a policy about
        # the CORPUS), but it was ALSO silently disabling "prove the file I
        # parsed is still the file on disk" (a fact about THIS read). So the
        # only caller that opts out -- check_consumer_delegation, which cannot
        # take the corpus check because its fixtures run in a scratch tree with
        # no todo/ -- lost rewrite detection it never asked to give up, and a
        # concurrent build.py rewrite could leave it comparing counts from one
        # generation against a walk of another (Codex adversarial, section 19
        # review, [medium]). This check needs no corpus and no todo_root, so it
        # now runs unconditionally.
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
