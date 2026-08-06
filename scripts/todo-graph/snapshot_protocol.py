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


SNAPSHOT_SCHEMA, PRE_RESOLUTION_BUCKETS, POST_RESOLUTION_BUCKETS = load()
# ORDER IS PART OF THE CONTRACT: the identity gate compares the ordered tuple,
# so a reorder is a protocol change even when the membership is identical.
ALL_BUCKETS = PRE_RESOLUTION_BUCKETS + POST_RESOLUTION_BUCKETS
