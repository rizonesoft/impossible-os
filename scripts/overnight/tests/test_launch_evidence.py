#!/usr/bin/env python3
"""The review recorder credits only a review a wrapper actually launched.

A launch is proven by the wrapper's own launch line (the broker's {logFile}
line per registered job, the direct dispatcher's transcript line printed once
nothing can refuse any more), never by the absence of a refusal banner. Every
writer of last-codex-review.json goes through one locked update, and a trigger
is published once and never over a newer one.

Run: python3 -m pytest -q scripts/overnight/tests/test_launch_evidence.py
"""
import importlib
import json
import os
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
HOOK = REPO / ".claude/hooks/codex_review_completed.py"
sys.path.insert(0, str(REPO / ".claude/hooks"))
crc = importlib.import_module("codex_review_completed")

BROKER = "bash scripts/overnight/review-broker-codex-dispatch.sh"
TODO = "todo/00-x/TODO-01-x.md"


def _repo(tmp):
    root = pathlib.Path(tmp)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / ".claude/state").mkdir(parents=True)
    (root / TODO).parent.mkdir(parents=True)
    (root / TODO).write_text("# x\n## 1. One\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.name=t", "-c", "user.email=t@t",
                    "commit", "-qm", "x"], check=True)
    return root


def _dispatch(kind):
    return f"{BROKER} '[review-kind: {kind}] {TODO} section 1 body'"


def _hook(root, command, stdout="", stderr=""):
    payload = {"tool_name": "Bash", "session_id": "s", "tool_input": {"command": command},
               "tool_response": {"stdout": stdout, "stderr": stderr}}
    env = {k: v for k, v in os.environ.items() if not k.startswith("SKIP_")}
    subprocess.run([sys.executable, str(HOOK)], input=json.dumps(payload), text=True,
                   cwd=root, env=env, capture_output=True, timeout=60)
    state = root / ".claude/state/last-codex-review.json"
    stamps = root / ".claude/state/last-review-stamps.json"
    return (json.loads(state.read_text()) if state.exists() else None,
            json.loads(stamps.read_text()) if stamps.exists() else {})


def _job(kind, log="/x/a.out"):
    return json.dumps({"kind": kind, "jobId": "detached:j", "logFile": log})


def test_a_registered_broker_job_is_recorded_and_stamped(tmp_path):
    state, stamps = _hook(_repo(tmp_path), _dispatch("adversarial"), stdout=_job("adversarial") + "\n")
    assert state and state["review_kind"] == "adversarial" and state["log_file"] == "/x/a.out"
    assert isinstance(stamps.get(TODO, {}).get("adversarial"), int), stamps


def test_a_broker_refusal_records_nothing_whatever_stream_it_is_on(tmp_path):
    root = _repo(tmp_path)
    banner = "[review-broker] BLOCK -- expected exactly one prompt argument.\n"
    for out, err in (("", banner), (banner, "")):          # stderr, or redirected 2>&1
        state, stamps = _hook(root, _dispatch("adversarial") + " extra", stdout=out, stderr=err)
        assert state is None and stamps == {}, (state, stamps)


def test_a_refused_leg_beside_a_launched_one_earns_no_stamp(tmp_path):
    cmd = _dispatch("adversarial") + " extra; " + _dispatch("perf")
    _state, stamps = _hook(_repo(tmp_path), cmd, stdout=_job("perf", "/x/p.out") + "\n",
                           stderr="[review-broker] BLOCK -- expected exactly one prompt argument.\n")
    entry = stamps.get(TODO, {})
    assert isinstance(entry.get("perf"), int) and not entry.get("adversarial"), entry


def test_the_direct_launch_line_names_its_kind():
    resp = {"tool_response": {"stderr": "[codex-dispatch] transcript: /r/20261121-140308-249807-round-direct.out\n"}}
    assert crc._launched_kinds(resp) == ["round"]
    assert crc._launched_kinds({"tool_response": {"stderr": "[codex-dispatch] launched: direct\n"}}) == ["direct"]
    # A refusal before the launch line prints only its banner.
    moved = {"tool_response": {"stderr": "[codex-dispatch] BLOCK -- HEAD moved from a to b\n"}}
    assert crc._launched_kinds(moved) == []


def test_the_direct_dispatcher_announces_only_after_its_last_refusal():
    text = (REPO / "scripts/codex-dispatch.sh").read_text()
    assert text.index("HEAD moved from") < text.index('echo "[codex-dispatch] transcript: $LOG_FILE"')


def test_a_trigger_never_replaces_a_newer_one(tmp_path):
    path = tmp_path / "last-codex-review.json"
    path.write_text(json.dumps({"timestamp_ns": 20, "review_kind": "round"}))
    crc._publish_trigger(path, {"timestamp_ns": 10, "review_kind": "adversarial"})
    assert json.loads(path.read_text())["review_kind"] == "round"
    crc._publish_trigger(path, {"timestamp_ns": 30, "review_kind": "perf"})
    assert json.loads(path.read_text())["review_kind"] == "perf"


def test_a_receive_that_cannot_take_the_lock_leaves_the_review_pending(tmp_path):
    import fcntl
    path = tmp_path / "last-codex-review.json"
    path.write_text(json.dumps({"timestamp_ns": 1, "received": False}))
    fd = os.open(str(path) + ".lock", os.O_RDWR | os.O_CREAT, 0o644)
    fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        got = crc._update_state(path, lambda cur: {**cur, "received": True})
    finally:
        os.close(fd)
    assert got is None and json.loads(path.read_text())["received"] is False
