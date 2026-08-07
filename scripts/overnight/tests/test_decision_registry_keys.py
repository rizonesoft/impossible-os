#!/usr/bin/env python3
"""decision-registry must read the keys the cache PRODUCER emits.

It asked for `target` / `target_file` / `text` / `raw`. The cache has never
contained any of them -- the real fields are `target_path`, `target_section`,
`item_name`, `severity`. So all 994 stamp-xref records carried a title ending
in "-> " with nothing after it and a body that fell through to a raw
`json.dumps`, and 923 ownership decisions were indexed with an empty target.

It stayed invisible for the reason such bugs usually do: the reader is
defensive at every step (`or ""`, `or json.dumps(x)`, a bare except around the
load), so WRONG KEY NAMES degrade to empty output instead of raising. Nothing
was ever red.

These tests pin the producer/consumer contract from BOTH sides, so renaming a
field on either side fails here rather than silently emptying the registry.
"""
from __future__ import annotations

import importlib.util
import json
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
TOOL = REPO / "scripts/overnight/decision-registry.py"
CACHE = REPO / "build/todo-cache.json"


def _load():
    spec = importlib.util.spec_from_file_location("decision_registry", TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _xrefs():
    if not CACHE.is_file():
        return []
    nodes = json.loads(CACHE.read_text(encoding="utf-8"))
    return [x for n in nodes for x in (n.get("stamps_xrefs") or [])
            if isinstance(x, dict)]


def test_producer_still_emits_the_fields_the_reader_reads():
    """The contract, from the PRODUCER side. If the cache stops emitting these,
    the reader silently empties again."""
    xs = _xrefs()
    if not xs:
        return  # no cache in this environment; the consumer test still runs
    for field in ("kind", "target_path", "target_section", "severity"):
        assert any(field in x for x in xs), f"cache no longer emits {field!r}"


def test_reader_does_not_ask_for_fields_that_do_not_exist():
    """The contract, from the CONSUMER side. `target` and `target_file` were
    never emitted; asking for them is what produced 923 empty decisions."""
    src = TOOL.read_text(encoding="utf-8")
    seg = src.split("def extract_stamp_xrefs", 1)[1].split("\ndef ", 1)[0]
    for dead in ('x.get("target")', 'x.get("target_file")',
                 'x.get("text")', 'x.get("raw")'):
        assert dead not in seg, f"reads a field the producer never emits: {dead}"


def test_records_carry_a_real_target():
    """The outcome that matters: no record may end in a dangling arrow."""
    mod = _load()
    recs = mod.extract_stamp_xrefs(REPO)
    if not recs:
        return
    empty = [r for r in recs if str(r.get("title", "")).rstrip().endswith("->")]
    assert not empty, f"{len(empty)} record(s) still have an empty target"
    assert any(str(r.get("body", "")).startswith("severity:") for r in recs), \
        "no record carries the producer's severity -- body is still a fallback"


if __name__ == "__main__":
    test_producer_still_emits_the_fields_the_reader_reads()
    test_reader_does_not_ask_for_fields_that_do_not_exist()
    test_records_carry_a_real_target()
    print("PASS: decision-registry reads the producer's real keys")
