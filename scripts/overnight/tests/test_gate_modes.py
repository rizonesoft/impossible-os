#!/usr/bin/env python3
"""Process gates can log instead of refuse; product and safety gates cannot."""
import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[3]
GATE = REPO / ".claude/hooks/gate.py"
sys.path.insert(0, str(GATE.parent))
import gate  # noqa: E402

REFUSER = "import sys\nsys.stderr.write('refused: because')\nsys.exit(2)\n"


def _run(d, hook_name, body, modes):
    root = pathlib.Path(d)
    (root / ".claude").mkdir(exist_ok=True)
    if modes is not None:
        (root / ".claude/gate-modes.json").write_text(modes)
    hook = root / f"{hook_name}.py"
    hook.write_text(body)
    return subprocess.run([sys.executable, str(GATE), str(hook)], input=b"{}",
                          capture_output=True,
                          env={**os.environ, "CLAUDE_PROJECT_DIR": str(root)})


def test_log_mode_lets_the_call_go_and_records_it():
    with tempfile.TemporaryDirectory() as d:
        r = _run(d, "skill_step_block", REFUSER, json.dumps({"skill_step_block": "log"}))
        assert r.returncode == 0 and not r.stderr, r
        line = json.loads((pathlib.Path(d) / ".claude/overnight/gate-log.jsonl").read_text())
        assert line["gate"] == "skill_step_block" and "because" in line["text"]


def test_refusal_enforce_mode_refuses():
    with tempfile.TemporaryDirectory() as d:
        r = _run(d, "skill_step_block", REFUSER, json.dumps({"skill_step_block": "enforce"}))
        assert r.returncode == 2 and b"because" in r.stderr, r


def test_refusal_no_file_or_a_bad_file_means_enforce():
    for modes in (None, "{not json", "[]"):
        with tempfile.TemporaryDirectory() as d:
            assert _run(d, "skill_step_block", REFUSER, modes).returncode == 2, modes


def test_refusal_data_cannot_turn_off_a_gate_outside_the_list():
    for name in ("section_commit_gate", "attended_repair_guard", "SEQ-TEARDOWN"):
        with tempfile.TemporaryDirectory() as d:
            assert _run(d, name, REFUSER, json.dumps({name: "log"})).returncode == 2, name


def test_a_passing_hook_keeps_its_output_and_a_crash_keeps_its_code():
    with tempfile.TemporaryDirectory() as d:
        r = _run(d, "build_offload_reminder", "print('{\"rewrite\": 1}')\n",
                 json.dumps({"build_offload_reminder": "log"}))
        assert r.returncode == 0 and b"rewrite" in r.stdout, r
        r = _run(d, "build_offload_reminder", "import sys\nsys.exit(3)\n",
                 json.dumps({"build_offload_reminder": "log"}))
        assert r.returncode == 3, r


def test_real_configuration_is_consistent():
    modes = json.loads((REPO / ".claude/gate-modes.json").read_text())
    assert set(modes) <= gate.LOGGABLE, set(modes) - gate.LOGGABLE
    assert all(v in ("log", "enforce") for v in modes.values())
    for kept in ("SEQ-TEARDOWN", "SEQ-WORKTREE", "SEQ-CODEX-WRITE", "SEQ-ASK",
                 "section_commit_gate", "attended_repair_guard", "runner_bash_guard"):
        assert kept not in gate.LOGGABLE, kept
    settings = (REPO / ".claude/settings.json").read_text()
    for name in gate.LOGGABLE:
        if not name.startswith("SEQ-"):
            assert f'gate.py\\" \\"${{CLAUDE_PROJECT_DIR:-.}}/.claude/hooks/{name}.py' in settings, name


def test_the_frozen_supervisor_stays_refused_in_the_guard():
    import run_phase_guard as guard
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude").mkdir()
        (root / ".claude/gate-modes.json").write_text(json.dumps({
            "SEQ-CONTROL-PLANE": "log", "SEQ-PHASE": "log", "SEQ-TEARDOWN": "log"}))
        guard.repo_root = lambda: root
        msg = "[SEQ-CONTROL-PLANE] blocked"
        assert not guard._refusal_is_only_logged(
            msg, {"file_path": str(root / ".claude/hooks/run_phase_guard.py")})
        assert guard._refusal_is_only_logged(
            msg, {"file_path": str(root / ".claude/hooks/skill_step_block.py")})
        assert guard._refusal_is_only_logged("[SEQ-PHASE] x", {})
        assert not guard._refusal_is_only_logged("[SEQ-TEARDOWN] x", {})
        assert not guard._refusal_is_only_logged("no code", {})


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("OK")
