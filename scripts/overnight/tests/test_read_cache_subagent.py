#!/usr/bin/env python3
"""A SUBAGENT's Read must never be charged to the MAIN session.

MEASURED 2026-08-06: a `section-context-mapper` dispatch read
`scripts/todo-graph/identity-gate.sh`, and the very next MAIN-session Read of
the same file was blocked as an already-cached re-read. The agent's read had
been recorded as the main session's.

This inverts the trust contract, which is why it matters more than the two
wasted calls it costs. Doctrine REQUIRES the main session to verify every
load-bearing agent claim at file:line -- and this gate was refusing exactly
that verification, on every agent-then-verify cycle, which the
interactive-offload doctrine makes the default route for any nontrivial task.

Mechanism: the PreToolUse payload delivered INSIDE a subagent carries the
PARENT's `transcript_path`, so a heuristic keyed on that path sees a main
session. The harness names the agent's own transcript separately
(`agent_transcript_path`). `runner_bash_guard.is_subagent_payload` was written
for this same misclassification in a different hook on 2026-07-31; this hook
had not been switched over.
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/read_cache_block.py"

SESSION = "/home/u/.claude/projects/p/SESSION.jsonl"
AGENT = "/home/u/.claude/projects/p/SESSION/subagents/agent-7.jsonl"


def _load():
    if str(HOOK.parent) not in sys.path:
        sys.path.insert(0, str(HOOK.parent))
    spec = importlib.util.spec_from_file_location("read_cache_block", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_subagent_payload_is_recognised_despite_the_parents_transcript_path():
    """The exact shape that failed: parent transcript + agent identity."""
    mod = _load()
    sub = {"transcript_path": SESSION, "agent_transcript_path": AGENT,
           "session_id": "SESSION"}
    assert mod._is_subagent_payload(sub), (
        "subagent payload not recognised -- its read will be charged to the "
        "main session and block the verification doctrine requires")


def test_a_main_session_payload_is_not_mistaken_for_a_subagent():
    """The other direction. If everything reads as a subagent the gate is dead,
    and the gate exists for a real cost (1,412 Reads over 180 files)."""
    mod = _load()
    assert not mod._is_subagent_payload(
        {"transcript_path": SESSION, "session_id": "SESSION"})


def test_the_transcript_heuristic_alone_is_NOT_sufficient():
    """Pins WHY both detectors are needed, so neither is 'simplified' away.

    A subagent's payload carries the parent's transcript_path, which the path
    heuristic reads as a main session -- that is the whole bug.
    """
    mod = _load()
    assert not mod._is_subagent_transcript(SESSION), (
        "parent transcript should NOT look like a subagent -- if it does, this "
        "test proves nothing")
    assert mod._is_subagent_transcript(AGENT), "agent path must still match"


if __name__ == "__main__":
    test_subagent_payload_is_recognised_despite_the_parents_transcript_path()
    test_a_main_session_payload_is_not_mistaken_for_a_subagent()
    test_the_transcript_heuristic_alone_is_NOT_sufficient()
    print("PASS: a subagent's Read is not charged to the main session")
