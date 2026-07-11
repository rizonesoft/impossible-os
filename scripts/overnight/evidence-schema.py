#!/usr/bin/env python3
"""Typed evidence-envelope schema + mechanical validator (review-result-v1).

Analysts return several incompatible prose shapes today, and the Codex
envelope re-extracts findings with severity regexes. This standardizes a
single validated JSON envelope so the main session (Opus) receives compact
typed facts, not transcripts -- and a malformed envelope is REJECTED
mechanically (no model spent deciding whether an analyst's prose is complete).

Envelope (review-result-v1):
  {
    "schema": "review-result-v1",
    "scope_digest": "<what was examined: files + blob shas or a hash>",
    "coverage":  ["<claim/item/area checked>", ...],
    "findings":  [{"severity":"critical|high|medium|low",
                   "file":"...", "line":<int>, "summary":"..."}...],
    "unknowns":  ["<what could not be determined>", ...],
    "confidence":"high|medium|low",
    "artifact":  {"path":"...", "sha256":"..."}   # optional
  }

Stdlib only. `validate.py FILE` / stdin; `template` prints a blank envelope.
Importable: `ok, errors = validate(obj)`.
"""
from __future__ import annotations

import json
import sys

SCHEMA = "review-result-v1"
SEVERITIES = {"critical", "high", "medium", "low"}
CONFIDENCE = {"high", "medium", "low"}


def validate(obj) -> tuple[bool, list]:
    e: list = []
    if not isinstance(obj, dict):
        return False, ["envelope is not a JSON object"]
    if obj.get("schema") != SCHEMA:
        e.append(f"schema must be '{SCHEMA}' (got {obj.get('schema')!r})")
    if not isinstance(obj.get("scope_digest"), str) or not obj.get("scope_digest"):
        e.append("scope_digest must be a non-empty string")
    for listkey in ("coverage", "unknowns"):
        if not isinstance(obj.get(listkey), list):
            e.append(f"{listkey} must be a list")
    if obj.get("confidence") not in CONFIDENCE:
        e.append(f"confidence must be one of {sorted(CONFIDENCE)}")
    findings = obj.get("findings")
    if not isinstance(findings, list):
        e.append("findings must be a list")
    else:
        for i, f in enumerate(findings):
            if not isinstance(f, dict):
                e.append(f"findings[{i}] is not an object")
                continue
            if f.get("severity") not in SEVERITIES:
                e.append(f"findings[{i}].severity invalid ({f.get('severity')!r})")
            if not isinstance(f.get("file"), str) or not f.get("file"):
                e.append(f"findings[{i}].file must be a non-empty string")
            if not isinstance(f.get("line"), int):
                e.append(f"findings[{i}].line must be an int")
            if not isinstance(f.get("summary"), str) or not f.get("summary"):
                e.append(f"findings[{i}].summary must be a non-empty string")
    art = obj.get("artifact")
    if art is not None:
        if not isinstance(art, dict) or not isinstance(art.get("path"), str) \
                or not isinstance(art.get("sha256"), str):
            e.append("artifact, when present, needs string path + sha256")
    return (not e), e


TEMPLATE = {
    "schema": SCHEMA,
    "scope_digest": "",
    "coverage": [],
    "findings": [],
    "unknowns": [],
    "confidence": "medium",
    "artifact": {"path": "", "sha256": ""},
}


def main(argv) -> int:
    if argv and argv[0] == "template":
        print(json.dumps(TEMPLATE, indent=1))
        return 0
    src = argv[0] if argv and argv[0] != "-" else None
    try:
        raw = open(src).read() if src else sys.stdin.read()
        obj = json.loads(raw)
    except (OSError, ValueError) as exc:
        print(json.dumps({"ok": False, "errors": [f"unparseable: {exc}"]}))
        return 1
    ok, errors = validate(obj)
    print(json.dumps({"ok": ok, "errors": errors}, indent=1))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
