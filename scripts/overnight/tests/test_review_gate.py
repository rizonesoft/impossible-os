#!/usr/bin/env python3
"""P1.2 / P1.3 review-gate fixes (surgical, fail-safe).

P1.3 -- the review BROKER detaches its review and returns instantly, so
`is_background_dispatch` must recognize it (its instant return is not a completed
review); `is_review_broker_dispatch` keeps it stampable (a completed broker leg
is real review proof, unlike a generic `task --background`).

P1.2 -- `_review_evidence` now (a) rejects a non-intersecting STALE record with a
precise message, and (b) when the single latest record's received:false was reset
by a newer multi-kind trigger, falls back to the received-review HISTORY and
accepts only if a genuinely-received review blob-covers the current staged tree.
Every acceptance stays content-bound (blob SHA), so nothing is loosened.
"""
import importlib.util
import json
import pathlib
import subprocess
import sys
import time
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent


def _load(name, rel):
    spec = importlib.util.spec_from_file_location(name, REPO / rel)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


cd = _load("cd", ".claude/hooks/_codex_dispatch.py")
scg = _load("scg", ".claude/hooks/section_commit_gate.py")
scg._ensure_crc()
crc = scg.crc


# ---------------------------------------------------------------- P1.3 detector
def test_broker_is_background_and_stampable():
    broker = ("bash scripts/overnight/review-broker-codex-dispatch.sh "
              "'[review-kind: adversarial] todo/x.md body'")
    assert cd.is_background_dispatch(broker) is True
    assert cd.is_review_broker_dispatch(broker) is True


def test_foreground_wrapper_unaffected():
    fg = "bash scripts/codex-dispatch.sh '[review-kind: design] todo/x.md body'"
    assert cd.is_background_dispatch(fg) is False
    assert cd.is_review_broker_dispatch(fg) is False


def test_generic_bg_task_is_background_but_not_broker():
    bg = "node /x/codex-companion.mjs task --background 'review-kind: design ...'"
    assert cd.is_background_dispatch(bg) is True
    assert cd.is_review_broker_dispatch(bg) is False


# ------------------------------------------------------------ P1.2 evidence
def _repo(d):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    (root / "src/kernel").mkdir(parents=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "src/kernel/x.c").write_text("int x(void){return 1;}\n")
    subprocess.run(["git", "-C", str(root), "add", "src/kernel/x.c"], check=True)
    return root


def _state(root, **kw):
    (root / ".claude/state/last-codex-review.json").write_text(json.dumps(kw))


def test_received_true_and_covered_accepts():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        assert staged, "fixture must stage a source file"
        blobs = crc._staged_source_blobs(root, staged)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=staged,
               trigger_blobs=blobs, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert ok, why


def test_non_intersecting_record_is_stale_rejected():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        _state(root, received=True, received_timestamp_ns=time.time_ns(),
               trigger="Bash(review)", trigger_files=["src/other/y.c"],
               trigger_blobs={"src/other/y.c": "100644:deadbeef"}, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert not ok and "STALE" in why, why


def test_received_false_but_history_covers_accepts():
    # The churn fix: a newer kind's trigger reset received:false, but a prior
    # RECEIVED review in the history covers the exact current staged content.
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        blobs = crc._staged_source_blobs(root, staged)
        _state(root, received=False, received_timestamp_ns=None,
               trigger="Bash(perf)", trigger_files=staged,
               trigger_blobs=blobs, head_sha="abc123")
        (root / ".claude/state/codex-review-history.jsonl").write_text(
            json.dumps({"received": True, "received_timestamp_ns": time.time_ns(),
                        "trigger_blobs": blobs}) + "\n")
        ok, why = scg._review_evidence(root, staged)
        assert ok, why


def test_received_false_and_no_history_blocks():
    with tempfile.TemporaryDirectory() as d:
        root = _repo(d)
        staged = scg._staged_source_files(root)
        blobs = crc._staged_source_blobs(root, staged)
        _state(root, received=False, received_timestamp_ns=None,
               trigger="Bash(perf)", trigger_files=staged,
               trigger_blobs=blobs, head_sha="abc123")
        ok, why = scg._review_evidence(root, staged)
        assert not ok and "was not" in why, why


if __name__ == "__main__":
    test_broker_is_background_and_stampable()
    test_foreground_wrapper_unaffected()
    test_generic_bg_task_is_background_but_not_broker()
    test_received_true_and_covered_accepts()
    test_non_intersecting_record_is_stale_rejected()
    test_received_false_but_history_covers_accepts()
    test_received_false_and_no_history_blocks()
    print("PASS: review-gate P1.2/P1.3")
