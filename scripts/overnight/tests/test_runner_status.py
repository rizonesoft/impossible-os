#!/usr/bin/env python3
import json, os, subprocess, sys, tempfile, time, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent  # scripts/overnight/tests -> repo root
SCRIPT = REPO / ".claude" / "hooks" / "runner_status.py"


def _run(root, anchor=False, conclave_home=None):
    args = [sys.executable, str(SCRIPT), "--root", str(root)]
    if anchor:
        args.append("--anchor")
    env = dict(os.environ)
    # Isolate from any real ~/conclave jobs unless a test opts in.
    env["CONCLAVE_HOME"] = str(conclave_home or (pathlib.Path(root) / "_no_conclave_home"))
    r = subprocess.run(args, text=True, capture_output=True, env=env)
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


def test_conclave_jobs_pending_and_done_surface():
    with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory() as home:
        root = _mkroot(d, state={"active": True, "phase": "SECTIONS"})
        jobs = pathlib.Path(home) / "data" / "projects" / "impossible-os" / "jobs"
        jobs.mkdir(parents=True)
        (jobs / "a.meta.json").write_text(json.dumps({"id": "a", "status": "pending", "target": "build"}))
        (jobs / "b.meta.json").write_text(json.dumps({"id": "b", "status": "done", "target": "test"}))
        # collected job (has outcome) must NOT be counted
        (jobs / "c.meta.json").write_text(json.dumps({"id": "c", "status": "done", "outcome": "resolved"}))
        anchor = _run(root, anchor=True, conclave_home=home)
        assert "conclave:1p/1d" in anchor, anchor
        full = _run(root, conclave_home=home)
        assert "CONCLAVE JOBS: 1 pending, 1 done" in full and "DONE b" in full, full


def test_no_conclave_jobs_no_conclave_line():
    with tempfile.TemporaryDirectory() as d:
        root = _mkroot(d, state={"active": True, "phase": "SECTIONS"})
        assert "conclave:" not in _run(root, anchor=True)
        assert "CONCLAVE JOBS" not in _run(root)


if __name__ == "__main__":
    test_anchor_has_phase_and_counts()
    test_unreceived_codex_is_an_obligation()
    test_expired_gotcha_dropped()
    test_missing_sources_fail_open()
    test_conclave_jobs_pending_and_done_surface()
    test_no_conclave_jobs_no_conclave_line()
    print("PASS: runner_status")
