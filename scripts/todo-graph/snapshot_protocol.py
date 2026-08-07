#!/usr/bin/env python3
"""The snapshot protocol: the published vocabulary and format version that the
resolution snapshot, the lint consumer and the identity gate all agree on.

WHY THIS MODULE EXISTS AT ALL (section 18, closing a section 16
blind spot). The identity gate proves that a resolver change did not alter any
prior verdict, by walking the corpus twice -- once with base resolver code, once
with head -- and diffing. To do that honestly it must be able to tell a PROTOCOL
migration (the bucket vocabulary or the snapshot format changed) apart from a
RESOLVER change (the rule that assigns them changed). While `ALL_BUCKETS` lived
inside `ref_resolution.py`, that separation was unsatisfiable: a bucket
migration necessarily edits the very file the rule needs byte-identical, so the
gate could only fail closed and name the extraction as its prerequisite
(`identity-gate.sh` die_infra, section 16). Same story for `SNAPSHOT_SCHEMA`,
which sat in `corpus_resolution_snapshot.py` beside `collect()` and `--strict`,
both verdict-affecting.

WHY THE VALUES ARE IN JSON AND NOT IN THIS FILE. The gate's whole inference is
"every executable file in the closure is byte-identical, therefore the change
that DID land cannot have altered a verdict." That only holds if the changed
thing cannot execute. A constants-only Python module still runs at import: a
schema bump could carry import-time behaviour, a dynamic constant, or a
monkeypatch of `resolve_symbol`, while every file the gate checks stayed
byte-identical -- and the gate would then skip the only differential capable of
exposing it (Codex design review, section 18). So the VALUES live in the inert
`snapshot_protocol.json`, read with `json.load` and never executed, and this
LOADER stays inside the byte-identity closure. A protocol migration is then a
data-only diff by construction, which is a mechanical guarantee rather than an
AST purity heuristic that has to anticipate every dynamic Python shape.

Consumers keep importing the names from their existing homes -- `ref_resolution`
re-exports the buckets, `corpus_resolution_snapshot` re-exports the schema -- so
this extraction changes no call site and is provably verdict-neutral, which is
exactly what the gate measures when it lands.
"""
from __future__ import annotations

import enum
import json
from pathlib import Path

PROTOCOL_PATH = Path(__file__).resolve().parent / "snapshot_protocol.json"


class ProtocolError(RuntimeError):
    """The protocol file is missing, unparseable, or not the declared shape."""


def _tuple_of_names(raw, field: str) -> tuple:
    if not isinstance(raw, list) or not raw:
        raise ProtocolError(
            f"{PROTOCOL_PATH.name}: {field!r} must be a non-empty list, got "
            f"{type(raw).__name__}")
    for v in raw:
        # A bucket name reaches a snapshot file and is compared for equality
        # forever after. An empty or non-string entry would compare unequal
        # against every future run with no way to clear it.
        if not isinstance(v, str) or not v:
            raise ProtocolError(
                f"{PROTOCOL_PATH.name}: {field!r} holds a non-string or empty "
                f"entry {v!r}")
    if len(set(raw)) != len(raw):
        raise ProtocolError(
            f"{PROTOCOL_PATH.name}: {field!r} holds duplicate names {raw!r}")
    return tuple(raw)


def load():
    """Return (snapshot_schema, pre_buckets, post_buckets).

    Raises ProtocolError rather than returning a default. A protocol this
    process cannot read is INFRASTRUCTURE, never a value: treated as a value it
    merely differs from the other side, which is the exact fail-open shape the
    gate's `UNREADABLE` handling exists to close.
    """
    try:
        raw = json.loads(PROTOCOL_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError, RecursionError) as exc:
        raise ProtocolError(f"cannot read {PROTOCOL_PATH}: {exc}") from exc
    if not isinstance(raw, dict):
        raise ProtocolError(f"{PROTOCOL_PATH.name}: top level is not an object")

    schema = raw.get("snapshot_schema")
    # bool is an int in Python, and `True` would sail through an isinstance
    # check and then format as "True" in the gate's protocol string.
    if isinstance(schema, bool) or not isinstance(schema, int) or schema < 1:
        raise ProtocolError(
            f"{PROTOCOL_PATH.name}: 'snapshot_schema' must be a positive int, "
            f"got {schema!r}")

    pre = _tuple_of_names(raw.get("pre_resolution_buckets"),
                          "pre_resolution_buckets")
    post = _tuple_of_names(raw.get("post_resolution_buckets"),
                           "post_resolution_buckets")
    overlap = set(pre) & set(post)
    if overlap:
        # The two halves say WHERE a verdict was reached. A name in both is not
        # a vocabulary, it is an ambiguity, and `check_stub_behind_stamp` routes
        # on POST membership specifically.
        raise ProtocolError(
            f"{PROTOCOL_PATH.name}: bucket name(s) {sorted(overlap)} appear in "
            f"both the pre- and post-resolution halves")
    # The migration metadata (`renamed_buckets` / `retired_buckets`) is read by
    # the identity gate, not by the resolution rule: it declares INTENT about a
    # vocabulary change, and no runtime consumer needs it. Validated only for
    # shape, so a malformed declaration cannot look like an absent one.
    renamed = raw.get("renamed_buckets") or {}
    retired = raw.get("retired_buckets") or []
    if not isinstance(renamed, dict) or not isinstance(retired, list):
        raise ProtocolError(
            f"{PROTOCOL_PATH.name}: 'renamed_buckets' must be an object and "
            f"'retired_buckets' a list")
    return schema, pre, post


class _StrEnumBase(str, enum.Enum):
    """A `str`-valued enum that behaves identically on every supported Python.

    NOT `enum.StrEnum`, which is 3.11+ while this repo's floor is 3.8
    (`docs/infrastructure/development-tooling.md` python3 row; `setup.sh
    --versions` checks it). On a 3.8-3.10 host `enum.StrEnum` raises
    AttributeError at IMPORT, which would take out the resolver, the lint
    consumer and the identity gate before any of them could return their
    documented exit codes -- a check that cannot start is the fail-open shape
    those exit codes exist to prevent (Codex adversarial, section 20).

    The two dunders are pinned deliberately. A bare `(str, Enum)` mixin renders
    as `Bucket.PATH_ESCAPE` under `str()` and in f-strings before 3.12, so
    bucket names would reach logs and reports in a spelling nothing compares
    equal to. Pinning them to `str`'s gives StrEnum semantics on every version,
    which is what makes this a drop-in for the bare strings it replaces.
    """

    __str__ = str.__str__
    __format__ = str.__format__


def _member_name(bucket: str) -> str:
    """The enum member name for a bucket, by the one documented convention.

    UPPERCASE of the bucket name. The convention has to be TOTAL and INJECTIVE
    or `Bucket.PATH_ESCAPE` in the emitter could not be mapped back to a
    vocabulary entry without executing anything -- which is precisely what the
    section 20 retirement proof reads.
    """
    return bucket.upper()


def build_bucket_enum(all_buckets) -> type:
    """The published vocabulary as an enum type, derived from the inert JSON.

    WHY AN ENUM AND NOT THE BARE STRINGS (section 20). The resolver used to
    return bucket names as string literals, so the only available proof that it
    could no longer produce a retired name was a SOURCE-TEXT search for that
    literal -- and text absence is not proof of non-emission. An indexed or
    concatenated emission preserves behaviour and leaves the literal absent, so
    a two-commit sequence (refactor to indirect emission, then retire) passed
    the search while the resolver could still emit a name the vocabulary no
    longer declares.

    WHY IT IS STILL DERIVED FROM THE JSON. Spelling the members out in Python
    would put the vocabulary back in an EXECUTABLE file and undo section 18: a
    bucket migration would once more have to edit code the identity gate needs
    byte-identical, which is the exact condition that left the gate failing
    closed on every migration in section 16. Deriving them keeps a migration a
    data-only diff; what section 20 adds is a separate, explicitly declared
    EMITTED set in the resolver (`ref_resolution.EMITTED_BUCKETS`), which is
    where the emission contract lives.

    A `str`-valued enum and not a plain `Enum`: members ARE `str`, so equality,
    hashing, dict-keying, membership, f-strings and `json.dumps` behave exactly
    as the bare names did at every existing call site. See `_StrEnumBase` for
    why it is not `enum.StrEnum`.
    """
    members = {}
    for name in all_buckets:
        member = _member_name(name)
        # LOWERCASE, so the member name maps back to the bucket name by
        # `.lower()` alone. The identity gate must read the emitted set from a
        # tree MID-RETIREMENT, where a declared member is deliberately no longer
        # in the vocabulary -- so it cannot resolve `Bucket.PATH_ESCAPE` by
        # looking the name up. A bijective convention is what lets it recover
        # the name with no table at all.
        if name != name.lower():
            raise ProtocolError(
                f"{PROTOCOL_PATH.name}: bucket {name!r} is not lowercase; the "
                f"member/name mapping must be reversible without consulting "
                f"the vocabulary")
        if not member.isidentifier() or member[:1].isdigit():
            raise ProtocolError(
                f"{PROTOCOL_PATH.name}: bucket {name!r} does not yield a usable "
                f"enum member name ({member!r}); a bucket name must be a plain "
                f"identifier so the emitter can name it as `Bucket.{member}`")
        if member in members:
            # Two names differing only by case would map onto ONE member, so
            # `Bucket.X` in the emitter would be ambiguous and the retirement
            # proof could not say which of them it referred to.
            raise ProtocolError(
                f"{PROTOCOL_PATH.name}: buckets {members[member]!r} and "
                f"{name!r} collide on the enum member name {member!r}")
        members[member] = name
    return _StrEnumBase("Bucket", members)


SNAPSHOT_SCHEMA, PRE_RESOLUTION_BUCKETS, POST_RESOLUTION_BUCKETS = load()
# ORDER IS PART OF THE CONTRACT: the identity gate compares the ordered tuple,
# so a reorder is a protocol change even when the membership is identical.
ALL_BUCKETS = PRE_RESOLUTION_BUCKETS + POST_RESOLUTION_BUCKETS
Bucket = build_bucket_enum(ALL_BUCKETS)
