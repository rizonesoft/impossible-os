#!/usr/bin/env python3
"""Tests for subagent_audit.py leaf-transcript tool-use counting.

Codex audit 2026-07-13 (verified): the hook read the (often-absent)
`transcript_path` and never consumed a count, so a real 36-tool subagent
dispatch recorded 0 tool_uses and runaway detection was effectively dead. The
fix counts from the subagent's OWN (leaf) transcript `agent_transcript_path`,
where every tool_use belongs to the subagent (no parent/child boundary
ambiguity, so the 2026-04-28 concern about parent-transcript counting does not
apply).
"""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parent.parent.parent / ".claude/hooks/subagent_audit.py"


def _load():
    spec = importlib.util.spec_from_file_location("subagent_audit", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _write_leaf(path, n_tools):
    lines = []
    for _ in range(n_tools):
        lines.append(json.dumps(
            {"message": {"content": [{"type": "tool_use", "name": "Grep",
                                      "input": {}}]}}))
    # non-tool content lines must not be counted
    lines.append(json.dumps({"message": {"content": [{"type": "text",
                                                      "text": "hi"}]}}))
    lines.append(json.dumps({"type": "result"}))
    path.write_text("\n".join(lines) + "\n")


def test_leaf_count_counts_all_tool_uses():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "leaf.jsonl"
        _write_leaf(p, 36)
        assert mod._count_leaf_tool_uses(str(p)) == 36


def test_leaf_count_missing_or_empty_path_returns_none():
    mod = _load()
    assert mod._count_leaf_tool_uses("") is None
    assert mod._count_leaf_tool_uses("/no/such/file.jsonl") is None


def test_payload_populates_tool_uses_from_leaf():
    # End-to-end: run the hook in a throwaway git repo so _repo_root() (git
    # rev-parse) resolves to the temp tree and the log write stays there.
    with tempfile.TemporaryDirectory() as d:
        repo = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        (repo / ".claude/state").mkdir(parents=True)
        leaf = repo / "leaf.jsonl"
        _write_leaf(leaf, 33)
        payload = {"subagent_type": "kernel-explorer",
                   "agent_transcript_path": str(leaf)}
        proc = subprocess.run([sys.executable, str(HOOK)],
                              input=json.dumps(payload), text=True,
                              capture_output=True, cwd=str(repo))
        assert proc.returncode == 0, proc.stderr
        log = repo / ".claude/state/subagent-log.jsonl"
        rec = [json.loads(l) for l in log.read_text().splitlines()
               if l.strip()][-1]
        assert rec["tool_uses_total"] == 33, rec
        assert rec["subagent_type"] == "kernel-explorer", rec


def test_duration_is_derived_from_leaf_timestamps():
    """v17 close-out (2026-08-29): the duration arm was dead for three cycles
    because the harness never sends duration_ms (0 of 1955 payloads). The leaf
    transcript carries an ISO-8601 `timestamp` on every entry, so elapsed is
    last minus first; a single timestamp or an absent leaf stays None (the arm
    must not fire on a guess)."""
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        p = pathlib.Path(d) / "leaf.jsonl"
        p.write_text("\n".join([
            json.dumps({"timestamp": "2026-08-29T10:00:00.000Z", "message": {"content": []}}),
            "not json",
            json.dumps({"timestamp": "2026-08-29T10:00:07.500Z", "message": {"content": []}}),
        ]) + "\n")
        assert mod._leaf_duration_ms(str(p)) == 7500
        one = pathlib.Path(d) / "one.jsonl"
        one.write_text(json.dumps({"timestamp": "2026-08-29T10:00:00Z"}) + "\n")
        assert mod._leaf_duration_ms(str(one)) is None
        assert mod._leaf_duration_ms("/no/such/leaf.jsonl") is None


def test_payload_record_carries_derived_duration():
    with tempfile.TemporaryDirectory() as d:
        repo = pathlib.Path(d)
        subprocess.run(["git", "init", "-q", str(repo)], check=True)
        (repo / ".claude/state").mkdir(parents=True)
        leaf = repo / "leaf.jsonl"
        leaf.write_text("\n".join([
            json.dumps({"timestamp": "2026-08-29T10:00:00Z", "message": {"content": [
                {"type": "tool_use", "name": "Grep", "input": {}}]}}),
            json.dumps({"timestamp": "2026-08-29T10:00:03Z", "message": {"content": []}}),
        ]) + "\n")
        payload = {"subagent_type": "kernel-explorer", "agent_transcript_path": str(leaf)}
        proc = subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload),
                              text=True, capture_output=True, cwd=str(repo))
        assert proc.returncode == 0, proc.stderr
        log = repo / ".claude/state/subagent-log.jsonl"
        recs = [json.loads(l) for l in log.read_text().splitlines() if l.strip()]
        assert recs and recs[-1]["duration_ms"] == 3000, recs


if __name__ == "__main__":
    test_leaf_count_counts_all_tool_uses()
    test_leaf_count_missing_or_empty_path_returns_none()
    test_payload_populates_tool_uses_from_leaf()
    test_duration_is_derived_from_leaf_timestamps()
    test_payload_record_carries_derived_duration()
    print("PASS: subagent_audit leaf-transcript counting")
