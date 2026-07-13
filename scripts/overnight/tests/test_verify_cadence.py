#!/usr/bin/env python3
# Protects P3.3 (2026-07-12 plan): the fix loop should run TARGETED suites and
# reserve ONE full suite + smoke for the section boundary; a 2026-07-13 run did
# ~40 builds / ~17 smoke mid-loop. verify_cadence_reminder.py counts full-suite +
# smoke runs per cursor section and nudges (WARN, never a gate) past a threshold.
import importlib.util
import io
import json
import sys
import tempfile
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
HOOK = HERE.parents[2] / ".claude/hooks/verify_cadence_reminder.py"


def _load():
    spec = importlib.util.spec_from_file_location("vcr", HOOK)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _sections_repo(d, section="todo/T.md#5"):
    root = pathlib.Path(d)
    (root / ".claude/state").mkdir(parents=True)
    file, idx = section.split("#")
    (root / ".claude/state/sequencer-run.json").write_text(json.dumps(
        {"active": True, "phase": "SECTIONS", "file": file, "section_idx": int(idx)}))
    return root


def _run(mod, cmd):
    payload = {"tool_name": "Bash", "tool_input": {"command": cmd}}
    old = (sys.stdin, sys.stdout)
    sys.stdin, sys.stdout = io.StringIO(json.dumps(payload)), io.StringIO()
    try:
        mod.main()
        out = sys.stdout.getvalue()
    finally:
        sys.stdin, sys.stdout = old
    return out


def test_smoke_warns_at_second():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = _sections_repo(d)
        mod._repo_root = lambda: root
        assert _run(mod, "bash scripts/test-smoke.sh").strip() == ""   # 1st: quiet
        out = _run(mod, "bash scripts/test-smoke.sh")                   # 2nd: warn
        assert "verify-cadence" in out and "smoke" in out, out


def test_full_suite_warns_at_third():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = _sections_repo(d)
        mod._repo_root = lambda: root
        assert _run(mod, "bash scripts/test.sh").strip() == ""
        assert _run(mod, "bash scripts/test.sh").strip() == ""
        out = _run(mod, "bash scripts/overnight/run-artifact.sh l -- bash scripts/test.sh")
        assert "verify-cadence" in out and "full suite" in out, out


def test_targeted_suite_not_counted():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = _sections_repo(d)
        mod._repo_root = lambda: root
        for _ in range(5):
            out = _run(mod, "bash scripts/overnight/run-artifact.sh l -- "
                            "bash scripts/test.sh SUITE=mm")
            assert out.strip() == "", "targeted SUITE= must never nag: " + repr(out)


def test_non_sections_silent():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        (root / ".claude/state").mkdir(parents=True)  # no active sequencer run
        mod._repo_root = lambda: root
        for _ in range(5):
            assert _run(mod, "bash scripts/test-smoke.sh").strip() == ""


def test_section_change_resets_counter():
    mod = _load()
    with tempfile.TemporaryDirectory() as d:
        root = _sections_repo(d, "todo/T.md#5")
        mod._repo_root = lambda: root
        _run(mod, "bash scripts/test-smoke.sh")                         # smoke #1 in sec 5
        # cursor advances to section 6 -> counters reset
        (root / ".claude/state/sequencer-run.json").write_text(json.dumps(
            {"active": True, "phase": "SECTIONS", "file": "todo/T.md", "section_idx": 6}))
        assert _run(mod, "bash scripts/test-smoke.sh").strip() == "", "reset on new section"


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"ok {name}")
    print("PASS: verify-cadence (P3.3)")
