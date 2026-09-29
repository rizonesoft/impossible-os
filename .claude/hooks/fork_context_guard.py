#!/usr/bin/env python3
# block-via: exit 2 (fork dispatch from a parent context above THRESHOLD tokens, no [fork-ok] marker)
"""Refuse a `fork` dispatch that would drag a huge parent context along.

A fork inherits the parent's WHOLE conversation and its model, and every fork
turn re-reads that context. MEASURED 2026-09-29 over the retained subagent
transcripts: forks from Opus sessions started at a median ~700K tokens, ran ~49
turns and cost ~$8 each, against ~$1 for a named Sonnet agent doing the same
legwork from a ~20-45K start. Forks from small contexts (median ~80K) are cheap
and pass untouched.

The hook reads the parent's current context size from the last assistant usage
record in `transcript_path` (input + cache read + cache creation). Above
THRESHOLD it BLOCKS once with the alternative: a named agent
(section-context-mapper, kernel-explorer, a researcher) and a self-contained
prompt. A fork that genuinely needs this conversation re-dispatches with
`[fork-ok: <reason>]` in its prompt.

Opt-outs: `[fork-ok` in the prompt; FORK_CONTEXT_GUARD_DISABLE=1. Fail-open
everywhere: an unreadable transcript never blocks. Code: [FORK-CONTEXT].
Selftest: python3 fork_context_guard.py --selftest
"""
from __future__ import annotations

import json
import os
import sys

THRESHOLD = 150_000
TAIL_BYTES = 4 * 1024 * 1024

_MSG = ("[FORK-CONTEXT] BLOCKED: a fork inherits this session's full context (~{k}K tokens) and model, "
        "and re-reads it on every turn (measured 2026-09-29: forks from large Opus contexts averaged ~$8 each, "
        "a named Sonnet agent ~$1). Dispatch a named agent instead (section-context-mapper, kernel-explorer, "
        "a researcher) with a self-contained prompt that names the files and the question. If the task truly "
        "needs this conversation, re-dispatch the fork with `[fork-ok: <reason>]` in its prompt.")


def context_tokens(transcript: str) -> int | None:
    """Context size of the parent's latest assistant turn, or None if unknown."""
    try:
        with open(transcript, "rb") as fh:
            fh.seek(0, os.SEEK_END)
            size = fh.tell()
            fh.seek(max(0, size - TAIL_BYTES))
            lines = fh.read().splitlines()
    except OSError:
        return None
    for raw in reversed(lines):
        if b'"usage"' not in raw:
            continue
        try:
            d = json.loads(raw)
        except ValueError:
            continue
        if d.get("isSidechain"):
            continue
        u = (d.get("message") or {}).get("usage")
        if isinstance(u, dict):
            return sum(int(u.get(k) or 0) for k in
                       ("input_tokens", "cache_read_input_tokens", "cache_creation_input_tokens"))
    return None


def verdict(payload: dict) -> str | None:
    """The block message, or None to allow."""
    if os.environ.get("FORK_CONTEXT_GUARD_DISABLE") == "1":
        return None
    if payload.get("tool_name") not in ("Agent", "Task"):
        return None
    ti = payload.get("tool_input") or {}
    if ti.get("subagent_type") != "fork":
        return None
    if "[fork-ok" in str(ti.get("prompt") or ""):
        return None
    tokens = context_tokens(str(payload.get("transcript_path") or ""))
    if tokens is None or tokens <= THRESHOLD:
        return None
    return _MSG.format(k=tokens // 1000)


def main() -> int:
    try:
        payload = json.load(sys.stdin)
    except ValueError:
        return 0
    if not isinstance(payload, dict):
        return 0
    msg = verdict(payload)
    if msg:
        sys.stderr.write(msg + "\n")
        return 2
    return 0


def _selftest() -> int:
    import tempfile

    def transcript(ctx: int, sidechain_after: bool = False) -> str:
        fd, path = tempfile.mkstemp(suffix=".jsonl")
        with os.fdopen(fd, "w") as fh:
            fh.write(json.dumps({"type": "user", "message": {"content": "hi"}}) + "\n")
            fh.write(json.dumps({"message": {"usage": {"input_tokens": 10, "cache_read_input_tokens": ctx - 10,
                                                        "cache_creation_input_tokens": 0}}}) + "\n")
            if sidechain_after:
                fh.write(json.dumps({"isSidechain": True, "message": {"usage": {"input_tokens": 5}}}) + "\n")
        return path

    big, small, side = transcript(700_000), transcript(80_000), transcript(700_000, sidechain_after=True)

    def pay(atype="fork", prompt="map the scheduler", path=big, tool="Agent"):
        return {"tool_name": tool, "transcript_path": path,
                "tool_input": {"subagent_type": atype, "prompt": prompt}}

    cases = [
        ("large-context fork blocks", pay(), True),
        ("Task spelling blocks too", pay(tool="Task"), True),
        ("sidechain usage is ignored", pay(path=side), True),
        ("small-context fork passes", pay(path=small), False),
        ("[fork-ok] marker passes", pay(prompt="[fork-ok: needs our findings] continue"), False),
        ("named agent passes", pay(atype="section-context-mapper"), False),
        ("missing transcript fails open", pay(path="/nonexistent.jsonl"), False),
    ]
    bad = [name for name, p, want in cases if (verdict(p) is not None) != want]
    os.environ["FORK_CONTEXT_GUARD_DISABLE"] = "1"
    if verdict(pay()) is not None:
        bad.append("disable env passes")
    del os.environ["FORK_CONTEXT_GUARD_DISABLE"]
    for p in (big, small, side):
        os.unlink(p)
    if bad:
        sys.stderr.write("fork_context_guard selftest FAIL: " + "; ".join(bad) + "\n")
        return 1
    print(f"fork_context_guard selftest OK ({len(cases) + 1} cases)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_selftest() if "--selftest" in sys.argv else main())
    except Exception:
        sys.exit(0)   # fail-open: a broken guard must never wedge a session
