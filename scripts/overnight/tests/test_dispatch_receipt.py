#!/usr/bin/env python3
"""WS2 -- a fresh, content-bound section-pack satisfies agent_dispatch_required
in lieu of a discovery-agent dispatch. The load-bearing property is FAIL-CLOSED:
an absent, stale, or wrong-section receipt must NOT satisfy the gate.

Tests the receipt predicate directly (the acceptance logic), so it needs no
headless-session harness."""
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "scripts/overnight"))
from worktree_hash import worktree_key  # noqa: E402

_spec = importlib.util.spec_from_file_location(
    "adr", REPO / ".claude/hooks/agent_dispatch_required.py")
adr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(adr)

_rspec = importlib.util.spec_from_file_location(
    "rdg", REPO / ".claude/hooks/review_dispatch_gate.py")
rdg = importlib.util.module_from_spec(_rspec)
_rspec.loader.exec_module(rdg)


def _fixture(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "src/kernel").mkdir(parents=True)
    (root / "todo").mkdir()
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "todo/TODO-42.md").write_text("# t\n## 1. S\n- [ ] x\n")
    (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
    (root / ".claude/state/sequencer-run.json").write_text(
        json.dumps({"active": True, "phase": "SECTIONS",
                    "file": "todo/TODO-42.md"}))
    return root


def _write_receipt(root, todo, key_inputs, digest):
    (root / ".claude/state/last-section-pack.json").write_text(json.dumps({
        "todo": todo, "section": 1, "digest": digest,
        "key_inputs": key_inputs, "ts_ns": 1, "pack_path": "x"}))


def test_receipt_fail_closed_and_fresh_accept():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        key_inputs = ["todo/TODO-42.md", "src/kernel/x.c"]
        # A) no receipt -> not satisfied (fail-closed)
        assert adr._fresh_section_pack(root) is False
        # B) fresh receipt (digest == current worktree key) -> satisfied
        good = worktree_key(root, key_inputs)
        _write_receipt(root, "todo/TODO-42.md", key_inputs, good)
        assert adr._fresh_section_pack(root) is True
        # C) edit an input -> recomputed key != recorded digest -> stale, refused
        (root / "src/kernel/x.c").write_text("int x(void){return 2;}\n")
        assert adr._fresh_section_pack(root) is False
        # D) receipt for a DIFFERENT todo than the cursor -> refused
        (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")  # restore
        assert adr._fresh_section_pack(root) is True  # fresh again
        _write_receipt(root, "todo/OTHER.md", key_inputs,
                       worktree_key(root, key_inputs))
        assert adr._fresh_section_pack(root) is False


def test_malformed_receipt_refused():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        (root / ".claude/state/last-section-pack.json").write_text("{not json")
        assert adr._fresh_section_pack(root) is False
        (root / ".claude/state/last-section-pack.json").write_text(
            json.dumps({"todo": "todo/TODO-42.md"}))  # missing digest/inputs
        assert adr._fresh_section_pack(root) is False


def test_diff_facts_receipt_fail_closed_and_window():
    with tempfile.TemporaryDirectory() as d:
        root = _fixture(d)
        files = ["src/kernel/x.c"]
        window = 1000
        rp = root / ".claude/state/last-diff-facts.json"
        # A) absent -> refused
        assert rdg._fresh_diff_facts(root, window) is False
        # B) fresh + in-window -> accepted
        rp.write_text(json.dumps({"digest": worktree_key(root, files),
                                  "changed_files": files, "ts_ns": window + 5}))
        assert rdg._fresh_diff_facts(root, window) is True
        # C) out-of-window (older than pass start) -> refused
        rp.write_text(json.dumps({"digest": worktree_key(root, files),
                                  "changed_files": files, "ts_ns": window - 5}))
        assert rdg._fresh_diff_facts(root, window) is False
        # D) stale content (edit since the receipt) -> refused
        rp.write_text(json.dumps({"digest": worktree_key(root, files),
                                  "changed_files": files, "ts_ns": window + 5}))
        (root / "src/kernel/x.c").write_text("int x(void){return 9;}\n")
        assert rdg._fresh_diff_facts(root, window) is False


if __name__ == "__main__":
    test_receipt_fail_closed_and_fresh_accept()
    test_malformed_receipt_refused()
    test_diff_facts_receipt_fail_closed_and_window()
    print("PASS: dispatch-receipt (WS2)")
