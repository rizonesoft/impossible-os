#!/usr/bin/env python3
import json, subprocess, sys, tempfile, time, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # scripts/overnight/tests -> repo root
SCRIPT = REPO / ".claude" / "hooks" / "runner_status.py"


def _run(root, anchor=False):
    args = [sys.executable, str(SCRIPT), "--root", str(root)]
    if anchor:
        args.append("--anchor")
    r = subprocess.run(args, text=True, capture_output=True)
    assert r.returncode == 0, r.stderr
    return r.stdout


def _mkroot(d, *, state=None, gotchas=None, codex=None):
    root = pathlib.Path(d)
    (root / ".claude" / "state").mkdir(parents=True, exist_ok=True)
    if state is not None:
        (root / ".claude" / "state" / "sequencer-run.json").write_text(json.dumps(state))
    if gotchas is not None:
        (root / ".claude" / "state" / "live-gotchas.md").write_text(gotchas)
    if codex is not None:
        (root / ".claude" / "state" / "last-codex-review.json").write_text(json.dumps(codex))
    return root


def test_anchor_has_phase_and_counts():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "SECTIONS", "file": "TODO-08.md"},
                       gotchas="- 2026-06-14: recorder dead\n")
        out = _run(root, anchor=True).strip()
        assert "phase SECTIONS" in out
        assert "gotchas:1" in out
        assert "obligations:0" in out


def test_unreceived_codex_is_an_obligation():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "SECTIONS"},
                       codex={"received": False, "timestamp_ns": time.time_ns()})
        out = _run(root, anchor=True)
        assert "obligations:1" in out
        full = _run(root)
        assert "Codex review unreceived" in full


def test_expired_gotcha_dropped():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "TRIAGE"},
                       gotchas="- 2020-01-01: old hazard (expires 2020-02-01)\n- 2026-06-14: live one\n")
        out = _run(root, anchor=True)
        assert "gotchas:1" in out


def test_missing_sources_fail_open():
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)  # nothing created
        out = _run(root)
        assert "WHERE" in out  # still prints a brief


if __name__ == "__main__":
    test_anchor_has_phase_and_counts()
    test_unreceived_codex_is_an_obligation()
    test_expired_gotcha_dropped()
    test_missing_sources_fail_open()
    print("PASS: runner_status")
